// Pre-trade risk in the strategy host: every reject reason fires before the
// request leaves the strategy, is counted, and is recorded in the lifecycle.

#include "tests/exsim/research/research_test_support.hpp"
#include "tests/framework/test.hpp"

using namespace research_test;

TEST_CASE(research_risk, every_host_side_reject_reason_is_enforced_and_recorded) {
    ExperimentConfig config = quiet_market_config();
    config.risk.max_order_qty = 100;
    config.risk.max_open_orders = 3;
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::BUY, cents(9990), 100)));
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(2, Side::SELL, cents(10010), 100)));
    bool tested = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (tested || !update.top.two_sided()) return;
        tested = true;
        const auto rejected_with = [&](RiskRejectReason reason) { return context.last_risk_reject() == reason; };
        CHECK_EQ(context.submit_limit_order(Side::BUY, cents(9900), 0, TimeInForce::Day, false), 0u);
        CHECK(rejected_with(RiskRejectReason::InvalidQuantity));
        CHECK_EQ(context.submit_limit_order(Side::BUY, cents(9900) + 1, 10, TimeInForce::Day, false), 0u);
        CHECK(rejected_with(RiskRejectReason::InvalidPrice));  // off the tick grid
        CHECK_EQ(context.submit_limit_order(Side::BUY, cents(20000), 10, TimeInForce::Day, false), 0u);
        CHECK(rejected_with(RiskRejectReason::InvalidPrice));  // outside the band
        CHECK_EQ(context.submit_limit_order(Side::BUY, cents(9900), 101, TimeInForce::Day, false), 0u);
        CHECK(rejected_with(RiskRejectReason::MaxOrderQuantity));
        CHECK(!context.cancel_order(12345));
        CHECK(rejected_with(RiskRejectReason::UnknownOrder));

        const ClOrdId first = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
        REQUIRE(first != 0);
        CHECK(context.last_risk_reject() == RiskRejectReason::None);
        CHECK(!context.replace_order(first, cents(9901), 10));
        CHECK(rejected_with(RiskRejectReason::NotYetAcknowledged));
        CHECK(context.cancel_order(first));  // allowed while pending
        CHECK(!context.cancel_order(first));
        CHECK(rejected_with(RiskRejectReason::RequestInFlight));

        CHECK_NE(context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false), 0u);
        CHECK_NE(context.submit_limit_order(Side::SELL, cents(10100), 10, TimeInForce::Day, false), 0u);
        CHECK_EQ(context.open_order_count(), 3u);  // the canceling order still counts
        CHECK_EQ(context.submit_limit_order(Side::SELL, cents(10100), 10, TimeInForce::Day, false), 0u);
        CHECK(rejected_with(RiskRejectReason::MaxOpenOrders));
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE(tested);

    const RunMetrics& metrics = simulation.metrics();
    const auto count = [&](RiskRejectReason reason) { return metrics.risk_rejects_by_reason[static_cast<std::size_t>(reason)]; };
    CHECK_EQ(metrics.risk_rejects, 8u);
    CHECK_EQ(count(RiskRejectReason::InvalidQuantity), 1u);
    CHECK_EQ(count(RiskRejectReason::InvalidPrice), 2u);
    CHECK_EQ(count(RiskRejectReason::MaxOrderQuantity), 1u);
    CHECK_EQ(count(RiskRejectReason::UnknownOrder), 1u);
    CHECK_EQ(count(RiskRejectReason::NotYetAcknowledged), 1u);
    CHECK_EQ(count(RiskRejectReason::RequestInFlight), 1u);
    CHECK_EQ(count(RiskRejectReason::MaxOpenOrders), 1u);
    CHECK_EQ(metrics.new_orders_submitted, 3u);  // rejected requests never reach the exchange
    CHECK_EQ(metrics.orders_accepted, 3u);
    std::uint64_t risk_rejected_rows = 0;
    for (const auto& row : records.lifecycle)
        if (row.stage == LifecycleStage::RiskRejected) ++risk_rejected_rows;
    CHECK_EQ(risk_rejected_rows, 8u);

    // Outside a strategy callback nothing can be sent.
    CHECK_EQ(simulation.strategy_submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false), 0u);
    CHECK(simulation.strategy_view().last_risk_reject == RiskRejectReason::RunNotActive);
}

TEST_CASE(research_risk, short_limit_counts_in_flight_sells) {
    ExperimentConfig config = quiet_market_config();
    config.risk.max_short_position = 100;
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::BUY, cents(9990), 1'000)));
    bool tested = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (tested || !update.top.has_bid) return;
        tested = true;
        CHECK_NE(context.submit_limit_order(Side::SELL, cents(9990), 60, TimeInForce::IOC, false), 0u);
        CHECK_EQ(context.position_headroom(Side::SELL), 40);
        CHECK_EQ(context.submit_limit_order(Side::SELL, cents(10050), 50, TimeInForce::Day, false), 0u);
        CHECK(context.last_risk_reject() == RiskRejectReason::MaxShortPosition);
        CHECK_NE(context.submit_limit_order(Side::SELL, cents(10050), 40, TimeInForce::Day, false), 0u);
        CHECK_EQ(context.position_headroom(Side::SELL), 0);
        CHECK_EQ(context.position_headroom(Side::BUY), config.risk.max_long_position);
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_EQ(simulation.ledger().state().position, -60);
    CHECK_EQ(simulation.metrics().limit_breaches, 0u);
}

TEST_CASE(research_risk, busy_quoting_never_breaches_its_limits) {
    ExperimentConfig config = busy_market_config(10 * kNanosPerSecond);
    config.risk.max_long_position = 150;
    config.risk.max_short_position = 150;
    Simulation<HookStrategy> simulation(config, 13);
    // Quote both sides at the touch with large clips, ignoring inventory:
    // only the host's worst-case check keeps the true position in bounds.
    simulation.strategy().start = [](StrategyContext& context) { context.wake_at(context.now()); };
    simulation.strategy().wakeup = [&](StrategyContext& context) {
        context.for_each_open_order([&](const StrategyOrderView& order) {
            if (order.state == OrderViewState::Live && order.pending == PendingRequest::None) (void)context.cancel_order(order.cl_ord_id);
        });
        const TopOfBook& top = context.top_of_book();
        if (top.two_sided()) {
            (void)context.submit_limit_order(Side::BUY, top.bid_price, 100, TimeInForce::Day, true);
            (void)context.submit_limit_order(Side::SELL, top.ask_price, 100, TimeInForce::Day, true);
        }
        context.wake_at(context.now() + 25 * kMillisecond);
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    const RunMetrics& metrics = simulation.metrics();
    CHECK_GT(metrics.fill_count, 20u);
    CHECK_GT(metrics.risk_rejects, 0u);
    CHECK_EQ(metrics.limit_breaches, 0u);
    CHECK_LE(metrics.max_position, config.risk.max_long_position);
    CHECK_GE(metrics.min_position, -config.risk.max_short_position);
}
