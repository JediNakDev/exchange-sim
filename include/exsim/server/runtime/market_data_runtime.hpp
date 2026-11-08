#ifndef EXSIM_SERVER_RUNTIME_MARKET_DATA_RUNTIME_HPP
#define EXSIM_SERVER_RUNTIME_MARKET_DATA_RUNTIME_HPP

// The server's MarketData policy (docs/SERVER.md 3, exsim/server/core/
// policies.hpp): MarketDataPublisher -> sim::PacketFaultInjector -> UDP
// sender, plus the MoldUDP64 retransmission server on its own UDP socket.
//
//   on_events / flush        every command batch, from the core's dispatch
//   on_timer(now, core)      NOII and MoldUDP64 heartbeat
//   packet_faults()          target of `fault md ...`
//   reset_packet_faults()    `fault md off`: clears every rate and pending
//                            one-shot fault and releases held packets
//   on_shutdown(now)         ITCH S 'E', S 'C', MoldUDP64 End of Session
//
// A batch the publisher cannot represent stops the feed (stopped() turns
// true); the server loop then shuts the exchange down. Fault injection is
// switched off at end of day so the final messages and the End of Session
// packet, which cannot be recovered by request, reach every receiver.
//
// Hot path (on_events, flush, on_timer, retransmission answers): no heap
// allocation. Construction and open() allocate.

#include "exsim/event.hpp"
#include "exsim/md/publisher.hpp"
#include "exsim/md/retransmission_server.hpp"
#include "exsim/net/poller.hpp"
#include "exsim/net/socket.hpp"
#include "exsim/net/udp.hpp"
#include "exsim/server/config.hpp"
#include "exsim/sim/canonical.hpp"
#include "exsim/sim/packet_faults.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace exsim::server::runtime {

struct MarketDataRuntimeSettings {
    std::uint64_t heartbeat_interval_ns = 1'000'000'000;  // MoldUDP64 heartbeat after this much silence
    std::size_t max_held_packets = 256;                  // reorder fault capacity
    int send_buffer_bytes = 4 << 20;                     // SO_SNDBUF of the feed socket; 0 = OS default
};

struct MarketDataRuntimeStats {
    std::uint64_t datagrams_sent = 0;
    std::uint64_t datagram_send_failures = 0;  // ENOBUFS and friends: receivers recover by request
    std::uint64_t retransmission_requests = 0;
    std::uint64_t retransmission_responses = 0;
    std::uint64_t events_published = 0;
};

class MarketDataRuntime {
public:
    MarketDataRuntime(const ServerConfig& config, const MarketDataRuntimeSettings& settings)
        : settings_(settings),
          packet_faults_(config.market_data.max_payload, settings.max_held_packets),
          publisher_(publisher_config(config, settings), FeedPacketSink{this}),
          retransmission_server_(publisher_.ring(), md::RetransmissionConfig{config.market_data.session,
                                                                            config.market_data.max_payload, 1024}) {}

    MarketDataRuntime(const MarketDataRuntime&) = delete;
    MarketDataRuntime& operator=(const MarketDataRuntime&) = delete;

    // Opens the feed sender and the retransmission socket and registers the
    // latter with `poller` under `retransmission_token`. Returns "" or an
    // error message.
    [[nodiscard]] std::string open(const ServerConfig& config, net::Poller& poller, std::uint64_t retransmission_token) {
        const MarketDataSection& section = config.market_data;
        std::uint32_t feed_address = 0;
        if (!net::parse_ipv4_address(section.address, feed_address)) {
            return "market_data.address is not an IPv4 address: " + section.address;
        }
        feed_destination_ = net::Ipv4Endpoint{feed_address, section.port};
        if (net::ErrorCode error = feed_sender_.open(); error != 0) {
            return "market data socket: " + std::string(std::strerror(error));
        }
        if (settings_.send_buffer_bytes > 0) {
            (void)net::set_send_buffer_size(feed_sender_.fd(), settings_.send_buffer_bytes);
        }
        if (net::is_ipv4_multicast_address(feed_address)) {
            std::uint32_t interface_address = net::kIpv4AnyAddress;
            if (!section.interface_address.empty() &&
                !net::parse_ipv4_address(section.interface_address, interface_address)) {
                return "market_data.interface is not an IPv4 address: " + section.interface_address;
            }
            if (net::ErrorCode error = feed_sender_.multicast_options(section.ttl, section.loopback, interface_address);
                error != 0) {
                return "market data multicast options: " + std::string(std::strerror(error));
            }
        }
        std::uint32_t bind_address = 0;
        if (!net::parse_ipv4_address(config.exchange.bind_address, bind_address)) {
            return "exchange.bind_address is not an IPv4 address: " + config.exchange.bind_address;
        }
        if (net::ErrorCode error = retransmission_socket_.open({bind_address, section.retransmit_port}); error != 0) {
            return "retransmission socket on port " + std::to_string(section.retransmit_port) + ": " +
                   std::strerror(error);
        }
        if (net::ErrorCode error = poller.add(retransmission_socket_.fd(), retransmission_token, net::PollInterest::Read);
            error != 0) {
            return "poller: " + std::string(std::strerror(error));
        }
        (void)net::local_endpoint(retransmission_socket_.fd(), retransmission_endpoint_);
        return {};
    }

    // Registers the engine's instruments and sends the start-of-day messages.
    template <class Engine>
    [[nodiscard]] md::PublishStatus start(Timestamp now, const Engine& engine) {
        const md::PublishStatus registered = publisher_.add_instruments_from(engine);
        if (registered != md::PublishStatus::Ok) return registered;
        return publisher_.start_session(now);
    }

    // ---- MarketData policy ------------------------------------------------------
    void on_events(std::span<const Event> batch) {
        for (const Event& event : batch) {
            event_stream_hash_ = sim::hash_event(event, event_stream_hash_);
        }
        stats_.events_published += batch.size();
        (void)publisher_.on_events(batch);  // a failure stops the feed: see stopped()
    }
    void flush() { publisher_.flush(); }

    template <class Core>
    void on_timer(Timestamp now, const Core& core) {
        if (publisher_.state() == md::PublisherState::Live || publisher_.state() == md::PublisherState::Ended) {
            (void)publisher_.poll(now, core.engine());
        }
    }

    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return publisher_.next_sequence(); }
    [[nodiscard]] sim::PacketFaultInjector& packet_faults() noexcept { return packet_faults_; }
    void reset_packet_faults() {
        packet_faults_.clear_faults();
        packet_faults_.flush([this](std::span<const char> packet) { send_datagram(packet); });
    }

    void on_shutdown(Timestamp now) {
        reset_packet_faults();
        if (publisher_.state() == md::PublisherState::Live) {
            (void)publisher_.end_of_day(now);
        }
        if (publisher_.state() == md::PublisherState::Stopped) {
            (void)publisher_.end_session(now);
        }
    }

    // ---- retransmission ----------------------------------------------------------
    // Answers every pending request datagram (level-triggered readiness).
    void handle_retransmission_requests() {
        // Level-triggered readiness will report remaining packets next time.
        // Bound every drain, including malformed/oversized traffic, so the
        // reactor can service order entry, timers and shutdown.
        for (std::size_t drained = 0; drained < 64; ++drained) {
            net::Ipv4Endpoint requester{};
            const ssize_t length = retransmission_socket_.receive_from(request_buffer_, requester);
            if (length < 0) {
                if (errno == EINTR || errno == EMSGSIZE) continue;
                return;  // would block (or a transient error): wait for the next readiness
            }
            ++stats_.retransmission_requests;
            const md::RetransmitResult result = retransmission_server_.answer(
                std::span<const char>(request_buffer_.data(), static_cast<std::size_t>(length)),
                [this, requester](std::span<const char> response) {
                    (void)retransmission_socket_.send_to(response, requester);
                });
            if (result.status == md::RetransmitStatus::Answered) ++stats_.retransmission_responses;
        }
    }

    // ---- queries -------------------------------------------------------------------
    // True once the publisher stopped because a batch could not be published.
    [[nodiscard]] bool stopped() const noexcept { return publisher_.state() == md::PublisherState::Stopped; }
    [[nodiscard]] md::PublishStatus stop_reason() const noexcept { return publisher_.stop_reason(); }
    [[nodiscard]] md::PublisherState state() const noexcept { return publisher_.state(); }
    // sim::hash_event over every published event, in order: equals the hash
    // exsim_replay prints for the journal of the same day.
    [[nodiscard]] std::uint64_t event_stream_hash() const noexcept { return event_stream_hash_; }
    [[nodiscard]] const MarketDataRuntimeStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const md::PublisherStats& publisher_stats() const noexcept { return publisher_.stats(); }
    [[nodiscard]] net::Ipv4Endpoint feed_destination() const noexcept { return feed_destination_; }
    [[nodiscard]] net::Ipv4Endpoint retransmission_endpoint() const noexcept { return retransmission_endpoint_; }

private:
    struct FeedPacketSink {
        MarketDataRuntime* runtime;
        void operator()(std::span<const char> packet) const { runtime->submit_packet(packet); }
    };

    static md::PublisherConfig publisher_config(const ServerConfig& config, const MarketDataRuntimeSettings& settings) {
        md::PublisherConfig publisher;
        publisher.session = config.market_data.session;
        publisher.max_payload = config.market_data.max_payload;
        publisher.retransmit_buffer_messages = config.market_data.retransmit_buffer;
        publisher.max_instruments = config.engine.engine.max_instruments;
        publisher.day_origin = 0;  // core timestamps are already ns since local midnight
        publisher.heartbeat_interval_ns = settings.heartbeat_interval_ns;
        publisher.noii_interval_ns = std::uint64_t{config.market_data.noii_interval_ms} * 1'000'000;
        return publisher;
    }

    void submit_packet(std::span<const char> packet) {
        if (packet_faults_.idle()) {
            send_datagram(packet);
            return;
        }
        packet_faults_.submit(packet, [this](std::span<const char> released) { send_datagram(released); });
    }

    void send_datagram(std::span<const char> packet) noexcept {
        if (feed_destination_.port == 0) return;  // feed disabled (no destination port configured)
        if (feed_sender_.send_to(packet, feed_destination_) < 0) {
            ++stats_.datagram_send_failures;
        } else {
            ++stats_.datagrams_sent;
        }
    }

    MarketDataRuntimeSettings settings_;
    sim::PacketFaultInjector packet_faults_;
    md::MarketDataPublisher<FeedPacketSink> publisher_;
    md::RetransmissionServer retransmission_server_;
    net::UdpSender feed_sender_;
    net::UdpReceiver retransmission_socket_;
    net::Ipv4Endpoint feed_destination_{};
    net::Ipv4Endpoint retransmission_endpoint_{};
    std::array<char, 2048> request_buffer_{};
    std::uint64_t event_stream_hash_ = 14695981039346656037ull;
    MarketDataRuntimeStats stats_{};
};

}  // namespace exsim::server::runtime

#endif
