#include "api/exchange_service.hpp"
#include "api/http_server.hpp"
#include <charconv>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <pthread.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
int main(int argc, char** argv) {
    exsim::api::HttpServerOptions options;
    std::filesystem::path directory = "exchanges";
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (flag == "--help") { std::cout << "exsim_api [--bind IPv4] [--port PORT] [--data-dir PATH]\n"; return 0; }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string_view value = argv[++i];
            if (flag == "--bind") options.bind_address = value;
            else if (flag == "--data-dir") directory = value;
            else if (flag == "--port") {
                unsigned port = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
                if (error != std::errc{} || end != value.data() + value.size() || port > 65535) throw std::invalid_argument("invalid port");
                options.port = static_cast<std::uint16_t>(port);
            } else throw std::invalid_argument("unknown option");
        }
        ::umask(0077);
        std::signal(SIGPIPE, SIG_IGN);
        sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#ifdef __APPLE__
        std::uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
        std::string path(size, '\0');
        if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("cannot resolve executable");
        const auto binary = std::filesystem::canonical(path.c_str());
#else
        const auto binary = std::filesystem::canonical("/proc/self/exe");
#endif
        exsim::api::ExchangeService service(directory, binary.parent_path() / "exsim_exchanged");
        exsim::api::HttpServer server(options, [&service](const auto& request) { return service.handle(request); }, exsim::api::exchange_error);
        server.start();
        std::cout << "exsim_api listening on http://" << options.bind_address << ':' << server.port() << std::endl;
        int signal = 0; sigwait(&signals, &signal);
        server.stop();
    } catch (const std::exception& error) { std::cerr << "exsim_api: " << error.what() << '\n'; return 1; }
    return 0;
}
