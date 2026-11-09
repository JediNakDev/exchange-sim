// Causal pipeline: each latency component delays exactly the stage it names,
// and a strategy never observes exchange state before it is delivered.

#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

using namespace research_test;

namespace {

// Two-sided scripted book at 1 ms: bid 99.90 x 100, ask 100.10 x 100.
void schedule_scripted_book(SimulationCore& core) {
    REQUIRE(core.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::BUY, cents(9990), 100)));
    REQUIRE(core.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(2, Side::SELL, cents(10010), 100)));
}

}  // namespace

TEST_CASE(research_causality, feed_latency_delays_market_data_and_hides_the_future_book) {
    const ExperimentConfig config = quiet_market_config();
    const Timestamp feed = config.latency.feed_ns;
    Simulation<HookStrategy> simulation(config, 1);
    schedule_scripted_book(simulation);
    std::vector<Timestamp> wakeup_times;
    std::vector<TopOfBook> visible_at_wakeup;
    std::vector<std::pair<Timestamp, Timestamp>> deliveries;  // (delivered at, exchange time)
    simulation.strategy().start = [&](StrategyContext& context) { context.wake_at(1 * kMillisecond + feed - 1); };
    simulation.strategy().wakeup = [&](StrategyContext& context) {
        wakeup_times.push_back(context.now());
        visible_at_wakeup.push_back(context.top_of_book());
        if (wakeup_times.size() == 1) context.wake_at(1 * kMillisecond + feed);
    };
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        deliveries.emplace_back(context.now(), update.exchange_time);
        CHECK(context.top_of_book() == update.top);
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);

    REQUIRE_EQ(wakeup_times.size(), 2u);
    // One nanosecond before delivery the engine already holds both orders,
    // but the strategy still sees an empty book.
    CHECK(!visible_at_wakeup[0].has_bid);
    CHECK(!visible_at_wakeup[0].has_ask);
    // At delivery time the market data (enqueued earlier) has been applied.
    CHECK(visible_at_wakeup[1].two_sided());
    CHECK_EQ(visible_at_wakeup[1].bid_price, cents(9990));
    CHECK_EQ(visible_at_wakeup[1].ask_price, cents(10010));
    REQUIRE_EQ(deliveries.size(), 2u);
    for (const auto& [delivered_at, exchange_time] : deliveries) {
        CHECK_EQ(exchange_time, 1 * kMillisecond);
        CHECK_EQ(delivered_at, exchange_time + feed);
    }
    CHECK_EQ(simulation.metrics().feed_latency.min, feed);
    CHECK_EQ(simulation.metrics().feed_latency.max, feed);
}

TEST_CASE(research_causality, order_compute_and_ack_latencies_compose_for_new_orders) {
    const ExperimentConfig config = quiet_market_config();
    const LatencyModel& latency = config.latency;
    Simulation<HookStrategy> simulation(config, 1);
    schedule_scripted_book(simulation);
    Timestamp decision_time = 0;
    std::vector<ExecutionReport> reports;
    std::vector<Timestamp> report_delivery;
    std::vector<TopOfBook> visible_at_report;
    std::vector<std::pair<Timestamp, TopOfBook>> market_data_after_order;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (decision_time == 0 && update.top.two_sided()) {
            decision_time = context.now();
            // Improves the best bid; does not cross the 100.10 ask.
            CHECK_NE(context.submit_limit_order(Side::BUY, cents(10000), 10, TimeInForce::Day, true), 0u);
        } else if (decision_time != 0) {
            market_data_after_order.emplace_back(context.now(), update.top);
        }
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        reports.push_back(report);
        report_delivery.push_back(context.now());
        visible_at_report.push_back(context.top_of_book());
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);

    REQUIRE_EQ(reports.size(), 2u);
    CHECK(reports[0].kind == ReportKind::Accepted);
    CHECK(reports[1].kind == ReportKind::Rested);
    const Timestamp exchange_time = decision_time + latency.strategy_compute_ns + latency.order_ns;
    for (std::size_t i = 0; i < reports.size(); ++i) {
        CHECK_EQ(reports[i].exchange_time, exchange_time);
        CHECK_EQ(report_delivery[i], exchange_time + latency.ack_ns);
        // ack (30us) arrives before feed (50us): the strategy's own order is not
        // yet visible in the book it has been shown.
        CHECK_EQ(visible_at_report[i].bid_price, cents(9990));
    }
    REQUIRE_EQ(market_data_after_order.size(), 1u);
    CHECK_EQ(market_data_after_order[0].first, exchange_time + latency.feed_ns);
    CHECK_EQ(market_data_after_order[0].second.bid_price, cents(10000));
    CHECK_EQ(simulation.metrics().order_latency.min, latency.strategy_compute_ns + latency.order_ns);
    CHECK_EQ(simulation.metrics().ack_latency.max, latency.ack_ns);
}

TEST_CASE(research_causality, cancel_latency_applies_to_cancel_and_replace_requests) {
    const ExperimentConfig config = quiet_market_config();
    const LatencyModel& latency = config.latency;
    Simulation<HookStrategy> simulation(config, 1);
    ClOrdId token = 0;
    Timestamp replace_decision = 0;
    Timestamp cancel_decision = 0;
    std::vector<ExecutionReport> reports;
    simulation.strategy().start = [&](StrategyContext& context) {
        token = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
        CHECK_NE(token, 0u);
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        reports.push_back(report);
        if (report.kind == ReportKind::Rested && replace_decision == 0) {
            replace_decision = context.now();
            CHECK(context.replace_order(token, cents(9901), 10));
        } else if (report.kind == ReportKind::Replaced) {
            cancel_decision = context.now();
            CHECK(context.cancel_order(token));
        }
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    // accepted, rested, replaced, rested again (the price change lost priority), canceled
    REQUIRE_EQ(reports.size(), 5u);
    CHECK(reports[2].kind == ReportKind::Replaced);
    CHECK(!reports[2].priority_kept);
    CHECK_EQ(reports[2].exchange_time, replace_decision + latency.strategy_compute_ns + latency.cancel_ns);
    CHECK(reports[3].kind == ReportKind::Rested);
    CHECK(reports[4].kind == ReportKind::Canceled);
    CHECK_EQ(reports[4].exchange_time, cancel_decision + latency.strategy_compute_ns + latency.cancel_ns);
    CHECK_EQ(simulation.metrics().cancel_latency.count, 2u);
    CHECK_EQ(simulation.metrics().cancel_latency.min, latency.strategy_compute_ns + latency.cancel_ns);
    CHECK_EQ(simulation.metrics().cancel_latency.max, latency.strategy_compute_ns + latency.cancel_ns);
}

TEST_CASE(research_causality, non_fifo_cancel_can_overtake_its_order_and_is_resent_after_ack) {
    ExperimentConfig config = quiet_market_config();
    config.order_entry_protocol = server::Protocol::Fix44;
    config.latency.order_ns = 100 * kMicrosecond;
    config.latency.cancel_ns = 10 * kMicrosecond;
    config.latency.order_channel_fifo = false;
    Simulation<HookStrategy> simulation(config, 1);
    std::vector<ReportKind> kinds;
    simulation.strategy().start = [&](StrategyContext& context) {
        const ClOrdId token = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
        REQUIRE(token != 0);
        CHECK(context.cancel_order(token));
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext&) { kinds.push_back(report.kind); };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    const RunMetrics& metrics = simulation.metrics();
    CHECK_EQ(metrics.cancel_requests_rejected, 1u);   // the cancel reached the exchange first
    CHECK_EQ(metrics.cancels_resent_after_ack, 1u);
    CHECK_EQ(metrics.orders_canceled_by_request, 1u);
    CHECK_EQ(simulation.strategy_view().open_order_count, 0u);
    REQUIRE_EQ(kinds.size(), 4u);
    CHECK(kinds[0] == ReportKind::Rejected);
    CHECK(kinds[1] == ReportKind::Accepted);
    CHECK(kinds[2] == ReportKind::Rested);
    CHECK(kinds[3] == ReportKind::Canceled);
}

TEST_CASE(research_causality, fifo_order_channel_keeps_cancel_behind_its_order) {
    ExperimentConfig config = quiet_market_config();
    config.latency.order_ns = 100 * kMicrosecond;
    config.latency.cancel_ns = 10 * kMicrosecond;
    config.latency.order_channel_fifo = true;
    Simulation<HookStrategy> simulation(config, 1);
    std::vector<ExecutionReport> reports;
    simulation.strategy().start = [&](StrategyContext& context) {
        const ClOrdId token = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
        REQUIRE(token != 0);
        CHECK(context.cancel_order(token));
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext&) { reports.push_back(report); };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_EQ(simulation.metrics().cancel_requests_rejected, 0u);
    CHECK_EQ(simulation.metrics().cancels_resent_after_ack, 0u);
    REQUIRE_EQ(reports.size(), 3u);
    CHECK(reports[0].kind == ReportKind::Accepted);
    CHECK(reports[1].kind == ReportKind::Rested);
    CHECK(reports[2].kind == ReportKind::Canceled);
    // The cancel waited for the order: both processed at the order's arrival.
    CHECK_EQ(reports[2].exchange_time, reports[0].exchange_time);
}

TEST_CASE(research_causality, delivered_messages_respect_latency_bounds_with_jitter) {
    const ExperimentConfig config = busy_market_config(5 * kNanosPerSecond);
    const LatencyModel& latency = config.latency;
    Simulation<HookStrategy> simulation(config, 21);
    Timestamp last_feed_delivery = 0;
    Timestamp last_report_delivery = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t market_data_seen = 0;
    std::uint64_t reports_seen = 0;
    bool quoting = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        ++market_data_seen;
        CHECK_GE(context.now(), update.exchange_time + latency.feed_ns);
        CHECK_LE(context.now(), update.exchange_time + latency.feed_ns + latency.feed_jitter_ns);
        CHECK_GE(context.now(), last_feed_delivery);
        CHECK_EQ(update.sequence, last_sequence + 1);  // gap-free, in order
        CHECK(context.top_of_book() == update.top);
        last_sequence = update.sequence;
        last_feed_delivery = context.now();
        if (!quoting && update.top.two_sided()) {
            quoting = true;
            context.wake_at(context.now());
        }
    };
    simulation.strategy().wakeup = [&](StrategyContext& context) {
        // Keep a passive bid and ask near the touch to receive fills and reports.
        const TopOfBook& top = context.top_of_book();
        if (top.two_sided() && context.open_order_count() == 0) {
            (void)context.submit_limit_order(Side::BUY, top.bid_price, 10, TimeInForce::Day, true);
            (void)context.submit_limit_order(Side::SELL, top.ask_price, 10, TimeInForce::Day, true);
        }
        context.wake_at(context.now() + 20 * kMillisecond);
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        ++reports_seen;
        CHECK_GE(context.now(), report.exchange_time + latency.ack_ns);
        CHECK_LE(context.now(), report.exchange_time + latency.ack_ns + latency.ack_jitter_ns);
        CHECK_GE(context.now(), last_report_delivery);
        last_report_delivery = context.now();
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK_GT(market_data_seen, 100u);
    CHECK_GT(reports_seen, 10u);
    const RunMetrics& metrics = simulation.metrics();
    CHECK_GE(metrics.order_latency.min, latency.strategy_compute_ns + latency.order_ns);
    CHECK_GT(metrics.fill_count, 0u);
}

TEST_CASE(research_causality, ouch_retries_early_cancel_intent_without_inventing_a_reject_reply) {
    auto config = quiet_market_config();
    config.order_entry_protocol = server::Protocol::Ouch42;
    config.latency.order_ns = 100*kMicrosecond;
    config.latency.cancel_ns = 10*kMicrosecond;
    config.latency.order_channel_fifo = false;
    Simulation<HookStrategy> simulation(config,1);
    std::vector<ReportKind> reports;
    simulation.strategy().start = [&](StrategyContext& context) {
        const auto token = context.submit_limit_order(Side::BUY,cents(9900),10,TimeInForce::Day,false);
        REQUIRE(token != 0);
        CHECK(context.cancel_order(token));
    };
    simulation.strategy().report = [&](const ExecutionReport& report,StrategyContext&) { reports.push_back(report.kind); };
    REQUIRE_EQ(run_to_end(simulation),RunStop::Completed);
    CHECK_EQ(simulation.metrics().cancel_requests_rejected,1u);
    CHECK_EQ(simulation.metrics().cancels_resent_after_ack,1u);
    CHECK_EQ(simulation.metrics().orders_canceled_by_request,1u);
    CHECK_EQ(simulation.strategy_view().open_order_count,0u);
    REQUIRE_EQ(reports.size(),3u);
    CHECK_EQ(reports[0],ReportKind::Accepted);
    CHECK_EQ(reports[1],ReportKind::Rested);
    CHECK_EQ(reports[2],ReportKind::Canceled);
}
