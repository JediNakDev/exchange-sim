#include "query_tests.hpp"
#include "test_framework.hpp"
#include <book/order_book.hpp>
#include <cassert>

using namespace exsim::book;

void test_best_bid_ask() {
    OrderBook book;
    
    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(9900, 50, Side::BUY);    // 99.00
    (void)book.add_order(10100, 50, Side::SELL);  // 101.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00
    
    assert(book.get_best_bid().has_value());
    assert(*book.get_best_bid() == 10000);
    assert(book.get_best_ask().has_value());
    assert(*book.get_best_ask() == 10100);
}

void test_spread_and_mid_price() {
    OrderBook book;
    
    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00
    
    assert(book.get_spread().has_value());
    assert(*book.get_spread() == 200);  // 2.00 in ticks
    assert(book.get_mid_price().has_value());
    assert(*book.get_mid_price() == 10100);  // 101.00
}

void test_empty_book_returns_nullopt() {
    OrderBook book;
    
    assert(!book.get_best_bid().has_value());
    assert(!book.get_best_ask().has_value());
    assert(!book.get_spread().has_value());
    assert(!book.get_mid_price().has_value());
}

void test_snapshot() {
    OrderBook book;
    
    (void)book.add_order(10000, 50, Side::BUY);   // 100.00
    (void)book.add_order(9900, 60, Side::BUY);    // 99.00
    (void)book.add_order(10100, 70, Side::SELL);  // 101.00
    (void)book.add_order(10200, 80, Side::SELL);  // 102.00
    
    auto snapshot = book.get_snapshot(5);
    
    assert(snapshot.bids.size() == 2);
    assert(snapshot.asks.size() == 2);
    assert(snapshot.bids[0].price == 10000);
    assert(snapshot.bids[0].quantity == 50);
    assert(snapshot.asks[0].price == 10100);
    assert(snapshot.asks[0].quantity == 70);
}

void run_query_tests() {
    std::cout << "[Query Tests]\n";
    RUN_TEST(test_best_bid_ask);
    RUN_TEST(test_spread_and_mid_price);
    RUN_TEST(test_empty_book_returns_nullopt);
    RUN_TEST(test_snapshot);
    std::cout << "\n";
}
