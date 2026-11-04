#include "exsim/research/run_summary.hpp"

#include "exsim/research/json.hpp"

#include <cmath>
#include <optional>

namespace exsim::research {

namespace {

// Dividing by the scale (rather than multiplying by 1e-4) gives the double nearest the exact decimal.
constexpr double kPriceUnitsPerCurrency = static_cast<double>(kPriceScale);

std::optional<double> ratio(double numerator, double denominator) {
    if (denominator == 0) return std::nullopt;
    return numerator / denominator;
}

void put_optional_number(JsonWriter& json, std::string_view name, const std::optional<double>& value) {
    json.key(name);
    if (value) json.number(*value);
    else json.null();
}

void put_duration_statistic(JsonWriter& json, std::string_view name, const DurationStatistic& statistic) {
    json.key(name).begin_object();
    json.key("count").integer_text(statistic.count);
    if (statistic.count == 0) {
        json.key("min_ns").null().key("max_ns").null().key("mean_ns").null().key("sum_ns").null();
    } else {
        json.key("min_ns").integer_text(statistic.min);
        json.key("max_ns").integer_text(statistic.max);
        json.key("mean_ns").number(static_cast<double>(statistic.sum) / static_cast<double>(statistic.count));
        json.key("sum_ns").integer_text(statistic.sum);
    }
    json.end_object();
}

std::optional<double> mean_ns(const DurationStatistic& statistic) {
    return ratio(static_cast<double>(statistic.sum), static_cast<double>(statistic.count));
}

// Per-share value in currency of a doubled price-unit total.
std::optional<double> per_share_currency(std::int64_t total_times_two, std::uint64_t qty) {
    if (qty == 0) return std::nullopt;
    return static_cast<double>(total_times_two) / 2.0 / static_cast<double>(qty) / kPriceUnitsPerCurrency;
}

}  // namespace

FinalValuation final_valuation(const SimulationCore& core) noexcept {
    FinalValuation result;
    const LedgerState& ledger_state = core.ledger().state();
    const std::int64_t initial_cash = core.config().accounting.initial_cash;
    result.open_cost_basis = open_cost_basis(ledger_state, initial_cash);
    if (const auto mark = core.mark_times_two()) {
        const DoubledValuation valuation = value_at_mark(ledger_state, initial_cash, *mark);
        result.has_mark = true;
        result.mark_times_two = *mark;
        result.has_equity = true;
        result.unrealized_pnl_times_two = valuation.unrealized_pnl_times_two;
        result.equity_times_two = valuation.equity_times_two;
    } else if (ledger_state.position == 0) {  // nothing to value: equity is cash
        result.has_equity = true;
        result.equity_times_two = 2 * ledger_state.cash;
    }
    if (result.has_equity) result.net_pnl_times_two = result.equity_times_two - 2 * initial_cash;
    return result;
}

ReconciliationResult reconcile(const SimulationCore& core) noexcept {
    ReconciliationResult result;
    const LedgerState& ledger_state = core.ledger().state();
    const RunMetrics& metrics = core.metrics();
    const std::int64_t initial_cash = core.config().accounting.initial_cash;
    result.cash_matches_fills =
        ledger_state.cash == initial_cash - metrics.buy_notional + metrics.sell_notional - ledger_state.fees;
    result.position_matches_fills = ledger_state.position == static_cast<std::int64_t>(ledger_state.bought_quantity) -
                                                                 static_cast<std::int64_t>(ledger_state.sold_quantity) &&
                                    ledger_state.fill_count == metrics.fill_count;
    result.fees_match_fills = ledger_state.fees == metrics.maker_fees + metrics.taker_fees + metrics.auction_fees;
    const FinalValuation valuation = final_valuation(core);
    result.pnl_identity_holds =
        !valuation.has_equity || valuation.net_pnl_times_two == 2 * ledger_state.gross_realized_pnl +
                                                                    valuation.unrealized_pnl_times_two - 2 * ledger_state.fees;
    if (result.pnl_identity_holds && valuation.has_mark && valuation.mark_times_two % 2 == 0) {
        // Independent check against the ledger's FIFO lots.
        const auto lot_valuation = core.ledger().valuation(valuation.mark_times_two / 2);
        result.pnl_identity_holds =
            lot_valuation.has_value() && 2 * lot_valuation->unrealized_pnl == valuation.unrealized_pnl_times_two;
    }
    return result;
}

std::string render_summary_json(const SimulationCore& core, const StrategyIdentity& strategy, const std::string& run_id,
                                bool replay_verified) {
    const ExperimentConfig& config = core.config();
    const RunMetrics& metrics = core.metrics();
    const LedgerState& ledger_state = core.ledger().state();
    const FinalValuation valuation = final_valuation(core);
    const ReconciliationResult reconciliation = reconcile(core);
    const RunDigests& digests = core.digests();
    const double duration_ns = static_cast<double>(config.limits.duration_ns);
    const std::uint64_t submitted_order_qty_denominator = metrics.accepted_order_qty + metrics.replace_added_qty;

    JsonWriter json;
    json.begin_object();
    json.key("schema").string(kSummarySchema);
    json.key("run_id").string(run_id);
    json.key("master_seed").integer_text(core.master_seed());
    json.key("config_hash").string(format_hex64(experiment_config_hash(config)));
    json.key("strategy").begin_object().key("name").string(strategy.name).key("version").integer_text(strategy.version).end_object();
    json.key("state").string(to_string(core.state()));
    json.key("termination_reason").string(to_string(core.termination()));
    json.key("failure_detail").integer_text(core.failure_detail());

    json.key("simulation").begin_object();
    json.key("end_time_ns").integer_text(core.end_time());
    json.key("final_time_ns").integer_text(core.now());
    json.key("simulation_events").integer_text(core.simulation_events_processed());
    json.key("observation_events").integer_text(core.observation_events_processed());
    json.key("simulation_queue_high_water").integer_text(core.simulation_queue_high_water_mark());
    json.key("observation_queue_high_water").integer_text(core.observation_queue_high_water_mark());
    json.key("engine_commands").integer_text(metrics.engine_commands);
    json.key("strategy_commands").integer_text(metrics.strategy_commands);
    json.key("background_commands").integer_text(metrics.background_commands);
    json.key("scripted_commands").integer_text(metrics.scripted_commands);
    json.key("engine_events").integer_text(metrics.engine_events);
    json.key("market_data_published").integer_text(metrics.market_data_published);
    json.key("market_data_delivered").integer_text(metrics.market_data_delivered);
    json.key("reports_scheduled").integer_text(metrics.reports_scheduled);
    json.key("reports_delivered").integer_text(metrics.reports_delivered);
    json.key("strategy_wakeups").integer_text(metrics.strategy_wakeups);
    json.key("market_trades").integer_text(metrics.market_trades);
    json.key("market_traded_qty").integer_text(metrics.market_traded_qty);
    json.end_object();

    json.key("digests").begin_object();
    json.key("command_digest").integer_text(digests.command_digest);
    json.key("event_digest").integer_text(digests.event_digest);
    json.key("ledger_digest").integer_text(digests.ledger_digest);
    json.key("command_count").integer_text(digests.command_count);
    json.key("event_count").integer_text(digests.event_count);
    json.key("fill_count").integer_text(digests.fill_count);
    json.key("replay_verified").boolean(replay_verified);
    json.end_object();

    json.key("accounting").begin_object();
    json.key("units").string("price units are 1/10000 currency; quantities are shares");
    json.key("mark_convention").string(to_string(config.accounting.mark));
    json.key("initial_cash").price_text(config.accounting.initial_cash);
    json.key("initial_position").signed_integer_text(0);
    json.key("cash").price_text(ledger_state.cash);
    json.key("position").signed_integer_text(ledger_state.position);
    json.key("bought_qty").integer_text(ledger_state.bought_quantity);
    json.key("sold_qty").integer_text(ledger_state.sold_quantity);
    json.key("buy_notional").price_text(metrics.buy_notional);
    json.key("sell_notional").price_text(metrics.sell_notional);
    json.key("open_cost_basis").price_text(valuation.open_cost_basis);
    put_optional_number(json, "average_cost_basis",
                        ledger_state.position == 0
                            ? std::nullopt
                            : std::optional<double>(static_cast<double>(valuation.open_cost_basis) /
                                                    static_cast<double>(ledger_state.position) / kPriceUnitsPerCurrency));
    json.key("realized_pnl").price_text(ledger_state.gross_realized_pnl);
    json.key("fees").price_text(ledger_state.fees);
    json.key("maker_fees").price_text(metrics.maker_fees);
    json.key("taker_fees").price_text(metrics.taker_fees);
    json.key("auction_fees").price_text(metrics.auction_fees);
    json.key("mark_price");
    if (valuation.has_mark) json.doubled_price_text(valuation.mark_times_two);
    else json.null();
    json.key("unrealized_pnl");
    if (valuation.has_equity) json.doubled_price_text(valuation.unrealized_pnl_times_two);
    else json.null();
    json.key("equity");
    if (valuation.has_equity) json.doubled_price_text(valuation.equity_times_two);
    else json.null();
    json.key("net_pnl");
    if (valuation.has_equity) json.doubled_price_text(valuation.net_pnl_times_two);
    else json.null();
    json.key("max_drawdown");
    if (metrics.has_equity) json.doubled_price_text(metrics.max_drawdown_times_two);
    else json.null();
    json.key("max_drawdown_time_ns");
    if (metrics.has_equity && metrics.max_drawdown_times_two > 0) json.integer_text(metrics.max_drawdown_time);
    else json.null();
    json.key("samples_without_mark").integer_text(metrics.samples_without_mark);
    json.key("reconciliation").begin_object();
    json.key("cash_matches_fills").boolean(reconciliation.cash_matches_fills);
    json.key("position_matches_fills").boolean(reconciliation.position_matches_fills);
    json.key("fees_match_fills").boolean(reconciliation.fees_match_fills);
    json.key("pnl_identity_holds").boolean(reconciliation.pnl_identity_holds);
    json.end_object();
    json.end_object();

    json.key("orders").begin_object();
    json.key("new_orders_submitted").integer_text(metrics.new_orders_submitted);
    json.key("cancel_requests_submitted").integer_text(metrics.cancel_requests_submitted);
    json.key("replace_requests_submitted").integer_text(metrics.replace_requests_submitted);
    json.key("risk_rejects").integer_text(metrics.risk_rejects);
    json.key("risk_rejects_by_reason").begin_object();
    for (std::size_t reason = 1; reason < kRiskRejectReasonCount; ++reason)
        json.key(to_string(static_cast<RiskRejectReason>(reason))).integer_text(metrics.risk_rejects_by_reason[reason]);
    json.end_object();
    json.key("cancels_resent_after_ack").integer_text(metrics.cancels_resent_after_ack);
    json.key("orders_accepted").integer_text(metrics.orders_accepted);
    json.key("orders_rejected").integer_text(metrics.orders_rejected);
    json.key("cancel_requests_rejected").integer_text(metrics.cancel_requests_rejected);
    json.key("replace_requests_rejected").integer_text(metrics.replace_requests_rejected);
    json.key("orders_canceled_by_request").integer_text(metrics.orders_canceled_by_request);
    json.key("ioc_remainders_canceled").integer_text(metrics.ioc_remainders_canceled);
    json.key("self_trade_cancels").integer_text(metrics.self_trade_cancels);
    json.key("replaces_applied").integer_text(metrics.replaces_applied);
    json.key("replaces_priority_kept").integer_text(metrics.replaces_priority_kept);
    json.key("orders_with_fill").integer_text(metrics.orders_with_fill);
    json.key("orders_filled_completely").integer_text(metrics.orders_filled_completely);
    json.key("orders_ended_partially_filled").integer_text(metrics.orders_ended_partially_filled);
    json.key("accepted_order_qty").integer_text(metrics.accepted_order_qty);
    json.key("replace_added_qty").integer_text(metrics.replace_added_qty);
    put_optional_number(json, "acceptance_ratio",
                        ratio(static_cast<double>(metrics.orders_accepted), static_cast<double>(metrics.new_orders_submitted)));
    put_optional_number(json, "order_fill_ratio",
                        ratio(static_cast<double>(metrics.orders_with_fill), static_cast<double>(metrics.orders_accepted)));
    put_optional_number(json, "full_fill_ratio",
                        ratio(static_cast<double>(metrics.orders_filled_completely), static_cast<double>(metrics.orders_accepted)));
    put_optional_number(json, "quantity_fill_ratio",
                        ratio(static_cast<double>(metrics.filled_qty), static_cast<double>(submitted_order_qty_denominator)));
    put_duration_statistic(json, "time_to_first_fill", metrics.time_to_first_fill);
    json.end_object();

    json.key("fills").begin_object();
    json.key("count").integer_text(metrics.fill_count);
    json.key("qty").integer_text(metrics.filled_qty);
    json.key("maker_count").integer_text(metrics.maker_fill_count);
    json.key("maker_qty").integer_text(metrics.maker_qty);
    json.key("taker_count").integer_text(metrics.taker_fill_count);
    json.key("taker_qty").integer_text(metrics.taker_qty);
    json.key("auction_count").integer_text(metrics.auction_fill_count);
    json.key("auction_qty").integer_text(metrics.auction_qty);
    json.key("spread_capture").begin_object();
    json.key("reference").string("reference_exchange_mid_before_the_filling_command");
    json.key("fills").integer_text(metrics.spread_capture_fills);
    json.key("qty").integer_text(metrics.spread_capture_qty);
    json.key("unavailable").integer_text(metrics.spread_capture_unavailable);
    json.key("total").doubled_price_text(metrics.spread_capture_total_times_two);
    put_optional_number(json, "per_share", per_share_currency(metrics.spread_capture_total_times_two, metrics.spread_capture_qty));
    json.end_object();
    json.end_object();

    json.key("markouts").begin_array();
    for (std::size_t horizon = 0; horizon < config.metrics.markout_horizon_count; ++horizon) {
        const auto& accumulator = metrics.markouts[horizon];
        json.begin_object();
        json.key("horizon_ns").integer_text(config.metrics.markout_horizons_ns[horizon]);
        json.key("reference").string("reference_exchange_mid_at_horizon");
        json.key("measured").integer_text(accumulator.measured);
        json.key("unavailable").integer_text(accumulator.unavailable);
        json.key("censored").integer_text(accumulator.censored);
        json.key("measured_qty").integer_text(accumulator.measured_qty);
        json.key("total").doubled_price_text(accumulator.total_times_two);
        put_optional_number(json, "per_share", per_share_currency(accumulator.total_times_two, accumulator.measured_qty));
        json.end_object();
    }
    json.end_array();

    json.key("quote_exposure").begin_object();
    json.key("source").string("engine_truth_resting_orders");
    json.key("bid_ns").integer_text(metrics.bid_exposure_ns);
    json.key("ask_ns").integer_text(metrics.ask_exposure_ns);
    json.key("two_sided_ns").integer_text(metrics.two_sided_exposure_ns);
    json.key("any_side_ns").integer_text(metrics.any_side_exposure_ns);
    put_optional_number(json, "two_sided_ratio", ratio(static_cast<double>(metrics.two_sided_exposure_ns), duration_ns));
    put_optional_number(json, "any_side_ratio", ratio(static_cast<double>(metrics.any_side_exposure_ns), duration_ns));
    json.end_object();

    json.key("inventory").begin_object();
    json.key("max_position").signed_integer_text(metrics.max_position);
    json.key("min_position").signed_integer_text(metrics.min_position);
    json.key("max_long_limit").signed_integer_text(config.risk.max_long_position);
    json.key("max_short_limit").signed_integer_text(config.risk.max_short_position);
    json.key("limit_breaches").integer_text(metrics.limit_breaches);
    json.key("final_known_position").signed_integer_text(core.strategy_view().known_position);
    json.key("final_open_orders").integer_text(core.strategy_view().open_order_count);
    json.end_object();

    json.key("latency").begin_object();
    put_duration_statistic(json, "feed", metrics.feed_latency);
    put_duration_statistic(json, "order", metrics.order_latency);
    put_duration_statistic(json, "cancel", metrics.cancel_latency);
    put_duration_statistic(json, "ack", metrics.ack_latency);
    json.end_object();

    const ParentOrderTracking& parent = metrics.parent;
    std::optional<double> shortfall_per_share;
    std::optional<double> completion;
    std::optional<double> participation;
    json.key("execution");
    if (!parent.declared) {
        json.null();
    } else {
        const std::int64_t sign = parent.side == Side::BUY ? 1 : -1;
        const std::uint64_t residual = parent.filled_qty >= parent.qty ? 0 : parent.qty - parent.filled_qty;
        completion = static_cast<double>(parent.filled_qty) / static_cast<double>(parent.qty);
        participation = ratio(static_cast<double>(parent.filled_qty),
                              static_cast<double>(parent.market_volume_at_last_fill - parent.market_volume_at_decision));
        json.begin_object();
        json.key("side").string(parent.side == Side::BUY ? "BUY" : "SELL");
        json.key("parent_qty").integer_text(parent.qty);
        json.key("decision_time_ns").integer_text(parent.decision_time);
        json.key("benchmark").string("reference_exchange_mid_at_decision");
        json.key("arrival_mid");
        if (parent.has_decision_mid) json.doubled_price_text(parent.decision_mid_times_two);
        else json.null();
        json.key("visible_mid_at_decision");
        if (parent.visible_at_decision.two_sided()) json.doubled_price_text(parent.visible_at_decision.mid_times_two());
        else json.null();
        json.key("filled_qty").integer_text(parent.filled_qty);
        json.key("residual_qty").integer_text(residual);
        json.key("completion_ratio").number(*completion);
        json.key("fill_notional").price_text(parent.fill_notional);
        json.key("fees").price_text(parent.fees);
        put_optional_number(json, "average_fill_price",
                            parent.filled_qty == 0 ? std::nullopt
                                                   : std::optional<double>(static_cast<double>(parent.fill_notional) /
                                                                           static_cast<double>(parent.filled_qty) / kPriceUnitsPerCurrency));
        json.key("first_fill_time_ns");
        if (parent.filled_qty > 0) json.integer_text(parent.first_fill_time);
        else json.null();
        json.key("last_fill_time_ns");
        if (parent.filled_qty > 0) json.integer_text(parent.last_fill_time);
        else json.null();
        json.key("opposite_side_qty").integer_text(parent.opposite_side_qty);
        // Shortfall (cost, positive = worse than arrival): sign * (fill notional - arrival mid * qty) + fees.
        json.key("implementation_shortfall");
        if (parent.has_decision_mid && parent.filled_qty > 0) {
            const std::int64_t price_cost_times_two =
                sign * (2 * parent.fill_notional - parent.decision_mid_times_two * static_cast<std::int64_t>(parent.filled_qty));
            const std::int64_t total_times_two = price_cost_times_two + 2 * parent.fees;
            shortfall_per_share = per_share_currency(total_times_two, parent.filled_qty);
            json.begin_object();
            json.key("price_cost").doubled_price_text(price_cost_times_two);
            json.key("fees").price_text(parent.fees);
            json.key("total").doubled_price_text(total_times_two);
            put_optional_number(json, "per_share", shortfall_per_share);
            put_optional_number(json, "bps",
                                ratio(static_cast<double>(total_times_two) / static_cast<double>(parent.filled_qty) * 1e4,
                                      static_cast<double>(parent.decision_mid_times_two)));
            json.end_object();
        } else {
            json.null();
        }
        put_optional_number(json, "participation_rate", participation);
        json.end_object();
    }

    json.key("background_agents").begin_array();
    for (const auto& agent : core.background_agent_summaries()) {
        json.begin_object();
        json.key("name").string(agent.name);
        json.key("kind").string(to_string(agent.kind));
        json.key("session_id").integer_text(agent.session_id);
        json.key("seed").integer_text(agent.seed);
        json.key("commands_submitted").integer_text(agent.stats.commands_submitted);
        json.key("fills_received").integer_text(agent.stats.fills_received);
        json.key("filled_qty").integer_text(agent.stats.filled_qty);
        json.key("rejects_received").integer_text(agent.stats.rejects_received);
        json.key("inventory").signed_integer_text(agent.inventory);
        json.end_object();
    }
    json.end_array();

    // Flat, derived convenience values (JSON numbers or null) for paired comparison.
    // Currency values are in currency units; exact values live in the sections above.
    const auto currency = [](std::int64_t price_units) { return static_cast<double>(price_units) / kPriceUnitsPerCurrency; };
    const auto doubled_currency = [](std::int64_t price_units_times_two) {
        return static_cast<double>(price_units_times_two) / (2 * kPriceUnitsPerCurrency);
    };
    json.key("metrics").begin_object();
    put_optional_number(json, "net_pnl", valuation.has_equity ? std::optional<double>(doubled_currency(valuation.net_pnl_times_two)) : std::nullopt);
    put_optional_number(json, "equity", valuation.has_equity ? std::optional<double>(doubled_currency(valuation.equity_times_two)) : std::nullopt);
    json.key("realized_pnl").number(currency(ledger_state.gross_realized_pnl));
    put_optional_number(json, "unrealized_pnl",
                        valuation.has_equity ? std::optional<double>(doubled_currency(valuation.unrealized_pnl_times_two)) : std::nullopt);
    json.key("fees").number(currency(ledger_state.fees));
    json.key("final_position").number(static_cast<double>(ledger_state.position));
    json.key("max_position").number(static_cast<double>(metrics.max_position));
    json.key("min_position").number(static_cast<double>(metrics.min_position));
    json.key("limit_breaches").number(static_cast<double>(metrics.limit_breaches));
    put_optional_number(json, "max_drawdown", metrics.has_equity ? std::optional<double>(doubled_currency(metrics.max_drawdown_times_two)) : std::nullopt);
    json.key("orders_submitted").number(static_cast<double>(metrics.new_orders_submitted));
    json.key("orders_accepted").number(static_cast<double>(metrics.orders_accepted));
    json.key("orders_rejected").number(static_cast<double>(metrics.orders_rejected));
    json.key("risk_rejects").number(static_cast<double>(metrics.risk_rejects));
    json.key("cancel_requests").number(static_cast<double>(metrics.cancel_requests_submitted));
    json.key("replace_requests").number(static_cast<double>(metrics.replace_requests_submitted));
    json.key("fill_count").number(static_cast<double>(metrics.fill_count));
    json.key("filled_qty").number(static_cast<double>(metrics.filled_qty));
    json.key("maker_qty").number(static_cast<double>(metrics.maker_qty));
    json.key("taker_qty").number(static_cast<double>(metrics.taker_qty));
    put_optional_number(json, "order_fill_ratio",
                        ratio(static_cast<double>(metrics.orders_with_fill), static_cast<double>(metrics.orders_accepted)));
    put_optional_number(json, "quantity_fill_ratio",
                        ratio(static_cast<double>(metrics.filled_qty), static_cast<double>(submitted_order_qty_denominator)));
    put_optional_number(json, "time_to_first_fill_mean_ns", mean_ns(metrics.time_to_first_fill));
    put_optional_number(json, "two_sided_exposure_ratio", ratio(static_cast<double>(metrics.two_sided_exposure_ns), duration_ns));
    put_optional_number(json, "spread_capture_per_share",
                        per_share_currency(metrics.spread_capture_total_times_two, metrics.spread_capture_qty));
    json.key("spread_capture_total").number(static_cast<double>(metrics.spread_capture_total_times_two) / 2.0 / kPriceUnitsPerCurrency);
    for (std::size_t horizon = 0; horizon < config.metrics.markout_horizon_count; ++horizon) {
        const auto& accumulator = metrics.markouts[horizon];
        const std::string prefix = "markout_" + format_duration(config.metrics.markout_horizons_ns[horizon]);
        put_optional_number(json, prefix + "_per_share", per_share_currency(accumulator.total_times_two, accumulator.measured_qty));
        json.key(prefix + "_censored").number(static_cast<double>(accumulator.censored));
    }
    put_optional_number(json, "completion_ratio", completion);
    put_optional_number(json, "residual_qty",
                        parent.declared ? std::optional<double>(static_cast<double>(parent.qty) - static_cast<double>(std::min<std::uint64_t>(parent.filled_qty, parent.qty)))
                                        : std::nullopt);
    put_optional_number(json, "implementation_shortfall_per_share", shortfall_per_share);
    put_optional_number(json, "participation_rate", participation);
    put_optional_number(json, "feed_latency_mean_ns", mean_ns(metrics.feed_latency));
    put_optional_number(json, "order_latency_mean_ns", mean_ns(metrics.order_latency));
    put_optional_number(json, "cancel_latency_mean_ns", mean_ns(metrics.cancel_latency));
    put_optional_number(json, "ack_latency_mean_ns", mean_ns(metrics.ack_latency));
    json.key("market_traded_qty").number(static_cast<double>(metrics.market_traded_qty));
    json.end_object();

    json.end_object();
    std::string text = json.take();
    text += '\n';
    return text;
}

}  // namespace exsim::research
