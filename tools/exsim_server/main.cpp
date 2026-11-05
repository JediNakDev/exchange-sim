// exsim_server: the exchange simulator server (docs/SERVER_RUNTIME.md).
//
//   exsim_server [--config PATH] [--busy-poll] [--cpu N] [--journal PATH]
//                [--day-time HH:MM[:SS]] [--md-heartbeat-ms N] [--no-bots]
//                [--admin-bind ADDR]
//
// Runs one trading day: until the admin `shutdown` command, SIGINT/SIGTERM,
// or the configured trading_day_end. Exit status 0 after a clean end of day,
// 1 on bad arguments, configuration or startup errors.

#include "exsim/server/config.hpp"
#include "exsim/server/runtime/thread_affinity.hpp"
#include "exsim/server/server_runtime.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> shutdown_signal_received{false};

extern "C" void on_shutdown_signal(int) { shutdown_signal_received.store(true, std::memory_order_relaxed); }

struct CommandLine {
    std::string config_path = "config/exsim.conf";
    bool busy_poll = false;
    int cpu = -1;
    std::string journal_path;
    bool journal_path_set = false;
    std::string day_time;
    long md_heartbeat_ms = 1000;
    bool bots_enabled = true;
    std::string admin_bind_address = "127.0.0.1";
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: exsim_server [--config PATH] [--busy-poll] [--cpu N] [--journal PATH]\n"
                 "                    [--day-time HH:MM[:SS]] [--md-heartbeat-ms N] [--no-bots]\n"
                 "                    [--admin-bind ADDR]\n");
}

bool parse_non_negative(std::string_view text, long maximum, long& value_out) {
    if (text.empty() || text.size() > 9) return false;
    long value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + (digit - '0');
    }
    if (value > maximum) return false;
    value_out = value;
    return true;
}

bool parse_command_line(int argc, char** argv, CommandLine& command_line) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--busy-poll") {
            command_line.busy_poll = true;
            continue;
        }
        if (flag == "--no-bots") {
            command_line.bots_enabled = false;
            continue;
        }
        if (index + 1 >= argc) return false;
        const std::string_view value = argv[++index];
        long number = 0;
        if (flag == "--config") {
            command_line.config_path = value;
        } else if (flag == "--cpu") {
            if (!parse_non_negative(value, 4095, number)) return false;
            command_line.cpu = static_cast<int>(number);
        } else if (flag == "--journal") {
            command_line.journal_path = value;
            command_line.journal_path_set = true;
        } else if (flag == "--day-time") {
            command_line.day_time = value;
        } else if (flag == "--md-heartbeat-ms") {
            if (!parse_non_negative(value, 3'600'000, number) || number == 0) return false;
            command_line.md_heartbeat_ms = number;
        } else if (flag == "--admin-bind") {
            command_line.admin_bind_address = value;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    CommandLine command_line;
    if (!parse_command_line(argc, argv, command_line)) {
        print_usage();
        return 1;
    }
    exsim::server::ServerConfig config;
    std::string error;
    if (!exsim::server::load_config(command_line.config_path.c_str(), config, error)) {
        std::fprintf(stderr, "exsim: %s: %s\n", command_line.config_path.c_str(), error.c_str());
        return 1;
    }
    if (command_line.journal_path_set) config.journal_path = command_line.journal_path;

    exsim::server::ServerRuntimeOptions options;
    options.busy_poll = command_line.busy_poll;
    options.admin_bind_address = command_line.admin_bind_address;
    options.bots_enabled_at_start = command_line.bots_enabled;
    options.market_data.heartbeat_interval_ns = static_cast<std::uint64_t>(command_line.md_heartbeat_ms) * 1'000'000;
    if (!command_line.day_time.empty()) {
        exsim::Timestamp day_time = 0;
        if (!exsim::server::parse_day_time(command_line.day_time, day_time)) {
            std::fprintf(stderr, "exsim: --day-time: expected HH:MM[:SS[.fraction]]\n");
            return 1;
        }
        options.day_time_at_start = day_time;
    }

    if (command_line.cpu >= 0) {
        std::string message;
        const bool pinned = exsim::server::runtime::pin_current_thread_to_cpu(command_line.cpu, message);
        std::fprintf(stderr, "exsim: %s%s\n", pinned ? "" : "warning: ", message.c_str());
    }

    try {
        auto server = std::make_unique<exsim::server::ServerRuntime>(config, options);
        const std::string failure = server->start();
        if (!failure.empty()) {
            std::fprintf(stderr, "exsim: startup failed: %s\n", failure.c_str());
            return 1;
        }
        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, on_shutdown_signal);
        std::signal(SIGTERM, on_shutdown_signal);
        std::fprintf(stderr, "exsim: running (%s)\n", command_line.busy_poll ? "busy poll" : "blocking poll");
        while (!server->finished()) {
            if (shutdown_signal_received.load(std::memory_order_relaxed)) server->request_shutdown();
            server->poll_once();
        }
        const bool journal_ok = server->core().journal_state() != exsim::server::JournalState::Failed;
        std::fprintf(stderr, "exsim: end of day, md next_sequence=%llu event_hash=%llu%s\n",
                     static_cast<unsigned long long>(server->market_data().next_sequence()),
                     static_cast<unsigned long long>(server->market_data().event_stream_hash()),
                     journal_ok ? "" : " (journal FAILED)");
        return journal_ok ? 0 : 1;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "exsim: %s\n", exception.what());
        return 1;
    }
}
