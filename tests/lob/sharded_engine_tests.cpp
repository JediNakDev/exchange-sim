// Legacy ShardedEngine: per-symbol isolation inside a shard (finding 16),
// fill-limit interplay (finding 36) and client-id mapping reclamation
// (finding 37).

#include "../framework/test.hpp"
#include <lob/engine/sharded_engine.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::engine::ShardedEngine;
using lob::engine::ShardedEngineConfig;
using lob::engine::SymbolId;

namespace {

ShardedEngineConfig small_test_config(std::size_t shard_count) {
    ShardedEngineConfig config;
    config.shard_count = shard_count;
    config.batch_size = 64;
    config.pin_workers = false;
    config.max_symbols_per_shard = 4;
    config.book_config.max_resting_orders = 1024;
    config.book_config.max_price_levels = 256;
    config.book_config.min_price = 0;
    config.book_config.max_price = 20000;
    config.book_config.max_fills_per_add_order = 64;
    return config;
}

// Retries while the shard queue is momentarily full. Only used for symbols
// the test knows are accepted, so it cannot spin on a capacity rejection.
ShardedEngine::OrderHandle add_order_waiting_for_queue(
    ShardedEngine& engine, SymbolId symbol, Price price, Quantity quantity, Side side) {
    for (;;) {
        if (const auto handle = engine.submit_add(symbol, price, quantity, side)) {
            return *handle;
        }
        std::this_thread::yield();
    }
}

void cancel_waiting_for_queue(ShardedEngine& engine, ShardedEngine::OrderHandle handle) {
    while (!engine.submit_cancel(handle)) {
        std::this_thread::yield();
    }
}

void modify_waiting_for_queue(ShardedEngine& engine, ShardedEngine::OrderHandle handle, Quantity quantity) {
    while (!engine.submit_modify(handle, quantity)) {
        std::this_thread::yield();
    }
}

std::size_t resting_order_total(const ShardedEngine& engine, const SymbolId* symbols, std::size_t symbol_count) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < symbol_count; ++i) {
        if (const lob::OrderBook* book = engine.find_book(symbols[i])) {
            total += book->get_total_orders();
        }
    }
    return total;
}

}  // namespace

TEST_CASE(sharded_engine, same_shard_different_symbols_never_match) {
    ShardedEngine engine(small_test_config(2));
    // 1 % 2 == 3 % 2: both symbols live on shard 1.
    (void)add_order_waiting_for_queue(engine, 1, 10000, 10, Side::BUY);
    (void)add_order_waiting_for_queue(engine, 3, 10000, 10, Side::SELL);
    // A more aggressive cross-symbol pair on shard 0 (0 and 2).
    (void)add_order_waiting_for_queue(engine, 0, 9000, 7, Side::SELL);
    (void)add_order_waiting_for_queue(engine, 2, 11000, 7, Side::BUY);
    engine.flush();

    const lob::OrderBook* symbol1 = engine.find_book(1);
    const lob::OrderBook* symbol3 = engine.find_book(3);
    REQUIRE(symbol1 != nullptr);
    REQUIRE(symbol3 != nullptr);
    CHECK(symbol1 != symbol3);
    CHECK_EQ(symbol1->get_total_orders(), 1u);
    CHECK_EQ(symbol1->get_best_bid().value_or(0), Price{10000});
    CHECK(!symbol1->get_best_ask().has_value());
    CHECK_EQ(symbol3->get_total_orders(), 1u);
    CHECK_EQ(symbol3->get_best_ask().value_or(0), Price{10000});
    CHECK(!symbol3->get_best_bid().has_value());

    const lob::OrderBook* symbol0 = engine.find_book(0);
    const lob::OrderBook* symbol2 = engine.find_book(2);
    REQUIRE(symbol0 != nullptr);
    REQUIRE(symbol2 != nullptr);
    CHECK_EQ(symbol0->get_ask_quantity_at_top(), 7u);
    CHECK_EQ(symbol2->get_bid_quantity_at_top(), 7u);
    CHECK_EQ(engine.client_order_mapping_count(), 4u);

    // Same-symbol orders still match, and the filled resting order's
    // mapping is reclaimed.
    (void)add_order_waiting_for_queue(engine, 1, 10000, 10, Side::SELL);
    engine.flush();
    CHECK_EQ(symbol1->get_total_orders(), 0u);
    CHECK_EQ(symbol3->get_total_orders(), 1u);
    CHECK_EQ(engine.client_order_mapping_count(), 3u);
    CHECK_EQ(engine.book_order_mapping_count(), 3u);
}

TEST_CASE(sharded_engine, cancel_with_other_symbol_handle_cannot_reach_order) {
    ShardedEngine engine(small_test_config(2));
    const auto bid = add_order_waiting_for_queue(engine, 1, 10000, 10, Side::BUY);
    (void)add_order_waiting_for_queue(engine, 3, 9000, 10, Side::BUY);
    engine.flush();

    // Same shard, same client id, wrong symbol: must not touch symbol 1.
    REQUIRE(engine.submit_cancel(ShardedEngine::OrderHandle{3, bid.client_order_id}));
    engine.flush();
    CHECK_EQ(engine.find_book(1)->get_total_orders(), 1u);
    CHECK_EQ(engine.find_book(3)->get_total_orders(), 1u);

    REQUIRE(engine.submit_cancel(bid));
    engine.flush();
    CHECK_EQ(engine.find_book(1)->get_total_orders(), 0u);
    CHECK_EQ(engine.client_order_mapping_count(), 1u);
}

TEST_CASE(sharded_engine, symbols_beyond_shard_capacity_are_rejected) {
    ShardedEngineConfig config = small_test_config(1);
    config.max_symbols_per_shard = 2;
    ShardedEngine engine(config);

    CHECK(engine.submit_add(10, 10000, 1, Side::BUY).has_value());
    CHECK(engine.submit_add(11, 10000, 1, Side::BUY).has_value());
    CHECK(!engine.submit_add(12, 10000, 1, Side::BUY).has_value());
    CHECK(engine.submit_add(10, 9999, 1, Side::BUY).has_value());  // bound symbols still accepted
    CHECK(!engine.submit_cancel(ShardedEngine::OrderHandle{12, 1}));
    CHECK(!engine.submit_modify(ShardedEngine::OrderHandle{12, 1}, 5));
    engine.flush();

    CHECK(engine.find_book(12) == nullptr);
    CHECK_EQ(engine.find_book(10)->get_total_orders(), 2u);
    CHECK_EQ(engine.find_book(11)->get_total_orders(), 1u);
}

TEST_CASE(sharded_engine, resting_orders_beyond_book_capacity_are_cancelled) {
    ShardedEngineConfig config = small_test_config(1);
    config.book_config.max_resting_orders = 4;
    config.book_config.allow_growth = true;  // ignored: shard books are always bounded
    ShardedEngine engine(config);

    for (int i = 0; i < 20; ++i) {
        (void)add_order_waiting_for_queue(engine, 5, 9000 + i, 1, Side::BUY);
    }
    engine.flush();

    CHECK_EQ(engine.client_order_mapping_count(), 4u);
    CHECK_EQ(engine.book_order_mapping_count(), 4u);
    CHECK_EQ(engine.capacity_rejection_count(), 16u);
    // No unaddressable order is left resting.
    CHECK_EQ(engine.find_book(5)->get_total_orders(), 4u);

    // A full book still trades: the aggressor needs no resting slot.
    (void)add_order_waiting_for_queue(engine, 5, 9003, 1, Side::SELL);
    engine.flush();
    CHECK_EQ(engine.find_book(5)->get_total_orders(), 3u);
    CHECK_EQ(engine.client_order_mapping_count(), 3u);
    CHECK_EQ(engine.capacity_rejection_count(), 16u);
}

TEST_CASE(sharded_engine, prices_outside_the_book_band_are_refused_at_submit) {
    ShardedEngine engine(small_test_config(1));  // band [0, 20000]
    CHECK(!engine.submit_add(3, 20001, 1, Side::BUY).has_value());
    CHECK(!engine.submit_add(3, -1, 1, Side::SELL).has_value());
    CHECK(!engine.submit_add(3, Price{1} << 40, 1, Side::BUY).has_value());
    CHECK(engine.find_book(3) == nullptr);  // refused adds bind no symbol

    (void)add_order_waiting_for_queue(engine, 3, 20000, 1, Side::BUY);
    (void)add_order_waiting_for_queue(engine, 3, 0, 1, Side::BUY);
    engine.flush();
    REQUIRE(engine.find_book(3) != nullptr);
    CHECK_EQ(engine.find_book(3)->get_total_orders(), 2u);
    CHECK_EQ(engine.capacity_rejection_count(), 0u);
}

// stop() from another thread while the producer is still submitting: every
// submission that was accepted is applied, later ones are refused, and only
// the producer thread ever writes the SPSC queue.
TEST_CASE(sharded_engine, stop_racing_the_producer_applies_every_accepted_submission) {
    ShardedEngineConfig config = small_test_config(1);
    config.book_config.max_resting_orders = 1 << 16;
    ShardedEngine engine(config);
    std::atomic<std::size_t> accepted{0};
    std::atomic<bool> stop_returned{false};

    std::thread producer([&] {
        for (int i = 0; i < 60000; ++i) {
            for (;;) {
                if (engine.submit_add(9, 10000, 1, Side::BUY)) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (stop_returned.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    });
    while (accepted.load(std::memory_order_relaxed) < 1000) {
        std::this_thread::yield();
    }
    engine.stop();
    stop_returned.store(true, std::memory_order_release);
    producer.join();

    CHECK(!engine.submit_add(9, 10000, 1, Side::BUY).has_value());
    const lob::OrderBook* book = engine.find_book(9);
    REQUIRE(book != nullptr);
    CHECK_EQ(book->get_total_orders(), accepted.load());
    CHECK_EQ(engine.client_order_mapping_count(), accepted.load());
}

TEST_CASE(sharded_engine, cancel_and_modify_reclaim_mappings) {
    ShardedEngine engine(small_test_config(1));
    const auto cancelled = add_order_waiting_for_queue(engine, 7, 10000, 10, Side::BUY);
    const auto reduced = add_order_waiting_for_queue(engine, 7, 9990, 10, Side::BUY);
    const auto emptied = add_order_waiting_for_queue(engine, 7, 9980, 10, Side::BUY);
    engine.flush();
    CHECK_EQ(engine.client_order_mapping_count(), 3u);

    cancel_waiting_for_queue(engine, cancelled);
    modify_waiting_for_queue(engine, reduced, 4);
    modify_waiting_for_queue(engine, emptied, 0);
    engine.flush();

    CHECK_EQ(engine.client_order_mapping_count(), 1u);
    CHECK_EQ(engine.book_order_mapping_count(), 1u);
    const lob::OrderBook* book = engine.find_book(7);
    CHECK_EQ(book->get_total_orders(), 1u);
    CHECK_EQ(book->get_bid_quantity_at_top(), 4u);

    // Partially execute, then modify down to the executed quantity.
    (void)add_order_waiting_for_queue(engine, 7, 9990, 3, Side::SELL);
    engine.flush();
    CHECK_EQ(engine.client_order_mapping_count(), 1u);
    modify_waiting_for_queue(engine, reduced, 3);
    engine.flush();
    CHECK_EQ(engine.client_order_mapping_count(), 0u);
    CHECK_EQ(book->get_total_orders(), 0u);
}

TEST_CASE(sharded_engine, fill_limit_sweep_reclaims_every_filled_order) {
    ShardedEngineConfig config = small_test_config(1);
    config.book_config.max_fills_per_add_order = 8;
    ShardedEngine engine(config);

    for (int i = 0; i < 10; ++i) {
        (void)add_order_waiting_for_queue(engine, 2, 10000 + (i % 3), 1, Side::SELL);
    }
    (void)add_order_waiting_for_queue(engine, 2, 10002, 10, Side::BUY);
    engine.flush();

    const lob::OrderBook* book = engine.find_book(2);
    CHECK_EQ(engine.fill_limit_cancellation_count(), 1u);
    CHECK_EQ(book->get_total_orders(), 2u);
    CHECK(!book->get_best_bid().has_value());  // remainder did not rest
    CHECK_EQ(engine.client_order_mapping_count(), 2u);
    CHECK_EQ(engine.book_order_mapping_count(), 2u);
}

// Finding 37 gate: millions of orders through a small, non-growing mapping
// capacity. Without reclamation of fully executed resting orders the maps
// fill after ~1024 rests per book and every later rest is rejected.
TEST_CASE(sharded_engine, mapping_count_stays_bounded_over_millions_of_orders) {
    ShardedEngine engine(small_test_config(2));
    constexpr SymbolId kSymbols[] = {0, 1, 2, 3};  // two per shard
    constexpr std::size_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);
    constexpr std::size_t kIterations = 500000;    // 4 adds each: 2,000,000 orders
    constexpr std::size_t kCheckEvery = 50000;

    std::size_t submitted_adds = 0;
    std::size_t max_mapping_count_seen = 0;
    bool mappings_matched_book = true;
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const SymbolId symbol = kSymbols[iteration % kSymbolCount];
        const Price price = 10000 + static_cast<Price>(iteration % 8);
        // Rests, then is partially and finally fully executed by two
        // aggressors that never rest themselves.
        (void)add_order_waiting_for_queue(engine, symbol, price, 2, Side::SELL);
        (void)add_order_waiting_for_queue(engine, symbol, price, 1, Side::BUY);
        (void)add_order_waiting_for_queue(engine, symbol, price + 1, 1, Side::BUY);
        // Rests far away, is reduced, then cancelled.
        const auto passive_bid = add_order_waiting_for_queue(engine, symbol, 5000, 5, Side::BUY);
        modify_waiting_for_queue(engine, passive_bid, 3);
        cancel_waiting_for_queue(engine, passive_bid);
        submitted_adds += 4;

        if ((iteration + 1) % kCheckEvery == 0) {
            engine.flush();
            const std::size_t mapping_count = engine.client_order_mapping_count();
            max_mapping_count_seen = std::max(max_mapping_count_seen, mapping_count);
            mappings_matched_book = mappings_matched_book &&
                mapping_count == engine.book_order_mapping_count() &&
                mapping_count == resting_order_total(engine, kSymbols, kSymbolCount);
        }
    }
    engine.flush();

    CHECK_EQ(submitted_adds, 2000000u);
    CHECK(mappings_matched_book);
    CHECK_EQ(max_mapping_count_seen, 0u);
    CHECK_EQ(engine.client_order_mapping_count(), 0u);
    CHECK_EQ(engine.book_order_mapping_count(), 0u);
    CHECK_EQ(engine.capacity_rejection_count(), 0u);
    CHECK_EQ(resting_order_total(engine, kSymbols, kSymbolCount), 0u);
}
