#include "api/api_json.hpp"
#include "api/runtime_snapshot.hpp"
#include "ipc/exchange_control.hpp"
#include "server/config.hpp"
#include "server/server_runtime.hpp"
#include <csignal>
#include <iostream>
#include <poll.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }
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
        while (!runtime.finished()) {
            if (stopping) runtime.request_shutdown();
            if (control >= 0) {
                pollfd event{control, POLLIN, 0};
                if (::poll(&event, 1, 0) > 0) {
                    try {
                        const auto command = exsim::ipc::receive_control(control);
                        if (command == "stop") runtime.request_shutdown();
                        else if (command != "status") throw std::runtime_error("invalid control command");
                        exsim::ipc::send_control(control, exsim::api::runtime_snapshot_json(runtime));
                    } catch (...) {
                        runtime.request_shutdown();
                        ::close(control);
                        control = -1;
                    }
                }
            }
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
