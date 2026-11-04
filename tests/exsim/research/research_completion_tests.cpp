#include "tests/exsim/research/research_test_support.hpp"
#include "tests/framework/test.hpp"
#include "tests/framework/alloc_counter.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/strategies/inventory_aware_market_maker.hpp"
#include "exsim/research/strategies/twap_executor.hpp"
#include "exsim/research/strategies/immediate_sweep_executor.hpp"
#include "exsim/research/run_compare.hpp"
#include "exsim/research/experiment_runner.hpp"
#include <cmath>
using namespace research_test;

TEST_CASE(research_allocations, ledger_ring_and_failure_paths) {
    if (!testfw::allocation_counting_available()) return;
    PositionLedger ledger(100'000'000, 2);
    bool passed = true;
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (int cycle = 0; cycle < 10000 && passed; ++cycle) {
            passed = ledger.record_fill(Side::BUY, 100, 10, 1) == LedgerStatus::Ok &&
                ledger.record_fill(Side::BUY, 110, 5, -1) == LedgerStatus::Ok &&
                ledger.record_fill(Side::BUY, 120, 1) == LedgerStatus::CapacityExceeded &&
                ledger.record_fill(Side::SELL, 120, 20, 2) == LedgerStatus::Ok &&
                ledger.record_fill(Side::BUY, 115, 5) == LedgerStatus::Ok && ledger.valuation(110).has_value();
        }
        allocations = scope.count();
    }
    CHECK(passed);
    CHECK_EQ(allocations, 0u);
}

template <typename Strategy>
void check_runner_allocations(StrategyKind kind) {
    auto config = busy_market_config(2 * kNanosPerSecond);
    config.strategy.kind = kind;
    config.strategy.parent.start_ns = 100 * kMillisecond;
    config.strategy.parent.execution_duration_ns = 500 * kMillisecond;
    config.strategy.parent.slice_interval_ns = 100 * kMillisecond;
    Simulation<Strategy> simulation(config, 5, config.strategy);
    std::size_t allocations = 0;
    RunStop stop;
    {
        testfw::AllocationScope scope;
        do {
            stop = simulation.run();
            simulation.recorder().clear();
        } while (stop == RunStop::FlushRequired);
        allocations = scope.count();
    }
    CHECK(stop == RunStop::Completed);
    CHECK_GT(simulation.metrics().engine_commands, 100u);
    CHECK_EQ(allocations, 0u);
}
TEST_CASE(research_allocations, all_compiled_strategies) {
    if (!testfw::allocation_counting_available()) return;
    check_runner_allocations<strategies::FixedSpreadMarketMaker>(StrategyKind::FixedSpreadMarketMaker);
    check_runner_allocations<strategies::InventoryAwareMarketMaker>(StrategyKind::InventoryAwareMarketMaker);
    check_runner_allocations<strategies::TwapExecutor>(StrategyKind::TwapExecutor);
    check_runner_allocations<strategies::ImmediateSweepExecutor>(StrategyKind::ImmediateSweepExecutor);
}

template <typename Strategy>
void check_execution(StrategyKind kind, Quantity liquidity, Quantity expected) {
    auto config = quiet_market_config();
    config.strategy.kind = kind;
    config.strategy.parent.start_ns = 100 * kMillisecond;
    config.strategy.parent.parent_qty = 100;
    config.strategy.parent.execution_duration_ns = 500 * kMillisecond;
    config.strategy.parent.slice_interval_ns = 100 * kMillisecond;
    Simulation<Strategy> simulation(config, 1, config.strategy);
    REQUIRE(simulation.schedule_scripted_command(0, scripted_limit_order(1, Side::SELL, cents(10000), liquidity)));
    REQUIRE(simulation.schedule_scripted_command(0, scripted_limit_order(2, Side::BUY, cents(9990), 100)));
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_EQ(simulation.metrics().parent.filled_qty, expected);
    CHECK_EQ(simulation.ledger().state().position, expected);
    CHECK_EQ(simulation.metrics().parent.qty - simulation.metrics().parent.filled_qty, 100u - expected);
}
TEST_CASE(research_strategies, twap_completion_and_residual) {
    check_execution<strategies::TwapExecutor>(StrategyKind::TwapExecutor, 1000, 100);
    check_execution<strategies::TwapExecutor>(StrategyKind::TwapExecutor, 35, 35);
}
TEST_CASE(research_strategies, immediate_sweep_completion_and_residual) {
    check_execution<strategies::ImmediateSweepExecutor>(StrategyKind::ImmediateSweepExecutor, 1000, 100);
    check_execution<strategies::ImmediateSweepExecutor>(StrategyKind::ImmediateSweepExecutor, 35, 35);
}
TEST_CASE(research_risk, pending_orders_reserve_inventory_before_reports) {
    auto config = quiet_market_config();
    config.risk.max_long_position = 100;
    config.latency.ack_ns = 500 * kMillisecond;
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(0, scripted_limit_order(1, Side::SELL, cents(10000), 1000)));
    bool sent = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate&, StrategyContext& context) {
        if (sent) return;
        sent = true;
        CHECK_NE(context.submit_limit_order(Side::BUY, cents(10000), 60, TimeInForce::IOC, false), 0u);
        CHECK_EQ(context.submit_limit_order(Side::BUY, cents(10000), 60, TimeInForce::IOC, false), 0u);
        CHECK(context.last_risk_reject() == RiskRejectReason::MaxLongPosition);
        CHECK_EQ(context.known_position(), 0);
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_EQ(simulation.ledger().state().position, 60);
    CHECK_EQ(simulation.metrics().risk_rejects, 1u);
    CHECK_EQ(simulation.metrics().limit_breaches, 0u);
}
TEST_CASE(research_markouts, horizons_past_end_are_censored) {
    auto config = quiet_market_config();
    config.metrics.markout_horizon_count = 2;
    config.metrics.markout_horizons_ns[0] = 10 * kMillisecond;
    config.metrics.markout_horizons_ns[1] = 2 * kNanosPerSecond;
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(0, scripted_limit_order(1, Side::SELL, cents(10000), 1000)));
    REQUIRE(simulation.schedule_scripted_command(0, scripted_limit_order(2, Side::BUY, cents(9990), 1000)));
    bool sent = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (sent || !update.top.two_sided()) return;
        sent = true;
        (void)context.submit_limit_order(Side::BUY, cents(10000), 10, TimeInForce::IOC, false);
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE_EQ(records.markouts.size(), 2u);
    CHECK(records.markouts[0].status == MarkoutStatus::Measured);
    CHECK(records.markouts[1].status == MarkoutStatus::Censored);
    CHECK_EQ(simulation.metrics().markouts[1].measured, 0u);
    CHECK_EQ(simulation.metrics().markouts[1].censored, 1u);
}
TEST_CASE(research_compare, paired_differences_and_sample_dispersion) {
    RunSet a, b;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        LoadedRun left, right;
        left.master_seed = right.master_seed = seed;
        left.state = right.state = "completed";
        left.market_world = right.market_world = "interactive_synthetic";
        left.metrics = {{"net_pnl", 10.0}};
        right.metrics = {{"net_pnl", 10.0 + static_cast<double>(seed)}};
        a.runs.push_back(left);
        b.runs.push_back(right);
    }
    const auto report = compare_run_sets(a, b);
    REQUIRE_EQ(report.metrics.size(), 1u);
    CHECK_EQ(report.metrics[0].pairs, 3u);
    CHECK_EQ(report.metrics[0].mean_difference, 2.0);
    REQUIRE(report.metrics[0].sd_difference.has_value());
    CHECK_EQ(*report.metrics[0].sd_difference, 1.0);
    CHECK(!render_comparison_text(report).empty());
    CHECK(!render_comparison_json(report).empty());
}
TEST_CASE(research_config, durations_seeds_and_unknown_keys) {
    Timestamp duration = 0;
    CHECK(parse_duration("12us", duration));
    CHECK_EQ(duration, 12000u);
    CHECK(!parse_duration("-1ns", duration));
    std::vector<std::uint64_t> seeds;
    CHECK(parse_seed_range("1..5", seeds));
    CHECK_EQ(seeds.size(), 5u);
    CHECK(!parse_seed_range("5..1", seeds));
    bool rejected = false;
    try { (void)parse_experiment_config("[latency]\nunknown = 1ns\n"); }
    catch (const ConfigError&) { rejected = true; }
    CHECK(rejected);
}
