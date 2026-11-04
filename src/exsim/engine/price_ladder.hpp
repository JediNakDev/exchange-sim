#ifndef EXSIM_ENGINE_PRICE_LADDER_HPP
#define EXSIM_ENGINE_PRICE_LADDER_HPP

// Tick-indexed price ladders.
//
// Each instrument side owns one Level per tick of its [min_price, max_price]
// band plus a three-level occupancy bitmap (bit per tick, bit per 64-tick
// word, bit per 4096-tick block). Best-price search after the best level
// empties is at most three word scans plus a <= 64-word top-level scan.

#include "fixed_array.hpp"
#include "order_arena.hpp"

#include <exsim/instrument.hpp>
#include <exsim/types.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace exsim::engine {

// Hard cap on ticks per side (16.7M). Keeps tick indices in 24 bits and the
// top bitmap level at <= 64 words.
inline constexpr std::uint64_t kMaxLadderTicks = std::uint64_t{1} << 24;

struct Level {
    std::uint64_t total_qty;  // sum of leaves of the orders at this level
    OrderIndex head;          // oldest order (matched first)
    OrderIndex tail;          // newest order
    std::uint32_t order_count;
};
static_assert(sizeof(Level) == 24);

class OccupancyBitmap {
public:
    static constexpr std::uint32_t kNotFound = std::numeric_limits<std::uint32_t>::max();

    OccupancyBitmap() = default;

    explicit OccupancyBitmap(std::uint32_t bit_count)
        : bit_count_(bit_count),
          tick_words_(words_for(bit_count)),
          word_summary_(words_for(tick_words_.size())),
          block_summary_(words_for(word_summary_.size())) {}

    [[nodiscard]] bool test(std::uint32_t tick) const noexcept {
        return ((tick_words_[tick >> 6] >> (tick & 63)) & 1u) != 0;
    }

    void set(std::uint32_t tick) noexcept {
        tick_words_[tick >> 6] |= bit(tick);
        word_summary_[tick >> 12] |= bit(tick >> 6);
        block_summary_[tick >> 18] |= bit(tick >> 12);
    }

    void clear(std::uint32_t tick) noexcept {
        std::uint64_t& tick_word = tick_words_[tick >> 6];
        tick_word &= ~bit(tick);
        if (tick_word != 0) {
            return;
        }
        std::uint64_t& summary_word = word_summary_[tick >> 12];
        summary_word &= ~bit(tick >> 6);
        if (summary_word != 0) {
            return;
        }
        block_summary_[tick >> 18] &= ~bit(tick >> 12);
    }

    // Lowest set tick >= `tick`, or kNotFound.
    [[nodiscard]] std::uint32_t find_at_or_above(std::uint32_t tick) const noexcept {
        if (tick >= bit_count_) {
            return kNotFound;
        }
        std::size_t word_index = tick >> 6;
        std::uint64_t bits = tick_words_[word_index] & (~std::uint64_t{0} << (tick & 63));
        if (bits != 0) {
            return static_cast<std::uint32_t>((word_index << 6) | lowest_bit(bits));
        }
        const std::size_t first_word_to_search = word_index + 1;
        if (first_word_to_search >= tick_words_.size()) {
            return kNotFound;
        }
        std::size_t summary_index = first_word_to_search >> 6;
        std::uint64_t summary_bits =
            word_summary_[summary_index] & (~std::uint64_t{0} << (first_word_to_search & 63));
        if (summary_bits == 0) {
            const std::size_t first_summary_to_search = summary_index + 1;
            if (first_summary_to_search >= word_summary_.size()) {
                return kNotFound;
            }
            std::size_t block_index = first_summary_to_search >> 6;
            std::uint64_t block_bits =
                block_summary_[block_index] & (~std::uint64_t{0} << (first_summary_to_search & 63));
            while (block_bits == 0) {
                if (++block_index >= block_summary_.size()) {
                    return kNotFound;
                }
                block_bits = block_summary_[block_index];
            }
            summary_index = (block_index << 6) | lowest_bit(block_bits);
            summary_bits = word_summary_[summary_index];
        }
        word_index = (summary_index << 6) | lowest_bit(summary_bits);
        return static_cast<std::uint32_t>((word_index << 6) | lowest_bit(tick_words_[word_index]));
    }

    // Highest set tick <= `tick` (clamped to the ladder), or kNotFound.
    [[nodiscard]] std::uint32_t find_at_or_below(std::uint32_t tick) const noexcept {
        if (bit_count_ == 0) {
            return kNotFound;
        }
        if (tick >= bit_count_) {
            tick = bit_count_ - 1;
        }
        std::size_t word_index = tick >> 6;
        std::uint64_t bits = tick_words_[word_index] & (~std::uint64_t{0} >> (63 - (tick & 63)));
        if (bits != 0) {
            return static_cast<std::uint32_t>((word_index << 6) | highest_bit(bits));
        }
        if (word_index == 0) {
            return kNotFound;
        }
        const std::size_t last_word_to_search = word_index - 1;
        std::size_t summary_index = last_word_to_search >> 6;
        std::uint64_t summary_bits =
            word_summary_[summary_index] & (~std::uint64_t{0} >> (63 - (last_word_to_search & 63)));
        if (summary_bits == 0) {
            if (summary_index == 0) {
                return kNotFound;
            }
            const std::size_t last_summary_to_search = summary_index - 1;
            std::size_t block_index = last_summary_to_search >> 6;
            std::uint64_t block_bits =
                block_summary_[block_index] & (~std::uint64_t{0} >> (63 - (last_summary_to_search & 63)));
            while (block_bits == 0) {
                if (block_index == 0) {
                    return kNotFound;
                }
                block_bits = block_summary_[--block_index];
            }
            summary_index = (block_index << 6) | highest_bit(block_bits);
            summary_bits = word_summary_[summary_index];
        }
        word_index = (summary_index << 6) | highest_bit(summary_bits);
        return static_cast<std::uint32_t>((word_index << 6) | highest_bit(tick_words_[word_index]));
    }

    // Raw 64-tick word (bit i = tick word_index * 64 + i).
    [[nodiscard]] std::uint64_t tick_word(std::size_t word_index) const noexcept { return tick_words_[word_index]; }

private:
    [[nodiscard]] static std::size_t words_for(std::size_t bits) noexcept { return bits == 0 ? 1 : (bits + 63) / 64; }
    [[nodiscard]] static std::uint64_t bit(std::size_t position) noexcept { return std::uint64_t{1} << (position & 63); }
    [[nodiscard]] static unsigned lowest_bit(std::uint64_t bits) noexcept {
        return static_cast<unsigned>(__builtin_ctzll(bits));
    }
    [[nodiscard]] static unsigned highest_bit(std::uint64_t bits) noexcept {
        return 63u - static_cast<unsigned>(__builtin_clzll(bits));
    }

    std::uint32_t bit_count_ = 0;
    FixedArray<std::uint64_t> tick_words_;
    FixedArray<std::uint64_t> word_summary_;
    FixedArray<std::uint64_t> block_summary_;
};

struct PriceLadder {
    FixedArray<Level> levels;
    OccupancyBitmap occupied;

    PriceLadder() = default;
    explicit PriceLadder(std::uint32_t tick_count) : levels(tick_count), occupied(tick_count) {}
};

// Sentinels chosen so that "does tick t cross the opposite best" is a single
// signed comparison even when that side is empty.
inline constexpr std::int32_t kNoBidTick = -1;
inline constexpr std::int32_t kNoAskTick = std::numeric_limits<std::int32_t>::max();

struct InstrumentBook {
    InstrumentConfig config;
    std::uint32_t tick_count = 0;
    InstrumentId id = 0;
    Phase phase = Phase::Closed;
    bool has_last_trade_price = false;
    Price last_trade_price = 0;
    std::int32_t best_bid_tick = kNoBidTick;
    std::int32_t best_ask_tick = kNoAskTick;
    PriceLadder ladders[2];  // indexed by Side: BUY = bids, SELL = asks

    InstrumentBook() = default;
    InstrumentBook(const InstrumentConfig& instrument_config, InstrumentId instrument_id, std::uint32_t ticks)
        : config(instrument_config),
          tick_count(ticks),
          id(instrument_id),
          phase(instrument_config.initial_phase),
          ladders{PriceLadder(ticks), PriceLadder(ticks)} {}

    [[nodiscard]] PriceLadder& ladder(Side side) noexcept { return ladders[static_cast<std::size_t>(side)]; }
    [[nodiscard]] const PriceLadder& ladder(Side side) const noexcept {
        return ladders[static_cast<std::size_t>(side)];
    }

    [[nodiscard]] Price price_of(std::int32_t tick) const noexcept {
        return config.min_price + static_cast<Price>(tick) * config.tick_size;
    }

    // SPEC 2: valid iff min_price <= p <= max_price and p % tick_size == 0
    // (min_price is itself a tick multiple, so p - min_price is checked).
    [[nodiscard]] bool tick_of(Price price, std::int32_t& tick) const noexcept {
        if (price < config.min_price || price > config.max_price) {
            return false;
        }
        const Price offset = price - config.min_price;
        const Price ticks = offset / config.tick_size;
        if (ticks * config.tick_size != offset) {
            return false;
        }
        tick = static_cast<std::int32_t>(ticks);
        return true;
    }

    // SPEC 2: valid iff q > 0, q % lot_size == 0, q <= max_order_qty.
    [[nodiscard]] bool is_valid_quantity(Quantity qty) const noexcept {
        return qty != 0 && qty <= config.max_order_qty && (config.lot_size == 1 || qty % config.lot_size == 0);
    }

    [[nodiscard]] bool is_crossed() const noexcept { return best_bid_tick >= best_ask_tick; }

    // Would a `side` order at `tick` cross the opposite best?
    [[nodiscard]] bool crosses_opposite_best(Side side, std::int32_t tick) const noexcept {
        return side == Side::BUY ? tick >= best_ask_tick : tick <= best_bid_tick;
    }

    [[nodiscard]] bool is_call_phase() const noexcept { return phase == Phase::PreOpen || phase == Phase::PreClose; }
};

}  // namespace exsim::engine

#endif
