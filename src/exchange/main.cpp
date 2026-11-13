#include "api/api_json.hpp"
#include "api/runtime_snapshot.hpp"
#include "ipc/exchange_control.hpp"
#include "server/config.hpp"
#include "server/server_runtime.hpp"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }

// The management API's end of the control socketpair, watched by the runtime
// poller. Commands: "status" replies with a snapshot, "stop" requests shutdown
// and then replies; anything else or an IPC error requests shutdown and closes
// the socket.
struct ControlSocket {
    int& fd;  // main's control fd: set to -1 here when the socket is closed
    exsim::server::ServerRuntime& runtime;
};

// True while the non-blocking socket has unread bytes, EOF or an error, so the
// next receive_control either reads a command or reports the failure.
bool control_has_input(int fd) noexcept {
    char byte = 0;
    const ssize_t count = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return count >= 0 || (errno != EINTR && !exsim::net::is_would_block(errno));
}

void handle_control_commands(void* context) noexcept {
    auto& control = *static_cast<ControlSocket*>(context);
    while (control.fd >= 0 && control_has_input(control.fd)) {
        try {
            const auto command = exsim::ipc::receive_control(control.fd);
            if (command == "stop") control.runtime.request_shutdown();
            else if (command != "status") throw std::runtime_error("invalid control command");
            exsim::ipc::send_control(control.fd, exsim::api::runtime_snapshot_json(control.runtime));
        } catch (...) {
            control.runtime.request_shutdown();
            control.runtime.unwatch_readable_fd();
            ::close(control.fd);
            control.fd = -1;
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    int control = -1;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            path = argv[++i];
        } else if (arg == "--control-fd" && i + 1 < argc && std::string_view(argv[++i]) == "3") {
            control = 3;
        } else {
            std::cerr << "usage: exsim_exchanged --config PATH [--control-fd 3]\n";
            return 2;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);
    try {
        exsim::server::ServerConfig config;
        std::string error;
        if (path.empty() || !exsim::server::load_config(path.c_str(), config, error))
            throw std::runtime_error("invalid exchange config: " + error);
        if (control >= 0) {
            (void)exsim::net::set_non_blocking(control);
            (void)exsim::net::set_no_sigpipe(control);
        }
        const auto session = std::to_string(static_cast<unsigned long>(::getpid())) + std::to_string(exsim::net::monotonic_ns());
        config.market_data.session = session.substr(session.size() - 10);
        config.exchange.soup_session = config.market_data.session;
        exsim::server::ServerRuntimeOptions options;
        exsim::server::ServerRuntime runtime(config, options);
        error = runtime.start();
        if (!error.empty()) throw std::runtime_error(error);
        const auto ready = exsim::api::runtime_snapshot_json(runtime);
        if (control >= 0) exsim::ipc::send_control(control, ready, 10000);
        std::cout << ready << std::endl;
        ControlSocket control_socket{control, runtime};
        if (control >= 0) {
            if (const exsim::net::ErrorCode watch_error =
                    runtime.watch_readable_fd(control, handle_control_commands, &control_socket);
                watch_error != 0) {
                throw std::runtime_error("control socket poller: " + std::string(std::strerror(watch_error)));
            }
        }
        while (!runtime.finished()) {
            if (stopping) runtime.request_shutdown();
            runtime.poll_once();
        }
    } catch (const std::exception& error) {
        if (control >= 0) {
            try {
                exsim::api::ApiJsonWriter json;
                json.begin_object().key("error").string(error.what()).end_object();
                exsim::ipc::send_control(control, json.take());
            } catch (...) {
            }
        }
        std::cerr << "exsim_exchanged: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
