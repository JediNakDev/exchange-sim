#pragma once

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace bench {

struct Stats {
    double mean = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double p999 = 0;
    double p9999 = 0;
    double min_val = 0;
    double max_val = 0;
    double stddev = 0;
    size_t count = 0;
    double throughput = 0;

    static Stats compute(std::vector<double>& samples) {
        Stats s;
        if (samples.empty()) return s;

        s.count = samples.size();
        std::sort(samples.begin(), samples.end());

        s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / s.count;
        s.p50 = samples[s.count / 2];
        s.p95 = samples[std::min(static_cast<size_t>(s.count * 0.95), s.count - 1)];
        s.p99 = samples[std::min(static_cast<size_t>(s.count * 0.99), s.count - 1)];
        s.p999 = samples[std::min(static_cast<size_t>(s.count * 0.999), s.count - 1)];
        s.p9999 = samples[std::min(static_cast<size_t>(s.count * 0.9999), s.count - 1)];
        s.min_val = samples.front();
        s.max_val = samples.back();

        double sq_sum = 0.0;
        for (double v : samples) {
            sq_sum += (v - s.mean) * (v - s.mean);
        }
        s.stddev = std::sqrt(sq_sum / s.count);

        if (s.mean > 0) {
            s.throughput = 1e9 / s.mean;
        }

        return s;
    }

    void report(benchmark::State& state) const {
        state.counters["Mean_ns"] = mean;
        state.counters["P50_ns"] = p50;
        state.counters["P95_ns"] = p95;
        state.counters["P99_ns"] = p99;
        state.counters["P99.9_ns"] = p999;
        state.counters["P99.99_ns"] = p9999;
        state.counters["Min_ns"] = min_val;
        state.counters["Max_ns"] = max_val;
        state.counters["StdDev_ns"] = stddev;
        state.counters["Throughput"] = benchmark::Counter(
            throughput, benchmark::Counter::kDefaults, benchmark::Counter::kIs1000);
    }
};

} 
