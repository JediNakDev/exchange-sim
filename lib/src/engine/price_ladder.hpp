#ifndef EXSIM_ENGINE_PRICE_LADDER_HPP
#define EXSIM_ENGINE_PRICE_LADDER_HPP

// Per-instrument engine state around one exsim::exchange::OrderBook. The book
// stores prices as ticks (0 .. tick_count - 1) and owns price levels,
// occupancy bitmaps and FIFO links; orders, levels and IDs come from the
// engine's shared OrderBookStorage.

#include <instrument.hpp>
#include <types.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <exchange/order_book.hpp>

namespace exsim::engine {

// Retain the public engine limit of 2^24 ticks per instrument side.
inline constexpr std::uint64_t kMaxLadderTicks = std::uint64_t{1} << 24;

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
    std::unique_ptr<exsim::exchange::OrderBook> order_book;

    InstrumentBook(const InstrumentConfig& instrument_config, InstrumentId instrument_id, std::uint32_t ticks,
                   std::shared_ptr<exsim::exchange::OrderBookStorage> shared_order_storage)
        : config(instrument_config),
          tick_count(ticks),
          id(instrument_id),
          phase(instrument_config.initial_phase) {
        exsim::exchange::OrderBookConfig book_config;
        book_config.min_price = 0;
        book_config.max_price = static_cast<exsim::exchange::Price>(ticks - 1);
        book_config.max_fills_per_add_order = 1;  // the engine never calls add_order
        book_config.allow_growth = false;
        order_book = std::make_unique<exsim::exchange::OrderBook>(book_config, std::move(shared_order_storage));
    }

    [[nodiscard]] Price price_of(std::int32_t tick) const noexcept {
        return config.min_price + static_cast<Price>(tick) * config.tick_size;
    }

    // Valid iff min_price <= p <= max_price and p % tick_size == 0
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

    // Valid iff q > 0, q % lot_size == 0, q <= max_order_qty.
    [[nodiscard]] bool is_valid_quantity(Quantity qty) const noexcept {
        return qty != 0 && qty <= config.max_order_qty && (config.lot_size == 1 || qty % config.lot_size == 0);
    }

    [[nodiscard]] bool is_crossed() const noexcept { return best_bid_tick >= best_ask_tick; }

    // Would a `side` order at `tick` cross the opposite best?
    [[nodiscard]] bool crosses_opposite_best(Side side, std::int32_t tick) const noexcept {
        return side == Side::BUY ? tick >= best_ask_tick : tick <= best_bid_tick;
    }

};

}  // namespace exsim::engine

#endif
