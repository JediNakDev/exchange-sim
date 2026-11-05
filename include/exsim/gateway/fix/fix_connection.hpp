#ifndef EXSIM_GATEWAY_FIX_FIX_CONNECTION_HPP
#define EXSIM_GATEWAY_FIX_FIX_CONNECTION_HPP

// Exchange side of one FIX 4.4 TCP connection (docs/SERVER.md 2). The server
// owns the transport and calls:
//   start(sink, connection_id, ctx)  on accept (the object is reusable)
//   on_data(bytes, ctx)              returns bytes consumed
//   on_timer(ctx)                    periodically (heartbeats, timeouts)
//   wants_disconnect()               true: flush and close, then
//   on_disconnected(ctx)             releases the session (ctx.unbind, which
//                                    triggers cancel-on-disconnect)
// A bound connection must get on_disconnected before it is destroyed (the
// destructor cannot reach the context; debug builds assert it). start() on a
// bound connection releases the binding first.
// Logon: SenderCompID must be a configured FIX user (authenticated with the
// optional Password(554)), TargetCompID the exchange CompID, EncryptMethod 0,
// HeartBtInt <= settings.max_heartbeat_interval_seconds. A refused Logon is
// answered with an unsequenced Logout (34=1, not stored) and disconnect.

#include "exsim/command.hpp"
#include "exsim/gateway/fix/fix_common.hpp"
#include "exsim/gateway/fix/fix_session_layer.hpp"
#include "exsim/gateway/fix/fix_user_state.hpp"
#include "exsim/instrument.hpp"
#include "exsim/server/core/policies.hpp"
#include "exsim/server/config.hpp"

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace exsim::gateway::fix {

template <class Context>
concept FixGatewayContext = requires(Context& context, const Context& const_context, std::string_view text,
                                     SessionId session, server::ConnectionId connection, const Command& command,
                                     const Symbol& symbol, InstrumentId instrument) {
    { context.authenticate(text, text, server::Protocol::Fix44) } -> std::convertible_to<const server::UserConfig*>;
    { context.bind(session, connection) } -> std::convertible_to<bool>;
    context.unbind(session);
    { const_context.find_instrument(symbol) } -> std::convertible_to<InstrumentId>;
    { const_context.instrument(instrument) } -> std::convertible_to<const InstrumentConfig*>;
    context.submit(command);
    { const_context.now() } -> std::convertible_to<Timestamp>;
    { const_context.realtime_ns() } -> std::convertible_to<std::uint64_t>;
};

template <FixGatewayContext Context, FixByteSink Sink>
class FixConnection : public FixSessionLayer<FixConnection<Context, Sink>, Sink> {
    using Base = FixSessionLayer<FixConnection<Context, Sink>, Sink>;
    friend Base;

public:
    explicit FixConnection(FixUserDirectory<Sink>& directory, const FixSessionSettings& settings = {})
        : Base(settings), directory_(directory) {}

    ~FixConnection() {
        assert(!session_bound_ && "call on_disconnected(ctx) before destroying a bound FixConnection");
        this->end_connection();
    }

    FixConnection(const FixConnection&) = delete;
    FixConnection& operator=(const FixConnection&) = delete;

    void start(Sink& sink, server::ConnectionId connection_id, Context& ctx) noexcept {
        if (session_bound_) {
            on_disconnected(ctx);
        }
        this->end_connection();
        user_ = nullptr;
        session_bound_ = false;
        connection_id_ = connection_id;
        this->now_ = FixTime{ctx.now(), ctx.realtime_ns()};
        this->begin_connection(sink, FixConnectionPhase::AwaitingLogon);
    }

    std::size_t on_data(std::span<const char> bytes, Context& ctx) noexcept {
        context_ = &ctx;
        this->now_ = FixTime{ctx.now(), ctx.realtime_ns()};
        const std::size_t consumed = this->consume_inbound(bytes);
        context_ = nullptr;
        return consumed;
    }

    void on_timer(Context& ctx) noexcept {
        context_ = &ctx;
        this->now_ = FixTime{ctx.now(), ctx.realtime_ns()};
        this->run_timers();
        context_ = nullptr;
    }

    // Exchange-initiated Logout (end of day, admin). The peer's Logout or the
    // logout timeout completes it.
    bool begin_logout(std::string_view text, Context& ctx) noexcept {
        this->now_ = FixTime{ctx.now(), ctx.realtime_ns()};
        return this->start_logout(text);
    }

    // The transport is closed. Stops transmission first, so messages produced
    // by cancel-on-disconnect are stored for the next login, then unbinds.
    void on_disconnected(Context& ctx) noexcept {
        // A newer connection of the same user has bound the day state, so the
        // server already released this connection's binding: keep the new one.
        session_bound_ = session_bound_ && this->day_state_bound_to_this_connection();
        this->end_connection();
        if (session_bound_ && user_ != nullptr) {
            session_bound_ = false;
            ctx.unbind(user_->session_id());
        }
        user_ = nullptr;
    }

    [[nodiscard]] FixUserState<Sink>* user() const noexcept { return user_; }
    [[nodiscard]] server::ConnectionId connection_id() const noexcept { return connection_id_; }

private:
    void on_frame_before_active(const FixInboundFrame& frame) noexcept {
        if (frame.parse_status != Status::ok || frame.message_type != "A") {
            this->request_disconnect();
            return;
        }
        FixFieldReader reader(*frame.parser);
        const std::string_view sender = reader.required_text(49, kMaxCompIdLength);
        const std::string_view target = reader.required_text(56, kMaxCompIdLength);
        const std::uint64_t sequence = reader.required_uint(34);
        reader.required_timestamp(52);
        const std::uint64_t encrypt_method = reader.required_uint(98);
        const std::uint64_t heartbeat_seconds = reader.required_uint(108);
        const char reset_sequence = reader.optional_char(141, "YN", 'N');
        const std::string_view password = reader.optional_text(554);
        if (!reader.ok()) {
            return refuse_logon(sender, reader.problem().description);
        }
        if (sequence == 0) {
            return refuse_logon(sender, "MsgSeqNum(34) must be positive");
        }
        if (sequence > kMaxMsgSeqNum) {
            return refuse_logon(sender, "MsgSeqNum(34) out of range");
        }
        if (encrypt_method != 0) {
            return refuse_logon(sender, "EncryptMethod(98) must be 0");
        }
        if (heartbeat_seconds > this->settings_.max_heartbeat_interval_seconds) {
            return refuse_logon(sender, "HeartBtInt(108) out of range");
        }
        if (target != directory_.exchange_comp_id()) {
            return refuse_logon(sender, "invalid TargetCompID");
        }
        const server::UserConfig* config = context_->authenticate(sender, password, server::Protocol::Fix44);
        FixUserState<Sink>* user = config != nullptr ? directory_.find(config->session_id) : nullptr;
        if (user == nullptr || user->comp_id() != sender) {
            return refuse_logon(sender, "invalid SenderCompID or Password");
        }
        if (!context_->bind(user->session_id(), connection_id_)) {
            return refuse_logon(sender, "already logged in");
        }
        user_ = user;
        session_bound_ = true;
        FixSessionDayState<Sink>& day = user->session_day_state();
        this->attach_day_state(day);
        if (reset_sequence == 'Y') {
            if (sequence != 1) {
                return this->send_logout_and_disconnect("ResetSeqNumFlag(141)=Y requires MsgSeqNum(34)=1");
            }
            day.reset_sequence_numbers();
        } else if (day.store_exhausted()) {
            refuse_logon(sender, "resend store exhausted; explicit session reset required");
            return;
        } else if (sequence < day.expected_inbound_seq()) {
            std::array<char, 96> text_buffer{};
            return this->send_logout_and_disconnect(
                format_sequence_too_low(text_buffer, day.expected_inbound_seq(), sequence));
        }
        fix44::Logon response;
        response.encrypt_method = 0;
        response.heartbeat_interval = heartbeat_seconds;
        response.has_reset_sequence = reset_sequence == 'Y';
        response.reset_sequence = 'Y';
        day.send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(response, header, buffer.data(), buffer.size());
            },
            this->now_);
        this->enter_active(heartbeat_seconds);
        this->accept_logon_sequence(sequence);
    }

    void refuse_logon(std::string_view sender, std::string_view text) noexcept {
        std::array<char, 512> buffer{};
        fix44::Builder writer(buffer);
        writer.field(35, '5');
        writer.field(49, directory_.exchange_comp_id());
        writer.field(56, sender.empty() ? std::string_view("UNKNOWN") : sender);
        writer.uint_field(34, 1);
        writer.timestamp_field(52, this->now_.realtime_ns);
        writer.field(58, text);
        const std::size_t size = writer.finish();
        if (size != 0) {
            this->write_unsequenced({buffer.data(), size});
        }
        this->request_disconnect();
    }

    void on_application_message(const FixInboundFrame& frame) noexcept {
        const std::string_view type = frame.message_type;
        if (type == "D") {
            return decode_new_order(frame);
        }
        if (type == "F") {
            return decode_cancel(frame);
        }
        if (type == "G") {
            return decode_replace(frame);
        }
        if (type == "q") {
            return decode_mass_cancel(frame);
        }
        if (is_fix44_message_type(type)) {
            return send_unsupported_message_type_reject(frame);
        }
        this->send_session_reject(frame.sequence, 35, type, SessionRejectReason::InvalidMsgType,
                                  "invalid MsgType");
    }

    // A valid FIX 4.4 application MsgType this gateway does not support:
    // BusinessMessageReject(j) 380=3 (an application message, stored for resend).
    void send_unsupported_message_type_reject(const FixInboundFrame& frame) noexcept {
        constexpr std::uint64_t kUnsupportedMessageType = 3;
        fix44::BusinessMessageReject reject;
        reject.has_reference_sequence = true;
        reject.reference_sequence = frame.sequence;
        reject.reference_type.borrow(frame.message_type);
        reject.reason = kUnsupportedMessageType;
        reject.has_text = true;
        reject.text.borrow("unsupported MsgType");
        this->day_->send_application_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(reject, header, buffer.data(), buffer.size());
            },
            this->now_);
    }

    void on_session_reject_received(const FixInboundFrame&) noexcept {}
    void on_logout_received(std::string_view) noexcept {}

    static Side decode_side(char code) noexcept { return code == '2' ? Side::SELL : Side::BUY; }

    static StpMode decode_stp(char code, StpMode default_mode) noexcept {
        switch (code) {
            case 'N': return StpMode::None;
            case 'R': return StpMode::CancelResting;
            case 'A': return StpMode::CancelAggressor;
            case 'B': return StpMode::CancelBoth;
            default: return default_mode;
        }
    }

    void decode_new_order(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        FixNewOrderRequest request;
        request.client_order_id = reader.required_text(11);
        reader.optional_text(1);  // Account: accepted, not used
        request.symbol = reader.required_text(55);
        request.side = decode_side(reader.required_char(54, "12"));
        request.order_qty = reader.required_uint(38);
        request.order_type_code = reader.required_char(40, {});
        request.has_price = reader.optional_price(44, request.price);
        if (request.order_type_code == '2' && !request.has_price) {
            reader.report(44, SessionRejectReason::RequiredTagMissing, "Price(44) required for OrdType 2");
        }
        request.time_in_force_code = reader.optional_char(59, {}, '0');
        request.post_only = reader.optional_text(18).find('6') != std::string_view::npos;
        reader.required_timestamp(60);
        request.stp = decode_stp(reader.optional_char(7928, "NRAB", '\0'), user_->default_stp());
        if (!reader.ok()) {
            return this->send_session_reject(frame, reader.problem());
        }
        user_->submit_new_order(request, *context_);
    }

    void decode_cancel(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        FixCancelRequest request;
        request.original_client_order_id = reader.required_text(41);
        request.client_order_id = reader.required_text(11);
        reader.required_text(55);
        reader.required_char(54, "12");
        reader.required_timestamp(60);
        if (!reader.ok()) {
            return this->send_session_reject(frame, reader.problem());
        }
        user_->submit_cancel(request, *context_);
    }

    void decode_replace(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        FixReplaceRequest request;
        request.original_client_order_id = reader.required_text(41);
        request.client_order_id = reader.required_text(11);
        reader.required_text(55);
        reader.required_char(54, "12");
        reader.required_timestamp(60);
        request.order_qty = reader.required_uint(38);
        request.order_type_code = reader.required_char(40, {});
        request.has_price = reader.optional_price(44, request.price);
        if (request.order_type_code == '2' && !request.has_price) {
            reader.report(44, SessionRejectReason::RequiredTagMissing, "Price(44) required for OrdType 2");
        }
        if (!reader.ok()) {
            return this->send_session_reject(frame, reader.problem());
        }
        user_->submit_replace(request, *context_);
    }

    void decode_mass_cancel(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        FixMassCancelRequest request;
        request.client_order_id = reader.required_text(11);
        // Every FIX 4.4 MassCancelRequestType is accepted here; the user state
        // answers the unsupported ones (2..6) with 531=0 532=0.
        request.request_type = reader.required_char(530, "1234567");
        request.symbol = request.request_type == '1' ? reader.required_text(55) : reader.optional_text(55);
        if (!reader.ok()) {
            return this->send_session_reject(frame, reader.problem());
        }
        user_->submit_mass_cancel(request, *context_);
    }

    FixUserDirectory<Sink>& directory_;
    FixUserState<Sink>* user_ = nullptr;
    Context* context_ = nullptr;
    server::ConnectionId connection_id_{};
    bool session_bound_ = false;
};

}  // namespace exsim::gateway::fix

#endif
