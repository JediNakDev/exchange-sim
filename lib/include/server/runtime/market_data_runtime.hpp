#ifndef EXSIM_SERVER_RUNTIME_MARKET_DATA_RUNTIME_HPP
#define EXSIM_SERVER_RUNTIME_MARKET_DATA_RUNTIME_HPP

// The server's MarketData policy: ITCH 5.0 over MoldUDP64 (ItchRuntime) or
// CME MDP 3.0 (MdpRuntime), chosen once from the configuration. Only the
// selected runtime is constructed.
//
// A std::variant rather than a template parameter on the server runtime:
// ExchangeCore and every gateway endpoint are instantiated once instead of
// once per protocol, which halves that part of the binary. Each call costs
// one test of an index fixed at startup, which the branch predictor always
// gets right; the runtimes themselves contain no protocol branches.

#include "engine/event.hpp"
#include "md/publisher.hpp"
#include "net/poller.hpp"
#include "server/config.hpp"
#include "server/runtime/itch_runtime.hpp"
#include "server/runtime/market_data_settings.hpp"
#include "server/runtime/mdp_runtime.hpp"
#include "md/packet_faults.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace exsim::server::runtime {

class MarketDataRuntime {
public:
    MarketDataRuntime(const ServerConfig& config, const MarketDataRuntimeSettings& settings)
        : runtime_(construct_selected_runtime(config, settings)) {}

    MarketDataRuntime(const MarketDataRuntime&) = delete;
    MarketDataRuntime& operator=(const MarketDataRuntime&) = delete;

    // Opens the feed and recovery sockets and registers the recovery socket
    // with `poller` under `recovery_token`. Returns "" or an error message.
    [[nodiscard]] std::string open(const ServerConfig& config, net::Poller& poller, std::uint64_t recovery_token) {
        return std::visit([&](auto& runtime) { return runtime.open(config, poller, recovery_token); }, runtime_);
    }
    // Sends the start-of-day messages for the engine's instruments.
    template <class Engine>
    [[nodiscard]] md::PublishStatus start(Timestamp now, const Engine& engine, std::uint64_t epoch) {
        return std::visit([&](auto& runtime) { return runtime.start(now, engine, epoch); }, runtime_);
    }

    // ---- MarketData policy ------------------------------------------------------
    void on_events(std::span<const Event> batch) {
        std::visit([&](auto& runtime) { runtime.on_events(batch); }, runtime_);
    }
    void flush() {
        std::visit([](auto& runtime) { runtime.flush(); }, runtime_);
    }
    template <class Core>
    void on_timer(Timestamp now, const Core& core) {
        std::visit([&](auto& runtime) { runtime.on_timer(now, core); }, runtime_);
    }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.next_sequence(); }, runtime_);
    }
    [[nodiscard]] md::PacketFaultInjector& packet_faults() noexcept {
        return std::visit([](auto& runtime) -> md::PacketFaultInjector& { return runtime.packet_faults(); }, runtime_);
    }
    void reset_packet_faults() {
        std::visit([](auto& runtime) { runtime.reset_packet_faults(); }, runtime_);
    }
    void on_shutdown(Timestamp now) {
        std::visit([&](auto& runtime) { runtime.on_shutdown(now); }, runtime_);
    }

    // ---- recovery -----------------------------------------------------------------
    void handle_recovery_event(const net::PollEvent& event) {
        std::visit([&](auto& runtime) { runtime.handle_recovery_event(event); }, runtime_);
    }
    [[nodiscard]] std::uint32_t recovery_connection_count() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.recovery_connection_count(); }, runtime_);
    }
    void close_recovery_now() noexcept {
        std::visit([](auto& runtime) { runtime.close_recovery_now(); }, runtime_);
    }

    // ---- queries -------------------------------------------------------------------
    // True once the publisher stopped because a batch could not be published.
    [[nodiscard]] bool stopped() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.stopped(); }, runtime_);
    }
    [[nodiscard]] md::PublishStatus stop_reason() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.stop_reason(); }, runtime_);
    }
    [[nodiscard]] md::PublisherState state() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.state(); }, runtime_);
    }
    [[nodiscard]] net::Ipv4Endpoint feed_destination() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.feed_destination(); }, runtime_);
    }
    [[nodiscard]] net::Ipv4Endpoint retransmission_endpoint() const noexcept {
        return std::visit([](const auto& runtime) { return runtime.retransmission_endpoint(); }, runtime_);
    }

private:
    using SelectedRuntime = std::variant<ItchRuntime, MdpRuntime>;

    // Returned as a prvalue, so the runtime is built in place: both runtimes
    // hand their own address to their packet sinks and cannot move.
    static SelectedRuntime construct_selected_runtime(const ServerConfig& config,
                                                      const MarketDataRuntimeSettings& settings) {
        if (config.market_data.protocol == MarketDataProtocol::Mdp3)
            return SelectedRuntime(std::in_place_type<MdpRuntime>, config, settings);
        return SelectedRuntime(std::in_place_type<ItchRuntime>, config, settings);
    }

    SelectedRuntime runtime_;
};

}  // namespace exsim::server::runtime

#endif
