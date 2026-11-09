#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

#include <string>

using namespace bench;

static void BM_GetSnapshot(benchmark::State& state) {
    PrePopulatedBook prepop;
    int depth = state.range(0);
    BenchmarkRunner runner(state, "GetSnapshot_depth" + std::to_string(depth));
    runner.run([&](size_t) { return prepop.book().get_snapshot(depth); });
}

BENCHMARK(BM_GetSnapshot)->Arg(5)->Arg(10)->Arg(20)->Unit(benchmark::kNanosecond)->MinTime(3.0);
