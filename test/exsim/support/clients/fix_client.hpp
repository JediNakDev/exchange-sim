#ifndef EXSIM_TEST_SUPPORT_FIX_CLIENT_HPP
#define EXSIM_TEST_SUPPORT_FIX_CLIENT_HPP

// Client side of FIX 4.4 order entry: Logon, MsgSeqNum
// tracking and gap recovery, heartbeats and TestRequests, answering the
// exchange's ResendRequests from its own store (orders go out again with
// PossDupFlag=Y), and typed callbacks. Transport-agnostic: bytes go to a
// ByteSink and come back through on_data, so tests wire it to a
// FixConnection in memory. The clock is passed in on every call. No per
// message allocation; the session day state (MsgSeqNums and store) survives
// reconnects of the same object.
//
// Handler callbacks receive codec structs whose text fields borrow the input
// buffer: copy what must outlive the callback. A handler may also provide
// on_business_message_reject(const fix44::BusinessMessageReject&, info);
// without it a BusinessMessageReject(j) is accepted and ignored.

#include "gateway/fix/fix_common.hpp"
#include "gateway/fix/fix_session_day_state.hpp"
#include "gateway/fix/fix_session_layer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace exsim::gateway::fix {

struct FixInboundMessageInfo {
    std::uint64_t sequence = 0;
    bool possible_duplicate = false;
};

template <class Handler>
concept FixClientHandler =
    requires(Handler& handler, const FixInboundMessageInfo& info, const fix44::ExecutionReport& execution_report,
             const fix44::OrderCancelReject& cancel_reject, const fix44::OrderMassCancelReport& mass_cancel_report,
             const fix44::Reject& session_reject, std::string_view text) {
        handler.on_logon(info);
        handler.on_execution_report(execution_report, info);
        handler.on_order_cancel_reject(cancel_reject, info);
        handler.on_order_mass_cancel_report(mass_cancel_report, info);
        handler.on_session_reject(session_reject, info);
        handler.on_logout(text);
    };

struct FixClientSettings {
    std::string_view sender_comp_id{};           // the configured FIX username
    std::string_view target_comp_id = "EXSIM";
    std::string_view password{};                 // sent as Password(554) when not empty
    std::uint64_t heartbeat_interval_seconds = 30;
    std::size_t store_max_messages = 1u << 16;
    std::size_t store_max_bytes = 8u << 20;
    FixSessionSettings session{};
};

template <FixClientHandler Handler, ByteSink Sink>
class FixClient : public FixSessionLayer<FixClient<Handler, Sink>, Sink> {
    using Base = FixSessionLayer<FixClient<Handler, Sink>, Sink>;
    friend Base;

public:
    FixClient(Handler& handler, const FixClientSettings& settings)
        : Base(settings.session),
          handler_(handler),
          heartbeat_interval_seconds_(settings.heartbeat_interval_seconds),
          day_state_(settings.sender_comp_id, settings.target_comp_id, settings.store_max_messages,
                     settings.store_max_bytes) {
        password_.assign(settings.password.substr(0, kMaxInboundTextFieldLength));
    }

    ~FixClient() { this->end_connection(); }

    FixClient(const FixClient&) = delete;
    FixClient& operator=(const FixClient&) = delete;

    // Sends Logon on a freshly connected transport. reset_sequence_numbers
    // sends ResetSeqNumFlag(141)=Y and restarts both directions at 1.
    bool connect(Sink& sink, bool reset_sequence_numbers, FixTime now) noexcept {
        this->end_connection();
        this->now_ = now;
        this->begin_connection(sink, FixConnectionPhase::LogonSent);
        this->attach_day_state(day_state_);
        if (reset_sequence_numbers) {
            day_state_.reset_sequence_numbers();
        }
        return day_state_.send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                fix44::Builder writer(buffer);
                writer.field(35, 'A');
                writer.field(49, header.sender);
                writer.field(56, header.target);
                writer.uint_field(34, header.sequence);
                writer.timestamp_field(52, header.sending_time);
                writer.uint_field(98, 0);
                writer.uint_field(108, heartbeat_interval_seconds_);
                if (reset_sequence_numbers) {
                    writer.field(141, 'Y');
                }
                if (!password_.empty()) {
                    writer.field(554, password_.view());
                }
                return writer.finish();
            },
            now);
    }

    std::size_t on_data(std::span<const char> bytes, FixTime now) noexcept {
        this->now_ = now;
        return this->consume_inbound(bytes);
    }

    void on_timer(FixTime now) noexcept {
        this->now_ = now;
        this->run_timers();
    }

    bool logout(std::string_view text, FixTime now) noexcept {
        this->now_ = now;
        return this->start_logout(text);
    }

    // The transport is gone; the day state is kept for the next connect().
    void on_disconnected() noexcept { this->end_connection(); }

    bool send_new_order_single(const fix44::NewOrderSingle& message, FixTime now) noexcept {
        return send_application(message, now);
    }
    bool send_order_cancel_request(const fix44::OrderCancelRequest& message, FixTime now) noexcept {
        return send_application(message, now);
    }
    bool send_order_cancel_replace_request(const fix44::OrderCancelReplaceRequest& message, FixTime now) noexcept {
        return send_application(message, now);
    }
    bool send_order_mass_cancel_request(const fix44::OrderMassCancelRequest& message, FixTime now) noexcept {
        return send_application(message, now);
    }

    [[nodiscard]] FixSessionDayState<Sink>& session_day_state() noexcept { return day_state_; }
    [[nodiscard]] bool active() const noexcept {
        return this->phase_ == FixConnectionPhase::Active && !this->wants_disconnect();
    }

private:
    template <class Message>
    bool send_application(const Message& message, FixTime now) noexcept {
        if (!active()) {
            return false;
        }
        this->now_ = now;
        return day_state_.send_application_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(message, header, buffer.data(), buffer.size());
            },
            now);
    }

    void on_frame_before_active(const FixInboundFrame& frame) noexcept {
        if (frame.parse_status != Status::ok) {
            this->request_disconnect();
            return;
        }
        if (frame.message_type == "5") {
            handler_.on_logout(frame.parser->get(58));
            this->request_disconnect();
            return;
        }
        FixFieldReader reader(*frame.parser);
        const std::string_view sender = reader.required_text(49, kMaxCompIdLength);
        const std::string_view target = reader.required_text(56, kMaxCompIdLength);
        reader.required_timestamp(52);
        reader.required_uint(108);
        if (frame.message_type != "A" || !reader.ok() || frame.sequence == 0 || frame.sequence > kMaxMsgSeqNum ||
            sender != day_state_.peer_comp_id() || target != day_state_.own_comp_id()) {
            this->request_disconnect();
            return;
        }
        if (frame.sequence < day_state_.expected_inbound_seq()) {
            std::array<char, 96> text_buffer{};
            this->send_logout_and_disconnect(
                format_sequence_too_low(text_buffer, day_state_.expected_inbound_seq(), frame.sequence));
            return;
        }
        this->enter_active(heartbeat_interval_seconds_);
        this->accept_logon_sequence(frame.sequence);
        handler_.on_logon(FixInboundMessageInfo{frame.sequence, frame.possible_duplicate});
    }

    void on_application_message(const FixInboundFrame& frame) noexcept {
        const FixInboundMessageInfo info{frame.sequence, frame.possible_duplicate};
        Status status = Status::invalid_type;
        if (frame.message_type == "8") {
            fix44::ExecutionReport report;
            status = fix44::decode(*frame.parser, report);
            if (status == Status::ok) {
                handler_.on_execution_report(report, info);
            }
        } else if (frame.message_type == "9") {
            fix44::OrderCancelReject reject;
            status = fix44::decode(*frame.parser, reject);
            if (status == Status::ok) {
                handler_.on_order_cancel_reject(reject, info);
            }
        } else if (frame.message_type == "r") {
            fix44::OrderMassCancelReport report;
            status = fix44::decode(*frame.parser, report);
            if (status == Status::ok) {
                handler_.on_order_mass_cancel_report(report, info);
            }
        } else if (frame.message_type == "j") {
            fix44::BusinessMessageReject reject;
            status = fix44::decode(*frame.parser, reject);
            if constexpr (requires { handler_.on_business_message_reject(reject, info); }) {
                if (status == Status::ok) {
                    handler_.on_business_message_reject(reject, info);
                }
            }
        } else {
            this->send_session_reject(frame.sequence, 35, frame.message_type, SessionRejectReason::InvalidMsgType,
                                      "unsupported MsgType");
            return;
        }
        if (status != Status::ok) {
            this->send_session_reject(frame.sequence, 0, frame.message_type,
                                      status == Status::missing_field ? SessionRejectReason::RequiredTagMissing
                                                                      : SessionRejectReason::ValueIsIncorrect,
                                      "undecodable application message");
        }
    }

    void on_session_reject_received(const FixInboundFrame& frame) noexcept {
        fix44::Reject reject;
        if (fix44::decode(*frame.parser, reject) == Status::ok) {
            handler_.on_session_reject(reject, FixInboundMessageInfo{frame.sequence, frame.possible_duplicate});
        }
    }

    void on_logout_received(std::string_view text) noexcept { handler_.on_logout(text); }

    Handler& handler_;
    std::uint64_t heartbeat_interval_seconds_;
    FixedText<kMaxInboundTextFieldLength> password_;
    FixSessionDayState<Sink> day_state_;
};

}  // namespace exsim::gateway::fix

#endif
