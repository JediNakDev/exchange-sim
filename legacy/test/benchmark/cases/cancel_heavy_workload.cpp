#include "../utils/constants.hpp"
#include "../utils/csv_writer.hpp"
#include "../utils/stats.hpp"
#include "../utils/warmup.hpp"
#include "../utils/workload.hpp"
#include <lob/order_book.hpp>

using namespace bench;

// Realistic exchange workload: 93% cancel, 5% add, 2% modify.
// Real exchange traffic is dominated by cancellations — most orders are
// cancelled before execution. This benchmark reflects that pattern.
static void BM_CancelHeavyWorkload(benchmark::State& state) {
    warmup();
    const auto& w = workload();
    std::vector<double> latencies;
    latencies.reserve(BENCHMARK_SAMPLES);

    for (auto _ : state) {
        state.PauseTiming();
        PrePopulatedBook prepop(50, 10);
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

            if (op < 93) {
                // 93% cancel
                if (!active_ids.empty()) {
                    size_t cancel_idx = w.cancel_index(i) % active_ids.size();
                    auto result = book.cancel_order(active_ids[cancel_idx]);
                    benchmark::DoNotOptimize(result);
                    if (result) {
                        active_ids[cancel_idx] = active_ids.back();
                        active_ids.pop_back();
                    }
                }
            } else if (op < 98) {
                // 5% add — replenish the book
                const auto& order = w.get(idx + i);
                auto result = book.add_order(order.price, order.quantity, order.side);
                benchmark::DoNotOptimize(result);
                if (result.remaining_quantity > 0) {
                    active_ids.push_back(result.order_id);
                }
            } else {
                // 2% modify
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

        if (csv()) csv()->write("CancelHeavyWorkload", stats);

        idx += BENCHMARK_SAMPLES;
    }

    state.SetItemsProcessed(state.iterations() * BENCHMARK_SAMPLES);
    state.SetLabel("93% cancel, 5% add, 2% modify");
}

BENCHMARK(BM_CancelHeavyWorkload)->Unit(benchmark::kNanosecond)->MinTime(5.0);
