// exsim_w2w_bench: wire-to-wire latency of a running exsim server over
// loopback sockets (docs/SERVER_RUNTIME.md, "Wire-to-wire latency").
//
//   exsim_w2w_bench [--rate N] [--orders N] [--warmup N] [--window N]
//                   [--busy-poll] [--server-cpu N] [--client-cpu N]
//
// The server runs in this process on its own thread (the same
// ServerRuntime as exsim_server: OUCH, FIX, admin, ITCH feed, no bots, no
// journal). The client thread sends OUCH Enter Orders at a fixed rate
// (--rate per second; 0 = as fast as possible with at most --window orders
// waiting for their acceptance) and cancels each order once it is accepted.
// For every order it measures, on the client's monotonic clock:
//
//   ouch accept   Enter Order written to the socket -> Order Accepted read
//   itch add      Enter Order written to the socket -> ITCH Add Order read
//                 from the market-data feed (UDP)
//
// and prints p50/p90/p99/p99.9/max in nanoseconds plus the achieved order
// throughput. The first --warmup orders are not measured.
// Exit status 0 after a complete run, 1 on bad arguments or setup failure,
// 2 when the run timed out.

#include "exsim/client/exchange_client_sessions.hpp"
#include "exsim/server/runtime/thread_affinity.hpp"
#include "exsim/server/server_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace exsim;
namespace ouch42 = exsim::protocol::ouch42;

struct BenchOptions {
    std::uint64_t orders_per_second = 20'000;
    std::uint64_t measured_orders = 100'000;
    std::uint64_t warmup_orders = 10'000;
    std::uint64_t window = 64;  // only with --rate 0
    bool busy_poll = false;
    int server_cpu = -1;
    int client_cpu = -1;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: exsim_w2w_bench [--rate N] [--orders N] [--warmup N] [--window N] [--busy-poll]\n"
                 "                       [--server-cpu N] [--client-cpu N]\n");
}

bool parse_unsigned(std::string_view text, std::uint64_t maximum, std::uint64_t& value_out) {
    if (text.empty() || text.size() > 19) return false;
    std::uint64_t value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + std::uint64_t(digit - '0');
    }
    if (value > maximum) return false;
    value_out = value;
    return true;
}

bool parse_arguments(int argc, char** argv, BenchOptions& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--busy-poll") {
            options.busy_poll = true;
            continue;
        }
        if (index + 1 >= argc) return false;
        const std::string_view value = argv[++index];
        std::uint64_t number = 0;
        if (flag == "--rate") {
            if (!parse_unsigned(value, 10'000'000, options.orders_per_second)) return false;
        } else if (flag == "--orders") {
            if (!parse_unsigned(value, 2'000'000, options.measured_orders) || options.measured_orders == 0) {
                return false;
            }
        } else if (flag == "--warmup") {
            if (!parse_unsigned(value, 1'000'000, options.warmup_orders)) return false;
        } else if (flag == "--window") {
            if (!parse_unsigned(value, 100'000, options.window) || options.window == 0) return false;
        } else if (flag == "--server-cpu") {
            if (!parse_unsigned(value, 4095, number)) return false;
            options.server_cpu = static_cast<int>(number);
        } else if (flag == "--client-cpu") {
            if (!parse_unsigned(value, 4095, number)) return false;
            options.client_cpu = static_cast<int>(number);
        } else {
            return false;
        }
    }
    return true;
}

std::string bench_config_text(std::uint64_t total_orders) {
    // Every order uses a fresh token; each produces an accept and a cancel.
    const std::uint64_t tokens = total_orders + 1024;
    const std::uint64_t stored_messages = total_orders * 2 + 1024;
    const std::uint64_t stored_bytes = total_orders * 128 + (1u << 20);
    return "[exchange]\n"
           "ouch_port = 0\n"
           "fix_port = 0\n"
           "admin_port = 0\n"
           "heartbeat_interval_ms = 1000\n"
           "max_connections = 4\n"
           "[market_data]\n"
           "address = 127.0.0.1\n"
           "retransmit_port = 0\n"
           "noii_interval_ms = 1000\n"
           "[engine]\n"
           "max_instruments = 4\n"
           "max_orders = 65536\n"
           "max_tokens_per_session = " + std::to_string(tokens) + "\n"
           "store_messages_per_session = " + std::to_string(stored_messages) + "\n"
           "store_bytes_per_session = " + std::to_string(stored_bytes) + "\n"
           "[instrument BENCH]\n"
           "tick_size = 0.01\n"
           "min_price = 0.01\n"
           "max_price = 1000\n"
           "reference_price = 100.00\n"
           "initial_phase = continuous\n"
           "[user W2W]\n"
           "password = w2w\n"
           "protocol = ouch42\n"
           "session_id = 1\n"
           "owner = 1\n";
}

// Client-side bookkeeping, sized up front.
struct LatencyRecorder {
    std::vector<std::uint64_t> send_time_ns;   // per order index
    std::vector<std::uint64_t> accept_latency_ns;
    std::vector<std::uint64_t> itch_add_latency_ns;
    std::vector<std::uint32_t> orders_to_cancel;
    std::uint64_t warmup_orders = 0;
    std::uint64_t accepted_orders = 0;
    std::uint64_t canceled_orders = 0;
    std::uint64_t rejected_orders = 0;
    std::uint64_t itch_add_orders = 0;
    std::uint64_t last_accept_time_ns = 0;

    explicit LatencyRecorder(std::uint64_t total_orders, std::uint64_t warmup) : warmup_orders(warmup) {
        send_time_ns.assign(total_orders, 0);
        accept_latency_ns.reserve(total_orders);
        itch_add_latency_ns.reserve(total_orders);
        orders_to_cancel.reserve(total_orders);
    }

    static std::uint32_t order_index_from_token(const std::array<char, 14>& token) {
        std::uint32_t index = 0;
        for (std::size_t position = 1; position < token.size() && token[position] >= '0' && token[position] <= '9';
             ++position) {
            index = index * 10 + std::uint32_t(token[position] - '0');
        }
        return index;
    }

    // OUCH
    void on_order_accepted(const ouch42::OrderAccepted& message, std::uint64_t) {
        const std::uint64_t now = net::monotonic_ns();
        const std::uint32_t index = order_index_from_token(message.token);
        if (index >= warmup_orders) accept_latency_ns.push_back(now - send_time_ns[index]);
        ++accepted_orders;
        last_accept_time_ns = now;
        orders_to_cancel.push_back(index);
    }
    void on_order_canceled(const ouch42::OrderCanceled&, std::uint64_t) { ++canceled_orders; }
    void on_order_rejected(const ouch42::OrderRejected&, std::uint64_t) { ++rejected_orders; }

    // ITCH over MoldUDP64: one Add Order per accepted order, in send order
    // (one client, one TCP stream, no other activity on the book).
    void on_message(std::uint64_t, std::span<const char> message) {
        if (message.empty() || message[0] != 'A') return;
        const std::uint64_t index = itch_add_orders++;
        if (index >= warmup_orders && index < send_time_ns.size()) {
            itch_add_latency_ns.push_back(net::monotonic_ns() - send_time_ns[index]);
        }
    }
    void on_end_of_session(std::uint64_t) {}
};

struct PercentileSummary {
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
    std::size_t samples = 0;
};

PercentileSummary summarize(std::vector<std::uint64_t> samples) {
    PercentileSummary summary;
    summary.samples = samples.size();
    if (samples.empty()) return summary;
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double fraction) {
        const std::size_t rank = static_cast<std::size_t>(fraction * double(samples.size() - 1) + 0.5);
        return samples[std::min(rank, samples.size() - 1)];
    };
    summary.p50 = at(0.50);
    summary.p90 = at(0.90);
    summary.p99 = at(0.99);
    summary.p999 = at(0.999);
    summary.max = samples.back();
    return summary;
}

void print_summary(const char* label, const PercentileSummary& summary) {
    std::printf("%-12s samples=%zu p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64 " p99.9=%" PRIu64 " max=%" PRIu64
                " ns\n",
                label, summary.samples, summary.p50, summary.p90, summary.p99, summary.p999, summary.max);
}

}  // namespace

int main(int argc, char** argv) {
    BenchOptions options;
    if (!parse_arguments(argc, argv, options)) {
        print_usage();
        return 1;
    }
    const std::uint64_t total_orders = options.warmup_orders + options.measured_orders;

    server::ServerConfig config;
    std::string error;
    if (!server::parse_config(bench_config_text(total_orders), config, error)) {
        std::fprintf(stderr, "exsim_w2w_bench: config: %s\n", error.c_str());
        return 1;
    }
    LatencyRecorder recorder(total_orders, options.warmup_orders);
    md::ReceiverConfig receiver_config;
    receiver_config.session = "EXSIM";
    receiver_config.first_sequence = 1;
    receiver_config.request_timeout_ns = 20'000'000;
    receiver_config.max_request_attempts = 200;
    client::MarketDataFeed<LatencyRecorder> feed(recorder, receiver_config);
    if (feed.open(net::loopback_endpoint(0)) != 0) {
        std::fprintf(stderr, "exsim_w2w_bench: cannot open the feed socket\n");
        return 1;
    }
    config.market_data.port = feed.feed_endpoint().port;

    server::ServerRuntimeOptions runtime_options;
    runtime_options.busy_poll = options.busy_poll;
    runtime_options.log = nullptr;
    runtime_options.day_time_at_start = Timestamp{10} * 3600 * 1'000'000'000;
    auto runtime = std::make_unique<server::ServerRuntime>(config, runtime_options);
    if (const std::string failure = runtime->start(); !failure.empty()) {
        std::fprintf(stderr, "exsim_w2w_bench: server start: %s\n", failure.c_str());
        return 1;
    }
    feed.set_retransmission_server(runtime->retransmission_endpoint());
    std::string pin_message;
    std::atomic<bool> server_thread_finished{false};
    std::thread server_thread([&] {
        std::string server_pin_message;
        if (options.server_cpu >= 0 && !server::runtime::pin_current_thread_to_cpu(options.server_cpu,
                                                                                    server_pin_message)) {
            std::fprintf(stderr, "exsim_w2w_bench: server thread: %s\n", server_pin_message.c_str());
        }
        runtime->run();
        server_thread_finished.store(true, std::memory_order_release);
    });
    if (options.client_cpu >= 0 && !server::runtime::pin_current_thread_to_cpu(options.client_cpu, pin_message)) {
        std::fprintf(stderr, "exsim_w2w_bench: client thread: %s\n", pin_message.c_str());
    }

    client::OuchTcpSession<LatencyRecorder> session(recorder);
    bool logged_in = session.connect_and_login(runtime->listen_endpoint<server::runtime::OuchGateway>(),
                                               {"W2W", "w2w", ""}, 0);
    int exit_status = 0;
    if (logged_in) {
        // Wait for the System Event that follows Login Accepted.
        const std::uint64_t deadline = net::monotonic_ns() + 2'000'000'000;
        while (session.client().state() != gateway::ouch::OuchClientState::LoggedIn &&
               net::monotonic_ns() < deadline) {
            session.poll();
        }
        logged_in = session.client().state() == gateway::ouch::OuchClientState::LoggedIn;
    }
    if (!logged_in) {
        std::fprintf(stderr, "exsim_w2w_bench: login failed\n");
        exit_status = 1;
    } else {
        ouch42::EnterOrder order;
        order.side = 'B';
        order.shares = 100;
        order.stock = Symbol("BENCH");
        order.price = 99 * kPriceScale;
        order.time_in_force = 99999;
        order.display = 'Y';
        order.capacity = 'A';
        order.ise = 'N';
        order.minimum_quantity = 0;
        order.cross_type = 'N';
        order.customer_type = 'R';
        ouch42::CancelOrder cancel;
        const auto write_token = [](std::array<char, 14>& token, std::uint64_t index) {
            token.fill(' ');
            token[0] = 'W';
            char digits[20];
            int length = 0;
            do {
                digits[length++] = char('0' + index % 10);
                index /= 10;
            } while (index != 0);
            for (int position = 0; position < length; ++position) token[1 + position] = digits[length - 1 - position];
        };

        const std::uint64_t interval_ns = options.orders_per_second == 0 ? 0 : 1'000'000'000 / options.orders_per_second;
        const std::uint64_t start_ns = net::monotonic_ns();
        const std::uint64_t run_deadline_ns = start_ns + 600'000'000'000ull;
        std::uint64_t sent_orders = 0;
        std::uint64_t first_measured_send_ns = 0;
        std::size_t cancels_sent = 0;
        while (recorder.canceled_orders < total_orders || recorder.itch_add_orders < total_orders) {
            const std::uint64_t now = net::monotonic_ns();
            if (now > run_deadline_ns || !session.connected()) {
                exit_status = 2;
                break;
            }
            const bool may_send =
                sent_orders < total_orders &&
                (interval_ns == 0 ? sent_orders - recorder.accepted_orders < options.window
                                  : now >= start_ns + sent_orders * interval_ns &&
                                        sent_orders - recorder.accepted_orders < 50'000);
            if (may_send) {
                write_token(order.token, sent_orders);
                const std::uint64_t send_ns = net::monotonic_ns();
                recorder.send_time_ns[sent_orders] = send_ns;
                if (sent_orders == options.warmup_orders) first_measured_send_ns = send_ns;
                if (!session.client().enter_order(order, send_ns)) {
                    exit_status = 2;
                    break;
                }
                session.flush();
                ++sent_orders;
            }
            session.poll();
            feed.poll();
            if (cancels_sent < recorder.orders_to_cancel.size()) {
                // A cancel that does not fit in the send buffer is retried
                // on the next iteration, after the flush.
                for (; cancels_sent < recorder.orders_to_cancel.size(); ++cancels_sent) {
                    write_token(cancel.token, recorder.orders_to_cancel[cancels_sent]);
                    cancel.shares = 0;
                    if (!session.client().cancel_order(cancel, net::monotonic_ns())) break;
                }
                session.flush();
            }
        }

        const std::string rate_description =
            options.orders_per_second == 0 ? "max (window " + std::to_string(options.window) + ")"
                                           : std::to_string(options.orders_per_second) + "/s";
        std::printf("exsim_w2w_bench: server poll=%s rate=%s orders=%" PRIu64 " warmup=%" PRIu64 "\n",
                    options.busy_poll ? "busy" : "blocking", rate_description.c_str(), options.measured_orders,
                    options.warmup_orders);
        print_summary("ouch accept", summarize(recorder.accept_latency_ns));
        print_summary("itch add", summarize(recorder.itch_add_latency_ns));
        const double measured_seconds =
            double(recorder.last_accept_time_ns - first_measured_send_ns) / 1'000'000'000.0;
        std::printf("throughput   accepted_orders_per_second=%.0f (plus one cancel per order)\n",
                    measured_seconds > 0 ? double(options.measured_orders) / measured_seconds : 0.0);
        std::printf("feed         retransmission_requests=%" PRIu64 " messages_lost=%" PRIu64
                    " rejected_orders=%" PRIu64 "%s\n",
                    feed.stats().requests_sent, feed.stats().messages_lost, recorder.rejected_orders,
                    exit_status == 2 ? " (run timed out or the connection dropped)" : "");
        if (session.client().logout(net::monotonic_ns())) session.flush();
    }
    runtime->request_shutdown();
    const std::uint64_t shutdown_deadline = net::monotonic_ns() + 5'000'000'000;
    while (!server_thread_finished.load(std::memory_order_acquire) && net::monotonic_ns() < shutdown_deadline) {
        session.poll();
        feed.poll();
    }
    server_thread.join();
    return exit_status;
}
