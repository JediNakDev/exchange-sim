#ifndef MATCHING_TESTS_HPP
#define MATCHING_TESTS_HPP

void test_aggressive_buy_matches_asks();
void test_aggressive_sell_matches_bids();
void test_partial_fill_rests_on_book();
void test_sweep_multiple_price_levels();
void test_fifo_matching_order();
void test_price_priority();
void test_no_cross_when_price_doesnt_match();

void run_matching_tests();

#endif
