#include "api/exchange_process.hpp"
#include "api/json.hpp"
#include "ipc/exchange_control.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace exsim::api {
ExchangeProcess::~ExchangeProcess() { stop(); }
bool ExchangeProcess::spawn(const std::filesystem::path& executable, const std::filesystem::path& config_path) {
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) throw std::runtime_error("cannot create control socket");
    net::Socket parent(descriptors[0]), child(descriptors[1]);
    (void)net::set_no_sigpipe(parent.fd());
    (void)net::set_non_blocking(parent.fd());
    ::fcntl(parent.fd(), F_SETFD, FD_CLOEXEC);
    ::fcntl(child.fd(), F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child.fd(), 3);
#ifdef __linux__
    posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#endif
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t mask;
    sigemptyset(&mask);
    posix_spawnattr_setsigmask(&attributes, &mask);
    short flags = POSIX_SPAWN_SETSIGMASK;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    const std::string binary = executable.string(), config = config_path.string();
    char* arguments[] = {const_cast<char*>(binary.c_str()), const_cast<char*>("--config"), const_cast<char*>(config.c_str()),
                         const_cast<char*>("--control-fd"), const_cast<char*>("3"), nullptr};
    const int result = ::posix_spawn(&pid_, binary.c_str(), &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    child.close();
    if (result != 0) {
        pid_ = -1;
        state_ = ExchangeProcessState::Failed;
        error_ = std::strerror(result);
        return false;
    }
    control_ = std::move(parent);
    try {
        exit_watcher_ = std::thread([this, child_pid = pid_] {
            siginfo_t information{};
            while (::waitid(P_PID, static_cast<id_t>(child_pid), &information, WEXITED | WNOWAIT) != 0 && errno == EINTR) {}
            {
                std::lock_guard guard(exit_mutex_);
                exited_ = true;
            }
            exit_condition_.notify_all();
        });
    } catch (...) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        control_.close();
        throw;
    }
    return true;
}
bool ExchangeProcess::await_readiness() {
    try {
        snapshot_ = ipc::receive_control(control_.fd(), 10000);
        const auto ready = parse_json(snapshot_);
        if (ready.find("error")) throw std::runtime_error(ready.find("error")->text);
        if (!ready.find("state") || ready.find("state")->text != "running") throw std::runtime_error("exchange did not report readiness");
    } catch (const std::exception& error) {
        stop_after_failure(error.what());
        return false;
    }
    state_ = ExchangeProcessState::Running;
    error_.clear();
    return true;
}
bool ExchangeProcess::read_status() {
    try {
        ipc::send_control(control_.fd(), "status");
        snapshot_ = ipc::receive_control(control_.fd());
        if (!parse_json(snapshot_).find("connections")) throw std::runtime_error("exchange snapshot has no connections");
        return true;
    } catch (const std::exception&) {
        stop_after_failure("exchange control failed");
        return false;
    }
}
void ExchangeProcess::reap_if_exited() {
    if (pid_ <= 0 || !has_exited()) return;
    const bool clean_exit = reap();
    state_ = clean_exit ? ExchangeProcessState::Stopped : ExchangeProcessState::Failed;
    error_ = clean_exit ? "" : "exchange process exited unexpectedly";
}
void ExchangeProcess::request_stop() noexcept {
    if (pid_ > 0) ::kill(pid_, SIGTERM);
}
void ExchangeProcess::stop() noexcept {
    if (pid_ <= 0) return;
    request_stop();
    {
        std::unique_lock guard(exit_mutex_);
        if (!exit_condition_.wait_for(guard, std::chrono::milliseconds(4500), [this] { return exited_; })) ::kill(pid_, SIGKILL);
    }
    (void)reap();
    state_ = ExchangeProcessState::Stopped;
}
bool ExchangeProcess::has_exited() {
    std::lock_guard guard(exit_mutex_);
    return exited_;
}
void ExchangeProcess::clear_failure() noexcept {
    state_ = ExchangeProcessState::Stopped;
    error_.clear();
}
// Joins the watcher, collects the exit status and returns whether the child
// exited with status 0.
bool ExchangeProcess::reap() noexcept {
    if (exit_watcher_.joinable()) exit_watcher_.join();
    int status = 0;
    pid_t result;
    while ((result = ::waitpid(pid_, &status, 0)) < 0 && errno == EINTR) {}
    {
        std::lock_guard guard(exit_mutex_);
        exited_ = false;
    }
    pid_ = -1;
    control_.close();
    snapshot_.clear();
    return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
void ExchangeProcess::stop_after_failure(std::string message) noexcept {
    stop();
    state_ = ExchangeProcessState::Failed;
    error_ = std::move(message);
}
}
