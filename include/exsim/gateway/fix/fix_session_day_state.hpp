#ifndef EXSIM_GATEWAY_FIX_FIX_SESSION_DAY_STATE_HPP
#define EXSIM_GATEWAY_FIX_FIX_SESSION_DAY_STATE_HPP

// Day-lifetime FIX session state shared by the exchange side (inside
// FixUserState) and the client side (inside FixClient): CompIDs, both
// MsgSeqNum counters, the outbound message store used to answer
// ResendRequest(2), and the binding to the current connection's byte sink.
//
// Every sequenced outbound message gets the next MsgSeqNum. Only application
// messages take a store slot: they are stored in full with their SendingTime
// as metadata (for OrigSendingTime on resend), and a parallel array records
// the MsgSeqNum of each slot. Session messages (Logon, Heartbeat, TestRequest,
// ResendRequest, Reject, Logout) are not stored, so a resend gap-fills every
// sequence number the store does not hold and session traffic can never
// exhaust the store. When the store is exhausted the session fails closed.
// Recovery requires an explicit authenticated sequence reset or a new day;
// application reports are never gap-filled. The inbound expected MsgSeqNum
// lives here too: FIX sequence continuity spans reconnects for the whole day.

#include "exsim/gateway/fix/fix_common.hpp"
#include "exsim/gateway/sequenced_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace exsim::gateway::fix {

template <FixByteSink Sink>
class FixSessionDayState {
public:
    FixSessionDayState(std::string_view own_comp_id, std::string_view peer_comp_id,
                       std::size_t store_max_messages, std::size_t store_max_bytes)
        : store_(store_max_messages, store_max_bytes),
          sequence_by_store_slot_(std::make_unique<std::uint64_t[]>(store_max_messages + 1)) {
        own_comp_id_.assign(own_comp_id.substr(0, kMaxCompIdLength));
        peer_comp_id_.assign(peer_comp_id.substr(0, kMaxCompIdLength));
    }

    FixSessionDayState(const FixSessionDayState&) = delete;
    FixSessionDayState& operator=(const FixSessionDayState&) = delete;

    [[nodiscard]] std::string_view own_comp_id() const noexcept { return own_comp_id_.view(); }
    [[nodiscard]] std::string_view peer_comp_id() const noexcept { return peer_comp_id_.view(); }

    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return next_outbound_seq_; }
    [[nodiscard]] std::uint64_t expected_inbound_seq() const noexcept { return expected_inbound_seq_; }
    void set_expected_inbound_seq(std::uint64_t sequence) noexcept { expected_inbound_seq_ = sequence; }

    // ResetSeqNumFlag(141)=Y: both directions restart at 1 and the store is
    // emptied (messages before the reset can no longer be resent).
    void reset_sequence_numbers() noexcept {
        next_outbound_seq_ = 1;
        expected_inbound_seq_ = 1;
        store_.clear();
        store_exhausted_ = false;
    }

    void bind_sink(Sink& sink, Timestamp now_ns) noexcept {
        sink_ = &sink;
        output_overflowed_ = false;
        last_transmit_ns_ = now_ns;
    }
    // Clears the binding only when `sink` is the bound one: a stale connection
    // that is torn down after a newer connection bound must not silence it.
    void unbind_sink(const Sink& sink) noexcept {
        if (sink_ == &sink) {
            sink_ = nullptr;
        }
    }
    [[nodiscard]] bool has_sink() const noexcept { return sink_ != nullptr; }
    [[nodiscard]] bool is_bound_to(const Sink& sink) const noexcept { return sink_ == &sink; }
    // prepare() returned nullptr: the connection must be dropped (docs/SERVER.md 2.4).
    [[nodiscard]] bool output_overflowed() const noexcept { return output_overflowed_; }
    [[nodiscard]] bool store_exhausted() const noexcept { return store_exhausted_; }
    [[nodiscard]] Timestamp last_transmit_ns() const noexcept { return last_transmit_ns_; }

    // Fault injection `drop_out <n>`: the next n application messages are
    // stored but not transmitted.
    void suppress_application_transmissions(std::uint32_t count) noexcept {
        suppressed_application_transmissions_ = count;
    }

    // The application messages kept for resend, in MsgSeqNum order. A slot
    // index is not a MsgSeqNum; use stored_message() to look one up.
    [[nodiscard]] const SequencedStore& store() const noexcept { return store_; }
    [[nodiscard]] std::uint64_t stored_message_count() const noexcept { return store_.last_seq(); }
    // Stored application message with MsgSeqNum `sequence`; empty bytes when
    // that sequence number was a session message or was never sent.
    [[nodiscard]] SequencedStore::Message stored_message(std::uint64_t sequence) const noexcept {
        const std::uint64_t slot = first_slot_at_or_after(sequence);
        if (slot > store_.last_seq() || sequence_by_store_slot_[slot] != sequence) {
            return {};
        }
        return store_.get(slot);
    }
    [[nodiscard]] std::uint64_t messages_not_stored() const noexcept { return messages_not_stored_; }
    [[nodiscard]] std::uint64_t transmissions_suppressed() const noexcept { return transmissions_suppressed_; }

    // Encoder: std::size_t(const fix44::Header&, std::span<char> buffer),
    // returning the frame size or 0 on failure. Returns false when the message
    // could not be encoded (no MsgSeqNum is consumed then).
    template <class Encoder>
    bool send_session_message(Encoder&& encoder, FixTime now) noexcept {
        return send_sequenced(encoder, now, false);
    }
    template <class Encoder>
    bool send_application_message(Encoder&& encoder, FixTime now) noexcept {
        return send_sequenced(encoder, now, true);
    }

    // Writes a frame without assigning a MsgSeqNum or storing it (resends,
    // gap fills, a Logout answering a refused Logon).
    bool transmit(std::span<const char> frame, Timestamp now_ns) noexcept {
        if (sink_ == nullptr || output_overflowed_) {
            return false;
        }
        char* destination = sink_->prepare(frame.size());
        if (destination == nullptr) {
            output_overflowed_ = true;
            return false;
        }
        std::memcpy(destination, frame.data(), frame.size());
        sink_->commit(frame.size());
        last_transmit_ns_ = now_ns;
        return true;
    }

    // Answers ResendRequest(begin, end): stored application messages go out
    // again with PossDupFlag=Y and their original MsgSeqNum; every run of
    // sequence numbers the store does not hold (session messages) becomes one
    // SequenceReset-GapFill. end == 0 or beyond the last sent means "through
    // the last sent message". begin must be >= 1.
    void resend(std::uint64_t begin, std::uint64_t end, FixTime now) noexcept {
        const std::uint64_t last_sent = next_outbound_seq_ - 1;
        if (end == 0 || end > last_sent) {
            end = last_sent;
        }
        if (begin > end) {
            return;
        }
        std::uint64_t next_unsent = begin;
        for (std::uint64_t slot = first_slot_at_or_after(begin);
             slot <= store_.last_seq() && sequence_by_store_slot_[slot] <= end && !output_overflowed_; ++slot) {
            const std::uint64_t stored_sequence = sequence_by_store_slot_[slot];
            if (stored_sequence > next_unsent) {
                send_gap_fill(next_unsent, stored_sequence, now);
            }
            const SequencedStore::Message stored = store_.get(slot);
            const std::size_t size =
                write_possible_duplicate(stored.bytes, stored.meta, now.realtime_ns, resend_buffer_);
            if (size == 0) {
                output_overflowed_ = true;
                return;
            }
            transmit({resend_buffer_.data(), size}, now.monotonic_ns);
            next_unsent = stored_sequence + 1;
        }
        if (next_unsent <= end && !output_overflowed_) {
            send_gap_fill(next_unsent, end + 1, now);
        }
    }

private:
    template <class Encoder>
    bool send_sequenced(Encoder& encoder, FixTime now, bool is_application) noexcept {
        if (store_exhausted_) {
            ++messages_not_stored_;
            return false;
        }
        const fix44::Header header{own_comp_id_.view(), peer_comp_id_.view(), next_outbound_seq_,
                                   now.realtime_ns};
        const std::size_t size = encoder(header, std::span<char>(encode_buffer_));
        if (size == 0) {
            return false;
        }
        const std::uint64_t sequence = next_outbound_seq_;
        const std::span<const char> frame(encode_buffer_.data(), size);
        if (is_application) {
            if (!store_.append(frame, now.realtime_ns)) {
                store_exhausted_ = true;
                ++messages_not_stored_;
                return false;
            }
            sequence_by_store_slot_[store_.last_seq()] = sequence;
        }
        ++next_outbound_seq_;
        if (is_application && suppressed_application_transmissions_ > 0) {
            --suppressed_application_transmissions_;
            ++transmissions_suppressed_;
            return true;
        }
        transmit(frame, now.monotonic_ns);
        return true;
    }

    // Lowest store slot whose MsgSeqNum is >= `sequence`, or last_seq() + 1.
    // Slots hold strictly increasing MsgSeqNums (binary search).
    [[nodiscard]] std::uint64_t first_slot_at_or_after(std::uint64_t sequence) const noexcept {
        std::uint64_t low = 1;
        std::uint64_t high = store_.last_seq() + 1;
        while (low < high) {
            const std::uint64_t middle = low + (high - low) / 2;
            if (sequence_by_store_slot_[middle] < sequence) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low;
    }

    void send_gap_fill(std::uint64_t sequence, std::uint64_t new_sequence, FixTime now) noexcept {
        fix44::Builder writer(resend_buffer_);
        writer.field(35, '4');
        writer.field(49, own_comp_id_.view());
        writer.field(56, peer_comp_id_.view());
        writer.uint_field(34, sequence);
        writer.field(43, 'Y');
        writer.timestamp_field(52, now.realtime_ns);
        writer.timestamp_field(122, now.realtime_ns);
        writer.field(123, 'Y');
        writer.uint_field(36, new_sequence);
        const std::size_t size = writer.finish();
        if (size != 0) {
            transmit({resend_buffer_.data(), size}, now.monotonic_ns);
        }
    }

    FixedText<kMaxCompIdLength> own_comp_id_;
    FixedText<kMaxCompIdLength> peer_comp_id_;
    std::uint64_t next_outbound_seq_ = 1;
    std::uint64_t expected_inbound_seq_ = 1;
    SequencedStore store_;
    std::unique_ptr<std::uint64_t[]> sequence_by_store_slot_;  // [slot] = MsgSeqNum, slots 1..store_.last_seq()
    bool store_exhausted_ = false;
    std::uint64_t messages_not_stored_ = 0;
    Sink* sink_ = nullptr;
    bool output_overflowed_ = false;
    Timestamp last_transmit_ns_ = 0;
    std::uint32_t suppressed_application_transmissions_ = 0;
    std::uint64_t transmissions_suppressed_ = 0;
    std::array<char, kMaxOutboundMessageBytes> encode_buffer_{};
    std::array<char, kMaxResentMessageBytes> resend_buffer_{};
};

}  // namespace exsim::gateway::fix

#endif
