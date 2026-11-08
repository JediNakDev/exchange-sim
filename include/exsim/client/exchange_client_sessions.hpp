#ifndef EXSIM_CLIENT_EXCHANGE_CLIENT_SESSIONS_HPP
#define EXSIM_CLIENT_EXCHANGE_CLIENT_SESSIONS_HPP

// Client-side plumbing for talking to a running exsim server over real
// sockets: the protocol clients (OuchClient, FixClient, MoldUdp64Receiver)
// are transport-agnostic, these wrap them around net::TcpClient /
// net::UdpReceiver. Used by the end-to-end tests, exsim_client and
// exsim_w2w_bench. Every call is non-blocking except the connect helper and
// AdminLineClient, which wait with a timeout.
//
//   OuchTcpSession<Handler>     OUCH 4.2 over SoupBinTCP
//   FixTcpSession<Handler>      FIX 4.4
//   FixpSbeTcpSession<Handler>  SBE 1.0 over FIXP 1.0
//   MarketDataFeed<Handler>     ITCH 5.0 over MoldUDP64, with gap requests
//   AdminLineClient             admin console request/reply

#include "exsim/gateway/fix/fix_client.hpp"
#include "exsim/gateway/fixp_sbe/fixp_sbe_client.hpp"
#include "exsim/gateway/ouch/ouch_client.hpp"
#include "exsim/md/receiver.hpp"
#include "exsim/net/clocks.hpp"
#include "exsim/net/socket.hpp"
#include "exsim/net/tcp_client.hpp"
#include "exsim/net/udp.hpp"

#include <poll.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace exsim::client {

// OuchByteSink / FixByteSink over a TcpClient's send buffer.
struct TcpClientSendSink {
    net::TcpClient* tcp_client = nullptr;

    [[nodiscard]] char* prepare(std::size_t byte_count) noexcept {
        if (!tcp_client->connected()) return nullptr;
        auto& buffer = tcp_client->connection().send_buffer();
        if (byte_count > buffer.capacity() - buffer.pending_bytes()) return nullptr;
        const std::span<char> space = buffer.reserve(byte_count);
        return space.size() == byte_count && byte_count != 0 ? space.data() : nullptr;
    }
    void commit(std::size_t byte_count) noexcept { tcp_client->connection().send_buffer().commit(byte_count); }
};

// Connects and waits up to timeout_ms for the handshake. Returns 0 or errno.
inline net::ErrorCode connect_with_timeout(net::TcpClient& tcp_client, net::Ipv4Endpoint server, int timeout_ms) {
    net::ErrorCode error = tcp_client.connect(server);
    if (error == 0) return 0;
    if (error != EINPROGRESS) return error;
    pollfd descriptor{tcp_client.fd(), POLLOUT, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready == 0) {
        tcp_client.close();
        return ETIMEDOUT;
    }
    error = tcp_client.complete_connect();
    return error == EINPROGRESS ? ETIMEDOUT : error;
}

// Waits until any of `descriptors` is readable or timeout_ms passes (the
// idle step of a client loop that pumps several sessions).
inline void wait_for_input(std::span<const int> descriptors, int timeout_ms) {
    std::array<pollfd, 16> entries{};
    std::size_t count = 0;
    for (int descriptor : descriptors) {
        if (descriptor >= 0 && count < entries.size()) entries[count++] = pollfd{descriptor, POLLIN, 0};
    }
    (void)::poll(entries.data(), static_cast<nfds_t>(count), timeout_ms);
}

enum class PumpStatus : std::uint8_t { Open, Closed };

// Sends what is queued, then reads what arrived and hands it to `consume`
// (returns bytes consumed). Non-blocking.
template <class Consume>
PumpStatus pump_tcp_client(net::TcpClient& tcp_client, Consume&& consume) {
    if (!tcp_client.connected()) return PumpStatus::Closed;
    if (tcp_client.connection().send_buffer().has_pending() &&
        tcp_client.flush().status == net::FlushStatus::Error) {
        return PumpStatus::Closed;
    }
    for (;;) {
        const net::ReceiveResult received = tcp_client.receive();
        net::ReceiveBuffer& buffer = tcp_client.connection().receive_buffer();
        if (!buffer.empty()) buffer.consume(consume(buffer.readable()));
        if (received.status == net::ReceiveStatus::Received) continue;
        if (received.status == net::ReceiveStatus::WouldBlock) return PumpStatus::Open;
        return PumpStatus::Closed;
    }
}

// ---- OUCH ----------------------------------------------------------------------------

template <class Handler>
class OuchTcpSession {
public:
    using Client = gateway::ouch::OuchClient<TcpClientSendSink, Handler>;

    explicit OuchTcpSession(Handler& handler, const gateway::ouch::OuchClientSettings& settings = {})
        : client_(TcpClientSendSink{&tcp_client_}, handler, settings) {}

    OuchTcpSession(const OuchTcpSession&) = delete;
    OuchTcpSession& operator=(const OuchTcpSession&) = delete;

    // Connects and sends Login Request (sequence 0: new messages only; use
    // client().next_expected_sequence() to recover after a reconnect).
    bool connect_and_login(net::Ipv4Endpoint server, const gateway::ouch::OuchClientCredentials& credentials,
                           std::uint64_t requested_sequence, int timeout_ms = 2000) {
        if (connect_with_timeout(tcp_client_, server, timeout_ms) != 0) return false;
        if (!client_.login(credentials, requested_sequence, net::monotonic_ns())) return false;
        return flush();
    }

    // Flushes, reads, runs the client timer. Returns false once the
    // connection is closed (the client is then told: on_transport_closed).
    bool poll() {
        if (!tcp_client_.connected()) return false;
        const Timestamp now = net::monotonic_ns();
        const PumpStatus status = pump_tcp_client(
            tcp_client_, [&](std::span<const char> bytes) { return client_.on_data(bytes, now); });
        if (status == PumpStatus::Closed) {
            tcp_client_.close();
            client_.on_transport_closed();
            return false;
        }
        client_.on_timer(now);
        return true;
    }

    bool flush() {
        if (!tcp_client_.connected()) return false;
        return tcp_client_.flush().status != net::FlushStatus::Error;
    }

    void disconnect() {
        tcp_client_.close();
        client_.on_transport_closed();
    }

    [[nodiscard]] Client& client() noexcept { return client_; }
    [[nodiscard]] bool connected() const noexcept { return tcp_client_.connected(); }
    [[nodiscard]] int fd() const noexcept { return tcp_client_.fd(); }

private:
    net::TcpClient tcp_client_{1 << 16, 1 << 20};
    Client client_;
};

// ---- FIX -----------------------------------------------------------------------------

[[nodiscard]] inline gateway::fix::FixTime fix_time_now() noexcept {
    return gateway::fix::FixTime{net::monotonic_ns(), net::realtime_ns()};
}

template <class Handler>
class FixTcpSession {
public:
    using Client = gateway::fix::FixClient<Handler, TcpClientSendSink>;

    FixTcpSession(Handler& handler, const gateway::fix::FixClientSettings& settings) : client_(handler, settings) {}

    FixTcpSession(const FixTcpSession&) = delete;
    FixTcpSession& operator=(const FixTcpSession&) = delete;

    // Connects and sends Logon. reset_sequence_numbers: 141=Y.
    bool connect_and_logon(net::Ipv4Endpoint server, bool reset_sequence_numbers, int timeout_ms = 2000) {
        if (connect_with_timeout(tcp_client_, server, timeout_ms) != 0) return false;
        if (!client_.connect(sink_, reset_sequence_numbers, fix_time_now())) return false;
        return flush();
    }

    bool poll() {
        if (!tcp_client_.connected()) return false;
        const gateway::fix::FixTime now = fix_time_now();
        const PumpStatus status = pump_tcp_client(
            tcp_client_, [&](std::span<const char> bytes) { return client_.on_data(bytes, now); });
        if (status == PumpStatus::Open) client_.on_timer(now);
        if (status == PumpStatus::Closed || client_.wants_disconnect()) {
            if (tcp_client_.connected()) (void)tcp_client_.flush();
            tcp_client_.close();
            client_.on_disconnected();
            return false;
        }
        return true;
    }

    bool flush() {
        if (!tcp_client_.connected()) return false;
        return tcp_client_.flush().status != net::FlushStatus::Error;
    }

    void disconnect() {
        tcp_client_.close();
        client_.on_disconnected();
    }

    [[nodiscard]] Client& client() noexcept { return client_; }
    [[nodiscard]] bool connected() const noexcept { return tcp_client_.connected(); }
    [[nodiscard]] int fd() const noexcept { return tcp_client_.fd(); }

private:
    net::TcpClient tcp_client_{1 << 16, 1 << 20};
    TcpClientSendSink sink_{&tcp_client_};
    Client client_;
};

// ---- SBE over FIXP --------------------------------------------------------------------

// A fresh RFC 4122 version 4 UUID for FIXP Negotiate (setup path: seeds a
// generator from std::random_device).
[[nodiscard]] inline gateway::fixp_sbe::sbe::SessionUuid random_fixp_session_uuid() {
    std::random_device entropy;
    std::mt19937_64 generator((std::uint64_t{entropy()} << 32) ^ entropy());
    const std::uint64_t high_bits = generator();
    return gateway::fixp_sbe::version4_uuid_from_random_bits(high_bits, generator());
}

// The FIXP client runs on Unix epoch nanoseconds (Negotiate and Establish
// timestamps); poll() establishes as soon as the NegotiationResponse arrives.
template <class Handler>
class FixpSbeTcpSession {
public:
    using Client = gateway::fixp_sbe::FixpSbeClient<TcpClientSendSink, Handler>;

    FixpSbeTcpSession(Handler& handler, const gateway::fixp_sbe::FixpSbeClientSettings& settings)
        : client_(TcpClientSendSink{&tcp_client_}, handler, settings) {}

    FixpSbeTcpSession(const FixpSbeTcpSession&) = delete;
    FixpSbeTcpSession& operator=(const FixpSbeTcpSession&) = delete;

    // Connects and sends Negotiate for a new logical session; Establish
    // follows from poll(). Both flows start at sequence number 1.
    bool connect_and_negotiate(net::Ipv4Endpoint server, const gateway::fixp_sbe::sbe::SessionUuid& session_uuid,
                               int timeout_ms = 2000) {
        if (connect_with_timeout(tcp_client_, server, timeout_ms) != 0) return false;
        if (!client_.negotiate(session_uuid, net::realtime_ns())) return false;
        return flush();
    }

    // Reconnects the same logical session (Establish with credentials); the
    // client recovers missed server messages through RetransmitRequest.
    bool reconnect_and_establish(net::Ipv4Endpoint server, int timeout_ms = 2000) {
        if (tcp_client_.connected()) tcp_client_.close();
        if (connect_with_timeout(tcp_client_, server, timeout_ms) != 0) return false;
        client_.reset_transport(TcpClientSendSink{&tcp_client_});
        if (!client_.establish(net::realtime_ns())) return false;
        return flush();
    }

    // Flushes, reads, establishes after negotiation, runs the keepalive
    // timer. Returns false once the connection is closed: after the peer
    // closed it, or after a Terminate exchange (the answering Terminate is
    // flushed first).
    bool poll() {
        if (!tcp_client_.connected()) return false;
        const std::uint64_t now = net::realtime_ns();
        const PumpStatus status = pump_tcp_client(
            tcp_client_, [&](std::span<const char> bytes) { return client_.on_data(bytes, now); });
        if (status == PumpStatus::Open) {
            if (client_.state() == gateway::fixp_sbe::FixpSbeClientState::Negotiated) (void)client_.establish(now);
            client_.on_timer(now);
        }
        if (status == PumpStatus::Closed || client_.state() == gateway::fixp_sbe::FixpSbeClientState::Terminated) {
            if (tcp_client_.connected()) (void)tcp_client_.flush();
            tcp_client_.close();
            return false;
        }
        if (tcp_client_.connection().send_buffer().has_pending()) (void)tcp_client_.flush();
        return true;
    }

    bool flush() {
        if (!tcp_client_.connected()) return false;
        return tcp_client_.flush().status != net::FlushStatus::Error;
    }

    // Drops the transport without Terminate (the server sees the socket close).
    void disconnect() { tcp_client_.close(); }

    [[nodiscard]] Client& client() noexcept { return client_; }
    [[nodiscard]] bool established() const noexcept {
        return client_.state() == gateway::fixp_sbe::FixpSbeClientState::Established;
    }
    [[nodiscard]] bool connected() const noexcept { return tcp_client_.connected(); }
    [[nodiscard]] int fd() const noexcept { return tcp_client_.fd(); }

private:
    net::TcpClient tcp_client_{1 << 16, 1 << 20};
    Client client_;
};

// ---- market data ------------------------------------------------------------------------

// Listens for the feed on a unicast UDP port (port 0: the OS picks one, read
// it back with feed_endpoint()) and sends gap requests to the server's
// retransmission endpoint from a second socket.
template <class Handler>
class MarketDataFeed {
public:
    MarketDataFeed(Handler& handler, const md::ReceiverConfig& config)
        : receiver_(config, handler, RequestSink{this}) {}

    MarketDataFeed(const MarketDataFeed&) = delete;
    MarketDataFeed& operator=(const MarketDataFeed&) = delete;

    // Returns 0 or errno.
    net::ErrorCode open(net::Ipv4Endpoint feed_local_endpoint) {
        if (net::ErrorCode error = feed_socket_.open(feed_local_endpoint); error != 0) return error;
        (void)net::set_receive_buffer_size(feed_socket_.fd(), 8 << 20);
        if (net::ErrorCode error = net::local_endpoint(feed_socket_.fd(), feed_endpoint_); error != 0) return error;
        return request_socket_.open(net::loopback_endpoint(0));
    }
    void set_retransmission_server(net::Ipv4Endpoint server) noexcept { retransmission_server_ = server; }

    // Reads every pending datagram from both sockets and runs the request
    // timer. Returns the number of datagrams handled.
    std::size_t poll() {
        std::size_t handled = 0;
        for (net::UdpReceiver* socket : {&feed_socket_, &request_socket_}) {
            for (;;) {
                net::Ipv4Endpoint peer{};
                const ssize_t length = socket->receive_from(datagram_, peer);
                if (length < 0) {
                    if (errno == EINTR || errno == EMSGSIZE) continue;
                    break;
                }
                ++handled;
                if (socket == &request_socket_ &&
                    (peer.address != retransmission_server_.address || peer.port != retransmission_server_.port)) continue;
                (void)receiver_.on_packet(std::span<const char>(datagram_.data(), static_cast<std::size_t>(length)),
                                          net::monotonic_ns());
            }
        }
        receiver_.poll(net::monotonic_ns());
        return handled;
    }

    [[nodiscard]] net::Ipv4Endpoint feed_endpoint() const noexcept { return feed_endpoint_; }
    [[nodiscard]] int feed_fd() const noexcept { return feed_socket_.fd(); }
    [[nodiscard]] int request_fd() const noexcept { return request_socket_.fd(); }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return receiver_.next_sequence(); }
    [[nodiscard]] bool end_of_session_reached() const noexcept { return receiver_.end_of_session_reached(); }
    [[nodiscard]] const md::ReceiverStats& stats() const noexcept { return receiver_.stats(); }

private:
    struct RequestSink {
        MarketDataFeed* feed;
        void operator()(std::span<const char> request) const {
            if (feed->retransmission_server_.port != 0) {
                (void)feed->request_socket_.send_to(request, feed->retransmission_server_);
            }
        }
    };

    net::UdpReceiver feed_socket_;
    net::UdpReceiver request_socket_;
    net::Ipv4Endpoint feed_endpoint_{};
    net::Ipv4Endpoint retransmission_server_{};
    std::array<char, 65536> datagram_{};
    md::MoldUdp64Receiver<Handler, RequestSink> receiver_;
};

// ---- admin ---------------------------------------------------------------------------------

// Blocking request/reply over the admin line protocol (operator path).
class AdminLineClient {
public:
    bool connect(net::Ipv4Endpoint server, int timeout_ms = 2000) {
        return connect_with_timeout(tcp_client_, server, timeout_ms) == 0;
    }

    // Sends one command line and returns the reply through its final "OK" or
    // "ERR ..." line; an empty string on timeout or a closed connection.
    std::string request(std::string_view command_line, int timeout_ms = 5000) {
        std::string line(command_line);
        line.push_back('\n');
        if (!tcp_client_.send(line)) return {};
        const std::uint64_t deadline = net::monotonic_ns() + std::uint64_t(timeout_ms) * 1'000'000;
        while (net::monotonic_ns() < deadline) {
            const PumpStatus status = pump_tcp_client(tcp_client_, [&](std::span<const char> bytes) {
                received_.append(bytes.data(), bytes.size());
                return bytes.size();
            });
            const std::size_t reply_end = complete_reply_end();
            if (reply_end != 0) {
                std::string reply = received_.substr(0, reply_end);
                received_.erase(0, reply_end);
                return reply;
            }
            if (status == PumpStatus::Closed) return {};
            const int descriptor = tcp_client_.fd();
            wait_for_input(std::span<const int>(&descriptor, 1), 5);
        }
        return {};
    }

    [[nodiscard]] bool connected() const noexcept { return tcp_client_.connected(); }

private:
    // End offset of the first complete reply (after its OK / ERR line), or 0.
    [[nodiscard]] std::size_t complete_reply_end() const {
        std::size_t line_start = 0;
        for (;;) {
            const std::size_t newline = received_.find('\n', line_start);
            if (newline == std::string::npos) return 0;
            const std::string_view line(received_.data() + line_start, newline - line_start);
            if (line == "OK" || line.substr(0, 4) == "ERR ") return newline + 1;
            line_start = newline + 1;
        }
    }

    net::TcpClient tcp_client_;
    std::string received_;
};

}  // namespace exsim::client

#endif
