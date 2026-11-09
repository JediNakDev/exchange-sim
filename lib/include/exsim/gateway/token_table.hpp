#ifndef EXSIM_GATEWAY_TOKEN_TABLE_HPP
#define EXSIM_GATEWAY_TOKEN_TABLE_HPP

// Per-session interning of client order tokens (OUCH 14-byte tokens, FIX
// ClOrdID strings) into dense engine ClOrdId values 1, 2, 3, ... in
// first-seen order (gateway mapping §1).
//
// * token -> ClOrdId: open-addressing table of (hash tag, id) words, linear
//   probing; a probe touches the entry bytes only on a 32-bit tag match.
// * ClOrdId -> token: direct index into the entry array.
//
// Everything is allocated in the constructor; intern/find never allocate.
// Tokens are never erased during a trading day (both OUCH and FIX require
// day-unique tokens, and outbound messages for dead orders still need the
// original bytes); clear() resets the table at start of day.
//
// Tokens are compared byte-exactly: OUCH tokens keep their space padding so
// the gateway can echo the original 14 bytes back.

#include "exsim/types.hpp"
#include "lob/compiler.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

namespace exsim::gateway {

class TokenTable {
public:
    static constexpr std::size_t kMaxTokenLen = 20;  // FIX ClOrdID limit we accept

    enum class InternStatus : std::uint8_t {
        Inserted,  // new token, new id
        Existing,  // token seen before today, its id returned
        Full,      // capacity exhausted, id = 0
        Invalid,   // empty or longer than kMaxTokenLen, id = 0
    };

    struct InternResult {
        ClOrdId id;
        InternStatus status;
    };

    explicit TokenTable(std::uint32_t max_tokens)
        : max_tokens_(max_tokens == 0 ? 1 : max_tokens) {
        std::size_t cap = 16;
        while (cap < std::size_t{max_tokens_} * 2) {
            cap <<= 1;
        }
        mask_ = cap - 1;
        slots_ = std::make_unique<std::uint64_t[]>(cap);       // zeroed: all empty
        entries_ = std::make_unique<Entry[]>(max_tokens_);     // value-initialised
    }

    TokenTable(const TokenTable&) = delete;
    TokenTable& operator=(const TokenTable&) = delete;
    TokenTable(TokenTable&&) noexcept = default;
    TokenTable& operator=(TokenTable&&) noexcept = default;

    // Returns the id for `token`, assigning the next id if it is new.
    [[nodiscard]] InternResult intern(std::string_view token) noexcept {
        if (LOB_UNLIKELY(token.empty() || token.size() > kMaxTokenLen)) {
            return {0, InternStatus::Invalid};
        }
        const std::uint64_t h = hash(token);
        const std::uint32_t tag = static_cast<std::uint32_t>(h >> 32);
        std::size_t i = static_cast<std::size_t>(h) & mask_;
        for (;; i = (i + 1) & mask_) {
            const std::uint64_t slot = slots_[i];
            if (slot == 0) {
                break;
            }
            if (static_cast<std::uint32_t>(slot >> 32) == tag) {
                const auto id = static_cast<std::uint32_t>(slot);
                if (entries_[id - 1].equals(token)) {
                    return {id, InternStatus::Existing};
                }
            }
        }
        if (LOB_UNLIKELY(size_ == max_tokens_)) {
            return {0, InternStatus::Full};
        }
        const std::uint32_t id = ++size_;
        entries_[id - 1].assign(token);
        slots_[i] = (std::uint64_t{tag} << 32) | id;
        return {id, InternStatus::Inserted};
    }

    // Id of a previously interned token, or 0.
    [[nodiscard]] ClOrdId find(std::string_view token) const noexcept {
        if (LOB_UNLIKELY(token.empty() || token.size() > kMaxTokenLen)) {
            return 0;
        }
        const std::uint64_t h = hash(token);
        const std::uint32_t tag = static_cast<std::uint32_t>(h >> 32);
        for (std::size_t i = static_cast<std::size_t>(h) & mask_;; i = (i + 1) & mask_) {
            const std::uint64_t slot = slots_[i];
            if (slot == 0) {
                return 0;
            }
            if (static_cast<std::uint32_t>(slot >> 32) == tag) {
                const auto id = static_cast<std::uint32_t>(slot);
                if (entries_[id - 1].equals(token)) {
                    return id;
                }
            }
        }
    }

    // Original token bytes for an id, or an empty view if the id is unknown.
    [[nodiscard]] std::string_view token(ClOrdId id) const noexcept {
        if (LOB_UNLIKELY(id == 0 || id > size_)) {
            return {};
        }
        const Entry& e = entries_[id - 1];
        return {e.bytes, e.len};
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t max_tokens() const noexcept { return max_tokens_; }

    // Start of day: forget every token (O(capacity)).
    void clear() noexcept {
        std::memset(slots_.get(), 0, (mask_ + 1) * sizeof(std::uint64_t));
        size_ = 0;
    }

private:
    struct Entry {
        char bytes[kMaxTokenLen];
        std::uint8_t len;

        void assign(std::string_view t) noexcept {
            std::memcpy(bytes, t.data(), t.size());
            len = static_cast<std::uint8_t>(t.size());
        }
        [[nodiscard]] bool equals(std::string_view t) const noexcept {
            return len == t.size() && std::memcmp(bytes, t.data(), t.size()) == 0;
        }
    };

    // Up to 20 bytes: three overlapping-free 8-byte lanes (zero padded),
    // multiply-xorshift mixed, finished with the murmur3 fmix64 avalanche.
    [[nodiscard]] static std::uint64_t hash(std::string_view t) noexcept {
        std::uint64_t lanes[3] = {0, 0, 0};
        std::memcpy(lanes, t.data(), t.size());
        std::uint64_t h = 0x9E3779B97F4A7C15ull ^ (t.size() * 0xFF51AFD7ED558CCDull);
        h ^= lanes[0] * 0xC2B2AE3D27D4EB4Full;
        h = (h << 31) | (h >> 33);
        h ^= lanes[1] * 0x165667B19E3779F9ull;
        h = (h << 27) | (h >> 37);
        h ^= lanes[2] * 0x27D4EB2F165667C5ull;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ull;
        h ^= h >> 33;
        return h;
    }

    std::uint32_t max_tokens_;
    std::uint32_t size_ = 0;
    std::size_t mask_ = 0;
    std::unique_ptr<std::uint64_t[]> slots_;  // (tag << 32) | id, 0 = empty
    std::unique_ptr<Entry[]> entries_;        // index id - 1
};

}  // namespace exsim::gateway

#endif
