#include "matching_tests.hpp"
#include "test_framework.hpp"
#include <book/order_book.hpp>
#include <cassert>

using namespace exsim::book;

void test_aggressive_buy_matches_asks() {
    OrderBook book;
    
    (void)book.add_order(10100, 100, Side::SELL);  // 101.00
    (void)book.add_order(10200, 100, Side::SELL);  // 102.00
    
    auto result = book.add_order(10100, 50, Side::BUY);
    
    assert(result.fills.size() == 1);
    assert(result.fills[0].quantity == 50);
    assert(result.fills[0].price == 10100);
    assert(result.remaining_quantity == 0);
    assert(book.get_ask_quantity_at_top() == 50);
}

void test_aggressive_sell_matches_bids() {
    OrderBook book;
    
    (void)book.add_order(10000, 100, Side::BUY);   // 100.00
    (void)book.add_order(9900, 100, Side::BUY);    // 99.00
    
    auto result = book.add_order(10000, 50, Side::SELL);
    
    assert(result.fills.size() == 1);
    assert(result.fills[0].quantity == 50);
    assert(result.fills[0].price == 10000);
    assert(result.remaining_quantity == 0);
    assert(book.get_bid_quantity_at_top() == 50);
}

void test_partial_fill_rests_on_book() {
    OrderBook book;
    
    (void)book.add_order(10100, 30, Side::SELL);
    
    auto result = book.add_order(10100, 50, Side::BUY);
    
    assert(result.fills.size() == 1);
    assert(result.fills[0].quantity == 30);
    assert(result.remaining_quantity == 20);
    assert(book.get_total_orders() == 1);
    assert(book.get_bid_quantity_at_top() == 20);
}

void test_sweep_multiple_price_levels() {
    OrderBook book;
    
    (void)book.add_order(10100, 50, Side::SELL);  // 101.00
    (void)book.add_order(10200, 50, Side::SELL);  // 102.00
    (void)book.add_order(10300, 50, Side::SELL);  // 103.00
    
    auto result = book.add_order(10300, 120, Side::BUY);
    
    assert(result.fills.size() == 3);
    assert(result.fills[0].quantity == 50);
    assert(result.fills[0].price == 10100);
    assert(result.fills[1].quantity == 50);
    assert(result.fills[1].price == 10200);
    assert(result.fills[2].quantity == 20);
    assert(result.fills[2].price == 10300);
    assert(result.remaining_quantity == 0);
}

void test_fifo_matching_order() {
    OrderBook book;
    
    auto r1 = book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 50, Side::BUY);
    
    auto result = book.add_order(10000, 30, Side::SELL);
    
    assert(result.fills.size() == 1);
    assert(result.fills[0].buy_order_id == r1.order_id);
    assert(book.get_bid_quantity_at_top() == 70);
}

void test_price_priority() {
    OrderBook book;
    
    (void)book.add_order(9900, 50, Side::BUY);   // 99.00
    (void)book.add_order(10000, 50, Side::BUY);  // 100.00 (best)
    (void)book.add_order(9800, 50, Side::BUY);   // 98.00
    
    auto result = book.add_order(9800, 30, Side::SELL);
    
    assert(result.fills.size() == 1);
    assert(result.fills[0].price == 10000);  // Matched at best bid
}

void test_no_cross_when_price_doesnt_match() {
    OrderBook book;
    
    (void)book.add_order(10000, 50, Side::BUY);
    
    auto result = book.add_order(10100, 50, Side::SELL);
    
    assert(result.fills.empty());
    assert(result.remaining_quantity == 50);
    assert(book.get_total_orders() == 2);
}

void run_matching_tests() {
    std::cout << "[Matching Tests]\n";
    RUN_TEST(test_aggressive_buy_matches_asks);
    RUN_TEST(test_aggressive_sell_matches_bids);
    RUN_TEST(test_partial_fill_rests_on_book);
    RUN_TEST(test_sweep_multiple_price_levels);
    RUN_TEST(test_fifo_matching_order);
    RUN_TEST(test_price_priority);
    RUN_TEST(test_no_cross_when_price_doesnt_match);
    std::cout << "\n";
}
