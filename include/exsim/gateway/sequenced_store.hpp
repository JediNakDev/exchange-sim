#ifndef EXSIM_GATEWAY_SEQUENCED_STORE_HPP
#define EXSIM_GATEWAY_SEQUENCED_STORE_HPP

// Append-only, pre-allocated store of a session's outbound application
// messages for the trading day (docs/GATEWAYS.md §1). Message `seq` (1-based)
// is retrievable in O(1) so a reconnecting client can be replayed from any
// sequence number (SoupBinTCP login, FIX ResendRequest).
//
// Storage: one contiguous byte arena plus an end-offset array and a 64-bit
// per-message metadata word (e.g. original SendingTime for FIX PossDup
// resends). Sizes are fixed at construction and the memory is pre-faulted;
// append() never allocates and fails (returning false, store unchanged) when
// either limit is reached, so the gateway can apply its overflow policy.

#include "lob/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace exsim::gateway {

class SequencedStore {
public:
    struct Message {
        std::span<const char> bytes;  // empty if seq is out of range
        std::uint64_t meta = 0;
    };

    SequencedStore(std::size_t max_messages, std::size_t max_bytes)
        : max_messages_(max_messages),
          max_bytes_(max_bytes),
          arena_(std::make_unique<char[]>(max_bytes == 0 ? 1 : max_bytes)),
          ends_(std::make_unique<std::uint64_t[]>(max_messages + 1)),
          meta_(std::make_unique<std::uint64_t[]>(max_messages + 1)) {
        // make_unique<T[]> value-initialises, which writes every page: the
        // arena is resident before the first message arrives.
        ends_[0] = 0;
    }

    SequencedStore(const SequencedStore&) = delete;
    SequencedStore& operator=(const SequencedStore&) = delete;
    SequencedStore(SequencedStore&&) noexcept = default;
    SequencedStore& operator=(SequencedStore&&) noexcept = default;

    // Stores `msg` as sequence number next_seq(). False if full.
    [[nodiscard]] bool append(std::span<const char> msg, std::uint64_t meta = 0) noexcept {
        const std::uint64_t used = ends_[count_];
        if (LOB_UNLIKELY(count_ == max_messages_ || msg.size() > max_bytes_ - used)) {
            return false;
        }
        if (!msg.empty()) {
            std::memcpy(arena_.get() + used, msg.data(), msg.size());
        }
        ++count_;
        ends_[count_] = used + msg.size();
        meta_[count_] = meta;
        return true;
    }

    // Message with sequence number `seq` in [1, last_seq()].
    [[nodiscard]] Message get(std::uint64_t seq) const noexcept {
        if (LOB_UNLIKELY(seq == 0 || seq > count_)) {
            return {};
        }
        const std::uint64_t begin = ends_[seq - 1];
        return {{arena_.get() + begin, static_cast<std::size_t>(ends_[seq] - begin)}, meta_[seq]};
    }

    [[nodiscard]] std::uint64_t last_seq() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return count_ + 1; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return static_cast<std::size_t>(ends_[count_]); }
    [[nodiscard]] std::size_t max_messages() const noexcept { return max_messages_; }
    [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

    // Start of day.
    void clear() noexcept { count_ = 0; }

private:
    std::size_t max_messages_;
    std::size_t max_bytes_;
    std::uint64_t count_ = 0;
    std::unique_ptr<char[]> arena_;
    std::unique_ptr<std::uint64_t[]> ends_;  // ends_[s] = end offset of message s; ends_[0] = 0
    std::unique_ptr<std::uint64_t[]> meta_;
};

}  // namespace exsim::gateway

#endif
