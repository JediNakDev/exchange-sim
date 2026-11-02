#include "../framework/test.hpp"
#include <lob/order_book.hpp>

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
