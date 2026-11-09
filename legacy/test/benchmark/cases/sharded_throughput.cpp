#include "../utils/constants.hpp"
#include "../utils/csv_writer.hpp"
#include "../utils/stats.hpp"
#include "../utils/workload.hpp"
#include <benchmark/benchmark.h>
#include <lob/engine/sharded_engine.hpp>
#include <chrono>

using namespace bench;

static void BM_ShardedThroughput(benchmark::State& state) {
    const std::size_t shards = static_cast<std::size_t>(state.range(0));
    const std::size_t batch = static_cast<std::size_t>(state.range(1));
    const auto& w = workload();

    for (auto _ : state) {
        state.PauseTiming();
        lob::engine::ShardedEngine engine(shards, batch, true);
        state.ResumeTiming();

        std::size_t submitted = 0;
        for (std::size_t i = 0; i < BENCHMARK_SAMPLES; ++i) {
            const auto& order = w.get(i);
            const lob::engine::SymbolId symbol = static_cast<lob::engine::SymbolId>(i % (shards * 8));
            while (!engine.submit_add(symbol, order.price, order.quantity, order.side).has_value()) {
                benchmark::ClobberMemory();
            }
            ++submitted;
        }
        engine.flush();

        state.PauseTiming();
        engine.stop();
        state.ResumeTiming();

        state.counters["Shards"] = static_cast<double>(shards);
        state.counters["BatchSize"] = static_cast<double>(batch);
        state.SetItemsProcessed(state.items_processed() + static_cast<int64_t>(submitted));
    }
}

static void BM_ShardedEndToEndLatency(benchmark::State& state) {
    const std::size_t shards = static_cast<std::size_t>(state.range(0));
    const std::size_t batch = static_cast<std::size_t>(state.range(1));
    const auto& w = workload();
    constexpr std::size_t kLatencySamples = 10000;
    std::vector<double> latencies;
    latencies.reserve(kLatencySamples);

    for (auto _ : state) {
        state.PauseTiming();
        lob::engine::ShardedEngine engine(shards, batch, true);
        latencies.clear();
        state.ResumeTiming();

        for (std::size_t i = 0; i < kLatencySamples; ++i) {
            const auto& order = w.get(i);
            const lob::engine::SymbolId symbol = static_cast<lob::engine::SymbolId>(i % (shards * 8));
            const auto start = std::chrono::high_resolution_clock::now();
            while (!engine.submit_add(symbol, order.price, order.quantity, order.side).has_value()) {
                benchmark::ClobberMemory();
            }
            engine.flush();
            const auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration<double, std::nano>(end - start).count());
        }

        state.PauseTiming();
        engine.stop();
        state.ResumeTiming();

        auto stats = Stats::compute(latencies);
        stats.report(state);
        state.counters["P95_ns"] = stats.p95;
        state.counters["Shards"] = static_cast<double>(shards);
        state.counters["BatchSize"] = static_cast<double>(batch);
        if (csv()) {
            csv()->write("ShardedE2ELatency", stats);
        }
        state.SetItemsProcessed(state.items_processed() + static_cast<int64_t>(kLatencySamples));
    }
}

BENCHMARK(BM_ShardedThroughput)
    ->Args({1, 64})
    ->Args({2, 64})
    ->Args({4, 64})
    ->Args({8, 64})
    ->Args({1, 256})
    ->Args({2, 256})
    ->Args({4, 256})
    ->Args({8, 256})
    ->Unit(benchmark::kNanosecond)
    ->MinTime(2.0);

BENCHMARK(BM_ShardedEndToEndLatency)
    ->Args({1, 256})
    ->Args({2, 256})
    ->Args({4, 256})
    ->Unit(benchmark::kNanosecond)
    ->MinTime(2.0);
