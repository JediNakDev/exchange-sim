#include "exsim/api/worker_process.hpp"
#include "exsim/api/api_json.hpp"
#include "exsim/api/research_run.hpp"
#include "exsim/research/json.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <filesystem>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <system_error>

extern char** environ;

namespace exsim::api {
namespace {
constexpr std::uint32_t kMaxFrame = 64 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

void check_spawn(int error) {
    if (error != 0) throw std::system_error(error, std::generic_category(), "configure simulation worker");
}
struct SpawnConfiguration {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    SpawnConfiguration() {
        check_spawn(::posix_spawn_file_actions_init(&actions));
        const int error = ::posix_spawnattr_init(&attributes);
        if (error != 0) { ::posix_spawn_file_actions_destroy(&actions); check_spawn(error); }
    }
    ~SpawnConfiguration() {
        ::posix_spawn_file_actions_destroy(&actions);
        ::posix_spawnattr_destroy(&attributes);
    }
};

void transfer(int socket, char* data, std::size_t size, bool writing, bool idle, Clock::time_point deadline) {
    while (size != 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (!idle && remaining <= 0) throw std::runtime_error("worker control timeout");
        pollfd descriptor{socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready = ::poll(&descriptor, 1, idle ? -1 : static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) throw std::runtime_error("worker control timeout or poll failure");
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto count = writing ? ::send(socket, data, size, send_flags) : ::recv(socket, data, size, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("worker control channel closed");
        data += count;
        size -= static_cast<std::size_t>(count);
        // Only the first byte may wait indefinitely on an idle worker.
        if (idle) deadline = Clock::now() + std::chrono::seconds(15);
        idle = false;
    }
}

WorkerReply decode_reply(const std::string& message) {
    const auto json = research::parse_json(message);
    const auto* status = json.find("status");
    const auto* body = json.find("body");
    const auto* record = json.find("record");
    const auto* view = json.find("runView");
    std::uint64_t code = 0;
    if (!status || status->kind != research::JsonValue::Kind::Number ||
        !parse_unsigned_integer_text(status->text, code) || code < 200 || code > 599 ||
        !body || body->kind != research::JsonValue::Kind::String ||
        !record || record->kind != research::JsonValue::Kind::Object ||
        !view || view->kind != research::JsonValue::Kind::Object)
        throw std::runtime_error("invalid worker response");
    WorkerReply reply;
    reply.response.status = static_cast<int>(code);
    reply.response.body = body->text;
    reply.record = serialize_json_value(*record);
    reply.run_view = serialize_json_value(*view);
    return reply;
}
}

void require_worker_success(const WorkerReply& reply) {
    if (reply.response.status < 400) return;
    const auto error = research::parse_json(reply.response.body);
    const auto* detail = error.find("error");
    if (!detail || !detail->find("code") || !detail->find("message")) throw std::runtime_error("invalid worker rejection");
    ApiFailure failure(reply.response.status, detail->find("code")->text, detail->find("message")->text);
    if (const auto* state = detail->find("currentRunState")) failure.current_run_state = state->text;
    if (const auto* retryable = detail->find("retryable")) failure.retryable = retryable->boolean;
    if (const auto* issues = detail->find("fieldIssues")) {
        for (const auto& issue : issues->items) {
            const auto* path = issue.find("fieldPath");
            const auto* message = issue.find("issue");
            if (path && message) failure.field_issues.push_back({path->text, message->text});
        }
    }
    throw failure;
}

WorkerRootLock::WorkerRootLock(const std::string& path) {
    descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor_ < 0) throw std::runtime_error("cannot open worker ownership lock");
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        if (Clock::now() >= deadline) {
            ::close(descriptor_); descriptor_ = -1;
            throw std::runtime_error("another API or worker owns this artifact directory");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
WorkerRootLock::~WorkerRootLock() { if (descriptor_ >= 0) ::close(descriptor_); }

std::string read_worker_frame(int socket, bool idle) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    unsigned char header[4];
    transfer(socket, reinterpret_cast<char*>(header), 4, false, idle, deadline);
    std::uint32_t size = 0;
    for (const auto byte : header) size = (size << 8) | byte;
    if (size == 0 || size > kMaxFrame) throw std::runtime_error("invalid worker frame length");
    std::string message(size, '\0');
    transfer(socket, message.data(), size, false, false, Clock::now() + std::chrono::seconds(15));
    return message;
}

void write_worker_frame(int socket, const std::string& message) {
    if (message.empty() || message.size() > kMaxFrame) throw std::runtime_error("worker frame exceeds bound");
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    const auto size = static_cast<std::uint32_t>(message.size());
    char header[4];
    for (unsigned index = 0; index < 4; ++index) header[index] = static_cast<char>(size >> (24 - index * 8));
    transfer(socket, header, 4, true, false, deadline);
    transfer(socket, const_cast<char*>(message.data()), size, true, false, deadline);
}

WorkerProcess::WorkerProcess(const std::string& executable, const std::string& bootstrap, int ownership_fd) {
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) throw std::runtime_error("cannot create worker socket");
    // Source descriptors must not alias the child's fixed control/lock slots.
    const int child_socket = ::fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    ::close(sockets[1]);
    if (child_socket < 0) { ::close(sockets[0]); throw std::runtime_error("cannot reserve worker descriptor"); }
    sockets[1] = child_socket;
    socket_ = sockets[0];
    try {
        for (const int descriptor : sockets) {
            if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0)
                throw std::runtime_error("cannot configure worker socket");
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
                throw std::runtime_error("cannot configure worker SIGPIPE policy");
#endif
        }
        SpawnConfiguration setup;
        check_spawn(::posix_spawn_file_actions_addclose(&setup.actions, sockets[0]));
        // Keep root ownership alive after parent death until every child exits.
        check_spawn(::posix_spawn_file_actions_adddup2(&setup.actions, ownership_fd, 4));
        check_spawn(::posix_spawn_file_actions_adddup2(&setup.actions, sockets[1], 3));
        check_spawn(::posix_spawn_file_actions_addclose(&setup.actions, sockets[1]));
        sigset_t signals;
        sigemptyset(&signals);
        check_spawn(::posix_spawnattr_setsigmask(&setup.attributes, &signals));
        short flags = POSIX_SPAWN_SETSIGMASK;
#ifdef __APPLE__
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
        check_spawn(::posix_spawn_file_actions_addinherit_np(&setup.actions, STDERR_FILENO));
#else
        check_spawn(::posix_spawn_file_actions_addclosefrom_np(&setup.actions, 5));
#endif
        check_spawn(::posix_spawnattr_setflags(&setup.attributes, flags));
        const auto worker_executable = (std::filesystem::path(executable).parent_path()/"exsim_exchanged").string();
        char* arguments[] = {const_cast<char*>(worker_executable.c_str()), const_cast<char*>("--run-worker-fd"), const_cast<char*>("3"), nullptr};
        std::vector<char*> environment;
        for (char** entry = environ; *entry; ++entry) {
            const std::string_view value(*entry);
            if (!value.starts_with("EXSIM_DATABASE_URL=") && !value.starts_with("PGPASSWORD=") && !value.starts_with("PGPASSFILE=")) environment.push_back(*entry);
        }
        environment.push_back(nullptr);
        pid_t child = -1;
        check_spawn(::posix_spawn(&child, worker_executable.c_str(), &setup.actions, &setup.attributes, arguments, environment.data()));
        process_ = child;
    } catch (...) {
        ::close(sockets[1]); ::close(socket_); socket_ = -1;
        throw;
    }
    ::close(sockets[1]);
    try {
        write_worker_frame(socket_, bootstrap);
        const auto ready = decode_reply(read_worker_frame(socket_));
        if (ready.response.status != 200) throw std::runtime_error("worker initialization rejected");
    } catch (...) { stop(); throw; }
}

WorkerProcess::~WorkerProcess() { stop(); }
void WorkerProcess::stop() noexcept {
    std::lock_guard lock(mutex_);
    if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
    if (process_ >= 0) {
        ::kill(process_, SIGKILL);
        int status = 0;
        while (::waitpid(process_, &status, 0) < 0 && errno == EINTR) {}
        process_ = -1;
    }
}
WorkerReply WorkerProcess::exchange(const std::string& message) {
    std::lock_guard lock(mutex_);
    if (socket_ < 0) throw std::runtime_error("worker has stopped");
    try {
        write_worker_frame(socket_, message);
        return decode_reply(read_worker_frame(socket_));
    } catch (...) {
        // No subsequent request may mistake a late acknowledgment for its own.
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_); socket_ = -1;
        throw;
    }
}
WorkerReply WorkerProcess::request(const HttpRequest& request) {
    ApiJsonWriter json;
    json.begin_object().key("method").string(request.method).key("target").string(request.target)
        .key("body").string(request.body).end_object();
    return exchange(json.take());
}
WorkerReply WorkerProcess::shutdown() { return exchange("{\"shutdown\":true}"); }

} // namespace exsim::api
