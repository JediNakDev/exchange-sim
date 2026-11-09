#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_CancelOrder(benchmark::State& state) {
    std::unique_ptr<PrePopulatedBook> prepop;
    const std::vector<lob::OrderId>* ids = nullptr;

    BenchmarkRunner runner(state, "CancelOrder");
    runner.run_with_setup(
        [&] {
            prepop = std::make_unique<PrePopulatedBook>(500, 10);
            ids = &prepop->ids();
        },
        [&](size_t i) {
            if (i < ids->size()) {
                return prepop->book().cancel_order((*ids)[i]);
            }
            return false;
        });
}

BENCHMARK(BM_CancelOrder)->Unit(benchmark::kNanosecond)->MinTime(3.0);
