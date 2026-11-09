#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_ModifyOrder(benchmark::State& state) {
    const auto& w = workload();
    std::unique_ptr<PrePopulatedBook> prepop;
    const std::vector<lob::OrderId>* ids = nullptr;

    BenchmarkRunner runner(state, "ModifyOrder");
    runner.run_with_setup(
        [&] {
            prepop = std::make_unique<PrePopulatedBook>(100, 10);
            ids = &prepop->ids();
        },
        [&](size_t i) {
            size_t order_idx = i % ids->size();
            return prepop->book().modify_order((*ids)[order_idx], w.modify_quantity(i));
        });
}

BENCHMARK(BM_ModifyOrder)->Unit(benchmark::kNanosecond)->MinTime(3.0);
