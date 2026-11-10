#include "order_tests.hpp"
#include "test_framework.hpp"
#include <exchange/order_book.hpp>
#include <cassert>

using namespace exsim::exchange;

void test_add_order_to_empty_book() {
    OrderBook book;
    
    auto result = book.add_order(10000, 50, Side::BUY);  // 100.00
    
    assert(result.order_id == 1);
    assert(result.fills.empty());
    assert(result.remaining_quantity == 50);
    assert(book.get_total_orders() == 1);
    assert(book.get_bid_levels() == 1);
    assert(book.get_ask_levels() == 0);
}

void test_multiple_orders_same_price_level() {
    OrderBook book;
    
    (void)book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 30, Side::BUY);
    (void)book.add_order(10000, 20, Side::BUY);
    
    assert(book.get_total_orders() == 3);
    assert(book.get_bid_levels() == 1);
    assert(book.get_bid_quantity_at_top() == 100);
}

void test_cancel_order() {
    OrderBook book;
    
    auto r1 = book.add_order(10000, 50, Side::BUY);
    (void)book.add_order(10000, 30, Side::BUY);
    
    assert(book.get_total_orders() == 2);
    assert(book.get_bid_quantity_at_top() == 80);
    
    bool cancelled = book.cancel_order(r1.order_id);
    
    assert(cancelled);
    assert(book.get_total_orders() == 1);
    assert(book.get_bid_quantity_at_top() == 30);
}

void test_cancel_nonexistent_order() {
    OrderBook book;
    
    bool cancelled = book.cancel_order(999);
    
    assert(!cancelled);
}

void test_cancel_removes_empty_price_level() {
    OrderBook book;
    
    auto result = book.add_order(10000, 50, Side::BUY);
    
    assert(book.get_bid_levels() == 1);
    
    (void)book.cancel_order(result.order_id);
    
    assert(book.get_bid_levels() == 0);
}

void test_modify_order() {
    OrderBook book;
    
    auto result = book.add_order(10000, 50, Side::BUY);
    
    assert(book.get_bid_quantity_at_top() == 50);
    
    bool modified = book.modify_order(result.order_id, 100);
    
    assert(modified);
    assert(book.get_bid_quantity_at_top() == 100);
}

void test_modify_nonexistent_order() {
    OrderBook book;
    
    bool modified = book.modify_order(999, 100);
    
    assert(!modified);
}

void run_order_tests() {
    std::cout << "[Order Tests]\n";
    RUN_TEST(test_add_order_to_empty_book);
    RUN_TEST(test_multiple_orders_same_price_level);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_cancel_nonexistent_order);
    RUN_TEST(test_cancel_removes_empty_price_level);
    RUN_TEST(test_modify_order);
    RUN_TEST(test_modify_nonexistent_order);
    std::cout << "\n";
}
