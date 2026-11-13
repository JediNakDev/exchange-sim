#ifndef EXSIM_SERVER_RUNTIME_MDP_RUNTIME_HPP
#define EXSIM_SERVER_RUNTIME_MDP_RUNTIME_HPP

#include "md/mdp_publisher.hpp"
#include "md/mdp_replay_session.hpp"
#include "net/clocks.hpp"
#include "net/tcp_server.hpp"
#include "net/udp.hpp"
#include "server/config.hpp"
#include "server/runtime/market_data_settings.hpp"
#include "server/runtime/tcp_send_sink.hpp"
#include "sim/packet_faults.hpp"

namespace exsim::server::runtime {

// CME MDP 3.0 market data policy: incremental A/B feeds through the packet
// fault injector, snapshot and instrument definition loops, and TCP replay.
class MdpRuntime {
    struct PacketSink {
        MdpRuntime* runtime;
        void operator()(std::span<const char> packet, md::MdpStream stream) const noexcept {
            runtime->send_packet(packet, stream);
        }
    };
    using ReplaySession = md::MdpReplaySession<TcpSendSink>;
    struct Slot {
        Slot(const MarketDataSection& config, const md::MdpReplayRing& ring) : session(config, ring) {}
        ReplaySession session;
        net::ConnectionId id{};
        bool open = false;
    };
public:
    MdpRuntime(const ServerConfig& config, const MarketDataRuntimeSettings& settings)
        : settings_(settings), faults_(config.market_data.max_payload, settings.max_held_packets), publisher_(config, PacketSink{this}) {}

    MdpRuntime(const MdpRuntime&) = delete;
    MdpRuntime& operator=(const MdpRuntime&) = delete;

    std::string open(const ServerConfig& config, net::Poller& poller, std::uint64_t replay_token) {
        const auto& md = config.market_data;
        std::uint32_t address = 0, interface_address = 0, bind = 0;
        if (!net::parse_ipv4_address(md.address, address) ||
            !net::parse_ipv4_address(config.exchange.bind_address, bind) ||
            (!md.interface_address.empty() && !net::parse_ipv4_address(md.interface_address, interface_address)))
            return "invalid MDP IPv4 address";
        incremental_ = {address, md.port}; backup_ = {address, md.backup_port};
        snapshot_ = {address, md.snapshot_port}; instrument_ = {address, md.instrument_port};
        if (const auto error = sender_.open()) return "MDP UDP socket: " + std::string(std::strerror(error));
        if (settings_.send_buffer_bytes > 0) (void)net::set_send_buffer_size(sender_.fd(), settings_.send_buffer_bytes);
        if (net::is_ipv4_multicast_address(address)) {
            if (const auto error = sender_.multicast_options(md.ttl, md.loopback, interface_address))
                return "MDP multicast: " + std::string(std::strerror(error));
        }
        net::TcpServerConfig tcp;
        tcp.listen_endpoint = {bind, md.retransmit_port};
        tcp.max_connections = std::min(config.exchange.max_connections, std::uint32_t{4});
        tcp.receive_buffer_capacity = 8192;
        tcp.send_buffer_capacity = 4 * 1024 * 1024;
        replay_ = std::make_unique<net::TcpServer>(tcp, poller, net::poll_token_tag(replay_token));
        slots_.reserve(tcp.max_connections);
        for (std::size_t i = 0; i < tcp.max_connections; ++i)
            slots_.push_back(std::make_unique<Slot>(md, publisher_.ring()));
        if (const auto error = replay_->open()) return "MDP replay TCP: " + std::string(std::strerror(error));
        return {};
    }
    md::PublishStatus start(Timestamp now, const MatchingEngine& engine, std::uint64_t epoch) {
        return publisher_.start(now, epoch, engine);
    }
    void on_events(std::span<const Event> events) noexcept { (void)publisher_.on_events(events); }
    void flush() noexcept { if (replay_) replay_->flush_pending(*this); }
    template <class Core>
    void on_timer(Timestamp now, const Core&) noexcept {
        publisher_.poll(now, settings_.heartbeat_interval_ns);
        if (!replay_) return;
        const auto monotonic = net::monotonic_ns(), epoch = net::realtime_ns();
        for (auto& slot : slots_) {
            if (!slot->open) continue;
            slot->session.on_timer(monotonic, epoch);
            finish_if_requested(*slot);
        }
        replay_->flush_pending(*this);
    }
    void on_shutdown(Timestamp now) noexcept {
        reset_packet_faults();
        publisher_.shutdown(now);
        if (!replay_) return;
        for (auto& slot : slots_) {
            if (!slot->open) continue;
            slot->session.shutdown(net::monotonic_ns(), net::realtime_ns());
            finish_if_requested(*slot);
        }
        replay_->flush_pending(*this);
    }
    void handle_recovery_event(const net::PollEvent& event) noexcept { replay_->handle_event(event, *this); }
    void on_connection_opened(net::ConnectionId id, net::Ipv4Endpoint) noexcept {
        auto& slot = *slots_[id.slot];
        slot.id = id; slot.open = true;
        slot.session.open(TcpSendSink{replay_.get(), id}, net::monotonic_ns());
        if (publisher_.state() != md::PublisherState::Live) {
            slot.session.shutdown(net::monotonic_ns(), net::realtime_ns());
            finish_if_requested(slot);
        }
    }
    void on_connection_data(net::ConnectionId id, net::ReceiveBuffer& received) noexcept {
        auto& slot = *slots_[id.slot];
        received.consume(slot.session.on_data(received.readable(), net::monotonic_ns(), net::realtime_ns()));
        finish_if_requested(slot);
    }
    void on_connection_closed(net::ConnectionId id, net::CloseReason, net::ErrorCode) noexcept {
        if (slots_[id.slot]->id == id) slots_[id.slot]->open = false;
    }
    [[nodiscard]] bool stopped() const noexcept { return state() == md::PublisherState::Stopped; }
    [[nodiscard]] md::PublisherState state() const noexcept { return publisher_.state(); }
    [[nodiscard]] md::PublishStatus stop_reason() const noexcept { return publisher_.stop_reason(); }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return publisher_.next_sequence(); }
    [[nodiscard]] std::uint32_t recovery_connection_count() const noexcept { return replay_ ? replay_->connection_count() : 0; }
    void close_recovery_now() noexcept { if (replay_) replay_->close(); }
    [[nodiscard]] net::Ipv4Endpoint feed_destination() const noexcept { return incremental_; }
    [[nodiscard]] net::Ipv4Endpoint retransmission_endpoint() const noexcept { return replay_ ? replay_->listen_endpoint() : net::Ipv4Endpoint{}; }
    [[nodiscard]] sim::PacketFaultInjector& packet_faults() noexcept { return faults_; }
    void reset_packet_faults() noexcept {
        faults_.clear_faults();
        faults_.flush([this](std::span<const char> packet) { send_incremental(packet); });
    }
private:
    void finish_if_requested(Slot& slot) noexcept {
        if (!slot.session.wants_close()) return;
        if (slot.session.flush_before_close()) replay_->close_after_flush(slot.id);
        else replay_->close_now(slot.id);
        slot.open = false;
    }
    void send_incremental(std::span<const char> packet) noexcept {
        if (incremental_.port) (void)sender_.send_to(packet, incremental_);
        if (backup_.port) (void)sender_.send_to(packet, backup_);
    }
    void send_packet(std::span<const char> packet, md::MdpStream stream) noexcept {
        if (stream == md::MdpStream::Incremental) {
            if (faults_.idle()) send_incremental(packet);
            else faults_.submit(packet, [this](std::span<const char> released) { send_incremental(released); });
        } else {
            const auto endpoint = stream == md::MdpStream::Snapshot ? snapshot_ : instrument_;
            if (endpoint.port) (void)sender_.send_to(packet, endpoint);
        }
    }
    MarketDataRuntimeSettings settings_;
    sim::PacketFaultInjector faults_;
    md::MdpPublisher<PacketSink> publisher_;
    net::UdpSender sender_;
    net::Ipv4Endpoint incremental_{}, backup_{}, snapshot_{}, instrument_{};
    std::unique_ptr<net::TcpServer> replay_;
    std::vector<std::unique_ptr<Slot>> slots_;
};
}  // namespace exsim::server::runtime
#endif
