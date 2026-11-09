// exsim_api: the local research HTTP API (docs/API.md, docs/api/openapi.yaml).
//
//   exsim_api [--port N] [--bind ADDRESS] [--data-dir DIR] [--max-live-runs N]
//
// --port 0 binds an ephemeral port. Once listening, the service prints one
// line "exsim_api listening on http://<address>:<port>" to stdout, which the
// development script and the end-to-end test read. SIGINT or SIGTERM stops
// the service; runs that were still live are recorded as failed.

#include "exsim/api/http_server.hpp"
#include "exsim/api/research_service.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include <pthread.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

constexpr std::string_view kUsage =
    "usage: exsim_api [--port N] [--bind ADDRESS] [--data-dir DIR] [--max-live-runs N]\n"
    "  --port N           TCP port (default 7600; 0 picks an ephemeral port)\n"
    "  --bind ADDRESS     IPv4 address to bind (default 127.0.0.1; v1 has no access control)\n"
    "  --data-dir DIR     experiments, runs and artifacts (default ./exsim-api-data)\n"
    "  --max-live-runs N  simulations held at once, running or paused (default 8)\n"
    "  --source-revision ID  supplied source revision; omitted means unavailable\n"
    "  --release-id ID       supplied release identity; omitted means unavailable\n"
    "  --import-file-metadata  import existing metadata into an empty PostgreSQL database\n"
    "  EXSIM_DATABASE_URL     libpq connection string; unset selects legacy files\n";

bool parse_unsigned_argument(std::string_view text, std::uint64_t maximum, std::uint64_t& value) {
    if (text.empty() || text.size() > 20) return false;
    std::uint64_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10 + static_cast<std::uint64_t>(character - '0');
        if (parsed > maximum) return false;
    }
    value = parsed;
    return true;
}

}  // namespace

int main(int argument_count, char** arguments) {
    exsim::api::HttpServerOptions server_options;
    exsim::api::ResearchServiceOptions service_options;
    service_options.isolate_runs = true;
    if (const char* database = std::getenv("EXSIM_DATABASE_URL")) service_options.database_url = database;
#ifdef __APPLE__
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::string executable(length, '\0');
    if (_NSGetExecutablePath(executable.data(), &length) != 0) return 1;
    service_options.executable_path = std::filesystem::canonical(executable.c_str()).string();
#else
    service_options.executable_path = std::filesystem::canonical("/proc/self/exe").string();
#endif
    for (int index = 1; index < argument_count; ++index) {
        const std::string_view flag = arguments[index];
        if (flag == "--help" || flag == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (flag == "--import-file-metadata") {
            service_options.import_file_metadata = true;
            continue;
        }
        if (index + 1 >= argument_count) {
            std::cerr << "exsim_api: " << flag << " needs a value\n" << kUsage;
            return 2;
        }
        const std::string_view value = arguments[++index];
        std::uint64_t number = 0;
        if (flag == "--port" && parse_unsigned_argument(value, 65535, number)) {
            server_options.port = static_cast<std::uint16_t>(number);

        } else if (flag == "--bind") {
            server_options.bind_address = std::string(value);
        } else if (flag == "--source-revision") {
            service_options.source_revision = value;
        } else if (flag == "--release-id") {
            service_options.release_id = value;
        } else if (flag == "--data-dir") {
            service_options.data_directory = std::string(value);
        } else if (flag == "--max-live-runs" && parse_unsigned_argument(value, 256, number) && number > 0) {
            service_options.max_live_simulations = static_cast<std::size_t>(number);
        } else {
            std::cerr << "exsim_api: bad argument " << flag << " " << value << "\n" << kUsage;
            return 2;
        }
    }


    // Block the stop signals in every thread; the main thread waits for them.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        exsim::api::ResearchService service(service_options);
        exsim::api::HttpServer server(
            server_options, [&service](const exsim::api::HttpRequest& request) { return service.handle(request); },
            [&service](int status, std::string_view code, std::string_view message) { return service.rejection_response(status, code, message); });
        server.start();
        std::cout << "exsim_api listening on http://" << server_options.bind_address << ":" << server.port() << std::endl;
        int received_signal = 0;
        sigwait(&stop_signals, &received_signal);
        std::cerr << "exsim_api: stopping on signal " << received_signal << "\n";
        server.stop();
        service.shutdown();
    } catch (const std::exception& error) {
        std::cerr << "exsim_api: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
