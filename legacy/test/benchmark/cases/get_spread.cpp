#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_GetSpread(benchmark::State& state) {
    PrePopulatedBook prepop;
    BenchmarkRunner runner(state, "GetSpread");
    runner.run([&](size_t) { return prepop.book().get_spread(); });
}

BENCHMARK(BM_GetSpread)->Unit(benchmark::kNanosecond)->MinTime(3.0);
