#include "api/json.hpp"
#include "api/runtime_snapshot.hpp"
#include "ipc/exchange_control.hpp"
#include "server/server_runtime.hpp"
#include "test/framework/test.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace exsim;

server::ServerConfig loopback_config_with_ephemeral_ports() {
    server::ServerConfig config;
    std::string error;
    REQUIRE(server::load_config("test/fixtures/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.exchange.fix_port = 0;
    config.market_data.port = 0;
    config.market_data.retransmit_port = 0;
    return config;
}

struct ControlSocketPair {
    net::Socket exchange_end;
    net::Socket api_end;
    ControlSocketPair() {
        int fds[2] = {-1, -1};
        REQUIRE_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        exchange_end = net::Socket(fds[0]);
        api_end = net::Socket(fds[1]);
        REQUIRE_EQ(net::set_non_blocking(exchange_end.fd()), 0);
        REQUIRE_EQ(net::set_no_sigpipe(exchange_end.fd()), 0);
        REQUIRE_EQ(net::set_no_sigpipe(api_end.fd()), 0);
    }
};

// Test owner of the watched fd: the same command protocol as exsim_exchanged,
// with a count of callback invocations.
struct TestControlHandler {
    int fd = -1;
    server::ServerRuntime* runtime = nullptr;
    unsigned readable_callbacks = 0;
    unsigned commands_handled = 0;

    static void on_readable(void* context) noexcept {
        auto& handler = *static_cast<TestControlHandler*>(context);
        ++handler.readable_callbacks;
        try {
            const auto command = ipc::receive_control(handler.fd);
            if (command == "stop") handler.runtime->request_shutdown();
            else if (command != "status") throw std::runtime_error("invalid control command");
            ipc::send_control(handler.fd, api::runtime_snapshot_json(*handler.runtime));
            ++handler.commands_handled;
        } catch (...) {
            handler.runtime->request_shutdown();
            handler.runtime->unwatch_readable_fd();
        }
    }
};

void poll_until_command_handled(server::ServerRuntime& runtime, TestControlHandler& handler, unsigned expected) {
    const auto deadline = net::monotonic_ns() + 2'000'000'000;
    while (handler.commands_handled < expected && net::monotonic_ns() < deadline) runtime.poll_once();
    REQUIRE_EQ(handler.commands_handled, expected);
}

void check_status_then_stop(bool busy_poll) {
    const auto config = loopback_config_with_ephemeral_ports();
    server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.busy_poll = busy_poll;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    ControlSocketPair control;
    TestControlHandler handler{control.exchange_end.fd(), &runtime};
    REQUIRE_EQ(runtime.watch_readable_fd(control.exchange_end.fd(), TestControlHandler::on_readable, &handler), 0);

    // Nothing to read: the callback is not called.
    for (int iteration = 0; iteration < 5; ++iteration) runtime.poll_once();
    CHECK_EQ(handler.readable_callbacks, 0u);

    ipc::send_control(control.api_end.fd(), "status");
    poll_until_command_handled(runtime, handler, 1);
    const auto status = api::parse_json(ipc::receive_control(control.api_end.fd()));
    REQUIRE(status.find("liveOrders") != nullptr);
    CHECK_EQ(status.find("liveOrders")->text, "0");
    CHECK(!runtime.finished());

    ipc::send_control(control.api_end.fd(), "stop");
    poll_until_command_handled(runtime, handler, 2);
    CHECK(api::parse_json(ipc::receive_control(control.api_end.fd())).find("liveOrders") != nullptr);
    const auto deadline = net::monotonic_ns() + 4'000'000'000;
    while (!runtime.finished() && net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK(runtime.finished());
}

std::filesystem::path exchange_binary() {
    const char* build = std::getenv("EXSIM_BUILD");
    return std::filesystem::path(build ? build : "build/release") / "exsim_exchanged";
}

struct TemporaryConfigFile {
    std::filesystem::path path;
    TemporaryConfigFile() {
        char text[] = "/tmp/exsim-control-XXXXXX";
        const char* directory = ::mkdtemp(text);
        REQUIRE(directory != nullptr);
        path = std::filesystem::path(directory) / "exchange.conf";
        std::ifstream fixture("test/fixtures/exchange.conf");
        REQUIRE(fixture.good());
        std::string config((std::istreambuf_iterator<char>(fixture)), {});
        for (const std::string_view key : {"ouch_port = 15000", "port = 16000", "retransmit_port = 16001"}) {
            const auto position = config.find(key);
            REQUIRE(position != std::string::npos);
            const auto equals = key.find('=');
            config.replace(position, key.size(), std::string(key.substr(0, equals)) + "= 0");
        }
        std::ofstream(path) << config;
    }
    ~TemporaryConfigFile() {
        std::error_code error;
        std::filesystem::remove_all(path.parent_path(), error);
    }
};

// exsim_exchanged with the exchange end of a socketpair on fd 3, as the API
// starts it.
struct ExchangeChild {
    pid_t pid = -1;
    net::Socket api_end;
    explicit ExchangeChild(const std::filesystem::path& config) {
        int fds[2] = {-1, -1};
        REQUIRE_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        const std::string binary = exchange_binary().string();
        const std::string config_path = config.string();
        pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::close(fds[1]);
            if (fds[0] != 3) {
                ::dup2(fds[0], 3);
                ::close(fds[0]);
            }
            const int null_fd = ::open("/dev/null", O_WRONLY);
            if (null_fd >= 0) ::dup2(null_fd, STDOUT_FILENO);
            ::execl(binary.c_str(), binary.c_str(), "--config", config_path.c_str(), "--control-fd", "3", nullptr);
            ::_exit(127);
        }
        ::close(fds[0]);
        api_end = net::Socket(fds[1]);
        REQUIRE_EQ(net::set_no_sigpipe(api_end.fd()), 0);
    }
    // Exit status, or -1 if the child did not exit before the deadline.
    int wait_for_exit(std::uint64_t timeout_ns) {
        const auto deadline = net::monotonic_ns() + timeout_ns;
        while (net::monotonic_ns() < deadline) {
            int status = 0;
            const pid_t done = ::waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            ::usleep(1000);
        }
        return -1;
    }
    ~ExchangeChild() {
        if (pid <= 0) return;
        ::kill(pid, SIGKILL);
        int status = 0;
        (void)::waitpid(pid, &status, 0);
    }
};

// One write carrying several frames, as the API could send back to back.
void send_frames_in_one_write(int fd, std::initializer_list<std::string_view> commands) {
    std::string bytes;
    for (const auto command : commands) {
        const auto size = static_cast<std::uint32_t>(command.size());
        const char header[4] = {static_cast<char>(size >> 24), static_cast<char>(size >> 16),
                                static_cast<char>(size >> 8), static_cast<char>(size)};
        bytes.append(header, sizeof header);
        bytes.append(command);
    }
    REQUIRE_EQ(::send(fd, bytes.data(), bytes.size(), 0), static_cast<ssize_t>(bytes.size()));
}
}  // namespace

TEST_CASE(control_fd_runtime, status_replies_and_stop_finishes_in_idle_mode) { check_status_then_stop(false); }

TEST_CASE(control_fd_runtime, status_replies_and_stop_finishes_in_busy_poll_mode) { check_status_then_stop(true); }

TEST_CASE(control_fd_runtime, readable_control_fd_wakes_a_long_idle_wait) {
    const auto config = loopback_config_with_ephemeral_ports();
    server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.idle_poll_timeout_ms = 10'000;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    ControlSocketPair control;
    TestControlHandler handler{control.exchange_end.fd(), &runtime};
    REQUIRE_EQ(runtime.watch_readable_fd(control.exchange_end.fd(), TestControlHandler::on_readable, &handler), 0);
    // With a 10 s idle timeout, one poll_once returns promptly only because
    // the readable control fd ends the wait.
    ipc::send_control(control.api_end.fd(), "status");
    const auto started = net::monotonic_ns();
    runtime.poll_once();
    CHECK_LT(net::monotonic_ns() - started, 2'000'000'000u);
    REQUIRE_EQ(handler.commands_handled, 1u);
    CHECK(api::parse_json(ipc::receive_control(control.api_end.fd())).find("liveOrders") != nullptr);
}

TEST_CASE(control_fd_runtime, unwatched_fd_no_longer_calls_the_owner) {
    const auto config = loopback_config_with_ephemeral_ports();
    server::ServerRuntimeOptions options;
    options.log = nullptr;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    ControlSocketPair control;
    TestControlHandler handler{control.exchange_end.fd(), &runtime};
    REQUIRE_EQ(runtime.watch_readable_fd(control.exchange_end.fd(), TestControlHandler::on_readable, &handler), 0);
    runtime.unwatch_readable_fd();
    ipc::send_control(control.api_end.fd(), "status");
    for (int iteration = 0; iteration < 20; ++iteration) runtime.poll_once();
    CHECK_EQ(handler.readable_callbacks, 0u);
    // Invalid command: the owner requests shutdown and unwatches.
    REQUIRE_EQ(runtime.watch_readable_fd(control.exchange_end.fd(), TestControlHandler::on_readable, &handler), 0);
    poll_until_command_handled(runtime, handler, 1);
    (void)ipc::receive_control(control.api_end.fd());
    ipc::send_control(control.api_end.fd(), "bogus");
    const auto deadline = net::monotonic_ns() + 4'000'000'000;
    while (!runtime.finished() && net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK(runtime.finished());
    CHECK_EQ(handler.readable_callbacks, 2u);
}

TEST_CASE(control_fd_runtime, exchanged_answers_back_to_back_commands_and_stops) {
    TemporaryConfigFile config;
    ExchangeChild child(config.path);
    const auto ready = api::parse_json(ipc::receive_control(child.api_end.fd(), 10'000));
    REQUIRE(ready.find("liveOrders") != nullptr);
    // Two frames in one write: both are answered from one readable event.
    send_frames_in_one_write(child.api_end.fd(), {"status", "status"});
    CHECK(api::parse_json(ipc::receive_control(child.api_end.fd())).find("liveOrders") != nullptr);
    CHECK(api::parse_json(ipc::receive_control(child.api_end.fd())).find("liveOrders") != nullptr);
    ipc::send_control(child.api_end.fd(), "stop");
    CHECK(api::parse_json(ipc::receive_control(child.api_end.fd())).find("liveOrders") != nullptr);
    CHECK_EQ(child.wait_for_exit(10'000'000'000), 0);
}

TEST_CASE(control_fd_runtime, exchanged_stops_on_an_invalid_command) {
    TemporaryConfigFile config;
    ExchangeChild child(config.path);
    REQUIRE(api::parse_json(ipc::receive_control(child.api_end.fd(), 10'000)).find("liveOrders") != nullptr);
    ipc::send_control(child.api_end.fd(), "bogus");
    CHECK_EQ(child.wait_for_exit(10'000'000'000), 0);
}
