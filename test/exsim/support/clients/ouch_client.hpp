#ifndef EXSIM_TEST_SUPPORT_OUCH_CLIENT_HPP
#define EXSIM_TEST_SUPPORT_OUCH_CLIENT_HPP

// Client side of OUCH 4.2 over SoupBinTCP 3.0: login
// with a requested sequence number, implicit SoupBinTCP sequencing, client
// heartbeats, server-silence detection, recovery on reconnect and typed
// callbacks. Test support: the allocation tests drive it too, so it does not
// allocate per message.
//
// Transport: any ByteSink (prepare/commit) the bytes to the server are
// written to; bytes from the server are passed to on_data().
//
// Handler: every callback is optional (detected at compile time):
//   on_login_accepted(const soupbintcp::LoginAccepted&)
//   on_login_rejected(char reason)
//   on_sequence_mismatch(std::uint64_t requested, std::uint64_t server_next)
//   on_server_heartbeat()   on_end_of_session()   on_server_timeout()
//   on_debug(std::string_view text)
//   on_system_event(const ouch42::SystemEvent&, std::uint64_t sequence)
//   on_order_accepted / on_order_replaced / on_order_canceled /
//   on_order_executed / on_order_rejected / on_cancel_pending /
//   on_cancel_reject / on_order_modified (const message&, std::uint64_t sequence)
//   on_unknown_sequenced_message(std::string_view payload, std::uint64_t sequence)
//
// Recovery: next_expected_sequence() survives on_transport_closed(); pass it
// to login() on the new connection and the server replays what was missed.

#include "gateway/byte_sink.hpp"
#include "protocol/ouch42.hpp"
#include "protocol/soupbintcp.hpp"
#include "engine/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

namespace exsim::gateway::ouch {

struct OuchClientSettings {
    Timestamp client_heartbeat_interval_ns = 1'000'000'000;
    Timestamp server_idle_timeout_ns = 15'000'000'000;
};

struct OuchClientCredentials {
    std::string_view username;           // <= 6 characters
    std::string_view password;           // <= 10 characters
    std::string_view requested_session;  // <= 10 characters; empty = current session
};

enum class OuchClientState : std::uint8_t {
    Disconnected,
    AwaitingLoginResponse,
    LoggedIn,
    LoginRejected,
    SessionEnded,    // server sent End of Session
    ServerTimedOut,  // nothing from the server for server_idle_timeout_ns
    LoggedOut,       // logout() sent
    ProtocolError,   // undecodable frame from the server
};

template <ByteSink Transport, class Handler>
class OuchClient {
public:
    static constexpr std::size_t kInboundFrameCapacity = 512;

    OuchClient(Transport transport, Handler& handler, const OuchClientSettings& settings = {}) noexcept
        : transport_(std::move(transport)),
          handler_(handler),
          settings_(settings),
          frame_reader_(std::span<char>(inbound_frame_), kInboundFrameCapacity - 2) {}

    OuchClient(const OuchClient&) = delete;
    OuchClient& operator=(const OuchClient&) = delete;

    // Sends Login Request. requested_sequence 0 = only new messages; use
    // next_expected_sequence() to recover after a reconnect.
    bool login(const OuchClientCredentials& credentials, std::uint64_t requested_sequence, Timestamp now) noexcept {
        if (state_ == OuchClientState::AwaitingLoginResponse || state_ == OuchClientState::LoggedIn) return false;
        protocol::soupbintcp::LoginRequest request;
        if (!protocol::wire::alpha(request.username.data(), request.username.size(), credentials.username) ||
            !protocol::wire::alpha(request.password.data(), request.password.size(), credentials.password) ||
            !protocol::wire::alpha(request.session.data(), request.session.size(), credentials.requested_session)) {
            return false;
        }
        request.sequence = requested_sequence;
        frame_reader_.reset();
        if (!send_frame(request, now)) return false;
        requested_sequence_ = requested_sequence;
        last_received_ns_ = now;
        state_ = OuchClientState::AwaitingLoginResponse;
        return true;
    }

    bool enter_order(const protocol::ouch42::EnterOrder& message, Timestamp now) noexcept {
        return send_application_message(message, now);
    }
    bool replace_order(const protocol::ouch42::ReplaceOrder& message, Timestamp now) noexcept {
        return send_application_message(message, now);
    }
    bool cancel_order(const protocol::ouch42::CancelOrder& message, Timestamp now) noexcept {
        return send_application_message(message, now);
    }
    bool modify_order(const protocol::ouch42::ModifyOrder& message, Timestamp now) noexcept {
        return send_application_message(message, now);
    }

    bool send_heartbeat(Timestamp now) noexcept {
        return state_ == OuchClientState::LoggedIn && send_frame(protocol::soupbintcp::ClientHeartbeat{}, now);
    }

    bool logout(Timestamp now) noexcept {
        if (state_ != OuchClientState::LoggedIn) return false;
        if (!send_frame(protocol::soupbintcp::LogoutRequest{}, now)) return false;
        state_ = OuchClientState::LoggedOut;
        return true;
    }

    // Consumes every byte (partial frames are buffered).
    std::size_t on_data(std::span<const char> bytes, Timestamp now) noexcept {
        if (!receiving()) return bytes.size();
        last_received_ns_ = now;
        std::size_t offset = 0;
        while (offset < bytes.size() && receiving()) {
            std::size_t consumed = 0;
            protocol::soupbintcp::Packet packet;
            const protocol::Status status = frame_reader_.feed(bytes.subspan(offset), consumed, packet);
            offset += consumed;
            if (status == protocol::Status::incomplete) break;
            if (status != protocol::Status::ok) {
                state_ = OuchClientState::ProtocolError;
                break;
            }
            handle_packet(packet);
        }
        return bytes.size();
    }

    // Client heartbeat after client_heartbeat_interval_ns without sending;
    // ServerTimedOut after server_idle_timeout_ns without receiving.
    void on_timer(Timestamp now) noexcept {
        if (state_ != OuchClientState::LoggedIn && state_ != OuchClientState::AwaitingLoginResponse) return;
        if (elapsed(last_received_ns_, now) >= settings_.server_idle_timeout_ns) {
            state_ = OuchClientState::ServerTimedOut;
            if constexpr (requires { handler_.on_server_timeout(); }) handler_.on_server_timeout();
            return;
        }
        if (state_ == OuchClientState::LoggedIn && elapsed(last_sent_ns_, now) >= settings_.client_heartbeat_interval_ns) {
            send_frame(protocol::soupbintcp::ClientHeartbeat{}, now);
        }
    }

    // The connection is gone. Sequence state is kept for recovery.
    void on_transport_closed() noexcept {
        state_ = OuchClientState::Disconnected;
        frame_reader_.reset();
    }

    void reset_for_new_trading_day() noexcept { next_expected_sequence_ = 1; }

    [[nodiscard]] OuchClientState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t next_expected_sequence() const noexcept { return next_expected_sequence_; }
    [[nodiscard]] std::uint64_t sequenced_messages_received() const noexcept { return sequenced_messages_received_; }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }
    [[nodiscard]] const Transport& transport() const noexcept { return transport_; }

private:
    [[nodiscard]] bool receiving() const noexcept {
        return state_ == OuchClientState::AwaitingLoginResponse || state_ == OuchClientState::LoggedIn ||
               state_ == OuchClientState::LoggedOut;
    }

    void handle_packet(const protocol::soupbintcp::Packet& packet) noexcept {
        switch (packet.type) {
            case 'A': handle_login_accepted(packet); return;
            case 'J': {
                state_ = OuchClientState::LoginRejected;
                if constexpr (requires { handler_.on_login_rejected(packet.payload[0]); }) {
                    handler_.on_login_rejected(packet.payload[0]);
                }
                return;
            }
            case 'S': handle_sequenced_data(packet.payload); return;
            case 'H':
                if constexpr (requires { handler_.on_server_heartbeat(); }) handler_.on_server_heartbeat();
                return;
            case 'Z':
                state_ = OuchClientState::SessionEnded;
                if constexpr (requires { handler_.on_end_of_session(); }) handler_.on_end_of_session();
                return;
            case '+':
                if constexpr (requires { handler_.on_debug(packet.payload); }) handler_.on_debug(packet.payload);
                return;
            default: state_ = OuchClientState::ProtocolError; return;  // client-only packet types
        }
    }

    void handle_login_accepted(const protocol::soupbintcp::Packet& packet) noexcept {
        protocol::soupbintcp::LoginAccepted accepted;
        if (state_ != OuchClientState::AwaitingLoginResponse ||
            protocol::soupbintcp::decode(packet.payload.data() - 3, packet.payload.size() + 3, accepted) !=
                protocol::Status::ok) {
            state_ = OuchClientState::ProtocolError;
            return;
        }
        state_ = OuchClientState::LoggedIn;
        if (requested_sequence_ != 0 && accepted.sequence != requested_sequence_) {
            if constexpr (requires { handler_.on_sequence_mismatch(requested_sequence_, accepted.sequence); }) {
                handler_.on_sequence_mismatch(requested_sequence_, accepted.sequence);
            }
        }
        next_expected_sequence_ = accepted.sequence;
        if constexpr (requires { handler_.on_login_accepted(accepted); }) handler_.on_login_accepted(accepted);
    }

    void handle_sequenced_data(std::string_view payload) noexcept {
        if (state_ == OuchClientState::AwaitingLoginResponse) {
            state_ = OuchClientState::ProtocolError;  // sequenced data before Login Accepted
            return;
        }
        const std::uint64_t sequence = next_expected_sequence_++;
        ++sequenced_messages_received_;
        if (payload.empty()) {
            report_unknown(payload, sequence);
            return;
        }
        switch (payload.front()) {
            case protocol::ouch42::SystemEvent::type:
                decode_and_notify<protocol::ouch42::SystemEvent>(payload, sequence);
                return;
            case protocol::ouch42::OrderAccepted::type:
                decode_and_notify<protocol::ouch42::OrderAccepted>(payload, sequence);
                return;
            case protocol::ouch42::OrderReplaced::type:
                decode_and_notify<protocol::ouch42::OrderReplaced>(payload, sequence);
                return;
            case protocol::ouch42::OrderCanceled::type:
                decode_and_notify<protocol::ouch42::OrderCanceled>(payload, sequence);
                return;
            case protocol::ouch42::OrderExecuted::type:
                decode_and_notify<protocol::ouch42::OrderExecuted>(payload, sequence);
                return;
            case protocol::ouch42::OrderRejected::type:
                decode_and_notify<protocol::ouch42::OrderRejected>(payload, sequence);
                return;
            case protocol::ouch42::CancelPending::type:
                decode_and_notify<protocol::ouch42::CancelPending>(payload, sequence);
                return;
            case protocol::ouch42::CancelReject::type:
                decode_and_notify<protocol::ouch42::CancelReject>(payload, sequence);
                return;
            case protocol::ouch42::OrderModified::type:
                decode_and_notify<protocol::ouch42::OrderModified>(payload, sequence);
                return;
            default: report_unknown(payload, sequence); return;
        }
    }

    template <class Message>
    void decode_and_notify(std::string_view payload, std::uint64_t sequence) noexcept {
        Message message;
        if (protocol::ouch42::decode(payload.data(), payload.size(), message) != protocol::Status::ok) {
            report_unknown(payload, sequence);
            return;
        }
        notify(message, sequence);
    }

    void notify(const protocol::ouch42::SystemEvent& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_system_event(message, sequence); }) handler_.on_system_event(message, sequence);
    }
    void notify(const protocol::ouch42::OrderAccepted& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_accepted(message, sequence); }) handler_.on_order_accepted(message, sequence);
    }
    void notify(const protocol::ouch42::OrderReplaced& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_replaced(message, sequence); }) handler_.on_order_replaced(message, sequence);
    }
    void notify(const protocol::ouch42::OrderCanceled& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_canceled(message, sequence); }) handler_.on_order_canceled(message, sequence);
    }
    void notify(const protocol::ouch42::OrderExecuted& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_executed(message, sequence); }) handler_.on_order_executed(message, sequence);
    }
    void notify(const protocol::ouch42::OrderRejected& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_rejected(message, sequence); }) handler_.on_order_rejected(message, sequence);
    }
    void notify(const protocol::ouch42::CancelPending& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_cancel_pending(message, sequence); }) handler_.on_cancel_pending(message, sequence);
    }
    void notify(const protocol::ouch42::CancelReject& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_cancel_reject(message, sequence); }) handler_.on_cancel_reject(message, sequence);
    }
    void notify(const protocol::ouch42::OrderModified& message, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_order_modified(message, sequence); }) handler_.on_order_modified(message, sequence);
    }

    void report_unknown(std::string_view payload, std::uint64_t sequence) noexcept {
        if constexpr (requires { handler_.on_unknown_sequenced_message(payload, sequence); }) {
            handler_.on_unknown_sequenced_message(payload, sequence);
        }
    }

    template <class Message>
    bool send_application_message(const Message& message, Timestamp now) noexcept {
        if (state_ != OuchClientState::LoggedIn) return false;
        char payload[64];
        const std::size_t payload_size = protocol::ouch42::encode(message, payload, sizeof payload);
        if (payload_size == 0) return false;
        return send_frame(protocol::soupbintcp::UnsequencedData{{payload, payload_size}}, now);
    }

    template <class Packet>
    bool send_frame(const Packet& packet, Timestamp now) noexcept {
        char frame[96];
        const std::size_t frame_size = protocol::soupbintcp::encode(packet, frame, sizeof frame);
        char* destination = frame_size == 0 ? nullptr : transport_.prepare(frame_size);
        if (destination == nullptr) return false;
        std::memcpy(destination, frame, frame_size);
        transport_.commit(frame_size);
        last_sent_ns_ = now;
        return true;
    }

    [[nodiscard]] static Timestamp elapsed(Timestamp since, Timestamp now) noexcept {
        return now > since ? now - since : 0;
    }

    Transport transport_;
    Handler& handler_;
    OuchClientSettings settings_;
    char inbound_frame_[kInboundFrameCapacity]{};
    protocol::soupbintcp::FrameReader frame_reader_;
    std::uint64_t next_expected_sequence_ = 1;
    std::uint64_t requested_sequence_ = 0;
    std::uint64_t sequenced_messages_received_ = 0;
    Timestamp last_sent_ns_ = 0;
    Timestamp last_received_ns_ = 0;
    OuchClientState state_ = OuchClientState::Disconnected;
};

}  // namespace exsim::gateway::ouch

#endif
