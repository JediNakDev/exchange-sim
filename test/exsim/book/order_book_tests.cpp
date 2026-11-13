#include "test/framework/test.hpp"
#include <book/order_book.hpp>
#include <test/framework/alloc_counter.hpp>

#include <limits>
#include <memory>
#include <stdexcept>

using namespace exsim::book;

TEST_CASE(order_book, add_order_to_empty_book) {
    OrderBook book;

    auto result = book.add_order(10000, 50, Side::BUY);  // 100.00

    CHECK(result.order_id == 1);
    CHECK(result.fills.empty());
    CHECK(result.remaining_quantity == 50);
    CHECK(book.get_total_orders() == 1);
    CHECK(book.get_bid_levels() == 1);
    CHECK(book.get_ask_levels() == 0);
}

TEST_CASE(order_book, multiple_orders_same_price_level) {
    OrderBook book;

    (void)book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 30, Side::BUY);
    (void)book.add_order(10000, 20, Side::BUY);

    CHECK(book.get_total_orders() == 3);
    CHECK(book.get_bid_levels() == 1);
    CHECK(book.get_bid_quantity_at_top() == 100);
}

TEST_CASE(order_book, cancel_order) {
    OrderBook book;

    auto r1 = book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 30, Side::BUY);

    CHECK(book.get_total_orders() == 2);
    CHECK(book.get_bid_quantity_at_top() == 80);

    bool cancelled = book.cancel_order(r1.order_id);

    CHECK(cancelled);
    CHECK(book.get_total_orders() == 1);
    CHECK(book.get_bid_quantity_at_top() == 30);
}

TEST_CASE(order_book, cancel_nonexistent_order) {
    OrderBook book;

    bool cancelled = book.cancel_order(999);

    CHECK(!cancelled);
}

TEST_CASE(order_book, cancel_removes_empty_price_level) {
    OrderBook book;

    auto result = book.add_order(10000, 50, Side::BUY);

    CHECK(book.get_bid_levels() == 1);

    (void)book.cancel_order(result.order_id);

    CHECK(book.get_bid_levels() == 0);
}

TEST_CASE(order_book, modify_order) {
    OrderBook book;

    auto result = book.add_order(10000, 50, Side::BUY);

    CHECK(book.get_bid_quantity_at_top() == 50);

    bool modified = book.modify_order(result.order_id, 100);

    CHECK(modified);
    CHECK(book.get_bid_quantity_at_top() == 100);
}

TEST_CASE(order_book, modify_nonexistent_order) {
    OrderBook book;

    bool modified = book.modify_order(999, 100);

    CHECK(!modified);
}



TEST_CASE(order_book_matching, aggressive_buy_matches_asks) {
    OrderBook book;

    (void)book.add_order(10100, 100, Side::SELL);  // 101.00
    (void)book.add_order(10200, 100, Side::SELL);  // 102.00

    auto result = book.add_order(10100, 50, Side::BUY);

    CHECK(result.fills.size() == 1);
    CHECK(result.fills[0].quantity == 50);
    CHECK(result.fills[0].price == 10100);
    CHECK(result.remaining_quantity == 0);
    CHECK(book.get_ask_quantity_at_top() == 50);
}

TEST_CASE(order_book_matching, aggressive_sell_matches_bids) {
    OrderBook book;

    (void)book.add_order(10000, 100, Side::BUY);   // 100.00
    (void)book.add_order(9900, 100, Side::BUY);    // 99.00

    auto result = book.add_order(10000, 50, Side::SELL);

    CHECK(result.fills.size() == 1);
    CHECK(result.fills[0].quantity == 50);
    CHECK(result.fills[0].price == 10000);
    CHECK(result.remaining_quantity == 0);
    CHECK(book.get_bid_quantity_at_top() == 50);
}

TEST_CASE(order_book_matching, partial_fill_rests_on_book) {
    OrderBook book;

    (void)book.add_order(10100, 30, Side::SELL);

    auto result = book.add_order(10100, 50, Side::BUY);

    CHECK(result.fills.size() == 1);
    CHECK(result.fills[0].quantity == 30);
    CHECK(result.remaining_quantity == 20);
    CHECK(book.get_total_orders() == 1);
    CHECK(book.get_bid_quantity_at_top() == 20);
}

TEST_CASE(order_book_matching, sweep_multiple_price_levels) {
    OrderBook book;

    (void)book.add_order(10100, 50, Side::SELL);  // 101.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00
    (void)book.add_order(10300, 50, Side::SELL);  // 103.00

    auto result = book.add_order(10300, 120, Side::BUY);

    CHECK(result.fills.size() == 3);
    CHECK(result.fills[0].quantity == 50);
    CHECK(result.fills[0].price == 10100);
    CHECK(result.fills[1].quantity == 50);
    CHECK(result.fills[1].price == 10200);
    CHECK(result.fills[2].quantity == 20);
    CHECK(result.fills[2].price == 10300);
    CHECK(result.remaining_quantity == 0);
}

TEST_CASE(order_book_matching, fifo_matching_order) {
    OrderBook book;

    auto r1 = book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 50, Side::BUY);

    auto result = book.add_order(10000, 30, Side::SELL);

    CHECK(result.fills.size() == 1);
    CHECK(result.fills[0].buy_order_id == r1.order_id);
    CHECK(book.get_bid_quantity_at_top() == 70);
}

TEST_CASE(order_book_matching, price_priority) {
    OrderBook book;

    (void)book.add_order(9900, 50, Side::BUY);   // 99.00
    (void)book.add_order(10000, 50, Side::BUY);  // 100.00 (best)
    (void)book.add_order(9800, 50, Side::BUY);   // 98.00

    auto result = book.add_order(9800, 30, Side::SELL);

    CHECK(result.fills.size() == 1);
    CHECK(result.fills[0].price == 10000);  // Matched at best bid
}

TEST_CASE(order_book_matching, no_cross_when_price_doesnt_match) {
    OrderBook book;

    (void)book.add_order(10000, 50, Side::BUY);

    auto result = book.add_order(10100, 50, Side::SELL);

    CHECK(result.fills.empty());
    CHECK(result.remaining_quantity == 50);
    CHECK(book.get_total_orders() == 2);
}



TEST_CASE(order_book_query, best_bid_ask) {
    OrderBook book;

    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(9900, 50, Side::BUY);    // 99.00
    (void)book.add_order(10100, 50, Side::SELL);  // 101.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00

    CHECK(book.get_best_bid().has_value());
    CHECK(*book.get_best_bid() == 10000);
    CHECK(book.get_best_ask().has_value());
    CHECK(*book.get_best_ask() == 10100);
}

TEST_CASE(order_book_query, spread_and_mid_price) {
    OrderBook book;

    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00

    CHECK(book.get_spread().has_value());
    CHECK(*book.get_spread() == 200);  // 2.00 in ticks
    CHECK(book.get_mid_price().has_value());
    CHECK(*book.get_mid_price() == 10100);  // 101.00
}

TEST_CASE(order_book_query, empty_book_returns_nullopt) {
    OrderBook book;

    CHECK(!book.get_best_bid().has_value());
    CHECK(!book.get_best_ask().has_value());
    CHECK(!book.get_spread().has_value());
    CHECK(!book.get_mid_price().has_value());
}

TEST_CASE(order_book_query, snapshot) {
    OrderBook book;

    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(9900, 60, Side::BUY);    // 99.00
    (void)book.add_order(10100, 70, Side::SELL);  // 101.00
    (void)book.add_order(10200, 80, Side::SELL);  // 102.00

    auto snapshot = book.get_snapshot(5);

    CHECK(snapshot.bids.size() == 2);
    CHECK(snapshot.asks.size() == 2);
    CHECK(snapshot.bids[0].price == 10000);
    CHECK(snapshot.bids[0].quantity == 50);
    CHECK(snapshot.asks[0].price == 10100);
    CHECK(snapshot.asks[0].quantity == 70);
}


// Regression: AddResult::fills used to be std::move'd out of the internal
// buffer, forcing a fresh heap allocation on every following match.
TEST_CASE(order_book_matching, fill_view_reuses_buffer_across_calls) {
    OrderBook book;
    (void)book.add_order(10100, 10, Side::SELL);
    (void)book.add_order(10100, 10, Side::SELL);

    auto first = book.add_order(10100, 10, Side::BUY);
    REQUIRE_EQ(first.fills.size(), 1u);
    const Fill* buffer = first.fills.data();

    auto second = book.add_order(10100, 10, Side::BUY);
    REQUIRE_EQ(second.fills.size(), 1u);
    CHECK_EQ(second.fills[0].quantity, 10u);
    CHECK(second.fills.data() == buffer);  // same storage, no reallocation
}

// ---------------------------------------------------------------------------
// Finding 36: the per-call fill output is bounded and allocation-free.
// ---------------------------------------------------------------------------

namespace {

void rest_one_share_asks(OrderBook& book, std::size_t count, Price first_price) {
    for (std::size_t i = 0; i < count; ++i) {
        // Spread over a few levels so the sweep also walks the ladder.
        const Price price = first_price + static_cast<Price>(i % 4);
        const auto rested = book.add_order(price, 1, Side::SELL);
        REQUIRE_EQ(rested.remaining_quantity, 1u);
    }
}

}  // namespace

TEST_CASE(order_book_fill_limit, sweep_of_2000_resting_orders_is_allocation_free) {
    OrderBookConfig config;
    config.max_fills_per_add_order = 4096;
    OrderBook book(config);
    constexpr std::size_t kRestingOrderCount = 2000;  // well past the old 1024 reserve
    rest_one_share_asks(book, kRestingOrderCount, 10100);

    OrderBook::AddResult sweep{};
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        sweep = book.add_order(10103, kRestingOrderCount, Side::BUY);
        allocations = scope.count();
    }
    CHECK_EQ(allocations, 0u);
    CHECK_EQ(sweep.fills.size(), kRestingOrderCount);
    CHECK(!sweep.fill_limit_reached);
    CHECK_EQ(sweep.remaining_quantity, 0u);
    CHECK_EQ(sweep.cancelled_quantity, 0u);
    CHECK_EQ(book.get_total_orders(), 0u);
    CHECK(!book.get_best_ask().has_value());
    CHECK(!book.get_best_bid().has_value());
}

TEST_CASE(order_book_fill_limit, default_limit_stops_sweep_and_cancels_remainder) {
    OrderBook book;  // default limit: 1024 fills per call
    REQUIRE_EQ(book.max_fills_per_add_order(), 1024u);
    constexpr std::size_t kRestingOrderCount = 1500;
    rest_one_share_asks(book, kRestingOrderCount, 10100);

    OrderBook::AddResult sweep{};
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        sweep = book.add_order(10103, kRestingOrderCount, Side::BUY);
        allocations = scope.count();
    }
    CHECK_EQ(allocations, 0u);
    REQUIRE_EQ(sweep.fills.size(), 1024u);
    CHECK(sweep.fill_limit_reached);
    CHECK_EQ(sweep.remaining_quantity, 0u);
    CHECK_EQ(sweep.cancelled_quantity, kRestingOrderCount - 1024u);

    // Every reported fill really happened, and nothing else did: the
    // untouched resting orders are all still there and the aggressor's
    // remainder did not rest (the book is not crossed).
    Quantity filled = 0;
    for (const Fill& fill : sweep.fills) {
        filled += fill.quantity;
        CHECK(!book.has_order(fill.sell_order_id));
        CHECK_EQ(fill.buy_order_id, sweep.order_id);
    }
    CHECK_EQ(filled, 1024u);
    CHECK_EQ(book.get_total_orders(), kRestingOrderCount - 1024u);
    CHECK(!book.has_order(sweep.order_id));
    CHECK(!book.get_best_bid().has_value());
    REQUIRE(book.get_best_ask().has_value());

    // The next call reuses the same bounded buffer.
    const auto next = book.add_order(10103, 1, Side::BUY);
    CHECK_EQ(next.fills.size(), 1u);
    CHECK(next.fills.data() == sweep.fills.data());
}

TEST_CASE(order_book_fill_limit, limit_is_reported_only_when_it_stopped_a_cross) {
    OrderBookConfig config;
    config.max_fills_per_add_order = 4;

    {
        // Exactly 4 fills consume the order: not a limit stop.
        OrderBook book(config);
        rest_one_share_asks(book, 4, 10100);
        const auto result = book.add_order(10103, 4, Side::BUY);
        CHECK_EQ(result.fills.size(), 4u);
        CHECK(!result.fill_limit_reached);
        CHECK_EQ(result.remaining_quantity, 0u);
    }
    {
        // 4 fills exhaust the contra side; the remainder rests normally.
        OrderBook book(config);
        rest_one_share_asks(book, 4, 10100);
        const auto result = book.add_order(10103, 6, Side::BUY);
        CHECK_EQ(result.fills.size(), 4u);
        CHECK(!result.fill_limit_reached);
        CHECK_EQ(result.remaining_quantity, 2u);
        CHECK_EQ(result.cancelled_quantity, 0u);
        CHECK(book.has_order(result.order_id));
        CHECK_EQ(book.get_bid_quantity_at_top(), 2u);
    }
    {
        // A fifth crossing order exists: matching stops, remainder cancelled.
        OrderBook book(config);
        rest_one_share_asks(book, 5, 10100);
        const auto result = book.add_order(10103, 6, Side::BUY);
        CHECK_EQ(result.fills.size(), 4u);
        CHECK(result.fill_limit_reached);
        CHECK_EQ(result.remaining_quantity, 0u);
        CHECK_EQ(result.cancelled_quantity, 2u);
        CHECK(!book.has_order(result.order_id));
        CHECK_EQ(book.get_total_orders(), 1u);
        CHECK(!book.get_best_bid().has_value());
    }
    {
        // Same bound on the sell side.
        OrderBook book(config);
        for (int i = 0; i < 5; ++i) {
            (void)book.add_order(10000, 1, Side::BUY);
        }
        const auto result = book.add_order(10000, 5, Side::SELL);
        CHECK_EQ(result.fills.size(), 4u);
        CHECK(result.fill_limit_reached);
        CHECK_EQ(result.cancelled_quantity, 1u);
        CHECK_EQ(book.get_bid_quantity_at_top(), 1u);
        CHECK(!book.get_best_ask().has_value());
    }
}

TEST_CASE(order_book_fill_limit, steady_state_churn_allocates_nothing) {
    OrderBook book;
    // Warm up every path once (rest, partial fill, full fill, sweep, cancel,
    // modify) before counting.
    std::size_t allocations = 0;
    bool book_state_ok = true;
    for (int round = 0; round < 2; ++round) {
        testfw::AllocationScope scope;
        for (int i = 0; i < 200000; ++i) {
            const Price price = 10000 + (i % 16);
            const auto resting_sell = book.add_order(price, 3, Side::SELL);
            (void)book.add_order(price, 1, Side::BUY);        // partial fill
            const auto bid = book.add_order(9000 + (i % 16), 5, Side::BUY);
            (void)book.modify_order(bid.order_id, 2);
            (void)book.add_order(price + 1, 2, Side::BUY);    // completes the sell
            book_state_ok = book_state_ok && !book.has_order(resting_sell.order_id);
            (void)book.cancel_order(bid.order_id);
        }
        if (round == 1) {
            allocations = scope.count();
        }
    }
    CHECK(book_state_ok);
    CHECK_EQ(allocations, 0u);
    CHECK_EQ(book.get_total_orders(), 0u);
}

TEST_CASE(order_book, modify_to_filled_quantity_removes_order) {
    OrderBook book;
    const auto bid = book.add_order(10000, 10, Side::BUY);
    (void)book.add_order(10000, 4, Side::SELL);  // bid now has 4 filled, 6 left
    REQUIRE(book.has_order(bid.order_id));

    CHECK(book.modify_order(bid.order_id, 4));   // nothing left to execute
    CHECK(!book.has_order(bid.order_id));
    CHECK_EQ(book.get_total_orders(), 0u);
    CHECK_EQ(book.get_bid_levels(), 0u);
    CHECK(!book.get_best_bid().has_value());
}

// ---------------------------------------------------------------------------
// Bounded books: configured limits are exact and add_order never allocates.
// ---------------------------------------------------------------------------

namespace {

OrderBookConfig bounded_config(std::size_t max_resting_orders, std::size_t max_price_levels) {
    OrderBookConfig config;
    config.max_resting_orders = max_resting_orders;
    config.max_price_levels = max_price_levels;
    config.min_price = 0;
    config.max_price = 1000;
    config.allow_growth = false;
    return config;
}

}  // namespace

TEST_CASE(order_book_bounds, full_book_rests_exactly_its_capacity_and_still_trades) {
    OrderBook book(bounded_config(5, 16));  // 5 is not a pool block multiple
    std::size_t allocations = 0;
    bool five_rested = true;
    OrderBook::AddResult sixth{};
    std::size_t marketable_fill_count = 0;
    Quantity marketable_filled_quantity = 0;
    Quantity marketable_remaining_quantity = 0;
    OrderBook::AddResult refill{};
    {
        testfw::AllocationScope scope;
        for (int i = 0; i < 5; ++i) {
            five_rested = five_rested && book.add_order(500, 1, Side::SELL).remaining_quantity == 1;
        }
        sixth = book.add_order(501, 2, Side::SELL);
        // A full book must still accept an aggressor that only trades.
        // fills is a view valid only until the next add, so read it now.
        const OrderBook::AddResult marketable = book.add_order(500, 1, Side::BUY);
        marketable_fill_count = marketable.fills.size();
        marketable_filled_quantity = marketable_fill_count == 1 ? marketable.fills[0].quantity : 0;
        marketable_remaining_quantity = marketable.remaining_quantity;
        refill = book.add_order(502, 3, Side::SELL);
        allocations = scope.count();
    }
    CHECK(five_rested);
    CHECK(sixth.order_id != 0u);
    CHECK_EQ(sixth.remaining_quantity, 0u);
    CHECK_EQ(sixth.cancelled_quantity, 2u);
    CHECK(!sixth.fill_limit_reached);
    CHECK(!book.has_order(sixth.order_id));
    CHECK_EQ(marketable_fill_count, 1u);
    CHECK_EQ(marketable_filled_quantity, 1u);
    CHECK_EQ(marketable_remaining_quantity, 0u);
    CHECK_EQ(refill.remaining_quantity, 3u);  // the freed slot is reused
    CHECK_EQ(book.get_total_orders(), 5u);
    CHECK_EQ(allocations, 0u);
}

TEST_CASE(order_book_bounds, partially_filled_aggressor_rests_in_full_book_after_freeing_a_slot) {
    OrderBook book(bounded_config(2, 16));
    (void)book.add_order(500, 1, Side::SELL);
    (void)book.add_order(510, 1, Side::SELL);
    // Consumes one resting ask, then rests its remainder in the freed slot.
    const auto result = book.add_order(505, 4, Side::BUY);
    CHECK_EQ(result.fills.size(), 1u);
    CHECK_EQ(result.remaining_quantity, 3u);
    CHECK_EQ(result.cancelled_quantity, 0u);
    CHECK(book.has_order(result.order_id));
    CHECK_EQ(book.get_total_orders(), 2u);
    CHECK_EQ(book.get_best_bid().value_or(-1), 505);
    CHECK_EQ(book.get_best_ask().value_or(-1), 510);
}

TEST_CASE(order_book_bounds, price_outside_band_is_rejected_without_growth) {
    OrderBook book(bounded_config(8, 8));
    std::size_t allocations = 0;
    OrderBook::AddResult above{};
    OrderBook::AddResult below{};
    {
        testfw::AllocationScope scope;
        above = book.add_order(1001, 1, Side::BUY);
        below = book.add_order(-1, 1, Side::SELL);
        allocations = scope.count();
    }
    CHECK_EQ(above.order_id, 0u);
    CHECK_EQ(below.order_id, 0u);
    CHECK_EQ(book.get_total_orders(), 0u);
    CHECK_EQ(allocations, 0u);
    CHECK_EQ(book.add_order(1000, 1, Side::BUY).remaining_quantity, 1u);  // band edges are inclusive
    CHECK_EQ(book.add_order(0, 1, Side::BUY).remaining_quantity, 1u);
}

TEST_CASE(order_book_bounds, level_exhaustion_cancels_remainder_and_freed_level_is_reused) {
    OrderBook book(bounded_config(8, 2));
    CHECK_EQ(book.add_order(100, 1, Side::BUY).remaining_quantity, 1u);
    const auto second = book.add_order(101, 1, Side::BUY);
    CHECK_EQ(second.remaining_quantity, 1u);
    const auto third = book.add_order(102, 4, Side::BUY);
    CHECK_EQ(third.remaining_quantity, 0u);
    CHECK_EQ(third.cancelled_quantity, 4u);
    CHECK(!book.has_order(third.order_id));
    CHECK_EQ(book.get_bid_levels(), 2u);
    CHECK_EQ(book.get_total_orders(), 2u);
    CHECK_EQ(book.get_best_bid().value_or(-1), 101);  // never pointed at the dropped level

    CHECK(book.cancel_order(second.order_id));
    CHECK_EQ(book.add_order(102, 4, Side::BUY).remaining_quantity, 4u);
    CHECK_EQ(book.get_best_bid().value_or(-1), 102);
}

// ---------------------------------------------------------------------------
// Price-band arithmetic, ladder growth and level volume.
// ---------------------------------------------------------------------------

TEST_CASE(order_book_growth, unrepresentable_band_is_rejected_at_construction) {
    OrderBookConfig config;
    config.min_price = std::numeric_limits<Price>::min();
    config.max_price = std::numeric_limits<Price>::max();
    bool threw_length_error = false;
    try {
        OrderBook book(config);
    } catch (const std::length_error&) {
        threw_length_error = true;
    }
    CHECK(threw_length_error);
}

TEST_CASE(order_book_growth, narrow_band_at_extreme_prices_works) {
    OrderBookConfig config;
    config.min_price = std::numeric_limits<Price>::max() - 100;
    config.max_price = std::numeric_limits<Price>::max();
    config.allow_growth = false;
    OrderBook book(config);
    CHECK_EQ(book.add_order(config.max_price, 3, Side::SELL).remaining_quantity, 3u);
    CHECK_EQ(book.add_order(config.min_price, 2, Side::BUY).remaining_quantity, 2u);
    CHECK_EQ(book.get_best_ask().value_or(0), config.max_price);
    CHECK_EQ(book.get_spread().value_or(0), 100);
    const auto sweep = book.add_order(config.max_price, 3, Side::BUY);
    CHECK_EQ(sweep.fills.size(), 1u);
    CHECK(!book.get_best_ask().has_value());
}

namespace {

Price mid_price_of(Price bid, Price ask) {
    OrderBookConfig config;
    config.min_price = bid;
    config.max_price = ask;
    config.allow_growth = false;
    OrderBook book(config);
    (void)book.add_order(bid, 1, Side::BUY);
    (void)book.add_order(ask, 1, Side::SELL);
    return book.get_mid_price().value();
}

}  // namespace

TEST_CASE(order_book_query, mid_price_truncates_toward_zero_without_overflow) {
    constexpr Price kMax = std::numeric_limits<Price>::max();
    constexpr Price kMin = std::numeric_limits<Price>::min();
    // bid + ask would overflow in each of these.
    CHECK_EQ(mid_price_of(kMax - 1, kMax), kMax - 1);
    CHECK_EQ(mid_price_of(kMax - 3, kMax), kMax - 2);
    CHECK_EQ(mid_price_of(kMin, kMin + 1), kMin + 1);
    CHECK_EQ(mid_price_of(kMin, kMin + 3), kMin + 2);
    // Odd sums round toward zero, as (bid + ask) / 2 always did.
    CHECK_EQ(mid_price_of(3, 4), 3);
    CHECK_EQ(mid_price_of(-4, -3), -3);
    CHECK_EQ(mid_price_of(-3, 2), 0);
    CHECK_EQ(mid_price_of(-5, 2), -1);
    CHECK_EQ(mid_price_of(-4, 1), -1);
    CHECK_EQ(mid_price_of(-1, 4), 1);
    CHECK_EQ(mid_price_of(-6, 6), 0);
}

TEST_CASE(order_book_growth, growth_in_both_directions_keeps_levels_and_best_prices) {
    OrderBookConfig config;
    config.min_price = 0;
    config.max_price = 10;
    config.allow_growth = true;
    OrderBook book(config);
    (void)book.add_order(5, 1, Side::BUY);
    (void)book.add_order(8, 2, Side::SELL);
    (void)book.add_order(100000, 3, Side::SELL);  // grows upward
    (void)book.add_order(-50000, 4, Side::BUY);   // grows downward, shifting every index
    (void)book.add_order(9, 5, Side::SELL);

    CHECK_EQ(book.get_best_bid().value_or(0), 5);
    CHECK_EQ(book.get_best_ask().value_or(0), 8);
    CHECK_EQ(book.get_bid_levels(), 2u);
    CHECK_EQ(book.get_ask_levels(), 3u);

    const auto sweep = book.add_order(100000, 10, Side::BUY);
    REQUIRE_EQ(sweep.fills.size(), 3u);
    CHECK_EQ(sweep.fills[0].price, 8);
    CHECK_EQ(sweep.fills[1].price, 9);
    CHECK_EQ(sweep.fills[2].price, 100000);
    CHECK_EQ(sweep.remaining_quantity, 0u);
    CHECK(!book.get_best_ask().has_value());

    const auto bids = book.get_snapshot(5).bids;
    REQUIRE_EQ(bids.size(), 2u);
    CHECK_EQ(bids[0].price, 5);
    CHECK_EQ(bids[1].price, -50000);
    CHECK_EQ(bids[1].quantity, 4u);
}

TEST_CASE(order_book, level_volume_is_exact_above_int64_max) {
    OrderBook book;
    const Quantity big = (Quantity{1} << 63) + 10;
    (void)book.add_order(100, big, Side::SELL);
    (void)book.add_order(100, 5, Side::BUY);
    CHECK_EQ(book.get_ask_quantity_at_top(), big - 5);
    const auto bid = book.add_order(50, big, Side::BUY);
    CHECK(book.modify_order(bid.order_id, big + 1));
    CHECK_EQ(book.get_bid_quantity_at_top(), big + 1);
}

TEST_CASE(order_book_query, snapshot_walks_sparse_levels_across_bitmap_words) {
    OrderBook book;  // default band [-100000, 100000]; ladder word boundaries at prices = -32 (mod 64)
    const Price bid_prices[] = {-100000, -99999, -33, -32, 31, 32};
    const Price ask_prices[] = {95, 96, 4000, 99936, 99999, 100000};
    for (Price price : bid_prices) {
        (void)book.add_order(price, 1, Side::BUY);
    }
    for (Price price : ask_prices) {
        (void)book.add_order(price, 2, Side::SELL);
    }
    REQUIRE_EQ(book.get_total_orders(), 12u);

    const auto top = book.get_snapshot(4);
    REQUIRE_EQ(top.bids.size(), 4u);
    CHECK_EQ(top.bids[0].price, 32);
    CHECK_EQ(top.bids[1].price, 31);
    CHECK_EQ(top.bids[2].price, -32);
    CHECK_EQ(top.bids[3].price, -33);
    REQUIRE_EQ(top.asks.size(), 4u);
    CHECK_EQ(top.asks[3].price, 99936);

    const auto full = book.get_snapshot(100);
    REQUIRE_EQ(full.bids.size(), 6u);
    REQUIRE_EQ(full.asks.size(), 6u);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK_EQ(full.bids[i].price, bid_prices[5 - i]);
        CHECK_EQ(full.asks[i].price, ask_prices[i]);
        CHECK_EQ(full.asks[i].quantity, 2u);
        CHECK_EQ(full.asks[i].order_count, 1u);
    }
    CHECK(book.get_snapshot(0).bids.empty());
}

TEST_CASE(order_book_bounds, aggregate_quantity_overflow_does_not_wrap_or_mutate) {
    exsim::book::OrderBookConfig config;
    config.allow_growth = false;
    exsim::book::OrderBook book(config);
    const auto maximum = std::numeric_limits<exsim::book::Quantity>::max();
    const auto first = book.add_order(100, maximum - 10, exsim::book::Side::BUY);
    REQUIRE(first.remaining_quantity == maximum - 10);
    const auto second = book.add_order(100, 10, exsim::book::Side::BUY);
    REQUIRE(second.remaining_quantity == 10);
    const auto refused = book.add_order(100, 1, exsim::book::Side::BUY);
    CHECK_EQ(refused.remaining_quantity, 0u);
    CHECK_EQ(refused.cancelled_quantity, 1u);
    CHECK_EQ(book.get_bid_quantity_at_top(), maximum);
    CHECK(!book.modify_order(second.order_id, 11));
    CHECK_EQ(book.get_bid_quantity_at_top(), maximum);
    const auto trade = book.add_order(100, 5, exsim::book::Side::SELL);
    REQUIRE_EQ(trade.fills.size(), 1u);
    CHECK_EQ(trade.fills.front().quantity, 5u);
    CHECK_EQ(book.get_bid_quantity_at_top(), maximum - 5);
}

TEST_CASE(order_book_exchange_storage, passive_resting_preserves_fifo_ids_and_crossed_auction_books) {
    OrderBook book(OrderBookConfig{3,3,0,100,1,false});
    auto* first = book.rest_order(41,60,20,15,Side::BUY,7);
    REQUIRE(first != nullptr);
    REQUIRE(book.rest_order(42,60,10,10,Side::BUY,8) != nullptr);
    REQUIRE(book.rest_order(43,50,8,8,Side::SELL,9) != nullptr);
    // Passive storage must not secretly execute an auction/cross or assign IDs.
    CHECK_EQ(book.get_total_orders(),3u);
    CHECK_EQ(book.get_best_bid().value(),60);
    CHECK_EQ(book.get_best_ask().value(),50);
    const auto* level = book.price_level(Side::BUY,60);
    REQUIRE(level != nullptr);
    CHECK_EQ(level->total_volume,25u);
    CHECK_EQ(level->head_order->id,41u);
    CHECK_EQ(level->head_order->next_order->id,42u);
    CHECK_EQ(first->context_index,7u);
    CHECK(book.rest_order(44,70,1,1,Side::BUY,10) == nullptr);
    book.execute_order(*first,15);
    CHECK_EQ(first->remaining_quantity,0u);
    CHECK_EQ(level->total_volume,10u);
    CHECK(book.find_order(41) != nullptr);
    CHECK(book.cancel_order(41));
    CHECK_EQ(book.price_level(Side::BUY,60)->head_order->id,42u);
    CHECK(book.cancel_order(42));
    CHECK(!book.get_best_bid().has_value());
    CHECK(book.rest_order(44,70,1,1,Side::BUY,10) != nullptr);
    CHECK_EQ(book.next_price(Side::BUY,0,true).value(),70);
    CHECK_EQ(book.next_price(Side::BUY,100,false).value(),70);
    CHECK(book.cancel_order(44));
    REQUIRE(book.rest_order(std::numeric_limits<OrderId>::max(),70,1,1,Side::BUY,11) != nullptr);
    const auto exhausted = book.add_order(100,1,Side::BUY);
    CHECK_EQ(exhausted.order_id,0u);
    CHECK(exhausted.fills.empty());
    CHECK_EQ(book.find_order(43)->remaining_quantity,8u);
}

TEST_CASE(order_book_exchange_storage, reductions_reject_invalid_growth_and_allocate_nothing) {
    OrderBook book(OrderBookConfig{2,2,0,100,1,false});
    Order* const bid = book.rest_order(1,10,10,10,Side::BUY,1);
    REQUIRE(bid != nullptr);
    Order* const ask = book.rest_order(2,20,10,10,Side::SELL,2);
    REQUIRE(ask != nullptr);
    if (!testfw::allocation_counting_available()) return;
    std::size_t count;
    {
        testfw::AllocationScope allocations;
        CHECK(!book.amend_order(*bid,10,11));  // leaves may not grow
        CHECK(!book.amend_order(*bid,5,6));    // leaves may not exceed total
        book.execute_order(*bid,2);
        CHECK(book.amend_order(*bid,9,7));
        CHECK_EQ(book.find_order(1)->quantity,9u);
        CHECK_EQ(book.find_order(1)->remaining_quantity,7u);
        CHECK_EQ(book.price_level(Side::BUY,10)->total_volume,7u);
        CHECK_EQ(book.get_total_leaves(Side::BUY),7u);
        book.remove_order(*ask);
        CHECK(book.find_order(2) == nullptr);
        CHECK_EQ(book.get_total_leaves(Side::SELL),0u);
        CHECK(book.rest_order(3,30,5,5,Side::SELL,3) != nullptr);
        count = allocations.count();
    }
    CHECK_EQ(count,0u);
}

TEST_CASE(order_book_query, next_price_reaches_both_band_edges_on_both_sides) {
    // Regression: the previous-level search checked its start against the bid
    // ladder for both sides. Every edge of both ladders must be reachable.
    OrderBook book(bounded_config(8, 8));
    REQUIRE(book.rest_order(1, 0, 1, 1, Side::BUY, 0) != nullptr);
    REQUIRE(book.rest_order(2, 1000, 1, 1, Side::BUY, 0) != nullptr);
    REQUIRE(book.rest_order(3, 0, 1, 1, Side::SELL, 0) != nullptr);
    REQUIRE(book.rest_order(4, 1000, 1, 1, Side::SELL, 0) != nullptr);
    for (const Side side : {Side::BUY, Side::SELL}) {
        CHECK_EQ(book.next_price(side, 0, true).value_or(-1), 0);
        CHECK_EQ(book.next_price(side, 1, true).value_or(-1), 1000);
        CHECK_EQ(book.next_price(side, 1000, false).value_or(-1), 1000);
        CHECK_EQ(book.next_price(side, 999, false).value_or(-1), 0);
        CHECK_EQ(book.next_price(side, 5000, false).value_or(-1), 1000);  // clamped to the band
        CHECK_EQ(book.next_price(side, -5000, true).value_or(-1), 0);
        CHECK(!book.next_price(side, 1001, true).has_value());
        CHECK(!book.next_price(side, -1, false).has_value());
    }
    CHECK(book.cancel_order(1));
    CHECK(book.cancel_order(4));
    CHECK(!book.next_price(Side::BUY, 999, false).has_value());
    CHECK(!book.next_price(Side::SELL, 1, true).has_value());
}

TEST_CASE(order_book_exchange_storage, books_on_one_storage_share_capacity_but_not_orders) {
    auto storage = std::make_shared<OrderBookStorage>(3, 3, false);
    OrderBook first_book(bounded_config(3, 3), storage);
    OrderBook second_book(bounded_config(3, 3), storage);
    Order* const first_bid = first_book.rest_order(1, 100, 5, 5, Side::BUY, 0);
    REQUIRE(first_bid != nullptr);
    CHECK(second_book.rest_order(1, 99, 1, 1, Side::BUY, 3) == nullptr);  // ID taken in the other book
    REQUIRE(second_book.rest_order(2, 100, 7, 7, Side::SELL, 1) != nullptr);
    REQUIRE(second_book.rest_order(3, 101, 7, 7, Side::SELL, 2) != nullptr);
    CHECK(second_book.rest_order(4, 102, 1, 1, Side::SELL, 3) == nullptr);  // shared pool is full
    CHECK(first_book.rest_order(4, 50, 1, 1, Side::BUY, 3) == nullptr);
    CHECK_EQ(first_book.get_total_orders(), 1u);
    CHECK_EQ(second_book.get_total_orders(), 2u);

    // One book never sees or changes the other book's orders.
    CHECK(!first_book.has_order(2));
    CHECK(first_book.find_order(2) == nullptr);
    CHECK(!first_book.cancel_order(2));
    CHECK(!first_book.modify_order(3, 1));
    CHECK(!second_book.cancel_order(1));
    CHECK_EQ(second_book.find_order(2)->remaining_quantity, 7u);
    CHECK_EQ(second_book.get_total_leaves(Side::SELL), 14u);
    CHECK_EQ(first_book.get_total_leaves(Side::SELL), 0u);

    // A slot freed by one book is usable by the other.
    first_book.remove_order(*first_bid);
    CHECK_EQ(first_book.get_bid_levels(), 0u);
    CHECK(second_book.rest_order(4, 102, 1, 1, Side::SELL, 3) != nullptr);
    CHECK_EQ(second_book.get_ask_levels(), 3u);
    CHECK_EQ(second_book.get_best_ask().value_or(-1), 100);
}

TEST_CASE(order_book, level_order_and_leaves_counts_track_every_mutation) {
    OrderBookConfig config = bounded_config(16, 16);
    config.max_price = 10;
    config.allow_growth = true;
    OrderBook book(config);
    const auto check_counts = [&](std::size_t bid_levels, std::size_t ask_levels, std::size_t orders,
                                  Quantity bid_leaves, Quantity ask_leaves) {
        CHECK_EQ(book.get_bid_levels(), bid_levels);
        CHECK_EQ(book.get_ask_levels(), ask_levels);
        CHECK_EQ(book.get_total_orders(), orders);
        CHECK_EQ(book.get_total_leaves(Side::BUY), bid_leaves);
        CHECK_EQ(book.get_total_leaves(Side::SELL), ask_leaves);
    };
    check_counts(0, 0, 0, 0, 0);
    const auto bid_at_5 = book.add_order(5, 3, Side::BUY);
    (void)book.add_order(5, 4, Side::BUY);
    (void)book.add_order(4, 2, Side::BUY);
    (void)book.add_order(8, 6, Side::SELL);
    check_counts(2, 1, 4, 9, 6);
    (void)book.add_order(20000, 1, Side::SELL);  // grows the ladder upward
    (void)book.add_order(-300, 1, Side::BUY);    // grows it downward
    check_counts(3, 2, 6, 10, 7);
    const auto partial = book.add_order(5, 5, Side::SELL);  // fills 3 then 2 of 4 at 5
    REQUIRE_EQ(partial.fills.size(), 2u);
    const OrderId partly_filled_bid = partial.fills[1].buy_order_id;  // fills is only valid until the next add
    check_counts(3, 2, 5, 5, 7);
    CHECK(book.modify_order(partly_filled_bid, 10));  // 2 of 10 filled: leaves 8
    check_counts(3, 2, 5, 11, 7);
    CHECK(!book.cancel_order(bid_at_5.order_id));
    const auto sweep = book.add_order(20000, 20, Side::BUY);  // takes both asks, rests 13
    CHECK_EQ(sweep.fills.size(), 2u);
    check_counts(4, 0, 4, 24, 0);
    CHECK(book.cancel_order(sweep.order_id));
    CHECK(book.modify_order(partly_filled_bid, 2));  // nothing left: removed
    check_counts(2, 0, 2, 3, 0);
}
