#pragma once

#include <cstddef>
#include <cstdint>

namespace bench {

// Prices in ticks/cents (100.00 = 10000 ticks, 0.01 = 1 tick)
constexpr int64_t BASE_PRICE = 10000;  // 100.00
constexpr int64_t TICK_SIZE = 1;        // 0.01
constexpr int PRICE_LEVELS = 100;
constexpr int ORDERS_PER_LEVEL = 10;
constexpr size_t WARMUP_ITERATIONS = 10000;
constexpr size_t BENCHMARK_SAMPLES = 100000;

}
