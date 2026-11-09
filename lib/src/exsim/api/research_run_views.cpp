// Read views of a ResearchRun: run, manifest, summary, series, events,
// orders, lifecycle, fills, book and comparison inputs.

#include "exsim/api/api_json.hpp"
#include "exsim/api/research_run.hpp"
#include "exsim/research/json.hpp"
#include "simulation_driver.hpp"
#include "exsim/api/request_diagnostics.hpp"
#include "exsim/api/markout_values.hpp"

#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <fstream>

namespace exsim::api {

namespace {

constexpr std::uint64_t kMaxEventsScannedPerPage = 1'000'000;

const char* fill_liquidity_role(research::Liquidity liquidity) noexcept { return to_api_liquidity(liquidity); }

std::string doubled(std::int64_t value_times_two) { return format_doubled_price_units(value_times_two); }

void write_page_info(ApiJsonWriter& json, const std::optional<std::string>& next_cursor, std::uint64_t limit) {
    json.key("page").begin_object();
    json.key("nextCursor").optional_string(next_cursor);
    json.key("limit").integer_number(static_cast<std::int64_t>(limit));
    json.end_object();
}

void write_unsigned_or_null(ApiJsonWriter& json, bool present, std::uint64_t value) {
    if (present) json.unsigned_text(value);
    else json.null();
}

void write_position_or_null(ApiJsonWriter& json, std::uint64_t position) {
    write_unsigned_or_null(json, position != kNoEventPosition, position);
}

const char* allowed_actions_table(ApiRunState state, std::size_t index) noexcept {
    static const char* const queued[] = {"cancel", nullptr};
    static const char* const running[] = {"pause", "cancel", nullptr};
    static const char* const paused[] = {"resume", "step", "cancel", nullptr};
    static const char* const none[] = {nullptr};
    const char* const* row = none;
    if (state == ApiRunState::Queued) row = queued;
    else if (state == ApiRunState::Running) row = running;
    else if (state == ApiRunState::Paused) row = paused;
    return row[index];
}

std::string order_final_status(const OwnOrderRecord& order) {
    if (order.rejected) return "rejected";
    if (order.canceled) return order.cumulative_quantity > 0 ? "partially_filled_then_canceled" : "canceled";
    if (order.done && order.cumulative_quantity > 0) return "filled";
    return "open_at_run_end";
}

void write_order_summary(ApiJsonWriter& json, const OwnOrderRecord& order) {
    json.begin_object();
    json.key("orderId").unsigned_text(order.current_order_id);
    json.key("clientOrderId").unsigned_text(order.client_order_id);
    json.key("side").string(to_api_side(order.side));
    json.key("price").string(format_price_units(order.price));
    json.key("quantity").unsigned_text(order.quantity);
    json.key("cumulativeQuantity").unsigned_text(order.cumulative_quantity);
    json.key("finalStatus").string(order_final_status(order));
    json.key("submitEventPosition").unsigned_text(order.submit_position);
    json.key("submitSimulationTimeNs").unsigned_text(order.submit_time_ns);
    json.key("lastEventPosition").unsigned_text(order.last_position);
    json.key("fillCount").integer_number(order.fill_count);
    json.end_object();
}

// LifecycleStageKind of one own-order event; nullptr when the event is not a stage.
const char* lifecycle_stage_kind(const CanonicalEventRecord& event) noexcept {
    switch (event.kind) {
        case CanonicalEventKind::StrategyOrderSubmitted: return "strategy_submitted";
        case CanonicalEventKind::StrategyCancelSubmitted: return "cancel_submitted";
        case CanonicalEventKind::StrategyReplaceSubmitted: return "replace_submitted";
        case CanonicalEventKind::OrderArrivedAtExchange: return "arrived_at_exchange";
        case CanonicalEventKind::CancelArrivedAtExchange: return "cancel_arrived_at_exchange";
        case CanonicalEventKind::ReplaceArrivedAtExchange: return "replace_arrived_at_exchange";
        case CanonicalEventKind::OrderAccepted: return "accepted";
        case CanonicalEventKind::OrderRested: return "rested";
        case CanonicalEventKind::OrderFilled: return event.leaves_quantity == 0 ? "filled" : "partially_filled";
        case CanonicalEventKind::OrderCanceled: return "canceled";
        case CanonicalEventKind::OrderReplaced: return "replaced";
        case CanonicalEventKind::OrderRejected:
            if (event.rejected_command == CommandType::Cancel) return "cancel_rejected_too_late";
            if (event.rejected_command == CommandType::Replace) return "replace_rejected";
            return "rejected";
        case CanonicalEventKind::ExecutionReportDelivered:
            switch (event.report_kind) {
                case DeliveredReportKind::Accepted: return "acknowledgment_delivered";
                case DeliveredReportKind::Rested: return "rested_report_delivered";
                case DeliveredReportKind::Filled: return "fill_report_delivered";
                case DeliveredReportKind::Canceled: return "cancel_report_delivered";
                case DeliveredReportKind::Replaced: return "replace_report_delivered";
                case DeliveredReportKind::Rejected:
                case DeliveredReportKind::CancelRejected: return "reject_report_delivered";
            }
            return nullptr;
        default: return nullptr;
    }
}

struct SummaryInputs {
    research::LedgerState ledger;
    research::RunMetrics metrics;
    std::optional<std::int64_t> mark_times_two;
    Timestamp as_of_time_ns = 0;
};

bool is_execution_strategy(const research::ExperimentConfig& config) {
    return config.strategy.kind == research::StrategyKind::TwapExecutor ||
           config.strategy.kind == research::StrategyKind::ImmediateSweepExecutor;
}

bool is_execution_metric(std::string_view key) {
    return key == "completion_ratio" || key == "residual_qty" || key == "decision_slippage_total" ||
           key == "decision_slippage_per_share" || key == "implementation_shortfall_total" ||
           key == "implementation_shortfall_per_share" || key == "participation_rate";
}

const char* execution_unavailable_reason(const research::ExperimentConfig& config, const research::ParentOrderTracking& parent,
                                         std::string_view key) {
    if (!is_execution_strategy(config)) return "not_a_parent_order_strategy";
    if (!parent.declared) return "parent_order_not_declared";
    if (parent.filled_qty == 0) return "no_parent_fills";
    if (key == "participation_rate") return "no_market_volume_in_execution_window";
    return "no_reference_exchange_mid_at_decision";
}

std::vector<RunComparisonInputs::Metric> summary_metrics(const research::ExperimentConfig& config, const SummaryInputs& inputs) {
    const auto& ledger = inputs.ledger;
    const auto& metrics = inputs.metrics;
    std::optional<research::DoubledValuation> valuation;
    if (inputs.mark_times_two) valuation = research::value_at_mark(ledger, config.accounting.initial_cash, *inputs.mark_times_two);
    std::optional<std::string> net_pnl, equity, unrealized, fill_rate;
    if (valuation) {
        net_pnl = doubled(valuation->equity_times_two - 2 * config.accounting.initial_cash);
        equity = doubled(valuation->equity_times_two);
        unrealized = doubled(valuation->unrealized_pnl_times_two);
    }
    const auto submitted_shares = metrics.accepted_order_qty + metrics.replace_added_qty;
    if (submitted_shares > 0)
        fill_rate = format_derived_decimal(static_cast<double>(metrics.filled_qty) / static_cast<double>(submitted_shares));
    std::vector<RunComparisonInputs::Metric> result;
    const auto add = [&](const char* key, std::optional<std::string> value, const char* unit,
                         const char* exactness = "exact_fixed_point", std::optional<Timestamp> horizon = std::nullopt) {
        result.push_back({key, horizon, std::move(value), unit, exactness});
    };
    add("net_pnl", net_pnl, "currency");
    add("equity", equity, "currency");
    add("gross_realized_pnl", format_price_units(ledger.gross_realized_pnl), "currency");
    add("unrealized_pnl", unrealized, "currency");
    add("net_fees", format_price_units(ledger.fees), "currency");
    add("ending_position", std::to_string(ledger.position), "shares");
    add("max_long_position", std::to_string(metrics.max_position), "shares");
    add("max_short_position", std::to_string(metrics.min_position), "shares");
    add("max_drawdown", doubled(metrics.max_drawdown_times_two), "currency");
    add("fill_count", std::to_string(metrics.fill_count), "count");
    add("filled_shares", std::to_string(metrics.filled_qty), "shares");
    add("maker_fill_count", std::to_string(metrics.maker_fill_count), "count");
    add("taker_fill_count", std::to_string(metrics.taker_fill_count), "count");
    add("fill_rate", fill_rate, "ratio", "derived_float");
    add("submitted_order_count", std::to_string(metrics.new_orders_submitted), "count");
    add("new_order_rejected_count", std::to_string(metrics.orders_rejected), "count");
    add("cancel_request_count", std::to_string(metrics.cancel_requests_submitted), "count");
    add("cancel_rejected_count", std::to_string(metrics.cancel_requests_rejected), "count");
    add("replace_request_count", std::to_string(metrics.replace_requests_submitted), "count");
    add("replace_rejected_count", std::to_string(metrics.replace_requests_rejected), "count");
    add("canceled_order_count", std::to_string(metrics.orders_canceled_by_request), "count");
    add("cancel_race_lost_count", std::to_string(metrics.cancel_requests_rejected), "count");
    add("spread_capture_total", doubled(metrics.spread_capture_total_times_two), "currency");
    for (std::size_t horizon = 0; horizon < config.metrics.markout_horizon_count; ++horizon) {
        const auto& row = metrics.markouts[horizon];
        const auto ns = config.metrics.markout_horizons_ns[horizon];
        add("markout_total", markout_total_value(row), "currency", "exact_fixed_point", ns);
        add("markout_per_share", markout_per_share_value(row),
            "currency_per_share", "derived_float", ns);
        add("markout_eligible_shares", std::to_string(row.measured_qty), "shares", "exact_fixed_point", ns);
        add("markout_eligible_fills", std::to_string(row.measured), "count", "exact_fixed_point", ns);
        add("markout_no_mark_fills", std::to_string(row.unavailable), "count", "exact_fixed_point", ns);
        add("markout_censored_fills", std::to_string(row.censored + metrics.fill_count - row.measured - row.unavailable - row.censored), "count", "exact_fixed_point", ns);
    }
    add("quote_exposure_time", std::to_string(metrics.any_side_exposure_ns), "nanoseconds");
    const auto& parent = metrics.parent;
    std::optional<std::string> completion, residual, slippage, slippage_per_share, shortfall, shortfall_per_share, participation;
    if (parent.declared) {
        completion = format_derived_decimal(static_cast<double>(parent.filled_qty) / parent.qty);
        residual = std::to_string(parent.qty - std::min<std::uint64_t>(parent.filled_qty, parent.qty));
        const auto volume = parent.market_volume_at_last_fill - parent.market_volume_at_decision;
        if (volume > 0) participation = format_derived_decimal(static_cast<double>(parent.filled_qty) / static_cast<double>(volume));
        if (parent.has_decision_mid && parent.filled_qty > 0) {
            // Runner benchmark: exchange reference mid at parent declaration, called arrival_mid in native artifacts.
            // Executed quantity only; no opportunity cost for the unexecuted residual is inferred.
            const std::int64_t sign = parent.side == Side::BUY ? 1 : -1;
            const auto price_cost = sign * (2 * parent.fill_notional - parent.decision_mid_times_two * static_cast<std::int64_t>(parent.filled_qty));
            const auto total_cost = price_cost + 2 * parent.fees;
            slippage = doubled(price_cost);
            shortfall = doubled(total_cost);
            const double divisor = 2.0 * static_cast<double>(parent.filled_qty) * static_cast<double>(kPriceScale);
            slippage_per_share = format_derived_decimal(static_cast<double>(price_cost) / divisor);
            shortfall_per_share = format_derived_decimal(static_cast<double>(total_cost) / divisor);
        }
    }
    add("completion_ratio", completion, "ratio", "derived_float");
    add("residual_qty", residual, "shares");
    add("decision_slippage_total", slippage, "currency");
    add("decision_slippage_per_share", slippage_per_share, "currency_per_share", "derived_float");
    add("implementation_shortfall_total", shortfall, "currency");
    add("implementation_shortfall_per_share", shortfall_per_share, "currency_per_share", "derived_float");
    add("participation_rate", participation, "ratio", "derived_float");
    return result;
}

class ComparisonDirectory {
public:
    ComparisonDirectory() {
        const auto pattern = (std::filesystem::temp_directory_path() / "exsim_api_verify_XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        if (::mkdtemp(name.data()) == nullptr) throw research::ArtifactError("cannot create comparison verification directory");
        path = name.data();
    }
    ~ComparisonDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ComparisonDirectory(const ComparisonDirectory&) = delete;
    ComparisonDirectory& operator=(const ComparisonDirectory&) = delete;
    std::filesystem::path path;
};

void write_metric(ApiJsonWriter& json, const char* key, const std::optional<std::string>& value, const char* unit,
                  const char* exactness = "exact_fixed_point", std::optional<Timestamp> horizon = std::nullopt,
                  const char* unavailable_reason = "unavailable") {
    json.begin_object();
    json.key("metricKey").string(key);
    json.key("horizonNs");
    write_unsigned_or_null(json, horizon.has_value(), horizon.value_or(0));
    json.key("value").optional_string(value);
    json.key("unit").string(unit);
    json.key("exactness").string(exactness);
    json.key("unavailableReason");
    if (value) json.null();
    else json.string(unavailable_reason);
    json.end_object();
}

}  // namespace

// ---- Cursors ---------------------------------------------------------------------------

std::string encode_page_cursor(std::uint64_t next_index, std::string_view filter_fingerprint) {
    return "c" + std::to_string(next_index) + "_" + research::format_hex64(fnv1a64_bytes(filter_fingerprint)).substr(0, 8);
}

std::optional<std::uint64_t> decode_page_cursor(std::string_view cursor, std::string_view filter_fingerprint) {
    if (cursor.size() < 4 || cursor.front() != 'c') return std::nullopt;
    const std::size_t separator = cursor.find('_');
    if (separator == std::string_view::npos) return std::nullopt;
    std::uint64_t index = 0;
    if (!parse_unsigned_integer_text(cursor.substr(1, separator - 1), index)) return std::nullopt;
    if (cursor.substr(separator + 1) != research::format_hex64(fnv1a64_bytes(filter_fingerprint)).substr(0, 8)) return std::nullopt;
    return index;
}

// ---- Run ---------------------------------------------------------------------------------

std::string ResearchRun::render_run_json() const {
    std::lock_guard lock(mutex_);
    return render_run_locked();
}

std::string ResearchRun::render_run_locked() const {
    const TranslatedExperimentConfig& config = *parameters_.config;
    const std::uint64_t processed = artifacts_loaded_ ? processed_event_count_locked() : history_.back().at_event_position;
    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("experimentId").string(parameters_.experiment_id);
    json.key("masterSeed").unsigned_text(parameters_.master_seed);
    json.key("mode").string("deterministic_research");
    json.key("marketWorld").string(config.market_world);
    json.key("state").string(to_api_string(state_));
    json.key("allowedActions").begin_array();
    for (std::size_t index = 0; allowed_actions_table(state_, index) != nullptr; ++index) json.string(allowed_actions_table(state_, index));
    json.end_array();
    json.key("progress").begin_object();
    json.key("processedCanonicalEventCount").unsigned_text(processed);
    json.key("simulationTimeNs").unsigned_text(simulation_time_locked());
    json.key("endSimulationTimeNs").unsigned_text(config.runner_config.limits.duration_ns);
    json.end_object();
    json.key("stateHistory").begin_array();
    for (const RunStateTransitionRecord& transition : history_) {
        json.begin_object();
        json.key("fromState");
        if (transition.from_state) json.string(to_api_string(*transition.from_state));
        else json.null();
        json.key("toState").string(to_api_string(transition.to_state));
        json.key("transitionedAt").string(format_wall_clock_time(transition.transitioned_at_unix_ms));
        json.key("atEventPosition").unsigned_text(transition.at_event_position);
        json.key("atSimulationTimeNs").unsigned_text(transition.at_simulation_time_ns);
        json.key("reason").string(transition.reason);
        json.end_object();
    }
    json.end_array();
    json.key("createdAt").string(format_wall_clock_time(parameters_.created_at_unix_ms));
    json.key("terminalDiagnostics");
    if (diagnostics_ && is_terminal(state_)) {
        json.begin_object();
        json.key("outcomeCode").string(diagnostics_->outcome_code);
        json.key("message").string(diagnostics_->message);
        json.key("residualPositionShares").signed_text(diagnostics_->residual_position_shares);
        json.key("residualOpenOrderCount").unsigned_text(diagnostics_->residual_open_order_count);
        json.end_object();
    } else {
        json.null();
    }
    json.key("digests").begin_object();
    json.key("manifestDigest").begin_object().key("algorithm").string("sha256").key("hexDigest").string(manifest_digest_hex_).end_object();
    json.key("canonicalEventDigest");
    if (canonical_event_digest_hex_ && is_terminal(state_))
        json.begin_object().key("algorithm").string("fnv1a_64").key("hexDigest").string(*canonical_event_digest_hex_).end_object();
    else json.null();
    json.key("ledgerDigest");
    if (ledger_digest_hex_ && is_terminal(state_))
        json.begin_object().key("algorithm").string("fnv1a_64").key("hexDigest").string(*ledger_digest_hex_).end_object();
    else json.null();
    json.end_object();
    json.key("parentRunId").null();
    json.end_object();
    return json.take();
}

std::string ResearchRun::render_manifest_json() const {
    std::lock_guard lock(mutex_);
    return manifest_json_;
}

// ---- Summary ------------------------------------------------------------------------------

std::string ResearchRun::render_summary_json() const {
    std::lock_guard lock(mutex_);
    if (is_terminal(state_) && !final_summary_json_.empty()) {
        auto summary = research::parse_json(final_summary_json_);
        for (auto& member : summary.members) {
            if (member.name == "orders" && !member.value.find("requestDiagnostics")) {
                ApiJsonWriter evidence; write_request_diagnostics(evidence, artifacts_.events);
                member.value.members.push_back({"requestDiagnostics", research::parse_json(evidence.take())});
            }
            if (member.name == "markouts") for (auto& horizon : member.value.items) {
                if (horizon.find("availableShares")) continue;
                std::uint64_t ns = 0, shares = 0;
                if (const auto* value = horizon.find("horizonNs")) (void)parse_unsigned_integer_text(value->text, ns);
                for (const auto& row : artifacts_.markouts) if (row.horizon_ns == ns && row.status == research::MarkoutStatus::Measured) shares += row.qty;
                horizon.members.push_back({"availableShares", research::parse_json("\"" + std::to_string(shares) + "\"")});
            }
        }
        for (auto& member : summary.members) if (member.name == "metrics") {
            for (auto& metric : member.value.items) {
                const auto* key = metric.find("metricKey");
                if (!key || (key->text != "markout_total" && key->text != "markout_per_share")) continue;
                for (const auto& group : summary.members) if (group.name == "markouts") for (const auto& horizon : group.value.items) {
                    if (horizon.find("horizonNs")->text != metric.find("horizonNs")->text || horizon.find("availableFillCount")->text != "0") continue;
                    for (auto& value : metric.members) {
                        if (value.name == "value") value.value = research::parse_json("null");
                        if (value.name == "unavailableReason") value.value = research::parse_json("\"no_measured_markout_shares\"");
                    }
                }
            }
        }
        return serialize_json_value(summary);
    }
    if (is_terminal(state_) && !driver_ && !artifacts_loaded_)
        throw ApiFailure(409, "artifact_not_ready", "the failed run has no persisted summary; in-memory accounting was lost at restart");
    return render_summary_locked();
}

std::string ResearchRun::render_summary_locked() const {
    const research::ExperimentConfig& config = parameters_.config->runner_config;
    SummaryInputs inputs;
    inputs.ledger.cash = config.accounting.initial_cash;
    if (driver_) {
        research::SimulationCore& core = driver_->core();
        inputs.ledger = core.ledger().state();
        inputs.metrics = core.metrics();
        inputs.mark_times_two = core.mark_times_two();
        inputs.as_of_time_ns = core.now();
    } else {
        inputs.as_of_time_ns = final_simulation_time_ns_;
    }
    const bool final_summary = is_terminal(state_);
    const std::int64_t initial_cash = config.accounting.initial_cash;
    const research::LedgerState& ledger = inputs.ledger;
    const research::RunMetrics& metrics = inputs.metrics;

    std::optional<research::DoubledValuation> valuation;
    if (inputs.mark_times_two) valuation = research::value_at_mark(ledger, initial_cash, *inputs.mark_times_two);
    const char* mark_unavailable_reason = config.accounting.mark == research::MarkConvention::LastTrade
                                              ? "no trade has printed yet"
                                              : "no two-sided exchange book has existed yet";

    std::optional<std::string> net_pnl;
    std::optional<std::string> equity;
    std::optional<std::string> unrealized;
    if (valuation) {
        equity = doubled(valuation->equity_times_two);
        net_pnl = doubled(valuation->equity_times_two - 2 * initial_cash);
        unrealized = doubled(valuation->unrealized_pnl_times_two);
    }
    const std::uint64_t submitted_shares = metrics.accepted_order_qty + metrics.replace_added_qty;
    std::optional<std::string> fill_rate;
    if (submitted_shares > 0)
        fill_rate = format_derived_decimal(static_cast<double>(metrics.filled_qty) / static_cast<double>(submitted_shares));

    ApiJsonWriter json;
    json.begin_object();
    json.key("schemaVersion").string("exsim.run-summary/1");
    json.key("runId").string(parameters_.run_id);
    json.key("availability").string(final_summary ? "final" : "partial");
    json.key("asOfEventPosition").unsigned_text(processed_event_count_locked());
    json.key("asOfSimulationTimeNs").unsigned_text(inputs.as_of_time_ns);

    json.key("accounting").begin_object();
    json.key("initialCash").string(format_price_units(initial_cash));
    json.key("cash").string(format_price_units(ledger.cash));
    json.key("positionShares").signed_text(ledger.position);
    json.key("averageCostBasis");
    if (ledger.position == 0) {
        json.null();
    } else {
        const double open_cost = static_cast<double>(research::open_cost_basis(ledger, initial_cash));
        json.string(format_derived_decimal(std::fabs(open_cost) / std::fabs(static_cast<double>(ledger.position)) / static_cast<double>(kPriceScale)));
    }
    json.key("grossRealizedPnl").string(format_price_units(ledger.gross_realized_pnl));
    json.key("unrealizedPnl").optional_string(unrealized);
    json.key("netFees").string(format_price_units(ledger.fees));
    json.key("markPrice");
    if (inputs.mark_times_two) json.string(doubled(*inputs.mark_times_two));
    else json.null();
    json.key("markConvention").string(parameters_.config->mark_convention);
    json.key("equity").optional_string(equity);
    json.key("netPnl").optional_string(net_pnl);
    json.key("markUnavailableReason");
    if (inputs.mark_times_two) json.null();
    else json.string(mark_unavailable_reason);
    json.end_object();

    json.key("orders").begin_object();
    json.key("submittedOrderCount").unsigned_text(metrics.new_orders_submitted);
    json.key("acceptedOrderCount").unsigned_text(metrics.orders_accepted);
    json.key("rejectedOrderCount").unsigned_text(metrics.orders_rejected);
    json.key("canceledOrderCount").unsigned_text(metrics.orders_canceled_by_request);
    json.key("cancelRaceLostCount").unsigned_text(metrics.cancel_requests_rejected);
    json.key("requestDiagnostics"); write_request_diagnostics(json, artifacts_.events);
    json.key("fillCount").unsigned_text(metrics.fill_count);
    json.key("makerFillCount").unsigned_text(metrics.maker_fill_count);
    json.key("takerFillCount").unsigned_text(metrics.taker_fill_count);
    json.key("filledShares").unsigned_text(metrics.filled_qty);
    json.key("submittedShares").unsigned_text(submitted_shares);
    json.key("fillRateByShares").begin_object();
    json.key("numerator").unsigned_text(metrics.filled_qty);
    json.key("denominator").unsigned_text(submitted_shares);
    json.key("ratio").optional_string(fill_rate);
    json.key("definition").string(
        "filled shares of own orders divided by shares accepted by the exchange (new orders plus replace increases, including orders later cancelled)");
    json.end_object();
    json.key("quoteExposureTimeNs").unsigned_text(metrics.any_side_exposure_ns);
    json.end_object();

    json.key("risk").begin_object();
    json.key("maxLongPositionShares").signed_text(metrics.max_position);
    json.key("maxShortPositionShares").signed_text(metrics.min_position);
    json.key("blockedOrderCount").unsigned_text(metrics.risk_rejects);
    json.key("maxDrawdown").string(doubled(metrics.max_drawdown_times_two));
    json.key("maxDrawdownPeakTimeNs").null();
    json.key("maxDrawdownTroughTimeNs");
    write_unsigned_or_null(json, metrics.max_drawdown_times_two > 0, metrics.max_drawdown_time);
    json.end_object();

    json.key("spreadCaptureTotal").string(doubled(metrics.spread_capture_total_times_two));

    json.key("markouts").begin_array();
    for (std::size_t horizon = 0; horizon < config.metrics.markout_horizon_count; ++horizon) {
        const research::MarkoutAccumulator& markout = metrics.markouts[horizon];
        // A partial summary counts horizons not yet observed as censored at the as-of time.
        const std::uint64_t resolved = markout.measured + markout.unavailable + markout.censored;
        const std::uint64_t pending = metrics.fill_count > resolved ? metrics.fill_count - resolved : 0;
        json.begin_object();
        json.key("horizonNs").unsigned_text(config.metrics.markout_horizons_ns[horizon]);
        json.key("availableFillCount").unsigned_text(markout.measured);
        json.key("availableShares").unsigned_text(markout.measured_qty);
        json.key("censoredFillCount").unsigned_text(markout.censored + pending);
        json.key("unavailableFillCount").unsigned_text(markout.unavailable);
        json.key("totalMarkout"); if (markout.measured_qty) json.string(doubled(markout.total_times_two)); else json.null();
        json.key("meanMarkoutPerShare");
        if (markout.measured_qty == 0) json.null();
        else
            json.string(format_derived_decimal(static_cast<double>(markout.total_times_two) / 2.0 / static_cast<double>(markout.measured_qty) /
                                               static_cast<double>(kPriceScale)));
        json.end_object();
    }
    json.end_array();

    json.key("latency").begin_array();
    const auto latency = [&](const char* component, Timestamp configured, const research::DurationStatistic& statistic) {
        json.begin_object();
        json.key("component").string(component);
        json.key("configuredNs").unsigned_text(configured);
        json.key("sampleCount").unsigned_text(statistic.count);
        json.key("observedMinNs");
        write_unsigned_or_null(json, statistic.count > 0, statistic.min);
        json.key("observedMaxNs");
        write_unsigned_or_null(json, statistic.count > 0, statistic.max);
        json.end_object();
    };
    latency("feed", config.latency.feed_ns, metrics.feed_latency);
    latency("order_entry", config.latency.order_ns, metrics.order_latency);
    latency("acknowledgment", config.latency.ack_ns, metrics.ack_latency);
    latency("cancel", config.latency.cancel_ns, metrics.cancel_latency);
    json.end_array();

    json.key("execution");
    if (!is_execution_strategy(config)) {
        json.null();
    } else {
        const auto& parent = metrics.parent;
        const auto& parameters = config.strategy.parent;
        const Timestamp window_end = parameters.start_ns + parameters.execution_duration_ns;
        const char* status = inputs.as_of_time_ns >= window_end ? "window_observed" :
                             (final_summary ? "censored_run_end" : "ongoing");
        json.begin_object();
        json.key("parentSide").string(to_api_side(parameters.side));
        json.key("parentQuantity").unsigned_text(parameters.parent_qty);
        json.key("parentDeclared").boolean(parent.declared);
        json.key("observationStatus").string(status);
        json.key("benchmark").string("reference_exchange_mid_at_decision");
        json.key("benchmarkMid");
        if (parent.declared && parent.has_decision_mid) json.string(doubled(parent.decision_mid_times_two));
        else json.null();
        json.key("visibleMidAtDecision");
        if (parent.declared && parent.visible_at_decision.two_sided()) json.string(doubled(parent.visible_at_decision.mid_times_two()));
        else json.null();
        json.key("decisionTimeNs");
        write_unsigned_or_null(json, parent.declared, parent.decision_time);
        json.key("firstFillTimeNs");
        write_unsigned_or_null(json, parent.filled_qty > 0, parent.first_fill_time);
        json.key("lastFillTimeNs");
        write_unsigned_or_null(json, parent.filled_qty > 0, parent.last_fill_time);
        json.key("filledQuantity");
        write_unsigned_or_null(json, parent.declared, parent.filled_qty);
        json.key("fillNotional");
        if (parent.declared) json.string(format_price_units(parent.fill_notional));
        else json.null();
        json.key("netFees");
        if (parent.declared) json.string(format_price_units(parent.fees));
        else json.null();
        json.key("participationMarketQuantity");
        write_unsigned_or_null(json, parent.declared, parent.market_volume_at_last_fill - parent.market_volume_at_decision);
        json.key("unexecutedOpportunityCost").null();
        json.end_object();
    }

    json.key("metrics").begin_array();
    for (const auto& metric : summary_metrics(config, inputs))
        write_metric(json, metric.metric_key.c_str(), metric.value, metric.unit.c_str(), metric.exactness.c_str(), metric.horizon_ns,
                     is_execution_metric(metric.metric_key) ? execution_unavailable_reason(config, metrics.parent, metric.metric_key) :
                     (metric.metric_key.starts_with("markout_") ? "no_measured_markout_shares" : metric.metric_key == "fill_rate" ? "no shares submitted" : mark_unavailable_reason));
    json.end_array();
    json.end_object();
    return json.take();
}

// ---- Series --------------------------------------------------------------------------------

std::string ResearchRun::render_series_json(const SeriesQuery& query) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    const bool final_series = is_terminal(state_);
    const Timestamp latest = simulation_time_locked();
    const Timestamp default_to = final_series ? std::max(final_simulation_time_ns_, latest) : latest;
    const Timestamp from = query.from_time_ns.value_or(0);
    Timestamp to = query.to_time_ns.value_or(default_to);
    if (query.to_time_ns && *query.to_time_ns <= from) {
        ApiFailure failure(422, "validation_failed", "toSimulationTimeNs must be after fromSimulationTimeNs");
        failure.field_issues.push_back({"?toSimulationTimeNs", "must exceed fromSimulationTimeNs"});
        throw failure;
    }
    if (to <= from) to = from + 1;
    const Timestamp span = to - from;
    Timestamp bucket_width = (span + query.max_points - 1) / query.max_points;
    if (bucket_width == 0) bucket_width = 1;

    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("availability").string(final_series ? "final" : "partial");
    json.key("series").begin_array();
    for (const std::string& kind : query.kinds) {
        const char* unit = kind == "inventory" ? "shares"
                           : (kind == "exchange_mid" || kind == "quoted_spread") ? "currency_per_share"
                                                                                 : "currency";
        // Full-resolution values in [from, to].
        std::vector<std::pair<Timestamp, std::optional<std::string>>> values;
        bool has_peak = false;
        std::int64_t peak_equity_times_two = 0;
        for (const research::SeriesSample& sample : artifacts_.samples) {
            std::optional<std::string> value;
            if (kind == "equity") {
                if (sample.has_mark) value = doubled(sample.equity_times_two);
            } else if (kind == "inventory") {
                value = std::to_string(sample.position);
            } else if (kind == "exchange_mid") {
                if (sample.exchange.two_sided()) value = doubled(sample.exchange.mid_times_two());
            } else if (kind == "quoted_spread") {
                if (sample.visible.two_sided()) value = format_price_units(sample.visible.ask_price - sample.visible.bid_price);
            } else if (kind == "drawdown") {
                if (sample.has_mark) {
                    if (!has_peak || sample.equity_times_two > peak_equity_times_two) peak_equity_times_two = sample.equity_times_two;
                    has_peak = true;
                    value = doubled(peak_equity_times_two - sample.equity_times_two);
                }
            }
            if (sample.time < from || sample.time > to) continue;
            values.emplace_back(sample.time, std::move(value));
        }
        json.begin_object();
        json.key("seriesKind").string(kind);
        json.key("unit").string(unit);
        json.key("aggregation").string("last_value_in_bucket");
        json.key("bucketWidthNs").unsigned_text(bucket_width);
        json.key("fromSimulationTimeNs").unsigned_text(from);
        json.key("toSimulationTimeNs").unsigned_text(to);
        json.key("sourceSampleCount").unsigned_text(values.size());
        json.key("points").begin_array();
        std::size_t sample_index = 0;
        std::optional<std::string> last_value;
        for (Timestamp bucket_end = from + bucket_width;; bucket_end += bucket_width) {
            const Timestamp capped_end = bucket_end > to ? to : bucket_end;
            while (sample_index < values.size() && values[sample_index].first <= capped_end) last_value = values[sample_index++].second;
            json.begin_object().key("bucketEndTimeNs").unsigned_text(capped_end).key("value").optional_string(last_value).end_object();
            if (capped_end >= to) break;
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.take();
}

// ---- Events --------------------------------------------------------------------------------

std::string ResearchRun::render_events_json(const EventPageQuery& query, const std::string& filter_fingerprint) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    const std::vector<CanonicalEventRecord>& events = artifacts_.events;
    const OwnOrderRecord* order = query.order_id ? find_order_locked(*query.order_id) : nullptr;
    const std::uint64_t end_exclusive =
        std::min<std::uint64_t>(events.size(), query.to_position ? *query.to_position + 1 : events.size());

    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("totalEventCount").unsigned_text(events.size());
    json.key("items").begin_array();
    std::uint64_t returned = 0;
    std::uint64_t scanned = 0;
    std::uint64_t position = query.start_position;
    const auto matches = [&](std::uint64_t candidate) {
        const CanonicalEventRecord& event = events[candidate];
        if (!query.kinds.empty() && std::find(query.kinds.begin(), query.kinds.end(), event.kind) == query.kinds.end()) return false;
        if (query.visibility && event.visibility != *query.visibility) return false;
        if (query.actor && event.actor != *query.actor) return false;
        if (query.order_id) {
            if (order != nullptr) return std::binary_search(order->event_positions.begin(), order->event_positions.end(), candidate);
            return event.has(kHasOrderId) && event.order_id == *query.order_id;
        }
        return true;
    };
    if (order != nullptr) {
        // Jump through the order's own positions instead of scanning the log.
        auto iterator = std::lower_bound(order->event_positions.begin(), order->event_positions.end(), position);
        for (; iterator != order->event_positions.end() && *iterator < end_exclusive && returned < query.limit; ++iterator) {
            position = *iterator;
            if (!matches(position)) continue;
            write_canonical_event_json(json, events[position], position);
            ++returned;
        }
        position = iterator == order->event_positions.end() ? end_exclusive : *iterator;
    } else {
        for (; position < end_exclusive && returned < query.limit && scanned < kMaxEventsScannedPerPage; ++position, ++scanned) {
            if (!matches(position)) continue;
            write_canonical_event_json(json, events[position], position);
            ++returned;
        }
    }
    json.end_array();
    std::optional<std::string> next_cursor;
    // While the run is live the log keeps growing, so an open-ended page ends
    // with a cursor at the current end; a terminal log ends with null.
    const bool more = position < end_exclusive || (!is_terminal(state_) && !query.to_position);
    if (more) next_cursor = encode_page_cursor(position, filter_fingerprint);
    write_page_info(json, next_cursor, query.limit);
    json.end_object();
    return json.take();
}

// ---- Orders and lifecycle ---------------------------------------------------------------------

std::string ResearchRun::render_orders_json(const PageQuery& page, const std::optional<std::string>& final_status,
                                            const std::string& filter_fingerprint) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("items").begin_array();
    std::uint64_t index = page.start_index;
    std::uint64_t returned = 0;
    for (; index < artifacts_.orders.size() && returned < page.limit; ++index) {
        const OwnOrderRecord& order = artifacts_.orders[index];
        if (order.current_order_id == 0) continue;  // never accepted: no exchange order id (see /events)
        if (final_status && order_final_status(order) != *final_status) continue;
        write_order_summary(json, order);
        ++returned;
    }
    json.end_array();
    std::optional<std::string> next_cursor;
    if (index < artifacts_.orders.size()) next_cursor = encode_page_cursor(index, filter_fingerprint);
    write_page_info(json, next_cursor, page.limit);
    json.end_object();
    return json.take();
}

std::string ResearchRun::render_order_lifecycle_json(OrderId order_id) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    const OwnOrderRecord* order = find_order_locked(order_id);
    if (order == nullptr) throw ApiFailure(404, "not_found", "no own order with orderId " + std::to_string(order_id) + " in this run");
    const std::vector<CanonicalEventRecord>& events = artifacts_.events;

    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("order");
    write_order_summary(json, *order);
    json.key("matchingRule").string("price_time_fifo");
    json.key("stages").begin_array();
    std::optional<Timestamp> cancel_submit_time;
    std::optional<Timestamp> cancel_arrival_time;
    std::optional<Timestamp> first_fill_time;
    std::vector<std::uint64_t> fill_match_ids;
    for (const std::uint64_t position : order->event_positions) {
        const CanonicalEventRecord& event = events[position];
        const char* stage_kind = lifecycle_stage_kind(event);
        if (stage_kind == nullptr) continue;
        if (event.kind == CanonicalEventKind::StrategyCancelSubmitted && !cancel_submit_time) cancel_submit_time = event.simulation_time_ns;
        if (event.kind == CanonicalEventKind::CancelArrivedAtExchange && !cancel_arrival_time) cancel_arrival_time = event.simulation_time_ns;
        if (event.kind == CanonicalEventKind::OrderFilled) {
            if (!first_fill_time) first_fill_time = event.simulation_time_ns;
            fill_match_ids.push_back(event.match_id);
        }
        json.begin_object();
        json.key("stageKind").string(stage_kind);
        json.key("orderId"); write_unsigned_or_null(json, event.has(kHasOrderId), event.order_id);
        json.key("replacedOldOrderId"); write_unsigned_or_null(json, event.replaced_old_order_id != 0, event.replaced_old_order_id);
        json.key("eventPosition").unsigned_text(position);
        json.key("simulationTimeNs").unsigned_text(event.simulation_time_ns);
        json.key("visibility").string(to_api_string(event.visibility));
        json.key("quantity");
        write_unsigned_or_null(json, event.has(kHasQuantity), event.quantity);
        json.key("leavesQuantity");
        write_unsigned_or_null(json, event.has(kHasLeavesQuantity), event.leaves_quantity);
        json.key("matchId");
        write_unsigned_or_null(json, event.has(kHasMatchId), event.match_id);
        json.key("detail").string(describe_canonical_event(event));
        json.end_object();
    }
    json.end_array();

    std::vector<OrderId> order_ids;
    for (const auto& [alias, index] : artifacts_.order_by_exchange_order_id)
        if (&artifacts_.orders[index] == order) order_ids.push_back(alias);
    json.key("fifoAheadEvidence");
    const bool truncated = write_fifo_evidence(json, events, order_ids, order->last_position);
    json.key("fifoEvidenceTruncated").boolean(truncated);
    json.key("replaceTransits"); write_replace_transits(json, events, order->event_positions);
    json.key("latency").begin_object();
    json.key("orderEntryNs");
    write_unsigned_or_null(json, order->arrival_position != kNoEventPosition,
                           order->arrival_position == kNoEventPosition ? 0 : events[order->arrival_position].simulation_time_ns - order->submit_time_ns);
    json.key("acknowledgmentNs");
    write_unsigned_or_null(json, order->acknowledgment_position != kNoEventPosition && order->accepted_position != kNoEventPosition,
                           order->acknowledgment_position == kNoEventPosition || order->accepted_position == kNoEventPosition
                               ? 0
                               : events[order->acknowledgment_position].simulation_time_ns - events[order->accepted_position].simulation_time_ns);
    json.key("cancelNs");
    write_unsigned_or_null(json, cancel_submit_time && cancel_arrival_time,
                           cancel_submit_time && cancel_arrival_time ? *cancel_arrival_time - *cancel_submit_time : 0);
    json.key("submitToFirstFillNs");
    write_unsigned_or_null(json, first_fill_time.has_value(), first_fill_time ? *first_fill_time - order->submit_time_ns : 0);
    json.end_object();
    json.key("fillMatchIds").begin_array();
    for (const std::uint64_t match_id : fill_match_ids) json.unsigned_text(match_id);
    json.end_array();
    json.key("relatedEventPositions").begin_array();
    for (const std::uint64_t position : order->event_positions) json.unsigned_text(position);
    json.end_array();
    json.end_object();
    return json.take();
}

// ---- Fills ------------------------------------------------------------------------------------

std::string ResearchRun::render_fills_json(const PageQuery& page, std::optional<OrderId> order_id, const std::string& filter_fingerprint) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    const research::ExperimentConfig& config = parameters_.config->runner_config;
    const std::vector<CanonicalEventRecord>& events = artifacts_.events;
    const OwnOrderRecord* filter_order = nullptr;
    if (order_id) {
        filter_order = find_order_locked(*order_id);
        if (filter_order == nullptr) throw ApiFailure(404, "not_found", "no own order with orderId " + std::to_string(*order_id) + " in this run");
    }
    const std::size_t horizons = config.metrics.markout_horizon_count;

    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("items").begin_array();
    std::uint64_t index = page.start_index;
    std::uint64_t returned = 0;
    for (; index < artifacts_.fills.size() && returned < page.limit; ++index) {
        const research::FillRecord& fill = artifacts_.fills[index];
        const std::uint64_t position = artifacts_.fill_event_positions[index];
        if (position == kNoEventPosition || position >= events.size()) continue;
        const auto order_entry = artifacts_.order_by_client_order_id.find(fill.cl_ord_id);
        const OwnOrderRecord* order = order_entry == artifacts_.order_by_client_order_id.end() ? nullptr : &artifacts_.orders[order_entry->second];
        if (filter_order != nullptr && order != filter_order) continue;
        const CanonicalEventRecord& event = events[position];
        const std::int64_t signed_quantity = fill.side == Side::BUY ? static_cast<std::int64_t>(fill.qty) : -static_cast<std::int64_t>(fill.qty);
        const std::int64_t previous_realized = index == 0 ? 0 : artifacts_.fills[index - 1].realized_pnl_after;
        std::uint64_t report_position = kNoEventPosition;
        if (order != nullptr)
            for (const std::uint64_t candidate : order->event_positions)
                if (events[candidate].kind == CanonicalEventKind::ExecutionReportDelivered && events[candidate].source_event_position == position) {
                    report_position = candidate;
                    break;
                }
        std::optional<std::uint64_t> queue_ahead;
        if (fill.liquidity == research::Liquidity::Maker && order != nullptr && order->arrival_position != kNoEventPosition &&
            events[order->arrival_position].has(kHasQueuePosition))
            queue_ahead = events[order->arrival_position].queue_shares_ahead;

        json.begin_object();
        json.key("matchId").unsigned_text(fill.match_id);
        json.key("eventPosition").unsigned_text(position);
        json.key("simulationTimeNs").unsigned_text(fill.exchange_time);
        json.key("orderId").unsigned_text(fill.order_id);
        json.key("clientOrderId").unsigned_text(fill.cl_ord_id);
        json.key("side").string(to_api_side(fill.side));
        json.key("price").string(format_price_units(fill.price));
        json.key("quantity").unsigned_text(fill.qty);
        json.key("liquidityRole").string(fill_liquidity_role(fill.liquidity));
        json.key("counterpartyActor").string(to_api_string(event.counterparty_actor));
        json.key("feePerShare").string(format_price_units(fill.fee_per_share));
        json.key("midAtFill");
        if (fill.has_mid_before) json.string(doubled(fill.mid_before_times_two));
        else json.null();
        json.key("spreadCapturePerShare");
        if (fill.has_mid_before && fill.qty > 0) json.string(doubled(fill.spread_capture_times_two / static_cast<std::int64_t>(fill.qty)));
        else json.null();
        json.key("queueAheadAtRestShares");
        write_unsigned_or_null(json, queue_ahead.has_value(), queue_ahead.value_or(0));
        json.key("queueEvidenceSource").string("exact_simulated_truth");
        json.key("relatedEvents").begin_object();
        json.key("strategySubmitEventPosition").unsigned_text(order != nullptr ? order->submit_position : position);
        json.key("exchangeArrivalEventPosition")
            .unsigned_text(order != nullptr && order->arrival_position != kNoEventPosition ? order->arrival_position : event.command_arrival_position);
        json.key("acceptedEventPosition").unsigned_text(order != nullptr && order->accepted_position != kNoEventPosition ? order->accepted_position : position);
        json.key("acknowledgmentDeliveredEventPosition");
        write_position_or_null(json, order != nullptr ? order->acknowledgment_position : kNoEventPosition);
        json.key("fillEventPosition").unsigned_text(position);
        json.key("fillReportDeliveredEventPosition");
        write_position_or_null(json, report_position);
        json.key("aggressorArrivalEventPosition");
        write_position_or_null(json, fill.liquidity == research::Liquidity::Maker ? event.command_arrival_position : kNoEventPosition);
        json.end_object();
        json.key("ledgerEffect").begin_object();
        json.key("positionBeforeShares").signed_text(fill.position_after - signed_quantity);
        json.key("positionAfterShares").signed_text(fill.position_after);
        json.key("cashBefore").string(format_price_units(fill.cash_after + signed_quantity * fill.price + fill.fee));
        json.key("cashAfter").string(format_price_units(fill.cash_after));
        json.key("fee").string(format_price_units(fill.fee));
        json.key("realizedPnlChange").string(format_price_units(fill.realized_pnl_after - previous_realized));
        json.key("cumulativeRealizedPnlAfter").string(format_price_units(fill.realized_pnl_after));
        json.key("cumulativeFeesAfter").string(format_price_units(fill.fees_after));
        json.end_object();
        json.key("markouts").begin_array();
        for (std::size_t horizon = 0; horizon < horizons; ++horizon) {
            const std::size_t row = artifacts_.markout_row(fill.fill_index, horizon);
            const research::MarkoutRecord* markout = row == RunArtifacts::kNoMarkoutRow ? nullptr : &artifacts_.markouts[row];
            json.begin_object();
            json.key("horizonNs").unsigned_text(config.metrics.markout_horizons_ns[horizon]);
            if (markout != nullptr && markout->status == research::MarkoutStatus::Measured) {
                json.key("status").string("available");
                json.key("referenceMidPrice").string(doubled(markout->mid_times_two));
                json.key("markoutPerShare").string(doubled(markout->qty == 0 ? 0 : markout->markout_times_two / static_cast<std::int64_t>(markout->qty)));
                json.key("markoutTotal").string(doubled(markout->markout_times_two));
            } else {
                const bool unavailable = markout != nullptr && markout->status == research::MarkoutStatus::Unavailable;
                json.key("status").string(unavailable ? "unavailable_no_mark" : "censored_past_run_end");
                json.key("referenceMidPrice").null();
                json.key("markoutPerShare").null();
                json.key("markoutTotal").null();
            }
            json.end_object();
        }
        json.end_array();
        json.end_object();
        ++returned;
    }
    json.end_array();
    std::optional<std::string> next_cursor;
    if (index < artifacts_.fills.size()) next_cursor = encode_page_cursor(index, filter_fingerprint);
    write_page_info(json, next_cursor, page.limit);
    json.end_object();
    return json.take();
}

// ---- Book ---------------------------------------------------------------------------------------

std::string ResearchRun::render_book_json(std::optional<std::uint64_t> event_position, EventVisibility view, std::size_t depth) const {
    std::lock_guard lock(mutex_);
    require_artifacts_ready_locked();
    const std::vector<CanonicalEventRecord>& events = artifacts_.events;
    if (events.empty() || (event_position && *event_position >= events.size())) {
        ApiFailure failure(409, "artifact_not_ready",
                           "the run has processed " + std::to_string(events.size()) + " canonical events; the book is not available at that position yet");
        failure.current_run_state = to_api_string(state_);
        throw failure;
    }
    const std::uint64_t position = event_position.value_or(events.size() - 1);

    ApiJsonWriter json;
    json.begin_object();
    json.key("runId").string(parameters_.run_id);
    json.key("eventPosition").unsigned_text(position);
    json.key("simulationTimeNs").unsigned_text(events[position].simulation_time_ns);
    json.key("view").string(to_api_string(view));
    json.key("source").string("exact_simulated_truth");
    if (view == EventVisibility::ExchangeTruth) {
        ExchangeTruthBook book;
        for (std::uint64_t index = 0; index <= position; ++index) book.apply(events[index]);
        for (const auto& [side, key] : {std::pair{Side::BUY, "bids"}, std::pair{Side::SELL, "asks"}}) {
            json.key(key).begin_array();
            for (const ExchangeTruthBook::Level& level : book.levels(side, depth)) {
                json.begin_object();
                json.key("price").string(format_price_units(level.price));
                json.key("totalShares").unsigned_text(level.total_shares);
                json.key("orderCount").unsigned_text(level.order_count);
                json.key("ownShares").unsigned_text(level.own_shares);
                json.end_object();
            }
            json.end_array();
        }
        json.key("lastDeliveredSourceEventPosition").null();
    } else {
        // The strategy receives top of book only: one level per side, without
        // order counts or own shares.
        const CanonicalEventRecord* delivered = nullptr;
        for (std::uint64_t index = position + 1; index-- > 0;)
            if (events[index].kind == CanonicalEventKind::MarketDataDelivered) {
                delivered = &events[index];
                break;
            }
        for (const auto& [has_field, key] : {std::pair{kHasBestBid, "bids"}, std::pair{kHasBestAsk, "asks"}}) {
            json.key(key).begin_array();
            if (delivered != nullptr && delivered->has(has_field) && depth > 0) {
                const bool bid = has_field == kHasBestBid;
                json.begin_object();
                json.key("price").string(format_price_units(bid ? delivered->best_bid_price : delivered->best_ask_price));
                json.key("totalShares").unsigned_text(bid ? delivered->best_bid_quantity : delivered->best_ask_quantity);
                json.key("orderCount").null();
                json.key("ownShares").null();
                json.end_object();
            }
            json.end_array();
        }
        json.key("lastDeliveredSourceEventPosition");
        write_position_or_null(json, delivered != nullptr ? delivered->source_event_position : kNoEventPosition);
    }
    json.end_object();
    return json.take();
}

// ---- Comparison inputs ------------------------------------------------------------------------------

std::optional<RunComparisonInputs> ResearchRun::comparison_inputs(const PageQuery* trace_page) const {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Completed) return std::nullopt;
    // Re-execute the enclosing API run's config and seed. Only recorded build
    // labels come from the native manifest. This binds the evidence to the
    // experiment and produces the values used below without API cache reads.
    try {
        const auto recorded = parameters_.run_directory / "runner";
        const auto manifest = research::parse_json(research::read_text_file(recorded / "manifest.json"));
        const auto text = [](const research::JsonValue& object, std::string_view name) -> std::string {
            const auto* member = object.find(name);
            if (member == nullptr || member->kind != research::JsonValue::Kind::String)
                throw research::ArtifactError("native manifest field is missing or invalid: " + std::string(name));
            return member->text;
        };
        const auto& config = parameters_.config->runner_config;
        const auto expected_id = research::make_run_id(config.name, research::experiment_config_hash(config), parameters_.master_seed);
        if (text(manifest, "run_id") != expected_id || text(manifest, "master_seed") != std::to_string(parameters_.master_seed) ||
            research::read_text_file(recorded / "experiment.conf") != parameters_.config->runner_config_text)
            return std::nullopt;
        const auto* build = manifest.find("build");
        if (build == nullptr) return std::nullopt;
        const ComparisonDirectory scratch;
        research::RunArtifactOptions options;
        options.run_directory = scratch.path;
        options.run_id = expected_id;
        options.config_source_text = parameters_.config->runner_config_text;
        options.build.engine_build_id = text(*build, "engine_build_id");
        options.build.runner_build_id = text(*build, "runner_build_id");
        options.build.compiler = text(*build, "compiler");
        auto driver = make_simulation_driver(config, parameters_.master_seed, std::move(options));
        RunComparisonInputs inputs;
        std::vector<CanonicalEventRecord> request_evidence;
        inputs.run_id = parameters_.run_id;
        inputs.master_seed = parameters_.master_seed;
        for (;;) {
            const auto stop = driver->step();
            if (stop == research::RunStop::FlushRequired || stop == research::RunStop::Completed) {
                for (const auto& row : driver->core().recorder().lifecycle.records()) {
                    CanonicalEventRecord event;
                    event.actor = EventActor::ResearchStrategy;
                    event.present = kIsOwnOrder;
                    event.client_order_id = row.cl_ord_id;
                    event.simulation_time_ns = row.time;
                    event.rejected_command = row.request;
                    event.reject_reason = row.reject_reason;
                    if (row.stage == research::LifecycleStage::StrategySubmitted)
                        event.kind = row.request == CommandType::NewOrder ? CanonicalEventKind::StrategyOrderSubmitted :
                            row.request == CommandType::Cancel ? CanonicalEventKind::StrategyCancelSubmitted : CanonicalEventKind::StrategyReplaceSubmitted;
                    else if (row.stage == research::LifecycleStage::ExchangeArrival)
                        event.kind = row.request == CommandType::NewOrder ? CanonicalEventKind::OrderArrivedAtExchange :
                            row.request == CommandType::Cancel ? CanonicalEventKind::CancelArrivedAtExchange : CanonicalEventKind::ReplaceArrivedAtExchange;
                    else if (row.stage == research::LifecycleStage::ExchangeRejected) event.kind = CanonicalEventKind::OrderRejected;
                    else continue;
                    request_evidence.push_back(event);
                }
                for (const auto& decision : driver->core().recorder().decisions.records()) {
                    if (trace_page && inputs.decision_count >= trace_page->start_index &&
                        inputs.decision_count - trace_page->start_index < trace_page->limit) inputs.decisions.push_back(decision);
                    ++inputs.decision_count;
                }
                for (const auto& sample : driver->core().recorder().samples.records())
                    inputs.samples.push_back({sample.time, sample.has_mark, sample.equity_times_two, sample.position});
                if (stop == research::RunStop::Completed) break;
                driver->drain();
            } else if (stop == research::RunStop::Failed || stop == research::RunStop::Cancelled) {
                return std::nullopt;
            }
        }
        driver->finish();
        if (!driver->replay_verified()) return std::nullopt;
        for (const auto name : research::kRunArtifactFileNames) {
            if (name == "status.json") continue;  // wall clock and process identity
            std::error_code error;
            const bool recorded_exists = std::filesystem::exists(recorded / name, error);
            if (error) return std::nullopt;
            const bool reproduced_exists = std::filesystem::exists(scratch.path / name, error);
            if (error || recorded_exists != reproduced_exists) return std::nullopt;
            if (reproduced_exists &&
                research::read_text_file(recorded / name) != research::read_text_file(scratch.path / name))
                return std::nullopt;
        }
        if (std::filesystem::exists(recorded / research::kExecutionTraceFileName) &&
            research::read_text_file(recorded / research::kExecutionTraceFileName) != research::read_text_file(scratch.path / research::kExecutionTraceFileName)) return std::nullopt;
        const auto& core = driver->core();
        inputs.end_time_ns = core.now();
        if (is_execution_strategy(config)) {
            inputs.window_end_ns = config.strategy.parent.start_ns + config.strategy.parent.execution_duration_ns;
            inputs.execution_eligible = core.now() >= inputs.window_end_ns;
            inputs.observation_status = inputs.execution_eligible ? "window_observed" : "censored_run_end";
        }
        inputs.metrics = summary_metrics(config, {core.ledger().state(), core.metrics(), core.mark_times_two(), core.now()});
        ApiJsonWriter diagnostics; write_request_diagnostics(diagnostics, request_evidence);
        inputs.request_diagnostics_json = diagnostics.take();
        ApiJsonWriter coverage; coverage.begin_array();
        for (std::size_t index = 0; index < config.metrics.markout_horizon_count; ++index) {
            const auto& markout = core.metrics().markouts[index];
            coverage.begin_object().key("horizonNs").unsigned_text(config.metrics.markout_horizons_ns[index]);
            coverage.key("availableFillCount").unsigned_text(markout.measured).key("availableShares").unsigned_text(markout.measured_qty);
            coverage.key("censoredFillCount").unsigned_text(core.metrics().fill_count - markout.measured - markout.unavailable);
            coverage.key("unavailableFillCount").unsigned_text(markout.unavailable);
            coverage.key("totalMarkout").optional_string(markout_total_value(markout));
            coverage.key("meanMarkoutPerShare").optional_string(markout_per_share_value(markout)).end_object();
        }
        coverage.end_array(); inputs.markouts_json = coverage.take();
        return inputs;
    } catch (const research::ArtifactError&) {
        return std::nullopt;
    } catch (const research::JsonError&) {
        return std::nullopt;
    }
}


}  // namespace exsim::api
