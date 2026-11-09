#ifndef TESTS_EXSIM_RESEARCH_RESEARCH_TEST_SUPPORT_HPP
#define TESTS_EXSIM_RESEARCH_RESEARCH_TEST_SUPPORT_HPP

// Shared fixtures for the research runner tests: deterministic market
// configurations, a hook-driven test strategy, and record collection.

#include "research/experiment_config.hpp"
#include "research/run_records.hpp"
#include "research/simulation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace research_test {

using namespace exsim;
using namespace exsim::research;

inline constexpr SessionId kScriptedSession = 50;
inline constexpr Timestamp kMicrosecond = kNanosPerMicrosecond;
inline constexpr Timestamp kMillisecond = kNanosPerMillisecond;

// Price in cents (tick 0.01) to fixed-point price units.
[[nodiscard]] constexpr Price cents(std::int64_t value) noexcept { return value * 100; }

// No background agents, no jitter, every latency component distinct.
[[nodiscard]] inline ExperimentConfig quiet_market_config() {
    ExperimentConfig config = default_experiment_config();
    config.name = "research_test_quiet";
    config.limits.duration_ns = 1 * kNanosPerSecond;
    config.latency = LatencyModel{};
    config.latency.feed_ns = 50 * kMicrosecond;
    config.latency.order_ns = 100 * kMicrosecond;
    config.latency.cancel_ns = 70 * kMicrosecond;
    config.latency.ack_ns = 30 * kMicrosecond;
    config.latency.strategy_compute_ns = 5 * kMicrosecond;
    config.latency.order_channel_fifo = true;
    config.fees.maker_fee_per_share = -20;  // -0.002
    config.fees.taker_fee_per_share = 30;   //  0.003
    config.metrics.sample_interval_ns = 10 * kMillisecond;
    return config;
}

// The config/research example market: seeded market maker and noise taker, with jitter.
[[nodiscard]] inline ExperimentConfig busy_market_config(Timestamp duration_ns) {
    ExperimentConfig config = quiet_market_config();
    config.name = "research_test_busy";
    config.limits.duration_ns = duration_ns;
    config.latency.feed_jitter_ns = 10 * kMicrosecond;
    config.latency.order_jitter_ns = 20 * kMicrosecond;
    config.latency.cancel_jitter_ns = 20 * kMicrosecond;
    config.latency.ack_jitter_ns = 20 * kMicrosecond;
    config.metrics.sample_interval_ns = 100 * kMillisecond;
    config.risk.max_long_position = 500;
    config.risk.max_short_position = 500;
    config.risk.max_order_qty = 1'000;
    config.risk.max_open_orders = 8;

    BackgroundAgentConfig maker;
    maker.kind = BackgroundAgentKind::MarketMaker;
    maker.name = "liquidity";
    maker.market_maker.levels = 3;
    maker.market_maker.size = 100;
    maker.market_maker.spread_ticks = 1;
    maker.market_maker.fair_value_step_ms = 500;
    maker.market_maker.requote_ms = 100;
    BackgroundAgentConfig taker;
    taker.kind = BackgroundAgentKind::NoiseTaker;
    taker.name = "flow";
    taker.noise_taker.mean_interval_ms = 20;
    taker.noise_taker.min_size = 1;
    taker.noise_taker.max_size = 200;
    taker.noise_taker.limit_ppm = 100'000;
    config.agents = {maker, taker};
    return config;
}

[[nodiscard]] inline Command scripted_limit_order(ClOrdId cl_ord_id, Side side, Price price, Quantity qty,
                                                  TimeInForce tif = TimeInForce::Day) {
    NewOrder order;
    order.instrument = kResearchInstrumentId;
    order.session = kScriptedSession;
    order.owner = kScriptedSession;
    order.cl_ord_id = cl_ord_id;
    order.side = side;
    order.type = OrdType::Limit;
    order.tif = tif;
    order.stp = StpMode::None;
    order.price = price;
    order.qty = qty;
    return Command{order};
}

// Test strategy whose callbacks are std::function hooks set by each test.
struct HookStrategy {
    static constexpr std::string_view kStrategyName = "research_test_hook_strategy";
    static constexpr std::uint32_t kStrategyVersion = 1;

    std::function<void(StrategyContext&)> start;
    std::function<void(const MarketDataUpdate&, StrategyContext&)> market_data;
    std::function<void(const ExecutionReport&, StrategyContext&)> report;
    std::function<void(StrategyContext&)> wakeup;

    void on_start(StrategyContext& context) {
        if (start) start(context);
    }
    void on_market_data(const MarketDataUpdate& update, StrategyContext& context) {
        if (market_data) market_data(update, context);
    }
    void on_execution_report(const ExecutionReport& execution_report, StrategyContext& context) {
        if (report) report(execution_report, context);
    }
    void on_wakeup(StrategyContext& context) {
        if (wakeup) wakeup(context);
    }
};

struct CollectedRecords {
    std::vector<CommandRecord> commands;
    std::vector<LifecycleRecord> lifecycle;
    std::vector<FillRecord> fills;
    std::vector<SeriesSample> samples;
    std::vector<MarkoutRecord> markouts;

    void take(RunRecorder& recorder) {
        for (const auto& record : recorder.commands.records()) commands.push_back(record);
        for (const auto& record : recorder.lifecycle.records()) lifecycle.push_back(record);
        for (const auto& record : recorder.fills.records()) fills.push_back(record);
        for (const auto& record : recorder.samples.records()) samples.push_back(record);
        for (const auto& record : recorder.markouts.records()) markouts.push_back(record);
        recorder.clear();
    }
};

// Runs (or resumes) until a terminal state, draining the recorder on demand.
template <typename Strategy>
RunStop run_to_end(Simulation<Strategy>& simulation, CollectedRecords* records = nullptr) {
    while (true) {
        const RunStop stop = simulation.run();
        if (records != nullptr) records->take(simulation.recorder());
        else simulation.recorder().clear();
        if (stop != RunStop::FlushRequired) return stop;
    }
}

// Like Simulation::run_until, but drains the recorder when it fills.
template <typename Strategy>
RunStop run_until_time(Simulation<Strategy>& simulation, Timestamp time, CollectedRecords* records = nullptr) {
    while (true) {
        const RunStop stop = simulation.run_until(time);
        if (records != nullptr) records->take(simulation.recorder());
        else simulation.recorder().clear();
        if (stop != RunStop::FlushRequired) return stop;
    }
}

// A fresh, empty directory under the system temporary directory.
[[nodiscard]] inline std::filesystem::path fresh_test_directory(std::string_view name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("exsim_research_tests_" + std::to_string(::getpid()) + "_" + std::string(name));
    std::filesystem::remove_all(directory);
    return directory;
}

}  // namespace research_test

#endif
