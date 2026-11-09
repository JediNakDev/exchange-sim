// Compiled baseline strategies: quote placement from the visible mid, the
// inventory skew, and parent-order execution metrics.

#include "research/strategies/fixed_spread_market_maker.hpp"
#include "research/strategies/inventory_aware_market_maker.hpp"
#include "research/strategies/twap_executor.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

using namespace research_test;

namespace {

// Tight scripted book (99.99 / 100.01) inside the strategy's 2-tick half
// spread, so the strategy's own quotes never move the visible mid (100.00).
ExperimentConfig quoting_config(StrategyKind kind) {
    ExperimentConfig config = quiet_market_config();
    config.strategy.kind = kind;
    config.strategy.quoting.half_spread_ticks = 2;
    config.strategy.quoting.quote_size = 10;
    config.strategy.quoting.requote_interval_ns = 50 * kMillisecond;
    config.strategy.inventory.skew_unit = 10;
    config.strategy.inventory.skew_ticks_per_unit = 1;
    config.risk.max_long_position = 100;
    config.risk.max_short_position = 100;
    return config;
}

void schedule_tight_book_and_hit_the_strategy_bid(SimulationCore& core) {
    REQUIRE(core.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::BUY, cents(9999), 100)));
    REQUIRE(core.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(2, Side::SELL, cents(10001), 100)));
    // Sweeps the scripted 99.99 bid and then the strategy's 99.98 x 10 bid.
    REQUIRE(core.schedule_scripted_command(100 * kMillisecond, scripted_limit_order(3, Side::SELL, cents(9998), 110)));
}

struct QuoteRequest {
    CommandType request = CommandType::NewOrder;
    Side side = Side::BUY;
    Price price = 0;
    Quantity qty = 0;
};

std::vector<QuoteRequest> requests_between(const CollectedRecords& records, Timestamp from, Timestamp to) {
    std::vector<QuoteRequest> requests;
    for (const auto& row : records.lifecycle)
        if (row.stage == LifecycleStage::StrategySubmitted && row.time >= from && row.time < to)
            requests.push_back({row.request, row.side, row.price, row.qty});
    return requests;
}

}  // namespace

TEST_CASE(research_strategies, fixed_spread_quotes_around_the_visible_mid) {
    const ExperimentConfig config = quoting_config(StrategyKind::FixedSpreadMarketMaker);
    Simulation<strategies::FixedSpreadMarketMaker> simulation(config, 1, config.strategy);
    schedule_tight_book_and_hit_the_strategy_bid(simulation);
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);

    const auto opening = requests_between(records, 0, 50 * kMillisecond);
    REQUIRE_EQ(opening.size(), 2u);
    CHECK(opening[0].side == Side::BUY);
    CHECK_EQ(opening[0].price, cents(9998));
    CHECK_EQ(opening[0].qty, 10u);
    CHECK(opening[1].side == Side::SELL);
    CHECK_EQ(opening[1].price, cents(10002));
    CHECK_EQ(opening[1].qty, 10u);

    // After the bid fill the mid is unchanged, so the bid is simply replaced.
    const auto after_fill = requests_between(records, 100 * kMillisecond, 150 * kMillisecond);
    REQUIRE(!after_fill.empty());
    CHECK(after_fill[0].request == CommandType::NewOrder);
    CHECK(after_fill[0].side == Side::BUY);
    CHECK_EQ(after_fill[0].price, cents(9998));
    CHECK_EQ(after_fill[0].qty, 10u);
    CHECK_EQ(simulation.ledger().state().position, 10);
}

TEST_CASE(research_strategies, inventory_skew_lowers_quotes_and_shrinks_the_adding_side) {
    const ExperimentConfig config = quoting_config(StrategyKind::InventoryAwareMarketMaker);
    Simulation<strategies::InventoryAwareMarketMaker> simulation(config, 1, config.strategy);
    schedule_tight_book_and_hit_the_strategy_bid(simulation);
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);

    const auto opening = requests_between(records, 0, 50 * kMillisecond);
    REQUIRE_EQ(opening.size(), 2u);  // flat: identical to the fixed-spread quotes
    CHECK_EQ(opening[0].price, cents(9998));
    CHECK_EQ(opening[1].price, cents(10002));

    // Long 10 = one skew unit: the center drops one tick to 99.99, so the bid
    // is 99.97 and the ask 100.01; the bid (adding) side shrinks to
    // 10 * (100 - 10) / 100 = 9, the ask (reducing) side keeps 10.
    const auto after_fill = requests_between(records, 100 * kMillisecond, 150 * kMillisecond);
    bool saw_bid = false;
    bool saw_ask = false;
    for (const auto& request : after_fill) {
        if (request.side == Side::BUY && !saw_bid) {
            saw_bid = true;
            CHECK(request.request == CommandType::NewOrder);
            CHECK_EQ(request.price, cents(9997));
            CHECK_EQ(request.qty, 9u);
        }
        if (request.side == Side::SELL && !saw_ask) {
            saw_ask = true;
            CHECK(request.request == CommandType::Replace);
            CHECK_EQ(request.price, cents(10001));
            CHECK_EQ(request.qty, 10u);
        }
    }
    CHECK(saw_bid);
    CHECK(saw_ask);
}

TEST_CASE(research_strategies, twap_reports_shortfall_against_the_arrival_mid) {
    ExperimentConfig config = quiet_market_config();
    config.strategy.kind = StrategyKind::TwapExecutor;
    config.strategy.parent.side = Side::BUY;
    config.strategy.parent.parent_qty = 1'000;
    config.strategy.parent.start_ns = 100 * kMillisecond;
    config.strategy.parent.execution_duration_ns = 500 * kMillisecond;
    config.strategy.parent.slice_interval_ns = 100 * kMillisecond;
    config.strategy.parent.limit_offset_ticks = 0;
    Simulation<strategies::TwapExecutor> simulation(config, 1, config.strategy);
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::BUY, cents(9990), 100)));
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(2, Side::SELL, cents(10010), 300)));
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_EQ(simulation.strategy().slices_fired(), 5u);

    const ParentOrderTracking& parent = simulation.metrics().parent;
    REQUIRE(parent.declared);
    CHECK_EQ(parent.decision_time, 100 * kMillisecond);
    REQUIRE(parent.has_decision_mid);
    CHECK_EQ(parent.decision_mid_times_two, cents(9990) + cents(10010));
    CHECK_EQ(parent.filled_qty, 300u);  // only 300 offered: the rest is residual
    CHECK_EQ(parent.fill_notional, 300 * cents(10010));
    CHECK_EQ(parent.fees, 300 * config.fees.taker_fee_per_share);
    CHECK_EQ(parent.opposite_side_qty, 0u);
    // Price shortfall: 300 * (100.10 - 100.00) = 30.00 (doubled: 600000 units).
    CHECK_EQ(2 * parent.fill_notional - parent.decision_mid_times_two * 300, 600'000);
}
