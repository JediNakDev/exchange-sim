#ifndef EXSIM_SERVER_SERVER_RUNTIME_HPP
#define EXSIM_SERVER_SERVER_RUNTIME_HPP

// The exsim server: one thread, one reactor loop (docs/SERVER.md 1,
// docs/SERVER_RUNTIME.md). BasicServerRuntime<Gateways...> owns the poller,
// the exchange core, the market-data runtime, one order-entry endpoint per
// gateway and the admin console:
//
//   poll_once():
//     poller.wait(busy_poll ? 0 : idle timeout)
//     route events: gateway connections (-> on_data -> core.submit ->
//                   dispatch to sessions, ITCH, bots), admin lines,
//                   MoldUDP64 retransmission requests
//     core.timer(now): end of day, bots, NOII / MoldUDP64 heartbeat,
//                      journal flush
//     endpoints: delayed inbound bytes, gateway timers, requested closes
//     core disconnect requests (`fault user X disconnect`)
//     shutdown progress (end-of-day messages, drain, finish)
//     flush every connection with pending output
//
// Everything is allocated by the constructor and start(); poll_once does not
// allocate except on operator paths (admin commands, logging a connection
// failure). Gateways are compile-time template arguments: no virtual calls.

#include "exsim/net/clocks.hpp"
#include "exsim/net/poller.hpp"
#include "exsim/net/tcp_server.hpp"
#include "exsim/server/config.hpp"
#include "exsim/server/exchange_core.hpp"
#include "exsim/server/runtime/admin_endpoint.hpp"
#include "exsim/server/runtime/gateway_sessions.hpp"
#include "exsim/server/runtime/journal_worker.hpp"
#include "exsim/server/runtime/market_data_runtime.hpp"
#include "exsim/server/runtime/order_entry_endpoint.hpp"
#include "exsim/server/runtime/order_entry_gateways.hpp"
#include "exsim/server/runtime/server_clock.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace exsim::server {

struct ServerRuntimeOptions {
    bool busy_poll = false;          // poller.wait(0): lowest latency, one core at 100%
    int idle_poll_timeout_ms = 1;    // otherwise: timer resolution (bots, delays, heartbeats)
    // ns since local midnight at startup; nullopt reads the wall clock.
    std::optional<Timestamp> day_time_at_start;
    std::string admin_bind_address = "127.0.0.1";  // admin console: loopback unless told otherwise
    std::uint64_t end_of_day_drain_timeout_ns = 3'000'000'000;  // wait for logouts and flushes
    runtime::MarketDataRuntimeSettings market_data{};
    std::size_t connection_receive_buffer_bytes = 64 * 1024;
    std::size_t connection_send_buffer_bytes = 4 * 1024 * 1024;
    bool bots_enabled_at_start = true;
    std::FILE* log = stderr;  // startup listener reporting only; nullptr = silent
};

template <class... Gateways>
class BasicServerRuntime {
public:
    using Sessions = runtime::GatewaySessions<Gateways...>;
    using Core = ExchangeCore<Sessions, runtime::MarketDataRuntime, runtime::ServerClock, runtime::JournalWorker<>>;
    template <class Gateway>
    using Endpoint = runtime::OrderEntryEndpoint<Gateway, Core>;

    static constexpr std::uint8_t kAdminPollTag = 0x40;
    static constexpr std::uint8_t kRetransmissionPollTag = 0x41;

    // Setup: builds everything. Throws std::runtime_error / invalid_argument
    // on a configuration the components refuse.
    BasicServerRuntime(const ServerConfig& config, ServerRuntimeOptions options)
        : config_(config),
          options_(std::move(options)),
          clock_(options_.day_time_at_start),
          market_data_(config_, options_.market_data),
          sessions_(config_),
          core_(config_, sessions_, market_data_, clock_),
          endpoints_(make_endpoint<Gateways>(gateway_index<Gateways>())...),
          admin_(admin_tcp_config(), poller_, kAdminPollTag, core_) {
        core_.set_bots_enabled(options_.bots_enabled_at_start);
    }

    BasicServerRuntime(const BasicServerRuntime&) = delete;
    BasicServerRuntime& operator=(const BasicServerRuntime&) = delete;

    // Opens every socket and sends the market-data start of day. Returns ""
    // or an error message.
    [[nodiscard]] std::string start() {
        if (net::ErrorCode error = poller_.open(); error != 0) return "poller: " + std::string(std::strerror(error));
        std::string failure;
        (open_endpoint<Gateways>(failure), ...);
        if (!failure.empty()) return failure;
        if (net::ErrorCode error = admin_.open(); error != 0) {
            return "admin listener on " + options_.admin_bind_address + ":" +
                   std::to_string(config_.exchange.admin_port) + ": " + std::strerror(error);
        }
        failure = market_data_.open(config_, poller_, net::make_poll_token(kRetransmissionPollTag, 0));
        if (!failure.empty()) return failure;
        const md::PublishStatus started = market_data_.start(core_.now(), core_.engine());
        if (started != md::PublishStatus::Ok) {
            return "market data start of day failed (status " + std::to_string(static_cast<int>(started)) + ")";
        }
        log_listeners();
        return {};
    }

    // One reactor iteration.
    void poll_once() noexcept {
        const int timeout_ms = options_.busy_poll ? 0 : options_.idle_poll_timeout_ms;
        if (poller_.wait(timeout_ms) > 0) {
            for (const net::PollEvent& event : poller_.events()) route_event(event);
        }
        core_.timer(clock_.day_ns());
        (endpoint<Gateways>().run_timers(), ...);
        if (core_.has_disconnect_requests()) close_requested_sessions();
        if (!core_.shutdown_requested() &&
            (external_shutdown_requested_.load(std::memory_order_relaxed) || market_data_.stopped())) {
            core_.shutdown();
        }
        if (core_.shutdown_requested()) progress_shutdown();
        (endpoint<Gateways>().flush(), ...);
        admin_.flush();
    }

    void run() noexcept {
        while (!finished()) poll_once();
    }

    // Any thread (and signal handlers): ends the trading day at the next
    // iteration, as the admin `shutdown` command does.
    void request_shutdown() noexcept { external_shutdown_requested_.store(true, std::memory_order_relaxed); }
    // The day has ended and every session has been closed (or the drain
    // timed out): the process may exit.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // ---- access (server thread, or after it stopped) ---------------------------
    [[nodiscard]] Core& core() noexcept { return core_; }
    [[nodiscard]] const Core& core() const noexcept { return core_; }
    [[nodiscard]] runtime::MarketDataRuntime& market_data() noexcept { return market_data_; }
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    template <class Gateway>
    [[nodiscard]] Endpoint<Gateway>& endpoint() noexcept {
        return *std::get<std::unique_ptr<Endpoint<Gateway>>>(endpoints_);
    }
    template <class Gateway>
    [[nodiscard]] net::Ipv4Endpoint listen_endpoint() noexcept {
        return endpoint<Gateway>().listen_endpoint();
    }
    [[nodiscard]] net::Ipv4Endpoint admin_endpoint() const noexcept { return admin_.listen_endpoint(); }
    [[nodiscard]] net::Ipv4Endpoint retransmission_endpoint() const noexcept {
        return market_data_.retransmission_endpoint();
    }

private:
    template <class Gateway>
    static constexpr std::uint8_t gateway_index() noexcept {
        std::uint8_t index = 0;
        std::uint8_t found = 0;
        ((std::is_same_v<Gateway, Gateways> ? (found = index, ++index) : ++index), ...);
        return found;
    }
    template <class Gateway>
    static constexpr std::uint8_t gateway_poll_tag() noexcept {
        return static_cast<std::uint8_t>(gateway_index<Gateway>() + 1);
    }

    template <class Gateway>
    std::unique_ptr<Endpoint<Gateway>> make_endpoint(std::uint8_t index) {
        net::TcpServerConfig tcp;
        tcp.listen_endpoint = net::Ipv4Endpoint{parse_address(config_.exchange.bind_address, "exchange.bind_address"),
                                                Gateway::configured_port(config_)};
        tcp.max_connections = config_.exchange.max_connections;
        tcp.receive_buffer_capacity = options_.connection_receive_buffer_bytes;
        tcp.send_buffer_capacity = options_.connection_send_buffer_bytes;
        return std::make_unique<Endpoint<Gateway>>(tcp, poller_, static_cast<std::uint8_t>(index + 1),
                                                   sessions_.template user_directory<Gateway>(), core_);
    }

    net::TcpServerConfig admin_tcp_config() const {
        net::TcpServerConfig tcp;
        tcp.listen_endpoint =
            net::Ipv4Endpoint{parse_address(options_.admin_bind_address, "admin bind address"), config_.exchange.admin_port};
        tcp.max_connections = 8;
        tcp.receive_buffer_capacity = 16 * 1024;
        tcp.send_buffer_capacity = 4 * 1024 * 1024;
        return tcp;
    }

    static std::uint32_t parse_address(const std::string& text, const char* what) {
        std::uint32_t address = 0;
        if (!net::parse_ipv4_address(text, address)) {
            throw std::invalid_argument(std::string(what) + " is not an IPv4 address: " + text);
        }
        return address;
    }

    template <class Gateway>
    void open_endpoint(std::string& failure) {
        if (!failure.empty()) return;
        if (net::ErrorCode error = endpoint<Gateway>().open(); error != 0) {
            failure = std::string(Gateway::name) + " listener on " + config_.exchange.bind_address + ":" +
                      std::to_string(Gateway::configured_port(config_)) + ": " + std::strerror(error);
        }
    }

    void route_event(const net::PollEvent& event) noexcept {
        const std::uint8_t tag = net::poll_token_tag(event.token);
        if (tag == kRetransmissionPollTag) {
            market_data_.handle_retransmission_requests();
            return;
        }
        if (tag == kAdminPollTag) {
            admin_.handle_event(event);
            return;
        }
        (void)((tag == gateway_poll_tag<Gateways>() ? (endpoint<Gateways>().handle_event(event), true) : false) || ...);
    }

    // `fault user X disconnect`: the connection is closed without flushing
    // and the gateway's on_closed unbinds the session (cancel-on-disconnect).
    void close_requested_sessions() noexcept {
        std::array<SessionId, 64> sessions{};
        for (;;) {
            const std::size_t count = core_.take_disconnect_requests(sessions);
            for (std::size_t index = 0; index < count; ++index) {
                const std::optional<ConnectionId> connection = core_.connection(sessions[index]);
                if (!connection) continue;
                (void)((runtime::core_connection_endpoint_tag(*connection) == gateway_poll_tag<Gateways>() &&
                        endpoint<Gateways>().close_core_connection(*connection)) ||
                       ...);
            }
            if (count < sessions.size()) return;
        }
    }

    // End of day: OUCH End of Session, FIX Logout and FIXP Terminate on every
    // connection, then wait (bounded) for the sessions to close and flush.
    void progress_shutdown() noexcept {
        if (finished_) return;
        const std::uint64_t now = net::monotonic_ns();
        if (!end_of_day_started_) {
            end_of_day_started_ = true;
            drain_deadline_ns_ = now + options_.end_of_day_drain_timeout_ns;
            (endpoint<Gateways>().close_for_end_of_day(), ...);
        }
        std::uint32_t remaining = 0;
        ((remaining += endpoint<Gateways>().transport_connection_count()), ...);
        if (remaining != 0 && now < drain_deadline_ns_) return;
        if (remaining != 0) {
            (endpoint<Gateways>().close_all_now(), ...);
        }
        if (!journal_drain_started_) {
            journal_drain_started_ = true;
            (void)core_.flush_journal();
        }
        if (!core_.journal_drained()) {
            if (now < drain_deadline_ns_) return;
            core_.abort_journal();
        }
        finished_ = true;
    }

    void log_listeners() {
        if (options_.log == nullptr) return;
        std::array<char, net::kIpv4EndpointTextCapacity> text{};
        const auto print = [&](std::string_view what, net::Ipv4Endpoint where) {
            const std::size_t length = net::format_ipv4_endpoint(where, text);
            std::fprintf(options_.log, "exsim: %.*s listening on %.*s\n", static_cast<int>(what.size()), what.data(),
                         static_cast<int>(length), text.data());
        };
        (print(Gateways::name, endpoint<Gateways>().listen_endpoint()), ...);
        print("admin", admin_.listen_endpoint());
        print("md retransmission", market_data_.retransmission_endpoint());
        const std::size_t length = net::format_ipv4_endpoint(market_data_.feed_destination(), text);
        std::fprintf(options_.log, "exsim: md feed to %.*s%s\n", static_cast<int>(length), text.data(),
                     market_data_.feed_destination().port == 0 ? " (port 0: feed disabled)" : "");
        std::fflush(options_.log);
    }

    ServerConfig config_;
    ServerRuntimeOptions options_;
    net::Poller poller_;
    runtime::ServerClock clock_;
    runtime::MarketDataRuntime market_data_;
    Sessions sessions_;
    Core core_;
    std::tuple<std::unique_ptr<Endpoint<Gateways>>...> endpoints_;
    runtime::AdminEndpoint<Core> admin_;
    std::atomic<bool> external_shutdown_requested_{false};
    bool end_of_day_started_ = false;
    bool journal_drain_started_ = false;
    bool finished_ = false;
    std::uint64_t drain_deadline_ns_ = 0;
};

// The exsim server: OUCH 4.2, FIX 4.4 and SBE 1.0 over FIXP 1.0
// (docs/SERVER_RUNTIME.md, "Adding a protocol").
using ServerRuntime = BasicServerRuntime<runtime::OuchGateway, runtime::FixGateway, runtime::FixpSbeGateway>;

}  // namespace exsim::server

#endif
