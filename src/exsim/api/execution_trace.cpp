// Exact execution decision pagination and recorded partial terminal evidence.
#include "exsim/api/research_run.hpp"
#include "exsim/api/api_json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/json.hpp"
#include <fstream>
#include <limits>
namespace exsim::api {
std::string ResearchRun::render_execution_trace_json(const PageQuery& page) const {
    if (!(parameters_.config->runner_config.strategy.kind == research::StrategyKind::TwapExecutor || parameters_.config->runner_config.strategy.kind == research::StrategyKind::ImmediateSweepExecutor)) throw ApiFailure(409, "not_execution_strategy", "Execution trace is available for TWAP and sweep only");
    auto inputs = comparison_inputs(&page);
    std::string verification = "native_reproduction_and_replay_verified";
    if (!inputs) {
        std::lock_guard lock(mutex_);
        if (state_ == ApiRunState::Completed || !is_terminal(state_))
            throw ApiFailure(409, "evidence_unavailable", "Completed trace requires matching native reproduction and replay; active traces are not snapshots");
        // Cancellation timing is not replayable. Serve the actual terminal trace,
        // explicitly distinguished from completed reproduced evidence.
        const auto path = parameters_.run_directory / "runner" / research::kExecutionTraceFileName;
        if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
            throw ApiFailure(409, "evidence_unavailable", "This terminal run has no persisted execution trace");
        std::ifstream stream(path);
        if (!stream) throw ApiFailure(409, "evidence_unavailable", "Cannot read persisted execution trace");
        inputs.emplace();
        inputs->window_end_ns = parameters_.config->runner_config.strategy.parent.start_ns + parameters_.config->runner_config.strategy.parent.execution_duration_ns;
        inputs->observation_status = final_simulation_time_ns_ >= inputs->window_end_ns ? "window_observed" : "censored_run_end";
        char line[8192];
        while (stream.getline(line, sizeof line)) {
            const auto index = inputs->decision_count++;
            try {
                const auto native = research::parse_json(line);
                const auto number = [&](std::string_view key) {
                    const auto* value = native.find(key); std::uint64_t parsed = 0;
                    if (!value || value->kind != research::JsonValue::Kind::String || !parse_unsigned_integer_text(value->text, parsed)) throw std::runtime_error("invalid trace integer");
                    return parsed;
                };
                const auto signed_number = [&](std::string_view key) {
                    const auto* value = native.find(key); std::int64_t parsed = 0;
                    if (!value || value->kind != research::JsonValue::Kind::String || !parse_signed_integer_text(value->text, parsed)) throw std::runtime_error("invalid trace integer");
                    return parsed;
                };
                const auto quantity = [&](std::string_view key) {
                    const auto value = number(key);
                    if (value > std::numeric_limits<Quantity>::max()) throw std::runtime_error("trace quantity out of range");
                    return static_cast<Quantity>(value);
                };
                const auto* trace_kind = native.find("kind");
                const auto* has_touch = native.find("has_touch");
                if (!trace_kind || trace_kind->kind != research::JsonValue::Kind::String || (trace_kind->text != "decision" && trace_kind->text != "child_report") ||
                    !has_touch || has_touch->kind != research::JsonValue::Kind::Boolean) throw std::runtime_error("invalid trace kind or touch flag");
                research::ExecutionDecision row;
                row.time=number("time_ns"); row.source_time=number("source_time_ns"); row.source_sequence=number("source_sequence");
                row.target=number("target_qty"); row.filled=number("known_filled_qty"); row.requested=number("requested_qty");
                row.headroom=signed_number("headroom"); row.child_cap=quantity("child_cap");
                row.submitted=quantity("submitted_qty"); row.outstanding=quantity("outstanding_qty");
                row.child=number("child_client_order_id"); row.touch=signed_number("touch");row.limit=signed_number("limit");
                row.has_touch=has_touch->boolean;
                row.report=trace_kind->text=="child_report";
                const auto* reason=native.find("reason"); if (!reason || reason->kind != research::JsonValue::Kind::String) throw std::runtime_error("missing trace reason");
                bool recognized=false;
                for (const auto value : {"submitted", "child_in_flight", "target_satisfied", "no_delivered_touch", "no_headroom_or_lot"})
                    if (reason->text==value) {row.reason=value; recognized=true;}
                for (std::size_t value=0;value<research::kRiskRejectReasonCount;++value) {
                    const auto label=research::to_string(static_cast<research::RiskRejectReason>(value));
                    if (reason->text==label) {row.reason=label;recognized=true;}
                }
                for (unsigned value=0;value<=static_cast<unsigned>(research::ReportKind::Replaced);++value) {
                    const auto kind=static_cast<research::ReportKind>(value);
                    if(row.report && reason->text==research::to_string(kind)) {row.result.kind=kind;row.reason=research::to_string(kind);recognized=true;}
                }
                if(!recognized) throw std::runtime_error("unknown trace reason");
                row.result.exchange_time=number("exchange_time_ns");row.result.order_id=number("order_id");row.result.match_id=number("match_id");
                row.result.qty=quantity("report_qty");row.result.leaves_qty=quantity("report_leaves_qty");row.result.cum_qty=quantity("report_cum_qty");
                // Enum identity is persisted by name, not raw object bytes.
                bool valid_reject = false, valid_cancel = false;
                const auto* reject_reason = native.find("reject_reason");
                const auto* cancel_reason = native.find("cancel_reason");
                if (!reject_reason || reject_reason->kind != research::JsonValue::Kind::String || !cancel_reason || cancel_reason->kind != research::JsonValue::Kind::String)
                    throw std::runtime_error("missing trace report reasons");
                for(unsigned value=0;value<=static_cast<unsigned>(RejectReason::UnknownOrder);++value) {
                    const auto kind=static_cast<RejectReason>(value);if(reject_reason->text==to_string(kind)) { row.result.reject_reason=kind; valid_reject=true; }
                }
                for(unsigned value=0;value<=static_cast<unsigned>(CancelReason::EndOfDay);++value) {
                    const auto kind=static_cast<CancelReason>(value);if(cancel_reason->text==to_string(kind)) { row.result.cancel_reason=kind; valid_cancel=true; }
                }
                if (!valid_reject || !valid_cancel) throw std::runtime_error("unknown trace report reason");
                if (index >= page.start_index && index - page.start_index < page.limit) inputs->decisions.push_back(row);
            } catch (const std::exception&) { throw ApiFailure(409,"evidence_unavailable","Persisted execution trace is invalid"); }
        }
        if (!stream.eof()) throw ApiFailure(409,"evidence_unavailable","Persisted execution trace exceeds its line bound or cannot be read");
        verification = "recorded_terminal_partial";
    }
    ApiJsonWriter json;
    json.begin_object().key("runId").string(parameters_.run_id);
    json.key("verification").string(verification);
    json.key("windowEndTimeNs").unsigned_text(inputs->window_end_ns);
    const auto& parameters = parameters_.config->runner_config.strategy.parent;
    json.key("observationStatus").string(inputs->observation_status);
    json.key("startTimeNs").unsigned_text(parameters.start_ns);
    json.key("parentQuantity").unsigned_text(parameters.parent_qty);
    const bool twap = parameters_.config->runner_config.strategy.kind == research::StrategyKind::TwapExecutor;
    const auto slices = twap ? parameters.execution_duration_ns / parameters.slice_interval_ns : 1;
    json.key("finalTargetTimeNs").unsigned_text(twap ? parameters.start_ns + (slices - 1) * parameters.slice_interval_ns : parameters.start_ns);
    json.key("finalTargetQuantity").unsigned_text(parameters.parent_qty);
    json.key("totalCount").unsigned_text(inputs->decision_count);
    json.key("items").begin_array();
    std::uint64_t index = page.start_index;
    for (const auto& row : inputs->decisions) {
        json.begin_object().key("index").unsigned_text(index++);
        json.key("kind").string(row.report ? "child_report" : "decision");
        json.key("simulationTimeNs").unsigned_text(row.time);
        json.key("cumulativeTargetQuantity").unsigned_text(row.target);
        json.key("knownFilledQuantity").unsigned_text(row.filled);
        const auto parent = parameters_.config->runner_config.strategy.parent.parent_qty;
        json.key("parentResidualQuantity").unsigned_text(row.filled < parent ? parent - row.filled : 0);
        json.key("requestedQuantity").unsigned_text(row.requested);
        json.key("outstandingQuantity").unsigned_text(row.outstanding);
        json.key("positionHeadroom").signed_text(row.headroom);
        json.key("childCapQuantity").unsigned_text(row.child_cap);
        json.key("submittedQuantity").unsigned_text(row.submitted);
        json.key("childClientOrderId"); if (row.child) json.unsigned_text(row.child); else json.null();
        json.key("deliveredSourceSequence").unsigned_text(row.source_sequence);
        json.key("deliveredSourceTimeNs").unsigned_text(row.source_time);
        json.key("deliveredTouch"); if (row.has_touch) json.string(format_price_units(row.touch)); else json.null();
        json.key("limitPrice"); if (row.has_touch) json.string(format_price_units(row.limit)); else json.null();
        json.key("reason").string(row.reason);
        json.key("report");
        if (!row.report) json.null();
        else {
            json.begin_object().key("kind").string(research::to_string(row.result.kind));
            json.key("exchangeTimeNs").unsigned_text(row.result.exchange_time);
            json.key("orderId").unsigned_text(row.result.order_id);
            json.key("matchId").unsigned_text(row.result.match_id);
            json.key("quantity").unsigned_text(row.result.qty);
            json.key("leavesQuantity").unsigned_text(row.result.leaves_qty);
            json.key("cumulativeQuantity").unsigned_text(row.result.cum_qty);
            json.key("rejectReason").string(to_string(row.result.reject_reason));
            json.key("cancelReason").string(to_string(row.result.cancel_reason));
            json.end_object();
        }
        json.end_object();
    }
    json.end_array();
    const auto next = page.start_index + inputs->decisions.size();
    json.key("page").begin_object().key("nextCursor");
    if (next < inputs->decision_count) json.string(encode_page_cursor(next, "execution-trace|" + parameters_.run_id)); else json.null();
    json.key("limit").integer_number(static_cast<std::int64_t>(page.limit)).end_object();
    json.end_object();
    return json.take();
}

} // namespace exsim::api
