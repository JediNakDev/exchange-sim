#pragma once

#include "constants.hpp"
#include "workload.hpp"
#include <benchmark/benchmark.h>
#include <lob/order_book.hpp>

#include <atomic>

namespace bench {

inline void warmup() {
    lob::OrderBook book;
    const auto& w = workload();

    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        const auto& order = w.get(i);
        auto result = book.add_order(order.price, order.quantity, order.side);
        benchmark::DoNotOptimize(result);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

} 
