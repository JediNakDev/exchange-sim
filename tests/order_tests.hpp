#ifndef ORDER_TESTS_HPP
#define ORDER_TESTS_HPP

void test_add_order_to_empty_book();
void test_multiple_orders_same_price_level();
void test_cancel_order();
void test_cancel_nonexistent_order();
void test_cancel_removes_empty_price_level();
void test_modify_order();
void test_modify_nonexistent_order();

void run_order_tests();

#endif
