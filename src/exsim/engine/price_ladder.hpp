#ifndef EXSIM_ENGINE_PRICE_LADDER_HPP
#define EXSIM_ENGINE_PRICE_LADDER_HPP

// One side of one instrument's book: the price levels kept in a single
// contiguous array sorted so that the BEST level is at the END (the layout
// from David Gross, "When Nanoseconds Matter", CppCon 2024).
//
// Almost all activity happens a few ticks from the touch, so a price is found
// by scanning backwards from the best level; matching consumes the last
// element and an emptied best level is dropped by decrementing the size.
// Inserting or erasing a level memmoves only the (usually short) tail that
// lies between it and the touch. Memory is proportional to the number of
// distinct live prices, not to the width of the price band.
//
// Both sides share one code path by sorting on a "ladder key" that grows
// with aggressiveness: key = price for bids, key = -price for asks. Keys are
// ascending, so the best level always has the largest key.
//
// The keys live in their own array, separate from the level payloads, so the
// search loop streams 8-byte keys (8 per cache line) instead of striding
// over payloads.

#include <exsim/types.hpp>

#include "order_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

namespace exsim::engine {

struct PriceLevel {
    std::uint64_t total_qty;  // sum of leaves of the orders in the FIFO
    OrderSlot head;           // oldest order (next to trade)
    OrderSlot tail;           // newest order
};
static_assert(sizeof(PriceLevel) == 16, "four levels per cache line");

[[nodiscard]] constexpr Price ladder_key(Side side, Price price) noexcept {
    return side == Side::BUY ? price : -price;
}

[[nodiscard]] constexpr Price price_from_ladder_key(Side side, Price key) noexcept {
    return side == Side::BUY ? key : -key;
}

class PriceLadder {
public:
    static constexpr std::uint32_t kNotFound = UINT32_MAX;

    // Setup time only: allocates and pre-faults room for `max_levels` levels.
    void reserve(std::uint32_t max_levels) {
        const std::size_t allocated = std::max<std::uint32_t>(max_levels, 1);
        keys_.reset(new Price[allocated]());
        levels_.reset(new PriceLevel[allocated]());
        capacity_ = max_levels;
        size_ = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] Price best_key() const noexcept { return keys_[size_ - 1]; }
    [[nodiscard]] PriceLevel& best_level() noexcept { return levels_[size_ - 1]; }

    [[nodiscard]] Price key_at(std::uint32_t index) const noexcept { return keys_[index]; }
    [[nodiscard]] PriceLevel& level_at(std::uint32_t index) noexcept { return levels_[index]; }
    [[nodiscard]] const PriceLevel& level_at(std::uint32_t index) const noexcept { return levels_[index]; }

    // Number of keys <= `key`: the index right after the last level that is
    // not better than `key`. Scans backwards from the best level; when the
    // key is deeper than kLinearScanLevels it falls back to binary search.
    [[nodiscard]] std::uint32_t count_not_better_than(Price key) const noexcept {
        std::uint32_t index = size_;
        const std::uint32_t linear_floor = size_ > kLinearScanLevels ? size_ - kLinearScanLevels : 0;
        while (index > linear_floor && keys_[index - 1] > key) {
            --index;
        }
        if (index == linear_floor && linear_floor != 0 && keys_[index - 1] > key) {
            index = static_cast<std::uint32_t>(std::upper_bound(keys_.get(), keys_.get() + linear_floor, key) -
                                               keys_.get());
        }
        return index;
    }

    // Number of keys < `key` (first index whose key is >= `key`).
    [[nodiscard]] std::uint32_t count_worse_than(Price key) const noexcept {
        return static_cast<std::uint32_t>(std::lower_bound(keys_.get(), keys_.get() + size_, key) - keys_.get());
    }

    [[nodiscard]] std::uint32_t find(Price key) const noexcept {
        const std::uint32_t after = count_not_better_than(key);
        return (after != 0 && keys_[after - 1] == key) ? after - 1 : kNotFound;
    }

    // Opens a new level at `index` (from count_not_better_than), shifting the
    // better levels up by one. Caller guarantees size() < capacity().
    PriceLevel& insert_level(std::uint32_t index, Price key) noexcept {
        const std::uint32_t shifted = size_ - index;
        if (shifted != 0) {
            std::memmove(&keys_[index + 1], &keys_[index], shifted * sizeof(Price));
            std::memmove(&levels_[index + 1], &levels_[index], shifted * sizeof(PriceLevel));
        }
        keys_[index] = key;
        ++size_;
        return levels_[index];
    }

    void erase_level(std::uint32_t index) noexcept {
        const std::uint32_t shifted = size_ - index - 1;
        if (shifted != 0) {
            std::memmove(&keys_[index], &keys_[index + 1], shifted * sizeof(Price));
            std::memmove(&levels_[index], &levels_[index + 1], shifted * sizeof(PriceLevel));
        }
        --size_;
    }

    void pop_best() noexcept { --size_; }

private:
    static constexpr std::uint32_t kLinearScanLevels = 64;

    std::unique_ptr<Price[]> keys_;
    std::unique_ptr<PriceLevel[]> levels_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;
};

}  // namespace exsim::engine

#endif
