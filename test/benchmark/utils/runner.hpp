#pragma once

#include "constants.hpp"
#include "csv_writer.hpp"
#include "stats.hpp"
#include "warmup.hpp"
#include <benchmark/benchmark.h>

#include <chrono>
#include <string>
#include <vector>

namespace bench {

class BenchmarkRunner {
public:
    explicit BenchmarkRunner(benchmark::State& state, const std::string& name)
        : state_(state), name_(name) {
        warmup();
        latencies_.reserve(BENCHMARK_SAMPLES);
    }

    template <typename Func>
    void run(Func&& operation) {
        for (auto _ : state_) {
            latencies_.clear();

            for (size_t i = 0; i < BENCHMARK_SAMPLES; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                auto result = operation(i);
                auto end = std::chrono::high_resolution_clock::now();

                benchmark::DoNotOptimize(result);
                latencies_.push_back(
                    std::chrono::duration<double, std::nano>(end - start).count());
            }
        }

        finish();
    }

    template <typename SetupFunc, typename Func>
    void run_with_setup(SetupFunc&& setup, Func&& operation) {
        for (auto _ : state_) {
            state_.PauseTiming();
            setup();
            latencies_.clear();
            state_.ResumeTiming();

            for (size_t i = 0; i < BENCHMARK_SAMPLES; ++i) {
                auto start = std::chrono::high_resolution_clock::now();
                auto result = operation(i);
                auto end = std::chrono::high_resolution_clock::now();

                benchmark::DoNotOptimize(result);
                latencies_.push_back(
                    std::chrono::duration<double, std::nano>(end - start).count());
            }
        }

        finish();
    }

    void set_samples(size_t samples) { samples_ = samples; }

    void add_counter(const std::string& name, double value) {
        state_.counters[name] = value;
    }

    std::vector<double>& latencies() { return latencies_; }
    benchmark::State& state() { return state_; }

private:
    void finish() {
        auto stats = Stats::compute(latencies_);
        stats.report(state_);
        if (csv()) csv()->write(name_, stats);
        state_.SetItemsProcessed(state_.iterations() * samples_);
    }

    benchmark::State& state_;
    std::string name_;
    std::vector<double> latencies_;
    size_t samples_ = BENCHMARK_SAMPLES;
};

} 
