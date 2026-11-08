#ifndef EXSIM_MD_RECEIVER_HPP
#define EXSIM_MD_RECEIVER_HPP

// MoldUdp64Receiver: client-side MoldUDP64 sequencer. Feed it every datagram
// from the multicast feed AND from the retransmission socket; it delivers
// each message exactly once, in sequence order, to the handler.
//
//   * duplicates (sequence already delivered or already buffered) are dropped;
//   * messages ahead of the next expected sequence are buffered in a fixed
//     reorder window (reorder_window_messages slots of
//     max_buffered_message_size bytes); messages beyond the window or larger
//     than a slot are dropped and recovered later through the gap logic;
//   * a gap is any known sequence (from data, heartbeat or end-of-session
//     packets) that is missing: the receiver sends one request for the
//     missing run (capped at max_request_messages) through RequestSink, and
//     re-sends it every request_timeout_ns while no progress is made;
//   * after max_request_attempts unanswered requests for the same run the run
//     is declared lost: handler.on_messages_lost(first, count) (optional) and
//     delivery continues after it. A book built from the feed is then stale;
//   * End of Session (count 0xFFFF) completes once every message before its
//     sequence has been delivered or declared lost:
//     handler.on_end_of_session(next_sequence) (optional), once.
//
// Handler: `void on_message(std::uint64_t sequence, std::span<const char> message)`
//          (the span is valid only during the call), plus the optional
//          callbacks above. ItchBookBuilder and TypedItchHandler satisfy it.
// RequestSink: callable `sink(std::span<const char> request)` that sends the
//          20-byte MoldUDP64 request to the retransmission server.
//
// on_packet and poll never allocate. Time is any monotonic nanosecond clock.

#include <exsim/md/feed_common.hpp>
#include <exsim/protocol/moldudp64.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

namespace exsim::md {

struct ReceiverConfig {
    std::string_view session = {};                 // empty: adopt the first packet's session
    std::uint64_t first_sequence = 1;              // 0: start at the first packet seen (late join)
    std::size_t reorder_window_messages = 1u << 14;
    std::size_t max_buffered_message_size = 64;    // ITCH messages are at most 50 bytes
    std::uint16_t max_request_messages = 1024;
    std::uint64_t request_timeout_ns = 100'000'000;
    std::uint32_t max_request_attempts = 10;
};

enum class PacketStatus : std::uint8_t {
    Accepted = 0,
    Malformed,     // MoldUDP64 framing invalid, or sequence 0 / overflowing
    WrongSession,  // another session's packet: ignored
};

struct ReceiverStats {
    std::uint64_t packets = 0;
    std::uint64_t malformed_packets = 0;
    std::uint64_t wrong_session_packets = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t end_of_session_packets = 0;
    std::uint64_t messages_delivered = 0;
    std::uint64_t duplicate_messages = 0;
    std::uint64_t messages_buffered = 0;
    std::uint64_t messages_dropped_outside_window = 0;
    std::uint64_t requests_sent = 0;   // including retries
    std::uint64_t request_retries = 0;
    std::uint64_t messages_lost = 0;   // declared lost after the retry limit
};

template <class Handler, class RequestSink>
class MoldUdp64Receiver {
public:
    // Setup only: allocates the reorder window.
    MoldUdp64Receiver(const ReceiverConfig& config, Handler& handler, RequestSink request_sink)
        : handler_(handler),
          request_sink_(std::move(request_sink)),
          window_(checked_window(config.reorder_window_messages)),
          slot_size_(checked_slot_size(config.max_buffered_message_size)),
          max_request_messages_(config.max_request_messages == 0 ? 1 : config.max_request_messages),
          request_timeout_ns_(config.request_timeout_ns),
          max_request_attempts_(config.max_request_attempts == 0 ? 1 : config.max_request_attempts),
          buffered_sequences_(std::make_unique<std::uint64_t[]>(window_)),
          buffered_lengths_(std::make_unique<std::uint16_t[]>(window_)),
          buffered_bytes_(std::make_unique<char[]>(window_ * slot_size_)),
          next_sequence_(config.first_sequence),
          started_(config.first_sequence != 0),
          session_known_(!config.session.empty()) {
        if (!make_mold_session_name(config.session, session_name_))
            throw std::invalid_argument("MoldUDP64 session longer than 10 characters");
    }

    MoldUdp64Receiver(const MoldUdp64Receiver&) = delete;
    MoldUdp64Receiver& operator=(const MoldUdp64Receiver&) = delete;

    PacketStatus on_packet(std::span<const char> datagram, std::uint64_t now_ns) {
        ++stats_.packets;
        protocol::moldudp64::PacketReader reader;
        if (reader.reset(datagram) != protocol::Status::ok || reader.header.sequence == 0 ||
            reader.header.sequence > std::numeric_limits<std::uint64_t>::max() - kMoldEndOfSessionCount) {
            ++stats_.malformed_packets;
            return PacketStatus::Malformed;
        }
        const protocol::moldudp64::Request& header = reader.header;
        if (!session_known_) {
            session_name_ = header.session;
            session_known_ = true;
        } else if (header.session != session_name_) {
            ++stats_.wrong_session_packets;
            return PacketStatus::WrongSession;
        }
        if (!started_) {
            next_sequence_ = header.sequence;
            started_ = true;
        }
        if (header.count == kMoldEndOfSessionCount) {
            ++stats_.end_of_session_packets;
            end_of_session_seen_ = true;
            end_of_session_sequence_ = header.sequence;
            raise_known_end(header.sequence);
        } else if (header.count == 0) {
            ++stats_.heartbeats;
            raise_known_end(header.sequence);
        } else {
            std::span<const char> message;
            for (std::uint64_t sequence = header.sequence; reader.next(message) == protocol::Status::ok; ++sequence)
                accept_message(sequence, message);
            raise_known_end(header.sequence + header.count);
        }
        if (request_.active && next_sequence_ > request_.first_sequence) request_.active = false;  // progress
        update_requests(now_ns);
        report_end_of_session();
        return PacketStatus::Accepted;
    }

    // Timer: retries (or declares lost) an unanswered request.
    void poll(std::uint64_t now_ns) {
        update_requests(now_ns);
        report_end_of_session();
    }

    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    // One past the highest sequence the feed has announced so far.
    [[nodiscard]] std::uint64_t known_end_sequence() const noexcept { return known_end_sequence_; }
    [[nodiscard]] bool gap_open() const noexcept { return started_ && next_sequence_ < known_end_sequence_; }
    [[nodiscard]] bool request_outstanding() const noexcept { return request_.active; }
    [[nodiscard]] bool end_of_session_seen() const noexcept { return end_of_session_seen_; }
    [[nodiscard]] bool end_of_session_reached() const noexcept { return end_of_session_reported_; }
    [[nodiscard]] std::size_t buffered_messages() const noexcept { return buffered_count_; }
    [[nodiscard]] const ReceiverStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const MoldSessionName& session_name() const noexcept { return session_name_; }
    [[nodiscard]] RequestSink& request_sink() noexcept { return request_sink_; }

private:
    struct OutstandingRequest {
        bool active = false;
        std::uint64_t first_sequence = 0;
        std::uint64_t sent_at_ns = 0;
        std::uint32_t attempts = 0;
    };

    static std::size_t checked_window(std::size_t window) {
        if (window == 0 || window > std::numeric_limits<std::size_t>::max() / 65535)
            throw std::invalid_argument("reorder window out of range");
        return window;
    }
    static std::size_t checked_slot_size(std::size_t slot_size) {
        if (slot_size == 0 || slot_size > 65535) throw std::invalid_argument("buffered message size out of range");
        return slot_size;
    }

    void raise_known_end(std::uint64_t end_sequence) noexcept {
        if (end_sequence > known_end_sequence_) known_end_sequence_ = end_sequence;
    }

    void accept_message(std::uint64_t sequence, std::span<const char> message) {
        if (sequence < next_sequence_) {
            ++stats_.duplicate_messages;
            return;
        }
        if (sequence == next_sequence_) {
            deliver(sequence, message);
            ++next_sequence_;
            drain_buffer();
            return;
        }
        if (sequence - next_sequence_ >= window_ || message.size() > slot_size_) {
            ++stats_.messages_dropped_outside_window;
            return;
        }
        const std::size_t slot = static_cast<std::size_t>(sequence % window_);
        if (buffered_sequences_[slot] == sequence) {
            ++stats_.duplicate_messages;
            return;
        }
        // Only one sequence inside [next, next + window) maps to a slot and
        // delivered slots are cleared, so the slot is free here.
        buffered_sequences_[slot] = sequence;
        buffered_lengths_[slot] = static_cast<std::uint16_t>(message.size());
        std::memcpy(buffered_bytes_.get() + slot * slot_size_, message.data(), message.size());
        ++buffered_count_;
        ++stats_.messages_buffered;
    }

    void deliver(std::uint64_t sequence, std::span<const char> message) {
        ++stats_.messages_delivered;
        handler_.on_message(sequence, message);
    }

    void drain_buffer() {
        while (buffered_count_ != 0) {
            const std::size_t slot = static_cast<std::size_t>(next_sequence_ % window_);
            if (buffered_sequences_[slot] != next_sequence_) return;
            buffered_sequences_[slot] = 0;
            --buffered_count_;
            deliver(next_sequence_,
                    std::span<const char>(buffered_bytes_.get() + slot * slot_size_, buffered_lengths_[slot]));
            ++next_sequence_;
        }
    }

    // End (exclusive) of the missing run starting at next_sequence_: the first
    // buffered sequence or the known end, capped at max_request_messages.
    std::uint64_t missing_run_end() const noexcept {
        std::uint64_t end = known_end_sequence_;
        if (end - next_sequence_ > max_request_messages_) end = next_sequence_ + max_request_messages_;
        if (buffered_count_ == 0) return end;
        for (std::uint64_t sequence = next_sequence_ + 1; sequence < end; ++sequence) {
            if (sequence - next_sequence_ >= window_) break;
            if (buffered_sequences_[static_cast<std::size_t>(sequence % window_)] == sequence) return sequence;
        }
        return end;
    }

    void update_requests(std::uint64_t now_ns) {
        while (gap_open()) {
            if (!request_.active) {
                send_request(now_ns, 1);
                return;
            }
            if (now_ns < request_.sent_at_ns || now_ns - request_.sent_at_ns < request_timeout_ns_) return;
            if (request_.attempts < max_request_attempts_) {
                ++stats_.request_retries;
                send_request(now_ns, request_.attempts + 1);
                return;
            }
            declare_missing_run_lost();
        }
        request_.active = false;
    }

    void send_request(std::uint64_t now_ns, std::uint32_t attempts) {
        protocol::moldudp64::Request request;
        request.session = session_name_;
        request.sequence = next_sequence_;
        request.count = static_cast<std::uint16_t>(missing_run_end() - next_sequence_);
        char bytes[kMoldRequestSize];
        (void)protocol::moldudp64::encode(request, bytes, sizeof bytes);
        request_ = {true, next_sequence_, now_ns, attempts};
        ++stats_.requests_sent;
        request_sink_(std::span<const char>(bytes, sizeof bytes));
    }

    void declare_missing_run_lost() {
        const std::uint64_t first = next_sequence_;
        const std::uint64_t end = missing_run_end();
        stats_.messages_lost += end - first;
        next_sequence_ = end;
        request_.active = false;
        if constexpr (requires { handler_.on_messages_lost(first, end - first); }) {
            handler_.on_messages_lost(first, end - first);
        }
        drain_buffer();
    }

    void report_end_of_session() {
        if (!end_of_session_seen_ || end_of_session_reported_ || next_sequence_ < end_of_session_sequence_) return;
        end_of_session_reported_ = true;
        if constexpr (requires { handler_.on_end_of_session(next_sequence_); }) {
            handler_.on_end_of_session(next_sequence_);
        }
    }

    Handler& handler_;
    RequestSink request_sink_;
    std::size_t window_;
    std::size_t slot_size_;
    std::uint64_t max_request_messages_;
    std::uint64_t request_timeout_ns_;
    std::uint32_t max_request_attempts_;
    std::unique_ptr<std::uint64_t[]> buffered_sequences_;  // 0 = free slot
    std::unique_ptr<std::uint16_t[]> buffered_lengths_;
    std::unique_ptr<char[]> buffered_bytes_;
    std::size_t buffered_count_ = 0;
    MoldSessionName session_name_{};
    std::uint64_t next_sequence_;
    std::uint64_t known_end_sequence_ = 0;
    std::uint64_t end_of_session_sequence_ = 0;
    OutstandingRequest request_{};
    ReceiverStats stats_{};
    bool started_;
    bool session_known_;
    bool end_of_session_seen_ = false;
    bool end_of_session_reported_ = false;
};

}  // namespace exsim::md

#endif
