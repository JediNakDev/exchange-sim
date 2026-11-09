#ifndef EXSIM_MD_RETRANSMISSION_RING_HPP
#define EXSIM_MD_RETRANSMISSION_RING_HPP

// Pre-allocated ring of the most recent published ITCH messages, each with
// its MoldUDP64 sequence number. The publisher appends every message it
// sequences; the retransmission server answers requests from it. Fixed
// 64-byte slots, so append and lookup never allocate.

#include <md/feed_common.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>

namespace exsim::md {

class RetransmissionRing {
public:
    // Setup only: allocates `capacity_messages` slots (>= 1).
    explicit RetransmissionRing(std::size_t capacity_messages)
        : capacity_(capacity_messages), slots_(make_slots(capacity_messages)) {}

    RetransmissionRing(const RetransmissionRing&) = delete;
    RetransmissionRing& operator=(const RetransmissionRing&) = delete;

    // Stores `message` under `sequence`, evicting the oldest message once the
    // ring is full. Sequences must be consecutive and start at >= 1; messages
    // must be 1..kMaxItchMessageSize bytes. Returns false (and stores nothing)
    // when either rule is broken.
    [[nodiscard]] bool append(std::uint64_t sequence, std::span<const char> message) noexcept {
        if (sequence == 0 || message.empty() || message.size() > kMaxItchMessageSize) return false;
        if (newest_sequence_ != 0 && sequence != newest_sequence_ + 1) return false;
        Slot& slot = slots_[sequence % capacity_];
        slot.sequence = sequence;
        slot.length = static_cast<std::uint8_t>(message.size());
        std::memcpy(slot.bytes, message.data(), message.size());
        if (first_sequence_ == 0) first_sequence_ = sequence;
        newest_sequence_ = sequence;
        return true;
    }

    // The stored bytes of `sequence`, or an empty span when it is not held.
    [[nodiscard]] std::span<const char> find(std::uint64_t sequence) const noexcept {
        if (sequence == 0) return {};
        const Slot& slot = slots_[sequence % capacity_];
        if (slot.sequence != sequence) return {};
        return {slot.bytes, slot.length};
    }

    // Oldest and newest held sequence numbers; both 0 while empty.
    [[nodiscard]] std::uint64_t oldest_sequence() const noexcept {
        if (newest_sequence_ == 0) return 0;
        const std::uint64_t held = newest_sequence_ - first_sequence_ + 1;
        return held > capacity_ ? newest_sequence_ - capacity_ + 1 : first_sequence_;
    }
    [[nodiscard]] std::uint64_t newest_sequence() const noexcept { return newest_sequence_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Slot {
        std::uint64_t sequence = 0;  // 0 = never written
        std::uint8_t length = 0;
        char bytes[kMaxItchMessageSize] = {};
    };
    static_assert(sizeof(Slot) == 64, "one cache line per retained message");

    static std::unique_ptr<Slot[]> make_slots(std::size_t capacity_messages) {
        if (capacity_messages == 0) throw std::invalid_argument("RetransmissionRing capacity must be >= 1");
        return std::make_unique<Slot[]>(capacity_messages);
    }

    std::size_t capacity_;
    std::unique_ptr<Slot[]> slots_;
    std::uint64_t first_sequence_ = 0;
    std::uint64_t newest_sequence_ = 0;
};

}  // namespace exsim::md

#endif
