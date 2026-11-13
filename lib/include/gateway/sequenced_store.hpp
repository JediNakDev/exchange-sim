#ifndef EXSIM_GATEWAY_SEQUENCED_STORE_HPP
#define EXSIM_GATEWAY_SEQUENCED_STORE_HPP

// Append-only, pre-allocated store of a session's outbound application
// messages for the trading day. Message `seq` (1-based) is retrievable in
// O(1) so a reconnecting client can be replayed from any sequence number
// (SoupBinTCP login, FIX ResendRequest).
//
// Storage: one contiguous byte arena plus one entry per message holding its
// end offset next to its 64-bit metadata word (e.g. original SendingTime for
// FIX PossDup resends), so a lookup reads two adjacent entries. Sizes are
// fixed at construction and the memory is pre-faulted; append() never
// allocates and fails (returning false, store unchanged) when
// either limit is reached, so the gateway can apply its overflow policy.

#include "exchange/compiler.hpp"

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
          entries_(std::make_unique<Entry[]>(max_messages + 1)) {
        // make_unique<T[]> value-initialises, which writes every page: the
        // arena is resident before the first message arrives.
        entries_[0].end = 0;
    }

    SequencedStore(const SequencedStore&) = delete;
    SequencedStore& operator=(const SequencedStore&) = delete;
    SequencedStore(SequencedStore&&) noexcept = default;
    SequencedStore& operator=(SequencedStore&&) noexcept = default;

    // Stores `msg` as sequence number next_seq(). False if full.
    [[nodiscard]] bool append(std::span<const char> msg, std::uint64_t meta = 0) noexcept {
        const std::uint64_t used = entries_[count_].end;
        if (EXSIM_EXCHANGE_UNLIKELY(count_ == max_messages_ || msg.size() > max_bytes_ - used)) {
            return false;
        }
        if (!msg.empty()) {
            std::memcpy(arena_.get() + used, msg.data(), msg.size());
        }
        ++count_;
        entries_[count_].end = used + msg.size();
        entries_[count_].meta = meta;
        return true;
    }

    // Message with sequence number `seq` in [1, last_seq()].
    [[nodiscard]] Message get(std::uint64_t seq) const noexcept {
        if (EXSIM_EXCHANGE_UNLIKELY(seq == 0 || seq > count_)) {
            return {};
        }
        const std::uint64_t begin = entries_[seq - 1].end;
        return {{arena_.get() + begin, static_cast<std::size_t>(entries_[seq].end - begin)}, entries_[seq].meta};
    }

    [[nodiscard]] std::uint64_t last_seq() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return count_ + 1; }
    [[nodiscard]] std::size_t bytes_used() const noexcept { return static_cast<std::size_t>(entries_[count_].end); }
    [[nodiscard]] std::size_t max_messages() const noexcept { return max_messages_; }
    [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

    // Start of day.
    void clear() noexcept { count_ = 0; }

private:
    std::size_t max_messages_;
    std::size_t max_bytes_;
    std::uint64_t count_ = 0;
    std::unique_ptr<char[]> arena_;
    struct Entry {
        std::uint64_t end = 0;  // end offset of the message; entries_[0].end = 0
        std::uint64_t meta = 0;
    };
    std::unique_ptr<Entry[]> entries_;  // index = seq
};

}  // namespace exsim::gateway

#endif
