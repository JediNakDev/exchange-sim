#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_GetBestAsk(benchmark::State& state) {
    PrePopulatedBook prepop;
    BenchmarkRunner runner(state, "GetBestAsk");
    runner.run([&](size_t) { return prepop.book().get_best_ask(); });
}

BENCHMARK(BM_GetBestAsk)->Unit(benchmark::kNanosecond)->MinTime(3.0);
