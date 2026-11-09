#ifndef EXSIM_GATEWAY_FIX_FIX_SESSION_LAYER_HPP
#define EXSIM_GATEWAY_FIX_FIX_SESSION_LAYER_HPP

// FIX 4.4 session layer shared by FixConnection (exchange side) and FixClient,
// implemented once as a CRTP base (no virtual dispatch). It owns one
// connection's state: phase, heartbeat and TestRequest timers, ResendRequest
// recovery, and a bounded queue of early (high MsgSeqNum) messages. The
// day-lifetime counters and store live in FixSessionDayState.
//
// Inbound rules (gateway mapping section 3):
//   * Garbled input (bad prefix, BodyLength, CheckSum, missing MsgType, or
//     MsgType not the third field) is discarded without touching sequence
//     numbers; the stream resynchronises at the next "8=FIX.4.4<SOH>9=".
//   * MsgSeqNum 0 or above kMaxMsgSeqNum: Logout and disconnect (the expected
//     sequence never wraps).
//   * MsgSeqNum below expected without PossDupFlag: Logout and disconnect.
//     With PossDupFlag: ignored, including a GapFill (FIX 4.4), so a held
//     application message is never discarded by a duplicate.
//   * PossDupFlag=Y in order: OrigSendingTime(122) is required (Reject 373=1)
//     and must not be later than SendingTime (Reject 373=10, then Logout).
//   * MsgSeqNum above expected: one ResendRequest(expected, 0) is sent and the
//     message is held (or discarded when the queue is full, the open-ended
//     resend covers it). Held messages run only when their turn comes, so an
//     application message never runs before its predecessors. A held
//     ResendRequest, TestRequest or matching Heartbeat is serviced on arrival
//     and only consumes its MsgSeqNum later.
//   * SequenceReset in reset mode (123=N or absent) ignores MsgSeqNum:
//     decreasing NewSeqNo gets Reject(3) 373=5, increasing NewSeqNo advances
//     expected. A GapFillFlag(123) other than Y or N gets Reject(3) 373=5 and
//     leaves the sequence untouched.
//   * Parseable semantic errors consume their in-order MsgSeqNum and get
//     Reject(3) with 371/372/373.
//
// The derived class provides:
//   void on_frame_before_active(const FixInboundFrame&);   Logon exchange
//   void on_application_message(const FixInboundFrame&);   in-order business message
//   void on_session_reject_received(const FixInboundFrame&);
//   void on_logout_received(std::string_view text);

#include "exsim/gateway/fix/fix_common.hpp"
#include "exsim/gateway/fix/fix_session_day_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace exsim::gateway::fix {

enum class FixConnectionPhase : std::uint8_t {
    Idle,           // no connection
    AwaitingLogon,  // exchange side: connected, Logon not yet accepted
    LogonSent,      // client side: Logon sent, response not yet received
    Active,
    LogoutSent,     // we initiated Logout and wait for the reply
    Disconnecting,  // the owner must close the transport and call on_disconnected
};

struct FixSessionCounters {
    std::uint64_t garbled_frames = 0;
    std::uint64_t session_rejects_sent = 0;
    std::uint64_t session_rejects_received = 0;
    std::uint64_t duplicates_suppressed = 0;
    std::uint64_t messages_held = 0;
    std::uint64_t held_messages_discarded = 0;
    std::uint64_t resend_requests_sent = 0;
    std::uint64_t resend_requests_serviced = 0;
    std::uint64_t test_requests_sent = 0;
    std::uint64_t heartbeats_sent = 0;
};

// A well-framed inbound message. parser is valid only when parse_status is ok;
// otherwise sequence, message_type and possible_duplicate come from a raw scan.
struct FixInboundFrame {
    std::span<const char> bytes{};
    const FixParser* parser = nullptr;
    Status parse_status = Status::ok;
    std::uint64_t sequence = 0;
    std::string_view message_type{};
    bool possible_duplicate = false;
};

// Fixed-capacity store of early messages, keyed by MsgSeqNum.
class FixHeldMessageQueue {
public:
    enum class HoldResult : std::uint8_t { Held, AlreadyHeld, Full };

    FixHeldMessageQueue(std::size_t slot_count, std::size_t max_message_bytes)
        : slot_count_(slot_count == 0 ? 1 : slot_count),
          max_message_bytes_(max_message_bytes),
          arena_(std::make_unique<char[]>(slot_count_ * max_message_bytes_)),
          slots_(std::make_unique<Slot[]>(slot_count_)) {}

    HoldResult hold(std::uint64_t sequence, std::span<const char> frame, bool already_serviced) noexcept {
        std::size_t free_slot = slot_count_;
        for (std::size_t index = 0; index < slot_count_; ++index) {
            if (slots_[index].in_use && slots_[index].sequence == sequence) {
                return HoldResult::AlreadyHeld;
            }
            if (!slots_[index].in_use && free_slot == slot_count_) {
                free_slot = index;
            }
        }
        if (free_slot == slot_count_ || frame.size() > max_message_bytes_) {
            return HoldResult::Full;
        }
        Slot& slot = slots_[free_slot];
        if (!frame.empty()) {
            std::memcpy(arena_.get() + free_slot * max_message_bytes_, frame.data(), frame.size());
        }
        slot = {sequence, frame.size(), already_serviced, true};
        ++size_;
        return HoldResult::Held;
    }

    // Slot index holding `sequence`, or -1.
    [[nodiscard]] long find(std::uint64_t sequence) const noexcept {
        for (std::size_t index = 0; index < slot_count_; ++index) {
            if (slots_[index].in_use && slots_[index].sequence == sequence) {
                return static_cast<long>(index);
            }
        }
        return -1;
    }
    [[nodiscard]] std::span<const char> frame(long slot) const noexcept {
        const auto index = static_cast<std::size_t>(slot);
        return {arena_.get() + index * max_message_bytes_, slots_[index].length};
    }
    [[nodiscard]] bool already_serviced(long slot) const noexcept {
        return slots_[static_cast<std::size_t>(slot)].already_serviced;
    }
    [[nodiscard]] std::uint64_t sequence(long slot) const noexcept {
        return slots_[static_cast<std::size_t>(slot)].sequence;
    }
    void release(long slot) noexcept {
        slots_[static_cast<std::size_t>(slot)].in_use = false;
        --size_;
    }
    void release_below(std::uint64_t sequence) noexcept {
        for (std::size_t index = 0; index < slot_count_; ++index) {
            if (slots_[index].in_use && slots_[index].sequence < sequence) {
                slots_[index].in_use = false;
                --size_;
            }
        }
    }
    void clear() noexcept {
        for (std::size_t index = 0; index < slot_count_; ++index) {
            slots_[index].in_use = false;
        }
        size_ = 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    struct Slot {
        std::uint64_t sequence;
        std::size_t length;
        bool already_serviced;
        bool in_use;
    };

    std::size_t slot_count_;
    std::size_t max_message_bytes_;
    std::size_t size_ = 0;
    std::unique_ptr<char[]> arena_;
    std::unique_ptr<Slot[]> slots_;
};

template <class Derived, FixByteSink Sink>
class FixSessionLayer {
public:
    [[nodiscard]] FixConnectionPhase phase() const noexcept { return phase_; }
    // True when the transport should be closed (after flushing what was sent).
    [[nodiscard]] bool wants_disconnect() const noexcept {
        return phase_ == FixConnectionPhase::Disconnecting ||
               (day_ != nullptr && sink_bound_to_day_ && (day_->output_overflowed() || day_->store_exhausted()));
    }
    [[nodiscard]] const FixSessionCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] Timestamp heartbeat_interval_ns() const noexcept { return heartbeat_interval_ns_; }
    [[nodiscard]] bool test_request_outstanding() const noexcept { return test_request_outstanding_; }
    [[nodiscard]] bool resend_request_outstanding() const noexcept { return resend_request_outstanding_; }
    [[nodiscard]] std::size_t held_message_count() const noexcept { return held_messages_.size(); }

protected:
    explicit FixSessionLayer(const FixSessionSettings& settings)
        : settings_(settings),
          held_messages_(settings.held_inbound_message_slots, settings.max_inbound_message_bytes) {}

    FixSessionLayer(const FixSessionLayer&) = delete;
    FixSessionLayer& operator=(const FixSessionLayer&) = delete;

    Derived& derived() noexcept { return static_cast<Derived&>(*this); }

    // Starts a new transport connection. day state is attached separately.
    void begin_connection(Sink& sink, FixConnectionPhase initial_phase) noexcept {
        sink_ = &sink;
        phase_ = initial_phase;
        connection_started_ns_ = now_.monotonic_ns;
        last_inbound_ns_ = now_.monotonic_ns;
        heartbeat_interval_ns_ = 0;
        test_request_outstanding_ = false;
        resend_request_outstanding_ = false;
        recovery_target_seq_ = 0;
        held_messages_.clear();
    }

    void attach_day_state(FixSessionDayState<Sink>& day) noexcept {
        day_ = &day;
        day.bind_sink(*sink_, now_.monotonic_ns);
        sink_bound_to_day_ = true;
    }

    void end_connection() noexcept {
        if (day_ != nullptr && sink_bound_to_day_ && sink_ != nullptr) {
            day_->unbind_sink(*sink_);
        }
        sink_bound_to_day_ = false;
        day_ = nullptr;
        sink_ = nullptr;
        phase_ = FixConnectionPhase::Idle;
        held_messages_.clear();
    }

    // True while the day state still transmits through this connection's sink,
    // that is no newer connection of the same user has bound since.
    [[nodiscard]] bool day_state_bound_to_this_connection() const noexcept {
        return day_ != nullptr && sink_ != nullptr && sink_bound_to_day_ && day_->is_bound_to(*sink_);
    }

    void enter_active(std::uint64_t heartbeat_interval_seconds) noexcept {
        phase_ = FixConnectionPhase::Active;
        heartbeat_interval_ns_ = heartbeat_interval_seconds * kNanosecondsPerSecond;
        last_inbound_ns_ = now_.monotonic_ns;
    }

    // Consumes complete frames from `bytes`; returns the bytes consumed (a
    // partial trailing frame is left for the next call).
    std::size_t consume_inbound(std::span<const char> bytes) noexcept {
        std::size_t position = 0;
        while (position < bytes.size() && !wants_disconnect() &&
               phase_ != FixConnectionPhase::Idle) {
            const std::span<const char> rest = bytes.subspan(position);
            const std::size_t frame_start = find_frame_start(rest, 0);
            if (frame_start != 0) {
                ++counters_.garbled_frames;
                position += frame_start;
                continue;
            }
            std::size_t frame_length = 0;
            const Status framing = fix44::frame_size(rest, frame_length, settings_.max_inbound_message_bytes);
            if (framing == Status::incomplete) {
                const std::size_t embedded = find_embedded_frame_start(rest);
                if (embedded == 0) {
                    break;
                }
                ++counters_.garbled_frames;
                position += embedded;
                continue;
            }
            if (framing != Status::ok || !handle_well_framed(rest.first(frame_length))) {
                ++counters_.garbled_frames;
                position += find_frame_start(rest, 1);
                continue;
            }
            position += frame_length;
        }
        return position;
    }

    void run_timers() noexcept {
        const Timestamp now_ns = now_.monotonic_ns;
        if (day_state_bound_to_this_connection()) {
            day_->continue_resend(now_);  // a paused resend resumes as the transport drains
        }
        switch (phase_) {
            case FixConnectionPhase::AwaitingLogon:
            case FixConnectionPhase::LogonSent:
                if (now_ns - connection_started_ns_ >= settings_.logon_timeout_ns) {
                    request_disconnect();
                }
                return;
            case FixConnectionPhase::LogoutSent:
                if (now_ns - logout_sent_ns_ >= settings_.logout_timeout_ns) {
                    request_disconnect();
                }
                return;
            case FixConnectionPhase::Active:
                break;
            default:
                return;
        }
        if (heartbeat_interval_ns_ != 0) {
            if (test_request_outstanding_) {
                if (now_ns - test_request_sent_ns_ >= heartbeat_interval_ns_ / 2) {
                    send_logout_and_disconnect("heartbeat timeout");
                    return;
                }
            } else if (now_ns - last_inbound_ns_ >= heartbeat_interval_ns_ * 3 / 2) {
                send_test_request();
            }
            // A resend in progress is the traffic; a Heartbeat now would only
            // be withheld and gap-filled.
            if (!day_->resend_in_progress() && now_ns - day_->last_transmit_ns() >= heartbeat_interval_ns_) {
                send_heartbeat({});
            }
        }
        if (resend_request_outstanding_ && now_ns - resend_request_sent_ns_ >= settings_.resend_request_retry_ns) {
            if (day_->expected_inbound_seq() == expected_seq_when_resend_requested_) {
                send_resend_request();
            } else {
                expected_seq_when_resend_requested_ = day_->expected_inbound_seq();
                resend_request_sent_ns_ = now_ns;
            }
        }
    }

    // Initiates the Logout handshake; the peer's Logout completes it.
    bool start_logout(std::string_view text) noexcept {
        if (phase_ != FixConnectionPhase::Active) {
            return false;
        }
        send_logout(text);
        phase_ = FixConnectionPhase::LogoutSent;
        logout_sent_ns_ = now_.monotonic_ns;
        return true;
    }

    void request_disconnect() noexcept { phase_ = FixConnectionPhase::Disconnecting; }

    void send_logout(std::string_view text) noexcept {
        fix44::Logout logout;
        logout.has_text = !text.empty();
        logout.text.borrow(text);
        day_->send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(logout, header, buffer.data(), buffer.size());
            },
            now_);
    }

    void send_logout_and_disconnect(std::string_view text) noexcept {
        send_logout(text);
        request_disconnect();
    }

    void send_session_reject(std::uint64_t reference_sequence, unsigned reference_tag,
                             std::string_view reference_message_type, SessionRejectReason reason,
                             std::string_view text) noexcept {
        fix44::Reject reject;
        reject.reference_sequence = reference_sequence;
        reject.reference_tag = reference_tag;
        reject.reference_type.borrow(reference_message_type.empty() ? std::string_view("?")
                                                                    : reference_message_type);
        reject.reason = static_cast<std::uint64_t>(reason);
        reject.text.borrow(text.empty() ? std::string_view("rejected") : text);
        if (day_->send_session_message(
                [&](const fix44::Header& header, std::span<char> buffer) {
                    return fix44::encode(reject, header, buffer.data(), buffer.size());
                },
                now_)) {
            ++counters_.session_rejects_sent;
        }
    }

    void send_session_reject(const FixInboundFrame& frame, const FieldProblem& problem) noexcept {
        send_session_reject(frame.sequence, problem.tag, frame.message_type, problem.reason, problem.description);
    }

    // Writes a frame straight to the connection's sink (no day state yet).
    void write_unsequenced(std::span<const char> frame) noexcept {
        if (sink_ == nullptr) {
            return;
        }
        if (char* destination = sink_->prepare(frame.size())) {
            std::memcpy(destination, frame.data(), frame.size());
            sink_->commit(frame.size());
        }
    }

    // The Logon (or Logon response) was accepted with MsgSeqNum `sequence`.
    // Equal to expected: consume it. Higher: hold a placeholder and recover.
    void accept_logon_sequence(std::uint64_t sequence) noexcept {
        const std::uint64_t expected = day_->expected_inbound_seq();
        if (sequence == expected) {
            day_->set_expected_inbound_seq(sequence + 1);
            return;
        }
        recovery_target_seq_ = sequence;
        held_messages_.hold(sequence, {}, true);
        request_resend_for_gap();
    }

    [[nodiscard]] std::string_view header_field(const FixInboundFrame& frame, unsigned tag) const noexcept {
        return frame.parse_status == Status::ok ? frame.parser->get(tag) : raw_field_value(frame.bytes, tag);
    }

    FixSessionSettings settings_;
    FixSessionDayState<Sink>* day_ = nullptr;
    Sink* sink_ = nullptr;
    FixTime now_{};
    FixConnectionPhase phase_ = FixConnectionPhase::Idle;
    FixSessionCounters counters_{};
    FixParser parser_{};

private:
    // Returns false when the frame is garbled (bad CheckSum or BodyLength, or
    // no MsgType); such frames never touch sequence numbers.
    bool handle_well_framed(std::span<const char> bytes) noexcept {
        FixInboundFrame frame;
        if (!classify(bytes, frame)) {
            return false;
        }
        last_inbound_ns_ = now_.monotonic_ns;
        if (phase_ == FixConnectionPhase::AwaitingLogon || phase_ == FixConnectionPhase::LogonSent) {
            derived().on_frame_before_active(frame);
            return true;
        }
        if (frame.sequence == 0) {
            send_logout_and_disconnect("MsgSeqNum(34) missing or invalid");
            return true;
        }
        if (frame.sequence > kMaxMsgSeqNum) {
            send_logout_and_disconnect("MsgSeqNum(34) out of range");
            return true;
        }
        handle_sequenced(frame);
        return true;
    }

    bool classify(std::span<const char> bytes, FixInboundFrame& frame) noexcept {
        const Status status = parser_.parse(bytes);
        if (status == Status::checksum || status == Status::invalid_length || !msg_type_is_third_field(bytes)) {
            return false;
        }
        frame.bytes = bytes;
        frame.parser = &parser_;
        frame.parse_status = status;
        frame.message_type = header_field(frame, 35);
        if (frame.message_type.empty()) {
            return false;
        }
        if (!fix44::parse_uint(header_field(frame, 34), frame.sequence)) {
            frame.sequence = 0;
        }
        frame.possible_duplicate = header_field(frame, 43) == "Y";
        return true;
    }

    void handle_sequenced(const FixInboundFrame& frame) noexcept {
        if (header_field(frame, 49) != day_->peer_comp_id() || header_field(frame, 56) != day_->own_comp_id()) {
            const bool sender_wrong = header_field(frame, 49) != day_->peer_comp_id();
            if (frame.sequence == day_->expected_inbound_seq()) {
                day_->set_expected_inbound_seq(frame.sequence + 1);
            }
            send_session_reject(frame.sequence, sender_wrong ? 49u : 56u, frame.message_type,
                                SessionRejectReason::CompIdProblem, "CompID problem");
            send_logout_and_disconnect("CompID problem");
            return;
        }
        if (frame.message_type == "4" && frame.parse_status == Status::ok) {
            const std::string_view gap_fill_flag = frame.parser->get(123);
            if (frame.parser->has(123) && gap_fill_flag != "Y" && gap_fill_flag != "N") {
                send_session_reject(frame.sequence, 123, frame.message_type, SessionRejectReason::ValueIsIncorrect,
                                    "GapFillFlag(123) must be Y or N");
                return;
            }
            if (gap_fill_flag != "Y") {
                handle_sequence_reset(frame);
                return;
            }
        }
        const std::uint64_t expected = day_->expected_inbound_seq();
        if (frame.sequence < expected) {
            handle_low_sequence(frame, expected);
            return;
        }
        if (frame.sequence > expected) {
            handle_high_sequence(frame);
            return;
        }
        process_in_order(frame);
        drain_held_messages();
    }

    // A low PossDup message, a GapFill included, is ignored (FIX 4.4): letting
    // a duplicate GapFill advance expected could discard held application
    // messages that really were received.
    void handle_low_sequence(const FixInboundFrame& frame, std::uint64_t expected) noexcept {
        if (!frame.possible_duplicate) {
            std::array<char, 96> text_buffer{};
            send_logout_and_disconnect(format_sequence_too_low(text_buffer, expected, frame.sequence));
            return;
        }
        ++counters_.duplicates_suppressed;
    }

    void handle_high_sequence(const FixInboundFrame& frame) noexcept {
        if (frame.sequence > recovery_target_seq_) {
            recovery_target_seq_ = frame.sequence;
        }
        if (frame.parse_status == Status::ok) {
            if (frame.message_type == "5") {
                derived().on_logout_received(frame.parser->get(58));
                if (phase_ != FixConnectionPhase::LogoutSent) {
                    send_logout({});
                }
                request_disconnect();
                return;
            }
            if (frame.message_type == "A") {
                send_logout_and_disconnect("unexpected Logon");
                return;
            }
        }
        if (held_messages_.find(frame.sequence) >= 0) {
            // A repeat of a held message is not serviced again.
            ++counters_.duplicates_suppressed;
            request_resend_for_gap();
            return;
        }
        const bool serviced = frame.parse_status == Status::ok && service_early(frame);
        switch (held_messages_.hold(frame.sequence, frame.bytes, serviced)) {
            case FixHeldMessageQueue::HoldResult::Held:
                ++counters_.messages_held;
                break;
            case FixHeldMessageQueue::HoldResult::Full:
                ++counters_.held_messages_discarded;
                break;
            case FixHeldMessageQueue::HoldResult::AlreadyHeld:
                ++counters_.duplicates_suppressed;
                break;
        }
        request_resend_for_gap();
    }

    // ResendRequest, TestRequest and Heartbeat are answered on arrival even
    // when early, so both sides can recover at the same time.
    bool service_early(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        if (frame.message_type == "2") {
            const std::uint64_t begin = reader.required_uint(7);
            const std::uint64_t end = reader.required_uint(16);
            if (!reader.ok() || begin == 0 || (end != 0 && end < begin)) {
                return false;
            }
            ++counters_.resend_requests_serviced;
            day_->resend(begin, end, now_);
            return true;
        }
        if (frame.message_type == "1") {
            const std::string_view test_request_id = reader.required_text(112);
            if (!reader.ok()) {
                return false;
            }
            send_heartbeat(test_request_id);
            return true;
        }
        if (frame.message_type == "0") {
            const std::string_view test_request_id = reader.optional_text(112);
            if (!reader.ok()) {
                return false;
            }
            clear_probe_if_matching(test_request_id);
            return true;
        }
        return false;
    }

    void process_in_order(const FixInboundFrame& frame) noexcept {
        if (frame.parse_status != Status::ok) {
            day_->set_expected_inbound_seq(frame.sequence + 1);
            if (frame.parse_status == Status::duplicate_field) {
                send_session_reject(frame.sequence, find_duplicate_tag(frame.bytes), frame.message_type,
                                    SessionRejectReason::TagAppearsMoreThanOnce, "tag appears more than once");
            } else if (frame.parse_status == Status::full) {
                send_session_reject(frame.sequence, 0, frame.message_type, SessionRejectReason::Other,
                                    "too many fields");
            } else {
                send_session_reject(frame.sequence, 0, frame.message_type, SessionRejectReason::InvalidTagNumber,
                                    "invalid tag number");
            }
            return;
        }
        FixFieldReader reader(*frame.parser);
        if (frame.message_type == "4") {
            const std::uint64_t new_sequence = reader.required_uint(36);
            if (!reader.ok()) {
                day_->set_expected_inbound_seq(frame.sequence + 1);
                send_session_reject(frame, reader.problem());
            } else if (new_sequence <= frame.sequence) {
                day_->set_expected_inbound_seq(frame.sequence + 1);
                send_session_reject(frame.sequence, 36, frame.message_type, SessionRejectReason::ValueIsIncorrect,
                                    "NewSeqNo must exceed MsgSeqNum");
            } else {
                day_->set_expected_inbound_seq(new_sequence);
            }
            return;
        }
        day_->set_expected_inbound_seq(frame.sequence + 1);
        const std::uint64_t sending_time = reader.required_timestamp(52);
        if (!reader.ok()) {
            send_session_reject(frame, reader.problem());
            return;
        }
        if (frame.possible_duplicate && !validate_original_sending_time(frame, reader, sending_time)) {
            return;
        }
        if (frame.message_type.size() != 1) {
            derived().on_application_message(frame);
            return;
        }
        switch (frame.message_type[0]) {
            case '0': {
                const std::string_view test_request_id = reader.optional_text(112);
                if (!reader.ok()) {
                    send_session_reject(frame, reader.problem());
                    return;
                }
                clear_probe_if_matching(test_request_id);
                return;
            }
            case '1': {
                const std::string_view test_request_id = reader.required_text(112);
                if (!reader.ok()) {
                    send_session_reject(frame, reader.problem());
                    return;
                }
                send_heartbeat(test_request_id);
                return;
            }
            case '2': {
                const std::uint64_t begin = reader.required_uint(7);
                const std::uint64_t end = reader.required_uint(16);
                if (!reader.ok()) {
                    send_session_reject(frame, reader.problem());
                } else if (begin == 0) {
                    send_session_reject(frame.sequence, 7, frame.message_type, SessionRejectReason::ValueIsIncorrect,
                                        "BeginSeqNo must be positive");
                } else if (end != 0 && end < begin) {
                    send_session_reject(frame.sequence, 16, frame.message_type,
                                        SessionRejectReason::ValueIsIncorrect, "EndSeqNo below BeginSeqNo");
                } else {
                    ++counters_.resend_requests_serviced;
                    day_->resend(begin, end, now_);
                }
                return;
            }
            case '3':
                ++counters_.session_rejects_received;
                derived().on_session_reject_received(frame);
                return;
            case '5':
                derived().on_logout_received(frame.parser->get(58));
                if (phase_ != FixConnectionPhase::LogoutSent) {
                    send_logout({});
                }
                request_disconnect();
                return;
            case 'A':
                send_logout_and_disconnect("unexpected Logon");
                return;
            default:
                derived().on_application_message(frame);
                return;
        }
    }

    // PossDupFlag=Y: OrigSendingTime(122) must be present (else Reject 373=1,
    // the message is ignored) and not later than SendingTime (else Reject
    // 373=10 and Logout). Returns true when the message may be processed.
    bool validate_original_sending_time(const FixInboundFrame& frame, FixFieldReader& reader,
                                        std::uint64_t sending_time) noexcept {
        const std::uint64_t original_sending_time = reader.required_timestamp(122);
        if (!reader.ok()) {
            send_session_reject(frame, reader.problem());
            return false;
        }
        if (original_sending_time > sending_time) {
            send_session_reject(frame.sequence, 52, frame.message_type,
                                SessionRejectReason::SendingTimeAccuracyProblem,
                                "OrigSendingTime(122) later than SendingTime(52)");
            send_logout_and_disconnect("SendingTime accuracy problem");
            return false;
        }
        return true;
    }

    // SequenceReset-Reset (123=N): MsgSeqNum is ignored.
    void handle_sequence_reset(const FixInboundFrame& frame) noexcept {
        FixFieldReader reader(*frame.parser);
        const std::uint64_t new_sequence = reader.required_uint(36);
        const std::uint64_t expected = day_->expected_inbound_seq();
        if (!reader.ok()) {
            send_session_reject(frame, reader.problem());
            return;
        }
        if (new_sequence < expected) {
            send_session_reject(frame.sequence, 36, frame.message_type, SessionRejectReason::ValueIsIncorrect,
                                "attempt to lower sequence number");
            return;
        }
        if (new_sequence > expected) {
            day_->set_expected_inbound_seq(new_sequence);
            drain_held_messages();
        }
    }

    // Runs held messages whose turn has come.
    void drain_held_messages() noexcept {
        while (!held_messages_.empty() && !wants_disconnect()) {
            const std::uint64_t expected = day_->expected_inbound_seq();
            held_messages_.release_below(expected);
            const long slot = held_messages_.find(expected);
            if (slot < 0) {
                break;
            }
            if (held_messages_.already_serviced(slot)) {
                day_->set_expected_inbound_seq(expected + 1);
                held_messages_.release(slot);
                continue;
            }
            FixInboundFrame held_frame;
            if (classify(held_messages_.frame(slot), held_frame)) {
                process_in_order(held_frame);
            }
            held_messages_.release(slot);
        }
        if (day_->expected_inbound_seq() > recovery_target_seq_) {
            resend_request_outstanding_ = false;
        }
    }

    void request_resend_for_gap() noexcept {
        if (!resend_request_outstanding_) {
            send_resend_request();
        }
    }

    void send_resend_request() noexcept {
        fix44::ResendRequest request;
        request.begin_sequence = day_->expected_inbound_seq();
        request.end_sequence = 0;
        day_->send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(request, header, buffer.data(), buffer.size());
            },
            now_);
        ++counters_.resend_requests_sent;
        resend_request_outstanding_ = true;
        resend_request_sent_ns_ = now_.monotonic_ns;
        expected_seq_when_resend_requested_ = day_->expected_inbound_seq();
    }

    void send_heartbeat(std::string_view test_request_id) noexcept {
        fix44::Heartbeat heartbeat;
        heartbeat.has_test_request_id = !test_request_id.empty();
        heartbeat.test_request_id.borrow(test_request_id);
        day_->send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(heartbeat, header, buffer.data(), buffer.size());
            },
            now_);
        ++counters_.heartbeats_sent;
    }

    void send_test_request() noexcept {
        char identifier[24] = "TEST";
        const std::size_t digits = fix44::format_uint(++test_request_counter_, identifier + 4, 20);
        test_request_id_.assign(std::string_view(identifier, 4 + digits));
        fix44::TestRequest request;
        request.test_request_id.borrow(test_request_id_.view());
        day_->send_session_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(request, header, buffer.data(), buffer.size());
            },
            now_);
        ++counters_.test_requests_sent;
        test_request_outstanding_ = true;
        test_request_sent_ns_ = now_.monotonic_ns;
    }

    void clear_probe_if_matching(std::string_view test_request_id) noexcept {
        if (test_request_outstanding_ && !test_request_id.empty() && test_request_id == test_request_id_.view()) {
            test_request_outstanding_ = false;
        }
    }

    FixHeldMessageQueue held_messages_;
    bool sink_bound_to_day_ = false;
    Timestamp connection_started_ns_ = 0;
    Timestamp last_inbound_ns_ = 0;
    Timestamp logout_sent_ns_ = 0;
    Timestamp heartbeat_interval_ns_ = 0;
    bool test_request_outstanding_ = false;
    Timestamp test_request_sent_ns_ = 0;
    std::uint64_t test_request_counter_ = 0;
    FixedText<24> test_request_id_;
    bool resend_request_outstanding_ = false;
    Timestamp resend_request_sent_ns_ = 0;
    std::uint64_t expected_seq_when_resend_requested_ = 0;
    std::uint64_t recovery_target_seq_ = 0;
};

}  // namespace exsim::gateway::fix

#endif
