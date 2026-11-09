#include "../utils/constants.hpp"
#include "../utils/csv_writer.hpp"
#include "../utils/stats.hpp"
#include "../utils/warmup.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

static void BM_MixedWorkload(benchmark::State& state) {
    warmup();
    const auto& w = workload();
    std::vector<double> latencies;
    latencies.reserve(BENCHMARK_SAMPLES);

    for (auto _ : state) {
        state.PauseTiming();
        PrePopulatedBook prepop(30, 5);
        auto& book = prepop.book();
        const auto& ids = prepop.ids();
        std::vector<lob::OrderId> active_ids(ids);
        latencies.clear();
        size_t idx = 0;
        state.ResumeTiming();

        auto batch_start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < BENCHMARK_SAMPLES; ++i) {
            int op = (idx + i) % 100;

            auto start = std::chrono::high_resolution_clock::now();

            if (op < 60) {
                if (op % 3 == 0) {
                    benchmark::DoNotOptimize(book.get_best_bid());
                } else if (op % 3 == 1) {
                    benchmark::DoNotOptimize(book.get_best_ask());
                } else {
                    benchmark::DoNotOptimize(book.get_spread());
                }
            } else if (op < 85) {
                const auto& order = w.get(idx + i);
                auto result = book.add_order(order.price, order.quantity, order.side);
                benchmark::DoNotOptimize(result);
                if (active_ids.size() < 10000) {
                    active_ids.push_back(result.order_id);
                }
            } else if (op < 95) {
                if (!active_ids.empty()) {
                    size_t cancel_idx = w.cancel_index(i) % active_ids.size();
                    benchmark::DoNotOptimize(book.cancel_order(active_ids[cancel_idx]));
                }
            } else {
                if (!active_ids.empty()) {
                    size_t mod_idx = (idx + i) % active_ids.size();
                    benchmark::DoNotOptimize(book.modify_order(active_ids[mod_idx], w.modify_quantity(i)));
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::nano>(end - start).count());
            benchmark::ClobberMemory();
        }

        auto batch_end = std::chrono::high_resolution_clock::now();
        double batch_time_sec = std::chrono::duration<double>(batch_end - batch_start).count();

        auto stats = Stats::compute(latencies);
        stats.throughput = BENCHMARK_SAMPLES / batch_time_sec;
        stats.report(state);
        state.counters["Throughput_ops_sec"] = stats.throughput;

        if (csv()) csv()->write("MixedWorkload", stats);

        idx += BENCHMARK_SAMPLES;
    }

    state.SetItemsProcessed(state.iterations() * BENCHMARK_SAMPLES);
    state.SetLabel("60% query, 25% add, 10% cancel, 5% modify");
}

BENCHMARK(BM_MixedWorkload)->Unit(benchmark::kNanosecond)->MinTime(5.0);
