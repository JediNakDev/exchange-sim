#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <book/order_book.hpp>
#include <memory>

using namespace bench;

static void BM_AddOrder(benchmark::State& state) {
    const auto& w = bench::workload();
    std::unique_ptr<exsim::book::OrderBook> book;
    size_t idx = 0;

    BenchmarkRunner runner(state, "AddOrder");
    runner.run_with_setup(
        [&] {
            exsim::book::OrderBookConfig config;
            config.max_resting_orders = BENCHMARK_SAMPLES;
            book = std::make_unique<exsim::book::OrderBook>(config);
            idx = 0;
        },
        [&](size_t i) {
            const auto& order = w.get(idx++);
            exsim::book::Price price = order.side == exsim::book::Side::BUY
                               ? BASE_PRICE - 50 * TICK_SIZE - static_cast<exsim::book::Price>(i % 50) * TICK_SIZE
                               : BASE_PRICE + 50 * TICK_SIZE + static_cast<exsim::book::Price>(i % 50) * TICK_SIZE;
            return book->add_order(price, order.quantity, order.side);
        });
}

BENCHMARK(BM_AddOrder)->Unit(benchmark::kNanosecond)->MinTime(3.0);
