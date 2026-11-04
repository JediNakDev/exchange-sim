// Accounting: exact fee round trip, cash/position/P&L identities, the mark
// convention, and missing marks staying unavailable instead of invented.

#include "exsim/research/json.hpp"
#include "exsim/research/run_summary.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "tests/exsim/research/research_test_support.hpp"
#include "tests/framework/test.hpp"

#include <string>

using namespace research_test;

namespace {

constexpr std::int64_t kTakerFeePerShare = 30;   // quiet_market_config: 0.003
constexpr std::int64_t kMakerFeePerShare = -20;  // quiet_market_config: -0.002 (rebate)

const JsonValue& member(const JsonValue& object, std::string_view name) {
    const JsonValue* value = object.find(name);
    REQUIRE(value != nullptr);
    return *value;
}

}  // namespace

// Buy 100 @ 100.00 as taker, sell 100 @ 100.05 as maker:
//   cash  = -10000.00 - 0.30 + 10005.00 + 0.20 = +4.90
//   gross realized = 0.05 * 100 = 5.00, fees = 0.30 - 0.20 = 0.10
TEST_CASE(research_accounting, fee_round_trip_is_exact) {
    const ExperimentConfig config = quiet_market_config();
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::SELL, cents(10000), 100)));
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(2, Side::BUY, cents(9990), 50)));
    REQUIRE(simulation.schedule_scripted_command(100 * kMillisecond, scripted_limit_order(3, Side::BUY, cents(10005), 100)));
    bool bought = false;
    bool offered = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (!bought && update.top.two_sided()) {
            bought = true;
            CHECK_NE(context.submit_limit_order(Side::BUY, cents(10000), 100, TimeInForce::IOC, false), 0u);
        }
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        if (report.kind == ReportKind::Fill && report.side == Side::BUY && report.leaves_qty == 0 && !offered) {
            offered = true;
            CHECK_EQ(context.known_position(), 100);
            CHECK_NE(context.submit_limit_order(Side::SELL, cents(10005), 100, TimeInForce::Day, true), 0u);
        }
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);

    REQUIRE_EQ(records.fills.size(), 2u);
    const FillRecord& buy = records.fills[0];
    CHECK(buy.side == Side::BUY);
    CHECK(buy.liquidity == Liquidity::Taker);
    CHECK_EQ(buy.price, cents(10000));
    CHECK_EQ(buy.qty, 100u);
    CHECK_EQ(buy.fee_per_share, kTakerFeePerShare);
    CHECK_EQ(buy.fee, 3'000);
    CHECK_EQ(buy.position_after, 100);
    CHECK_EQ(buy.cash_after, config.accounting.initial_cash - 100 * cents(10000) - 3'000);
    REQUIRE(buy.has_mid_before);
    CHECK_EQ(buy.mid_before_times_two, cents(9990) + cents(10000));
    CHECK_EQ(buy.spread_capture_times_two, -100'000);  // bought 0.05 above mid: -5.00

    const FillRecord& sell = records.fills[1];
    CHECK(sell.side == Side::SELL);
    CHECK(sell.liquidity == Liquidity::Maker);
    CHECK_EQ(sell.fee_per_share, kMakerFeePerShare);
    CHECK_EQ(sell.fee, -2'000);
    CHECK_EQ(sell.position_after, 0);
    CHECK_EQ(sell.realized_pnl_after, 50'000);
    CHECK_EQ(sell.fees_after, 1'000);
    CHECK_EQ(sell.mid_before_times_two, cents(9990) + cents(10005));
    CHECK_EQ(sell.spread_capture_times_two, 150'000);  // sold 0.075 above mid: +7.50

    const LedgerState& ledger = simulation.ledger().state();
    CHECK_EQ(ledger.cash - config.accounting.initial_cash, 49'000);
    CHECK_EQ(ledger.gross_realized_pnl, 50'000);
    CHECK_EQ(ledger.fees, 1'000);
    CHECK_EQ(ledger.position, 0);
    const RunMetrics& metrics = simulation.metrics();
    CHECK_EQ(metrics.taker_fees, 3'000);
    CHECK_EQ(metrics.maker_fees, -2'000);
    CHECK_EQ(metrics.buy_notional, 100 * cents(10000));
    CHECK_EQ(metrics.sell_notional, 100 * cents(10005));
    CHECK_EQ(metrics.spread_capture_total_times_two, 50'000);

    // Flat at the end: equity is cash even though the final book is one-sided.
    const FinalValuation valuation = final_valuation(simulation);
    CHECK(!valuation.has_mark);
    CHECK(valuation.has_equity);
    CHECK_EQ(valuation.equity, ledger.cash);
    CHECK_EQ(valuation.net_pnl, 49'000);
    CHECK_EQ(valuation.net_pnl, ledger.gross_realized_pnl + valuation.unrealized_pnl - ledger.fees);
    const ReconciliationResult reconciliation = reconcile(simulation);
    CHECK(reconciliation.cash_matches_fills);
    CHECK(reconciliation.position_matches_fills);
    CHECK(reconciliation.fees_match_fills);
    CHECK(reconciliation.pnl_identity_holds);

    const JsonValue summary = parse_json(render_summary_json(simulation, StrategyIdentity{"hook", 1}, "accounting-test", false));
    const JsonValue& accounting = member(summary, "accounting");
    CHECK_EQ(member(accounting, "cash").text, std::string("1000004.90"));
    CHECK_EQ(member(accounting, "realized_pnl").text, std::string("5.00"));
    CHECK_EQ(member(accounting, "fees").text, std::string("0.10"));
    CHECK_EQ(member(accounting, "maker_fees").text, std::string("-0.20"));
    CHECK_EQ(member(accounting, "taker_fees").text, std::string("0.30"));
    CHECK_EQ(member(accounting, "net_pnl").text, std::string("4.90"));
    CHECK(member(accounting, "mark_price").is_null());
    CHECK(member(member(accounting, "reconciliation"), "pnl_identity_holds").boolean);
    const JsonValue& spread_capture = member(member(summary, "fills"), "spread_capture");
    CHECK_EQ(member(spread_capture, "total").text, std::string("2.50"));
    CHECK_EQ(member(member(summary, "metrics"), "net_pnl").number, 4.9);
}

TEST_CASE(research_accounting, open_position_without_a_mark_stays_unavailable) {
    ExperimentConfig config = quiet_market_config();
    Simulation<HookStrategy> simulation(config, 1);
    // One-sided book: there is never an exchange mid.
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::SELL, cents(10000), 100)));
    bool bought = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (!bought && update.top.has_ask) {
            bought = true;
            CHECK_NE(context.submit_limit_order(Side::BUY, cents(10000), 50, TimeInForce::IOC, false), 0u);
        }
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    CHECK_EQ(simulation.ledger().state().position, 50);
    const FinalValuation valuation = final_valuation(simulation);
    CHECK(!valuation.has_mark);
    CHECK(!valuation.has_equity);
    CHECK(reconcile(simulation).pnl_identity_holds);  // vacuous without equity, never invented
    CHECK_GT(simulation.metrics().samples_without_mark, 0u);
    for (const auto& sample : records.samples) {
        if (sample.position != 0) CHECK(!sample.has_equity);
    }
    const JsonValue summary = parse_json(render_summary_json(simulation, StrategyIdentity{"hook", 1}, "no-mark", false));
    const JsonValue& accounting = member(summary, "accounting");
    CHECK(member(accounting, "mark_price").is_null());
    CHECK(member(accounting, "unrealized_pnl").is_null());
    CHECK(member(accounting, "equity").is_null());
    CHECK(member(accounting, "net_pnl").is_null());
    CHECK(member(member(summary, "metrics"), "net_pnl").is_null());
    CHECK(member(member(summary, "fills"), "spread_capture").find("unavailable")->text == "1");
}

TEST_CASE(research_accounting, last_trade_mark_values_the_same_position) {
    ExperimentConfig config = quiet_market_config();
    config.accounting.mark = MarkConvention::LastTrade;
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(1 * kMillisecond, scripted_limit_order(1, Side::SELL, cents(10000), 100)));
    // Clear the rest of the ask, then a later scripted trade moves the last price.
    REQUIRE(simulation.schedule_scripted_command(200 * kMillisecond, scripted_limit_order(2, Side::BUY, cents(10000), 50)));
    REQUIRE(simulation.schedule_scripted_command(300 * kMillisecond, scripted_limit_order(3, Side::SELL, cents(10010), 10)));
    REQUIRE(simulation.schedule_scripted_command(400 * kMillisecond, scripted_limit_order(4, Side::BUY, cents(10010), 10)));
    bool bought = false;
    simulation.strategy().market_data = [&](const MarketDataUpdate& update, StrategyContext& context) {
        if (!bought && update.top.has_ask) {
            bought = true;
            CHECK_NE(context.submit_limit_order(Side::BUY, cents(10000), 50, TimeInForce::IOC, false), 0u);
        }
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
    REQUIRE_EQ(simulation.ledger().state().position, 50);
    const FinalValuation valuation = final_valuation(simulation);
    REQUIRE(valuation.has_mark);
    CHECK_EQ(valuation.mark_price, cents(10010));
    CHECK(valuation.has_equity);
    CHECK_EQ(valuation.unrealized_pnl, 50 * (cents(10010) - cents(10000)));
    CHECK_EQ(valuation.net_pnl, valuation.unrealized_pnl - 50 * kTakerFeePerShare);
    CHECK_EQ(valuation.open_cost_basis, 50 * cents(10000));
    CHECK(reconcile(simulation).pnl_identity_holds);
}

TEST_CASE(research_accounting, busy_run_reconciles_fill_by_fill) {
    const ExperimentConfig config = busy_market_config(5 * kNanosPerSecond);
    Simulation<strategies::FixedSpreadMarketMaker> simulation(config, 5, config.strategy);
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE(records.fills.size() > 10u);

    std::int64_t cash = config.accounting.initial_cash;
    std::int64_t position = 0;
    std::int64_t fees = 0;
    std::uint64_t maker_qty = 0;
    std::uint64_t taker_qty = 0;
    for (std::size_t index = 0; index < records.fills.size(); ++index) {
        const FillRecord& fill = records.fills[index];
        CHECK_EQ(fill.fill_index, index);
        const std::int64_t expected_rate =
            fill.liquidity == Liquidity::Maker ? config.fees.maker_fee_per_share : config.fees.taker_fee_per_share;
        CHECK_EQ(fill.fee_per_share, expected_rate);
        CHECK_EQ(fill.fee, expected_rate * static_cast<std::int64_t>(fill.qty));
        const std::int64_t notional = fill.price * static_cast<std::int64_t>(fill.qty);
        cash += fill.side == Side::BUY ? -notional : notional;
        cash -= fill.fee;
        position += fill.side == Side::BUY ? static_cast<std::int64_t>(fill.qty) : -static_cast<std::int64_t>(fill.qty);
        fees += fill.fee;
        (fill.liquidity == Liquidity::Maker ? maker_qty : taker_qty) += fill.qty;
        CHECK_EQ(fill.cash_after, cash);
        CHECK_EQ(fill.position_after, position);
        CHECK_EQ(fill.fees_after, fees);
        CHECK_LE(fill.position_after, config.risk.max_long_position);
        CHECK_GE(fill.position_after, -config.risk.max_short_position);
    }
    const LedgerState& ledger = simulation.ledger().state();
    CHECK_EQ(ledger.cash, cash);
    CHECK_EQ(ledger.position, position);
    CHECK_EQ(ledger.fees, fees);
    CHECK_EQ(simulation.metrics().maker_qty, maker_qty);
    CHECK_EQ(simulation.metrics().taker_qty, taker_qty);
    CHECK_EQ(simulation.metrics().limit_breaches, 0u);

    const ReconciliationResult reconciliation = reconcile(simulation);
    CHECK(reconciliation.cash_matches_fills);
    CHECK(reconciliation.position_matches_fills);
    CHECK(reconciliation.fees_match_fills);
    CHECK(reconciliation.pnl_identity_holds);
    const FinalValuation valuation = final_valuation(simulation);
    REQUIRE(valuation.has_equity);
    CHECK_EQ(valuation.equity - config.accounting.initial_cash,
             ledger.gross_realized_pnl + valuation.unrealized_pnl - ledger.fees);

    // Every series sample with equity satisfies the same identity at its time.
    for (const auto& sample : records.samples) {
        if (!sample.has_equity) continue;
        CHECK_EQ(sample.equity - config.accounting.initial_cash, sample.realized_pnl + sample.unrealized_pnl - sample.fees);
    }
}
