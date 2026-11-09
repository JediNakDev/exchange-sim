#include "../utils/runner.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_GetBestBid(benchmark::State& state) {
    PrePopulatedBook prepop;
    BenchmarkRunner runner(state, "GetBestBid");
    runner.run([&](size_t) { return prepop.book().get_best_bid(); });
}

BENCHMARK(BM_GetBestBid)->Unit(benchmark::kNanosecond)->MinTime(3.0);
