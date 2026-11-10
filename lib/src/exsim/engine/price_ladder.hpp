#ifndef EXSIM_ENGINE_PRICE_LADDER_HPP
#define EXSIM_ENGINE_PRICE_LADDER_HPP

// Instrument configuration and read-only ladder views over exsim::exchange::OrderBook.
// The legacy book owns price levels, occupancy words and FIFO links.

#include <instrument.hpp>
#include <types.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

#include <exchange/order_book.hpp>
#include <memory>
#include <cassert>

namespace exsim::engine {

// Retain the public engine limit of 2^24 ticks per instrument side.
inline constexpr std::uint64_t kMaxLadderTicks = std::uint64_t{1} << 24;

// Read-only views of the order book ladder and occupancy words. Mutations
// go through exsim::exchange::OrderBook, which owns levels, pooled orders and FIFO links.
struct LegacyLevels {
    exsim::exchange::OrderBook* book = nullptr;
    exsim::exchange::Side side{};
    // Construct the empty view at setup; no function-local static guard or
    // first-query initialization can enter the matching path.
    exsim::exchange::PriceLevel empty{0};
    const exsim::exchange::PriceLevel& operator[](std::size_t tick) const noexcept {
        const auto* level = book->price_level(side,static_cast<exsim::exchange::Price>(tick));
        return level ? *level : empty;
    }
};
struct LegacyOccupancy {
    static constexpr std::uint32_t kNotFound = std::numeric_limits<std::uint32_t>::max();
    exsim::exchange::OrderBook* book = nullptr;
    exsim::exchange::Side side{};
    std::uint32_t find_at_or_above(std::uint32_t tick) const noexcept {
        const auto price = book->next_price(side,tick,true);
        return price ? static_cast<std::uint32_t>(*price) : kNotFound;
    }
    std::uint32_t find_at_or_below(std::uint32_t tick) const noexcept {
        const auto price = book->next_price(side,tick,false);
        return price ? static_cast<std::uint32_t>(*price) : kNotFound;
    }
    std::uint64_t tick_word(std::size_t word) const noexcept { return book->active_word(side,word); }
};
struct PriceLadder {
    LegacyLevels levels;
    LegacyOccupancy occupied;
    PriceLadder(exsim::exchange::OrderBook* book, exsim::exchange::Side side) : levels{book,side}, occupied{book,side} {}
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
    std::unique_ptr<exsim::exchange::OrderBook> storage;
    PriceLadder ladders[2];  // indexed by Side: BUY = bids, SELL = asks


    InstrumentBook(const InstrumentConfig& instrument_config, InstrumentId instrument_id, std::uint32_t ticks, std::size_t max_orders)
        : config(instrument_config),
          tick_count(ticks),
          id(instrument_id),
          phase(instrument_config.initial_phase),
          storage(std::make_unique<exsim::exchange::OrderBook>(exsim::exchange::OrderBookConfig{
              max_orders,max_orders,0,static_cast<exsim::exchange::Price>(ticks - 1),1,false})),
          ladders{PriceLadder(storage.get(),exsim::exchange::Side::BUY), PriceLadder(storage.get(),exsim::exchange::Side::SELL)} {}

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

};

}  // namespace exsim::engine

#endif
