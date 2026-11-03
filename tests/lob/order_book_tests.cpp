#include "../framework/test.hpp"
#include <lob/order_book.hpp>
#include <tests/framework/alloc_counter.hpp>

using namespace lob;

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
