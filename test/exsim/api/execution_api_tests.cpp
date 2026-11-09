// Parent execution through the real HTTP router, scheduler, artifact writer and verifier.
// Executor threads are disabled; each test owns an isolated temporary data directory.
#include "exsim/api/api_json.hpp"
#include "exsim/api/sha256.hpp"
#include "exsim/api/comparison_selection.hpp"
#include "exsim/api/markout_values.hpp"
#include "exsim/api/request_diagnostics.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/api/http_message.hpp"
#include "exsim/api/research_service.hpp"
#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/run_verification.hpp"
#include "test/framework/test.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace exsim::api;
using exsim::research::JsonValue;
using exsim::research::parse_json;

namespace {
struct Reply {
    int status;
    JsonValue body;
    std::string text;
};

class ExecutionService {
public:
    explicit ExecutionService(const std::filesystem::path& directory, const std::string& executable = {}) : service(options(directory, executable)) {}
    Reply send(std::string_view method, const std::string& path, const std::string& body = {}) {
        const std::string raw = std::string(method) + " " + path + " HTTP/1.1\r\nHost: test\r\nIdempotency-Key: execution-key-" +
                                std::to_string(++key) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        HttpRequest request;
        if (parse_http_request(raw, HttpLimits{}, request).status != HttpParseStatus::Complete) throw std::runtime_error("bad test request");
        const auto response = service.handle(request);
        if (const char* capture = std::getenv("EXSIM_EXECUTION_API_RESPONSES")) {
            ApiJsonWriter row;
            row.begin_object().key("method").string(method).key("path").string(path).key("status").integer_number(response.status)
               .key("body").raw(response.body).end_object();
            std::ofstream output(capture, std::ios::app);
            output << row.take() << '\n';
        }
        return {response.status, parse_json(response.body), response.body};
    }
    void drain() {
        for (int i = 0; i < 10000; ++i) if (!service.run_executor_pass()) return;
        throw std::runtime_error("executor did not settle");
    }
    ResearchService service;
private:
    static ResearchServiceOptions options(const std::filesystem::path& directory, const std::string& executable) {
        ResearchServiceOptions result;
        result.data_directory = directory;
        result.start_executor = false;
        result.executable_path = executable;
        result.source_revision = "fixture-source";
        result.executor_slice_steps = 128;
        return result;
    }
    inline static unsigned key = 0;
};

std::filesystem::path fresh_directory(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("exsim_execution_api_" + std::to_string(getpid()) + "_" + name);
    std::filesystem::remove_all(path);
    return path;
}

std::string text(const JsonValue& value, std::string_view key) {
    const auto* field = value.find(key);
    if (!field) throw std::runtime_error("missing field: " + std::string(key));
    return field->text;
}

JsonValue& field(JsonValue& value, std::string_view key) {
    for (auto& member : value.members) if (member.name == key) return member.value;
    throw std::runtime_error("missing fixture field: " + std::string(key));
}

void set(JsonValue& value, std::string_view key, const std::string& json) { field(value, key) = parse_json(json); }

std::string request_body(bool twap, std::string_view side = "buy", std::string_view quantity = "500") {
    auto config = parse_json(baseline_configurations().at(0).config_json);
    set(config, "strategy", twap
        ? R"({"strategyKind":"twap_execution","parentSide":"buy","parentQuantity":"500","sliceIntervalNs":"1000000000","sliceOrderType":"limit","startTimeNs":"1000000000","executionDurationNs":"30000000000","limitOffsetTicks":"5"})"
        : R"({"strategyKind":"immediate_sweep_execution","parentSide":"buy","parentQuantity":"500","startTimeNs":"1000000000","executionDurationNs":"30000000000","limitOffsetTicks":"5"})");
    set(field(config, "bounds"), "endSimulationTimeNs", R"("5000000000")");
    set(field(config, "seedSet"), "masterSeeds", R"(["1","2"])");
    auto& strategy = field(config, "strategy");
    set(strategy, "executionDurationNs", R"("3000000000")");
    set(strategy, "parentSide", "\"" + std::string(side) + "\"");
    set(strategy, "parentQuantity", "\"" + std::string(quantity) + "\"");
    return std::string(R"({"name":"execution test","derivedFromBaselineId":")") + (twap ? "twap-execution" : "immediate-sweep-execution") +
           R"(","config":)" + serialize_json_value(config) + "}";
}

std::string create(ExecutionService& service, const std::string& request) {
    auto reply = service.send("POST", "/v1/experiment-previews", request);
    if (reply.status != 200 || !reply.body.find("valid")->boolean) throw std::runtime_error("preview failed: " + reply.text);
    reply = service.send("POST", "/v1/experiments", request);
    if (reply.status != 201) throw std::runtime_error("create failed: " + reply.text);
    return text(reply.body, "experimentId");
}

std::string run(ExecutionService& service, const std::string& experiment, std::string_view seed, bool paused = false) {
    const auto reply = service.send("POST", "/v1/runs", R"({"experimentId":")" + experiment + R"(","masterSeed":")" + std::string(seed) +
                                    R"(","startPaused":)" + (paused ? "true}" : "false}"));
    if (reply.status != 201) throw std::runtime_error("run failed: " + reply.text);
    return text(reply.body, "runId");
}

const JsonValue& metric(const JsonValue& summary, std::string_view key, std::string_view array = "metrics") {
    for (const auto& row : summary.find(array)->items) if (text(row, "metricKey") == key) return row;
    throw std::runtime_error("missing metric: " + std::string(key));
}

void check_decimal_equal(std::string_view left, std::string_view right) {
    const auto a = parse_scaled_decimal(left), b = parse_scaled_decimal(right);
    REQUIRE(a); REQUIRE(b);
    CHECK_EQ(compare_scaled_decimals(*a, *b), 0);
}

void check_native_summary(const JsonValue& summary, const std::filesystem::path& native) {
    const auto saved = parse_json(exsim::research::read_text_file(native / "summary.json"));
    const auto manifest = parse_json(exsim::research::read_text_file(native / "manifest.json"));
    CHECK_NE(text(manifest, "run_id"), text(summary, "runId"));
    const auto& execution = *saved.find("execution");
    const auto& http = *summary.find("execution");
    CHECK_EQ(text(http, "filledQuantity"), text(execution, "filled_qty"));
    CHECK_EQ(text(metric(summary, "residual_qty"), "value"), text(execution, "residual_qty"));
    check_decimal_equal(text(http, "benchmarkMid"), text(execution, "arrival_mid"));
    check_decimal_equal(text(http, "fillNotional"), text(execution, "fill_notional"));
    check_decimal_equal(text(http, "netFees"), text(execution, "fees"));
    const auto& cost = *execution.find("implementation_shortfall");
    REQUIRE(cost.kind == JsonValue::Kind::Object);
    check_decimal_equal(text(metric(summary, "decision_slippage_total"), "value"), text(cost, "price_cost"));
    check_decimal_equal(text(metric(summary, "implementation_shortfall_total"), "value"), text(cost, "total"));
    CHECK_EQ(text(metric(summary, "completion_ratio"), "value"), format_derived_decimal(execution.find("completion_ratio")->number));
    CHECK_EQ(text(metric(summary, "implementation_shortfall_per_share"), "value"), format_derived_decimal(cost.find("per_share")->number));
    CHECK_EQ(text(metric(summary, "participation_rate"), "value"), format_derived_decimal(execution.find("participation_rate")->number));
    CHECK_EQ(text(metric(summary, "decision_slippage_total"), "exactness"), std::string("exact_fixed_point"));
    CHECK_EQ(text(metric(summary, "implementation_shortfall_per_share"), "exactness"), std::string("derived_float"));
    CHECK_EQ(text(metric(summary, "implementation_shortfall_per_share"), "unit"), std::string("currency_per_share"));
    REQUIRE(exsim::research::reproduce_run(native).reproduced);
}
}

TEST_CASE(execution_api, capabilities_and_baselines_are_executable) {
    ExecutionService service(fresh_directory("catalogue"));
    const auto capabilities = service.send("GET", "/v1/service");
    REQUIRE_EQ(capabilities.status, 200);
    const auto& kinds = capabilities.body.find("executableStrategyKinds")->items;
    REQUIRE_EQ(kinds.size(), 4u);
    const auto baselines = service.send("GET", "/v1/baselines");
    REQUIRE_EQ(baselines.status, 200);
    REQUIRE_EQ(baseline_configurations().size(), 4u);
    for (const auto& baseline : baseline_configurations()) {
        const auto result = translate_experiment_config(parse_json(baseline.config_json), "/config");
        REQUIRE(result.translated);
        bool advertised = false;
        for (const auto& kind : kinds) if (kind.text == result.translated->strategy_kind) advertised = true;
        CHECK(advertised);
    }
}

TEST_CASE(execution_api, preview_and_create_reject_market_children_without_substitution) {
    ExecutionService service(fresh_directory("market_rejected"));
    auto request = parse_json(request_body(true));
    set(field(field(request, "config"), "strategy"), "sliceOrderType", R"("market")");
    for (const auto* path : {"/v1/experiment-previews", "/v1/experiments"}) {
        const auto reply = service.send("POST", path, serialize_json_value(request));
        const bool preview = std::string_view(path) == "/v1/experiment-previews";
        REQUIRE_EQ(reply.status, preview ? 200 : 422);
        if (preview) CHECK(!reply.body.find("valid")->boolean);
        else CHECK_EQ(text(*reply.body.find("error"), "code"), std::string("validation_failed"));
        bool field_found = false;
        const auto* issues = preview ? reply.body.find("fieldIssues") : reply.body.find("error")->find("fieldIssues");
        for (const auto& issue : issues->items)
            if (text(issue, "fieldPath") == "/config/strategy/sliceOrderType") field_found = true;
        CHECK(field_found);
    }
}

TEST_CASE(execution_api, translation_preserves_schedule_risk_and_runner_defaults) {
    for (const bool twap : {true, false}) {
        auto request = parse_json(request_body(twap, "sell", "900"));
        auto& config = field(request, "config");
        auto result = translate_experiment_config(config, "/config");
        REQUIRE(result.translated);
        const auto& native = result.translated->runner_config;
        CHECK(native.strategy.parent.side == exsim::Side::SELL);
        CHECK_EQ(native.strategy.parent.parent_qty, 900u);
        CHECK_EQ(native.strategy.parent.start_ns, 1000000000u);
        CHECK_EQ(native.strategy.parent.execution_duration_ns, 3000000000u);
        CHECK_EQ(native.strategy.parent.limit_offset_ticks, 5u);
        CHECK_EQ(native.risk.max_order_qty, 200u);
        CHECK_EQ(native.risk.max_short_position, 500);
        auto& fields = field(config, "strategy").members;
        std::erase_if(fields, [](const auto& field) {
            return field.name == "startTimeNs" || field.name == "executionDurationNs" || field.name == "limitOffsetTicks";
        });
        result = translate_experiment_config(config, "/config");
        REQUIRE(result.translated);
        CHECK_EQ(result.translated->runner_config.strategy.parent.execution_duration_ns, 30000000000u);
        CHECK_EQ(result.translated->runner_config.strategy.parent.start_ns, 1000000000u);
    }
}

TEST_CASE(execution_api, invalid_parent_and_schedule_are_rejected) {
    for (const bool twap : {true, false}) {
        for (const auto& [key, value] : {std::pair{"parentQuantity", "0"}, {"parentQuantity", "4294967296"},
                                       {"startTimeNs", "5000000000"}, {"executionDurationNs", "604800000000001"},
                                       {"limitOffsetTicks", "100001"}}) {
            auto request = parse_json(request_body(twap));
            auto& config = field(request, "config");
            set(field(config, "strategy"), key, "\"" + std::string(value) + "\"");
            const auto result = translate_experiment_config(config, "/config");
            CHECK(!result.translated);
        }
    }
    for (const auto* interval : {"0", "4000000000", "1"}) {
        auto request = parse_json(request_body(true));
        auto& config = field(request, "config");
        set(field(config, "strategy"), "sliceIntervalNs", "\"" + std::string(interval) + "\"");
        CHECK(!translate_experiment_config(config, "/config").translated);
    }
}

TEST_CASE(execution_api, both_sides_pause_step_resume_complete_and_reproduce) {
    for (const bool twap : {true, false}) for (const auto* side : {"buy", "sell"}) {
        const auto directory = fresh_directory(std::string(twap ? "twap_" : "sweep_") + side);
        ExecutionService service(directory);
        const auto experiment = create(service, request_body(twap, side));
        const auto id = run(service, experiment, "1", true);
        service.drain();
        CHECK_EQ(text(service.send("GET", "/v1/runs/" + id).body, "state"), std::string("paused"));
        const auto manifest = service.send("GET", "/v1/runs/" + id + "/manifest");
        REQUIRE_EQ(manifest.status, 200);
        CHECK_EQ(text(manifest.body, "strategyBuildId"), std::string(twap ? "twap_executor/1" : "immediate_sweep_executor/1"));
        const auto initial = service.send("GET", "/v1/runs/" + id + "/summary");
        REQUIRE_EQ(initial.status, 200);
        CHECK_EQ(text(metric(initial.body, "completion_ratio"), "unavailableReason"), std::string("parent_order_not_declared"));
        CHECK(metric(initial.body, "completion_ratio").find("value")->kind == JsonValue::Kind::Null);
        const auto stepped = service.send("POST", "/v1/runs/" + id + "/step", R"({"canonicalEventCount":1})");
        REQUIRE_EQ(stepped.status, 200);
        CHECK_EQ(text(stepped.body, "state"), std::string("paused"));
        REQUIRE_EQ(service.send("POST", "/v1/runs/" + id + "/resume", "{}").status, 200);
        service.drain();
        CHECK_EQ(text(service.send("GET", "/v1/runs/" + id).body, "state"), std::string("completed"));
        const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
        REQUIRE_EQ(summary.status, 200);
        const auto& execution = *summary.body.find("execution");
        const auto filled = std::stoull(text(execution, "filledQuantity"));
        const auto residual = std::stoull(text(metric(summary.body, "residual_qty"), "value"));
        CHECK_GT(filled, 0u);
        CHECK_EQ(filled + residual, 500u);
        CHECK_EQ(text(metric(summary.body, "completion_ratio"), "value"), format_derived_decimal(static_cast<double>(filled) / 500.0));
        CHECK_EQ(text(*summary.body.find("execution"), "observationStatus"), std::string("window_observed"));
        check_native_summary(summary.body, directory / "runs" / id / "runner");
        const auto uninterrupted = run(service, experiment, "1");
        service.drain();
        const auto other = service.send("GET", "/v1/runs/" + uninterrupted + "/summary");
        CHECK_EQ(serialize_json_value(*summary.body.find("execution")), serialize_json_value(*other.body.find("execution")));
        CHECK_EQ(exsim::research::read_text_file(directory / "runs" / id / "runner" / "events.jsonl"),
                 exsim::research::read_text_file(directory / "runs" / uninterrupted / "runner" / "events.jsonl"));
    }
}

TEST_CASE(execution_api, residuals_and_censored_windows_are_explicit) {
    for (const bool twap : {true, false}) {
        ExecutionService service(fresh_directory(twap ? "twap_residual" : "sweep_residual"));
        auto request = parse_json(request_body(twap, "buy", "900"));
        set(field(field(request, "config"), "strategy"), "executionDurationNs", R"("10000000000")");
        const auto experiment = create(service, serialize_json_value(request));
        const auto id = run(service, experiment, "1"); service.drain();
        const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
        REQUIRE_EQ(summary.status, 200);
        CHECK_EQ(text(*summary.body.find("execution"), "observationStatus"), std::string("censored_run_end"));
        const auto residual = parse_scaled_decimal(text(metric(summary.body, "residual_qty"), "value"));
        REQUIRE(residual); CHECK_GT(scaled_decimal_to_double(*residual), 0);
        CHECK(summary.body.find("execution")->find("unexecutedOpportunityCost")->kind == JsonValue::Kind::Null);
    }
}

TEST_CASE(execution_api, no_liquidity_keeps_completion_zero_and_costs_missing) {
    for (const bool twap : {true, false}) {
        ExecutionService service(fresh_directory(twap ? "twap_no_liquidity" : "sweep_no_liquidity"));
        auto request = parse_json(request_body(twap));
        set(field(field(request, "config"), "scenario"), "backgroundAgents", "[]");
        const auto experiment = create(service, serialize_json_value(request));
        const auto id = run(service, experiment, "1"); service.drain();
        const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
        REQUIRE_EQ(summary.status, 200);
        CHECK_EQ(text(metric(summary.body, "completion_ratio"), "value"), std::string("0.000000"));
        CHECK_EQ(text(metric(summary.body, "residual_qty"), "value"), std::string("500"));
        CHECK(summary.body.find("execution")->find("benchmarkMid")->kind == JsonValue::Kind::Null);
        for (const auto* key : {"implementation_shortfall_total", "decision_slippage_total", "participation_rate"}) {
            CHECK(metric(summary.body, key).find("value")->kind == JsonValue::Kind::Null);
            CHECK_EQ(text(metric(summary.body, key), "unavailableReason"), std::string("no_parent_fills"));
        }
    }
}

TEST_CASE(execution_api, paired_comparison_survives_reload_and_requires_native_evidence) {
    const auto directory = fresh_directory("reload_compare");
    std::string a, b, a_run, b_run, query;
    std::string comparison;
    {
        ExecutionService service(directory);
        a = create(service, request_body(true)); b = create(service, request_body(false));
        for (const auto* seed : {"1", "2"}) { a_run = run(service, a, seed); b_run = run(service, b, seed); }
        service.drain();
        query = "/v1/comparisons?baselineExperimentId=" + a + "&candidateExperimentId=" + b;
        const auto reply = service.send("GET", query); REQUIRE_EQ(reply.status, 200);
        CHECK(reply.body.find("comparable")->boolean);
        REQUIRE_EQ(reply.body.find("seedPairing")->find("pairedSeeds")->items.size(), 2u);
        for (const auto* key : {"completion_ratio", "residual_qty", "decision_slippage_total", "implementation_shortfall_total", "participation_rate"}) {
            const auto& row = metric(reply.body, key, "metricComparisons");
            CHECK_EQ(text(row, "betterDirection"), std::string(std::string_view(key) == "completion_ratio" ? "higher" :
                                                               (std::string_view(key) == "participation_rate" ? "neither" : "lower")));
            CHECK_EQ(row.find("perSeed")->items.size(), 2u);
            CHECK_EQ(row.find("dispersion")->find("sampleCount")->text, std::string("2"));
        }
        comparison = serialize_json_value(*reply.body.find("metricComparisons"));
    }
    {
        ExecutionService restarted(directory);
        const auto reply = restarted.send("GET", query); REQUIRE_EQ(reply.status, 200);
        CHECK_EQ(comparison, serialize_json_value(*reply.body.find("metricComparisons")));
        for (const auto& id : {a_run, b_run}) {
            const auto summary = restarted.send("GET", "/v1/runs/" + id + "/summary");
            check_native_summary(summary.body, directory / "runs" / id / "runner");
        }
        std::ofstream corrupt(directory / "runs" / b_run / "runner" / "ledger.jsonl", std::ios::app); corrupt << "{}\n"; corrupt.close();
        const auto excluded = restarted.send("GET", query); REQUIRE_EQ(excluded.status, 200);
        CHECK_EQ(excluded.body.find("seedPairing")->find("pairedSeeds")->items.size(), 1u);
    }
}

TEST_CASE(execution_api, native_seed_config_and_strategy_substitution_are_excluded) {
    for (const bool twap : {true, false}) for (const int substitution : {0, 1, 2}) {
        const auto directory = fresh_directory(std::string(twap ? "twap_identity_" : "sweep_identity_") + std::to_string(substitution));
        ExecutionService service(directory);
        const auto a = create(service, request_body(twap));
        const auto b = create(service, request_body(twap));
        const auto donor = create(service, request_body(substitution == 2 ? !twap : twap, substitution == 1 ? "sell" : "buy"));
        (void)run(service, a, "1");
        const auto target_run = run(service, b, "1");
        const auto donor_run = run(service, donor, substitution == 0 ? "2" : "1");
        service.drain();
        const auto query = "/v1/comparisons?baselineExperimentId=" + a + "&candidateExperimentId=" + b;
        auto reply = service.send("GET", query);
        REQUIRE(reply.body.find("comparable")->boolean);
        const auto target_path = directory / "runs" / target_run / "runner";
        std::filesystem::remove_all(target_path);
        std::filesystem::copy(directory / "runs" / donor_run / "runner", target_path, std::filesystem::copy_options::recursive);
        REQUIRE(exsim::research::reproduce_run(target_path).reproduced);
        reply = service.send("GET", query);
        REQUIRE_EQ(reply.status, 200);
        CHECK(!reply.body.find("comparable")->boolean);
        CHECK(reply.body.find("seedPairing")->find("pairedSeeds")->items.empty());
    }
}

TEST_CASE(execution_api, cancellation_preserves_observed_execution_and_censors_the_window) {
    for (const bool twap : {true, false}) {
        ExecutionService service(fresh_directory(twap ? "twap_cancel" : "sweep_cancel"));
        const auto experiment = create(service, request_body(twap));
        const auto id = run(service, experiment, "1", true);
        service.drain();
        bool has_fills = false;
        for (int step = 0; step < 100 && !has_fills; ++step) {
            REQUIRE_EQ(service.send("POST", "/v1/runs/" + id + "/step", R"({"canonicalEventCount":100})").status, 200);
            const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
            const auto& filled = *summary.body.find("execution")->find("filledQuantity");
            has_fills = filled.kind == JsonValue::Kind::String && filled.text != "0";
        }
        REQUIRE(has_fills);
        REQUIRE_EQ(service.send("POST", "/v1/runs/" + id + "/cancel", "{}").status, 200);
        const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
        REQUIRE_EQ(summary.status, 200);
        CHECK_EQ(text(summary.body, "availability"), std::string("final"));
        CHECK_EQ(text(*summary.body.find("execution"), "observationStatus"), std::string("censored_run_end"));
        CHECK(metric(summary.body, "completion_ratio").find("value")->kind == JsonValue::Kind::String);
        CHECK(metric(summary.body, "implementation_shortfall_total").find("value")->kind == JsonValue::Kind::String);
    }
}

TEST_CASE(execution_api, half_price_unit_benchmarks_and_costs_remain_exact) {
    const auto directory = fresh_directory("half_unit");
    ExecutionService service(directory);
    auto request = parse_json(request_body(false));
    auto& config = field(request, "config");
    set(field(config, "instrument"), "tickSize", R"("0.0001")");
    set(field(config, "instrument"), "minPrice", R"("99.9900")");
    set(field(config, "instrument"), "maxPrice", R"("100.0100")");
    set(field(config, "strategy"), "startTimeNs", R"("60000000")");
    const auto experiment = create(service, serialize_json_value(request));
    const auto id = run(service, experiment, "1"); service.drain();
    const auto summary = service.send("GET", "/v1/runs/" + id + "/summary");
    REQUIRE_EQ(summary.status, 200);
    const auto& execution = *summary.body.find("execution");
    CHECK_EQ(text(execution, "benchmarkMid"), std::string("100.00005"));
    check_native_summary(summary.body, directory / "runs" / id / "runner");
}

TEST_CASE(execution_api, zero_interval_reports_exact_editable_path_and_units) {
    auto request = parse_json(request_body(true));
    set(field(field(request, "config"), "strategy"), "sliceIntervalNs", R"("0")");
    const auto result = translate_experiment_config(field(request, "config"), "/config");
    REQUIRE(!result.translated); REQUIRE_EQ(result.semantic_issues.size(), 1u);
    CHECK_EQ(result.semantic_issues[0].field_path, std::string("/config/strategy/sliceIntervalNs"));
    CHECK(result.semantic_issues[0].issue.find("greater than 0 s") != std::string::npos);
}

TEST_CASE(execution_api, comparison_excludes_censor_on_both_sides_and_pins_attempts) {
    for (const bool censor_baseline : {true, false}) {
        const auto directory = fresh_directory(censor_baseline ? "censor_a" : "censor_b");
        ExecutionService service(directory);
        const auto full = request_body(true, "buy", "900"); // ordinary full-window partial completion
        auto censored = parse_json(full);
        set(field(field(censored, "config"), "strategy"), "executionDurationNs", R"("10000000000")");
        const auto a = create(service, censor_baseline ? serialize_json_value(censored) : full);
        const auto b = create(service, censor_baseline ? full : serialize_json_value(censored));
        const auto run_a = run(service, a, "1"), run_b = run(service, b, "1"); service.drain();
        auto native_a = exsim::research::load_run_set(directory / "runs" / run_a / "runner");
        auto native_b = exsim::research::load_run_set(directory / "runs" / run_b / "runner");
        exsim::research::check_reproduction(native_a); exsim::research::check_reproduction(native_b);
        const auto cli_comparison = exsim::research::compare_run_sets(native_a, native_b);
        CHECK(cli_comparison.paired_seeds.empty());
        REQUIRE_EQ(cli_comparison.excluded_runs.size(), 1u);
        CHECK_EQ(cli_comparison.excluded_runs[0].reason, std::string("execution_window_censored"));
        const auto& native_full = censor_baseline ? native_b : native_a;
        CHECK_EQ(exsim::research::compare_run_sets(native_full, native_full).paired_seeds.size(), 1u);
        const auto query = "/v1/comparisons?baselineExperimentId=" + a + "&candidateExperimentId=" + b;
        const auto reply = service.send("GET", query); REQUIRE_EQ(reply.status, 200);
        CHECK(!reply.body.find("comparable")->boolean);
        REQUIRE_EQ(reply.body.find("attemptLedger")->items.size(), 4u);
        unsigned excluded = 0, eligible = 0, unlaunched = 0;
        for (const auto& seed : reply.body.find("attemptLedger")->items) {
            if (seed.find("attempts")->items.empty()) ++unlaunched;
            for (const auto& attempt : seed.find("attempts")->items) {
                if (attempt.find("eligible")->boolean) ++eligible;
                if (text(attempt,"reason") == "execution_window_censored") ++excluded;
            }
        }
        CHECK_EQ(excluded, 1u); CHECK_EQ(eligible, 1u); CHECK_EQ(unlaunched, 2u);
    }
    const auto pinned_directory=fresh_directory("pinned");
    ExecutionService service(pinned_directory);
    const auto a = create(service, request_body(true)), b = create(service, request_body(false));
    const auto original = run(service, a, "1"); (void)run(service, b, "1"); service.drain();
    const auto query = "/v1/comparisons?baselineExperimentId=" + a + "&candidateExperimentId=" + b;
    const auto first = service.send("GET", query);
    const auto pin = text(first.body, "runSelection");
    const auto retry = run(service, a, "1"); service.drain();
    const auto latest = service.send("GET", query);
    CHECK(text(latest.body, "runSelection") != pin);
    CHECK_EQ(*decode_run_selection(text(latest.body, "runSelection"))->begin(), retry);
    const auto pinned = service.send("GET", query + "&runSelection=" + pin);
    REQUIRE_EQ(pinned.status, 200); CHECK_EQ(text(pinned.body,"runSelection"), pin);
    CHECK_EQ(serialize_json_value(*first.body.find("metricComparisons")), serialize_json_value(*pinned.body.find("metricComparisons")));
    for (const auto& row : first.body.find("metricComparisons")->items) {
        if (text(row, "betterDirection") != "neither") continue;
        std::size_t zeros = 0;
        for (const auto& seed : row.find("perSeed")->items) {
            const auto* value = seed.find("difference");
            if (value->kind == JsonValue::Kind::String) {
                const auto decimal = parse_scaled_decimal(value->text); REQUIRE(decimal);
                if (scaled_decimal_to_double(*decimal) == 0) ++zeros;
            }
        }
        CHECK_EQ(std::stoull(text(*row.find("dispersion"), "tiedCount")), zeros);
    }
    CHECK_EQ(service.send("GET", query + "&runSelection=run_unknown").status, 400);
    std::ofstream changed(pinned_directory/"runs"/original/"runner"/"ledger.jsonl", std::ios::app);changed<<"{}\n";changed.close();
    const auto rejected_pin=service.send("GET",query+"&runSelection="+pin);
    CHECK_EQ(rejected_pin.status,409);
    CHECK_EQ(text(*rejected_pin.body.find("error"),"code"),std::string("selected_run_ineligible"));
}

TEST_CASE(execution_api, decision_trace_explains_full_window_residuals_and_pages_exactly) {
    for (const bool capped : {false, true}) {
        const auto directory = fresh_directory(capped ? "trace_cap" : "trace_102");
        std::string id, trace_text;
        {
            ExecutionService service(directory);
            auto request = parse_json(request_body(true));
            auto& config = field(request,"config");
            set(field(config,"bounds"),"endSimulationTimeNs",R"("60000000000")");
            set(field(config,"strategy"),"executionDurationNs",R"("30000000000")");
            set(field(config,"seedSet"),"masterSeeds", capped ? R"(["101"])" : R"(["102"])");
            if (capped) set(field(config,"risk"),"maxOrderQuantity", R"("10")");
            const auto experiment = create(service,serialize_json_value(request));
            id=run(service,experiment,capped ? "101" : "102"); service.drain();
            const auto trace = service.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000");
            REQUIRE_EQ(trace.status,200); trace_text=serialize_json_value(trace.body);
            CHECK_EQ(text(trace.body,"finalTargetTimeNs"),std::string("30000000000"));
            CHECK_EQ(text(trace.body,"finalTargetQuantity"),std::string("500"));
            unsigned decisions=0, fills=0, capped_children=0, missing_touch=0;
            for (const auto& row: trace.body.find("items")->items) {
                if (text(row,"kind")=="decision") {
                    ++decisions;
                    if (text(row,"reason")=="no_delivered_touch") ++missing_touch;
                    if (text(row,"submittedQuantity")=="10") ++capped_children;
                } else if (text(*row.find("report"),"kind")=="fill") fills+=std::stoul(text(*row.find("report"),"quantity"));
            }
            CHECK_EQ(decisions,30u); CHECK_GT(missing_touch,0u);
            CHECK_EQ(fills,capped ? 240u:483u);
            if(capped) CHECK_EQ(capped_children,24u);
            std::vector<std::string> pages;
            std::string cursor;
            do {
                const auto page=service.send("GET","/v1/runs/"+id+"/execution-trace?limit=7"+(cursor.empty()?"":"&cursor="+cursor));
                REQUIRE_EQ(page.status,200);
                for(const auto& row:page.body.find("items")->items) pages.push_back(serialize_json_value(row));
                cursor=text(*page.body.find("page"),"nextCursor");
            }while(!cursor.empty());
            REQUIRE_EQ(pages.size(),trace.body.find("items")->items.size());
            for(std::size_t i=0;i<pages.size();++i) CHECK_EQ(pages[i],serialize_json_value(trace.body.find("items")->items[i]));
            CHECK_EQ(service.send("GET","/v1/runs/"+id+"/execution-trace?limit=1001").status,422);
        }
        ExecutionService restart(directory);
        const auto after=restart.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000");
        REQUIRE_EQ(after.status,200); CHECK_EQ(serialize_json_value(after.body),trace_text);
        const auto native_trace=directory/"runs"/id/"runner"/exsim::research::kExecutionTraceFileName;
        const auto recorded=exsim::research::read_text_file(native_trace);
        CHECK_EQ(static_cast<std::size_t>(std::count(recorded.begin(),recorded.end(),'\n')), std::stoull(text(after.body,"totalCount")));
        std::filesystem::remove(native_trace); // older canonical artifacts have no trace sidecar
        CHECK_EQ(restart.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000").status,200);
        REQUIRE(exsim::research::reproduce_run(directory/"runs"/id/"runner").reproduced);
        std::ofstream corrupt(native_trace);corrupt<<"{}\n";corrupt.close();
        CHECK_EQ(restart.send("GET","/v1/runs/"+id+"/execution-trace").status,409);
        CHECK(!exsim::research::reproduce_run(directory/"runs"/id/"runner").reproduced);
    }
}

TEST_CASE(execution_api, entirely_unobserved_markouts_are_not_paired_ties) {
    ExecutionService service(fresh_directory("unobserved_markouts"));
    auto request = parse_json(request_body(true));
    auto& config=field(request,"config");
    set(field(config,"strategy"),"executionDurationNs",R"("1000000000")");
    set(field(config,"strategy"),"sliceIntervalNs",R"("100000000")");
    set(field(config,"bounds"),"endSimulationTimeNs",R"("3000000000")");
    const auto a=create(service,serialize_json_value(request)), b=create(service,serialize_json_value(request));
    const auto id=run(service,a,"1"); (void)run(service,b,"1"); service.drain();
    const auto summary=service.send("GET","/v1/runs/"+id+"/summary");
    CHECK_EQ(text(*summary.body.find("execution"),"observationStatus"),std::string("window_observed"));
    bool found=false;
    for(const auto& row: summary.body.find("metrics")->items) if(text(row,"metricKey")=="markout_total" && text(row,"horizonNs")=="5000000000") {
        found=true; CHECK(row.find("value")->kind==JsonValue::Kind::Null);
        CHECK_EQ(text(row,"unavailableReason"),std::string("no_measured_markout_shares"));
    }
    CHECK(found);
    const auto reply=service.send("GET","/v1/comparisons?baselineExperimentId="+a+"&candidateExperimentId="+b);
    REQUIRE(reply.body.find("comparable")->boolean);
    for(const auto& row:reply.body.find("metricComparisons")->items) if(text(row,"metricKey")=="markout_total") {
        const auto& dispersion=*row.find("dispersion");
        if(text(row,"horizonNs")=="5000000000") {
            CHECK_EQ(text(dispersion,"sampleCount"),std::string("0"));
            CHECK(dispersion.find("meanDifference")->kind==JsonValue::Kind::Null);
            CHECK_EQ(text(dispersion,"tiedCount"),std::string("0"));
        } else if(text(row,"horizonNs")=="100000000") {
            CHECK_EQ(text(dispersion,"sampleCount"),std::string("1"));
            CHECK_EQ(text(dispersion,"tiedCount"),std::string("1"));
        }
    }
    exsim::research::MarkoutAccumulator measured_zero;
    measured_zero.censored=8;
    CHECK(!markout_total_value(measured_zero)); CHECK(!markout_per_share_value(measured_zero));
    measured_zero.measured=1; measured_zero.measured_qty=17;
    REQUIRE(markout_total_value(measured_zero)); REQUIRE(markout_per_share_value(measured_zero));
    CHECK_EQ(*markout_per_share_value(measured_zero),std::string("0.000000"));
}

TEST_CASE(execution_api, complete_audit_transfer_and_streaming_sha256_preserve_exact_bytes) {
    const auto directory=fresh_directory("audit_export"); ExecutionService service(directory);
    const auto experiment=create(service,request_body(true)); const auto id=run(service,experiment,"1");service.drain();
    const auto inventory=service.send("GET","/v1/runs/"+id+"/audit-export"); REQUIRE_EQ(inventory.status,200);
    REQUIRE(inventory.body.find("reproductionSupported")->boolean);
    bool events=false, journal=false;
    for(const auto& item:inventory.body.find("items")->items) {
        if(!item.find("present")->boolean) continue;
        const auto name=text(item,"name"), bytes=exsim::research::read_text_file(directory/"runs"/id/name);
        CHECK_EQ(text(item,"sha256"),sha256_hex(bytes));
        CHECK_EQ(text(item,"sha256"),sha256_file_hex(directory/"runs"/id/name));
        if(name=="runner/events.jsonl") { events=true; CHECK_GT(std::count(bytes.begin(),bytes.end(),'\n'),1000); }
        if(name=="runner/commands.journal") journal=true;
        std::string rebuilt,offset="0";
        do {
            const auto chunk=service.send("GET","/v1/runs/"+id+"/audit-export?artifactIndex="+text(item,"artifactIndex")+"&offset="+offset);
            REQUIRE_EQ(chunk.status,200); const auto hex=text(chunk.body,"contentHex");CHECK_LE(hex.size(),131072u);
            for(std::size_t i=0;i<hex.size();i+=2) rebuilt+=static_cast<char>(std::stoul(hex.substr(i,2),nullptr,16));
            offset=text(chunk.body,"nextOffset");
        }while(!offset.empty());
        CHECK_EQ(rebuilt,bytes);
    }
    CHECK(events);CHECK(journal);
    const auto cache=directory/"runs"/id/"events.bin";
    const auto saved=cache.string()+".saved";std::filesystem::rename(cache,saved);
    {std::ofstream sparse(cache,std::ios::binary);sparse.seekp(std::streamoff{2}<<30);sparse.put('x');}
    CHECK_EQ(service.send("GET","/v1/runs/"+id+"/audit-export").status,413);
    CHECK_EQ(service.send("GET","/v1/runs/"+id+"/audit-export?artifactIndex=3").status,413);
    std::filesystem::remove(cache);std::filesystem::rename(saved,cache);
    CHECK_EQ(service.send("GET","/v1/runs/"+id+"/audit-export?artifactIndex=999").status,404);
    CHECK_EQ(service.send("GET","/v1/runs/"+id+"/audit-export?artifactIndex=0&offset=18446744073709551615").status,400);
    const auto paused=run(service,experiment,"2",true);service.drain();
    CHECK_EQ(service.send("GET","/v1/runs/"+paused+"/audit-export").status,409);
    REQUIRE_EQ(service.send("POST","/v1/runs/"+paused+"/cancel","{}").status,200);
    const auto cancelled=service.send("GET","/v1/runs/"+paused+"/audit-export"); REQUIRE_EQ(cancelled.status,200);
    CHECK(!cancelled.body.find("reproductionSupported")->boolean); CHECK(cancelled.body.find("reproductionCommand")->kind==JsonValue::Kind::Null);
    const auto path=directory/"runs"/id/"runner"/"events.jsonl";
    std::filesystem::rename(path,path.string()+".saved");std::filesystem::create_symlink(path.string()+".saved",path);
    CHECK_EQ(service.send("GET","/v1/runs/"+id+"/audit-export").status,409);
}

TEST_CASE(execution_api, persisted_provenance_identifies_different_artifacts_without_relabeling) {
    const auto directory=fresh_directory("provenance");
    std::string id,manifest;
    {
        ExecutionService service(directory,"/bin/ls");
        const auto exp=create(service,request_body(false));id=run(service,exp,"1"); const auto second=run(service,exp,"1");service.drain();
        const auto first=service.send("GET","/v1/runs/"+id+"/manifest");
        manifest=first.text;
        CHECK_EQ(text(*first.body.find("provenance"),"executableSha256"),sha256_file_hex("/bin/ls"));
        const auto native=exsim::research::read_text_file(directory/"runs"/id/"runner"/"provenance.json");
        CHECK_EQ(text(parse_json(native),"executableSha256"),sha256_file_hex("/bin/ls"));
        const auto run1=service.send("GET","/v1/runs/"+id),run2=service.send("GET","/v1/runs/"+second);
        CHECK_EQ(serialize_json_value(*run1.body.find("digests")->find("canonicalEventDigest")),serialize_json_value(*run2.body.find("digests")->find("canonicalEventDigest")));
    }
    ExecutionService restart(directory,"/bin/cat");
    CHECK_EQ(restart.send("GET","/v1/runs/"+id+"/manifest").text,manifest);
    const auto exp=create(restart,request_body(false)); const auto other=run(restart,exp,"1"); restart.drain();
    const auto newer=restart.send("GET","/v1/runs/"+other+"/manifest");
    CHECK_EQ(text(*newer.body.find("provenance"),"executableSha256"),sha256_file_hex("/bin/cat"));
    CHECK(text(*newer.body.find("provenance"),"executableSha256")!=text(*parse_json(manifest).find("provenance"),"executableSha256"));
}

TEST_CASE(execution_api, replace_diagnostics_preserve_request_denominators_and_actual_fill_causality) {
    using namespace exsim;
    std::vector<CanonicalEventRecord> events(7);
    const CanonicalEventKind kinds[]={CanonicalEventKind::StrategyReplaceSubmitted,CanonicalEventKind::OrderFilled,CanonicalEventKind::ReplaceArrivedAtExchange,
        CanonicalEventKind::OrderRejected,CanonicalEventKind::StrategyReplaceSubmitted,CanonicalEventKind::ReplaceArrivedAtExchange,CanonicalEventKind::OrderRejected};
    for(std::size_t i=0;i<events.size();++i) { auto& e=events[i];e.kind=kinds[i];e.simulation_time_ns=i*100;e.order_id=7;e.actor=EventActor::ResearchStrategy;e.present=kIsOwnOrder; }
    // Native unknown-order arrivals omit the exchange ID. Fill events can
    // identify the resting order through their trade legs instead.
    events[2].order_id = 0;
    events[5].order_id = 0;
    events[1].order_id = 0;
    events[1].buy_leg_order_id = 7;
    events[1].sell_leg_order_id = 9;
    events[3].rejected_command=CommandType::Replace;events[3].reject_reason=RejectReason::UnknownOrder;
    events[6].rejected_command=CommandType::Replace;events[6].reject_reason=RejectReason::UnknownOrder;
    std::vector<std::uint64_t> positions={0,1,2,3,4,5,6};
    ApiJsonWriter transit;write_replace_transits(transit,events,positions);const auto rows=parse_json(transit.take());
    REQUIRE_EQ(rows.items.size(),2u);CHECK_EQ(text(rows.items[0],"transitNs"),std::string("200"));
    CHECK_EQ(text(rows.items[0],"fillBeforeArrivalEventPosition"),std::string("1"));
    CHECK(rows.items[1].find("fillBeforeArrivalEventPosition")->kind==JsonValue::Kind::Null);
    ApiJsonWriter counts;write_request_diagnostics(counts,events);const auto diagnostics=parse_json(counts.take());
    REQUIRE_EQ(diagnostics.items.size(),3u);
    CHECK_EQ(text(diagnostics.items[2],"submittedCount"),std::string("2"));
    CHECK_EQ(text(diagnostics.items[2],"exchangeRejectedCount"),std::string("2"));
    CHECK_EQ(text(diagnostics.items[0],"exchangeRejectedCount"),std::string("0"));
}

TEST_CASE(execution_api, fifo_replace_reset_nonzero_ahead_kept_priority_and_truncation_are_exact) {
    using namespace exsim;
    std::vector<CanonicalEventRecord> events;
    const auto rest=[&](OrderId id,Price price,Quantity qty) {CanonicalEventRecord e;e.kind=CanonicalEventKind::OrderRested;e.order_id=id;e.price=price;e.leaves_quantity=qty;e.side=Side::BUY;events.push_back(e);};
    rest(1,100,50);rest(2,101,30);rest(7,100,10);
    CanonicalEventRecord replace; replace.kind=CanonicalEventKind::OrderReplaced;replace.order_id=12;replace.replaced_old_order_id=7;replace.price=101;replace.leaves_quantity=10;events.push_back(replace);
    rest(12,101,10);
    replace.present=kPriorityKept;replace.replaced_old_order_id=12;replace.leaves_quantity=5;events.push_back(replace);
    CanonicalEventRecord cancel;cancel.kind=CanonicalEventKind::OrderCanceled;cancel.order_id=2;cancel.present=kWasResting;events.push_back(cancel);
    const std::vector<OrderId> ids={7,12};
    ApiJsonWriter writer;CHECK(!write_fifo_evidence(writer,events,ids,events.size()-1));const auto rows=parse_json(writer.take());
    REQUIRE_EQ(rows.items.size(),5u);
    CHECK_EQ(text(rows.items[0],"sharesAhead"),std::string("50"));
    CHECK_EQ(text(rows.items[2],"orderId"),std::string("12"));CHECK_EQ(text(rows.items[2],"replacedOldOrderId"),std::string("7"));
    CHECK_EQ(text(rows.items[2],"sharesAhead"),std::string("30"));CHECK(!rows.items[2].find("priorityKept")->boolean);
    CHECK_EQ(text(rows.items[3],"reason"),std::string("replace_priority_kept"));CHECK(rows.items[3].find("priorityKept")->boolean);
    CHECK_EQ(text(rows.items[4],"sharesAhead"),std::string("0"));CHECK_EQ(text(rows.items[4],"orderId"),std::string("12"));
    CHECK_EQ(text(rows.items[1],"reason"),std::string("replace_priority_reset"));
    CHECK(!rows.items[1].find("queuePositionAvailable")->boolean);
    CHECK(rows.items[1].find("sharesAhead")->kind==JsonValue::Kind::Null);
    for(int i=0;i<600;++i) events.push_back(replace);
    ApiJsonWriter bounded;CHECK(write_fifo_evidence(bounded,events,ids,events.size()-1));CHECK_EQ(parse_json(bounded.take()).items.size(),500u);
}

TEST_CASE(execution_api, slow_replace_native_study_exposes_own_rejections_and_markout_share_denominators) {
    ExecutionService service(fresh_directory("mm_slow_replace"));
    auto request=parse_json(request_body(true));auto& config=field(request,"config");
    const auto baseline=parse_json(baseline_configurations().at(0).config_json);
    set(config,"strategy",serialize_json_value(*baseline.find("strategy")));
    set(field(config,"bounds"),"endSimulationTimeNs",R"("60000000000")");
    set(field(config,"seedSet"),"masterSeeds",R"(["103"])");
    set(field(config,"latency"),"cancelLatencyNs",R"("100000000")");
    const auto experiment=create(service,serialize_json_value(request));const auto id=run(service,experiment,"103");service.drain();
    const auto summary=service.send("GET","/v1/runs/"+id+"/summary");REQUIRE_EQ(summary.status,200);
    // This fixture uses the documented default OUCH/full-ITCH model. Exact
    // counts and share denominators were independently audited from canonical
    // events/markouts and reproduced through real process sessions. Receipt:
    // docs/quality/LEGACY_LOB_FULL_SESSIONS_2026-11-09.md.
    const auto* counts=summary.body.find("orders")->find("requestDiagnostics");REQUIRE(counts);REQUIRE_EQ(counts->items.size(),3u);
    CHECK_EQ(text(counts->items[0],"exchangeRejectedCount"),std::string("4"));
    CHECK_EQ(text(counts->items[1],"exchangeRejectedCount"),std::string("0"));
    CHECK_EQ(text(counts->items[2],"exchangeRejectedCount"),std::string("503"));
    bool unknown=false,post_only=false;
    for(const auto& reason:counts->items[2].find("rejectReasons")->items) {
        if(text(reason,"reason")=="unknown_order") {unknown=true;CHECK_EQ(text(reason,"count"),std::string("502"));}
        if(text(reason,"reason")=="post_only_would_cross") {post_only=true;CHECK_EQ(text(reason,"count"),std::string("1"));}
    }
    CHECK(unknown);CHECK(post_only);
    for(const auto& row:summary.body.find("markouts")->items) {CHECK_GT(std::stoull(text(row,"availableShares")),0u);CHECK_GT(std::stoull(text(row,"availableFillCount")),0u);}
    set(field(config,"latency"),"cancelLatencyNs",R"("80000")");
    const auto baseline_experiment=create(service,serialize_json_value(request));const auto baseline_run=run(service,baseline_experiment,"103");service.drain();
    const auto baseline_summary=service.send("GET","/v1/runs/"+baseline_run+"/summary");
    const std::vector<std::string> shares={"136137","133639","125136"};
    for(std::size_t i=0;i<shares.size();++i)CHECK_EQ(text(baseline_summary.body.find("markouts")->items[i],"availableShares"),shares[i]);
    const auto comparison=service.send("GET","/v1/comparisons?baselineExperimentId="+baseline_experiment+"&candidateExperimentId="+experiment);
    bool conditional=false;
    for(const auto& row:comparison.body.find("metricComparisons")->items)if(text(row,"metricKey")=="markout_per_share" && text(row,"horizonNs")=="100000000") {
        conditional=true;REQUIRE_EQ(row.find("perSeed")->items.size(),1u);
        CHECK_EQ(text(row.find("perSeed")->items[0],"baselineValue"),text(baseline_summary.body.find("markouts")->items[0],"meanMarkoutPerShare"));
    }
    CHECK(conditional);
}

TEST_CASE(execution_api, execution_trace_overflow_fails_instead_of_dropping_evidence) {
    auto config=exsim::research::default_experiment_config();
    exsim::research::SimulationCore core(config,1);
    const auto capacity=core.recorder().decisions.capacity();
    exsim::research::ExecutionDecision row;
    for(std::size_t i=0;i<capacity;++i) core.record_execution_decision(row);
    CHECK(core.recorder().needs_flush());
    core.record_execution_decision(row);
    CHECK(core.termination()==exsim::research::TerminationReason::RecorderOverflow);
    CHECK_EQ(core.recorder().decisions.size(),capacity);
}

TEST_CASE(execution_api, cancelled_decision_trace_is_recorded_and_durable_without_reproduction_claim) {
    const auto directory=fresh_directory("cancelled_trace");std::string id, recorded;
    {
        ExecutionService service(directory);const auto experiment=create(service,request_body(true));id=run(service,experiment,"1",true);service.drain();
        REQUIRE_EQ(service.send("POST","/v1/runs/"+id+"/step",R"({"canonicalEventCount":600})").status,200);
        REQUIRE_EQ(service.send("POST","/v1/runs/"+id+"/cancel","{}").status,200);
        const auto trace=service.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000");REQUIRE_EQ(trace.status,200);
        CHECK_EQ(text(trace.body,"verification"),std::string("recorded_terminal_partial"));
        CHECK_GT(trace.body.find("items")->items.size(),0u);
        recorded=serialize_json_value(trace.body);
    }
    ExecutionService restarted(directory);const auto trace=restarted.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000");
    REQUIRE_EQ(trace.status,200);CHECK_EQ(serialize_json_value(trace.body),recorded);
    const auto path = directory / "runs" / id / "runner" / exsim::research::kExecutionTraceFileName;
    const auto original = exsim::research::read_text_file(path);
    { std::ofstream corrupt(path, std::ios::app); corrupt << "{invalid-json}\n"; }
    for (const auto limit : {1, 1000}) {
        const auto rejected = restarted.send("GET", "/v1/runs/" + id + "/execution-trace?limit=" + std::to_string(limit));
        CHECK_EQ(rejected.status, 409);
        CHECK_EQ(text(*rejected.body.find("error"), "code"), std::string("evidence_unavailable"));
    }
    { std::ofstream restored(path); restored << original; }
    CHECK_EQ(restarted.send("GET", "/v1/runs/" + id + "/execution-trace?limit=1").status, 200);
}

TEST_CASE(execution_api, largest_selection_token_fits_native_request_bound_and_roundtrips) {
    std::vector<std::string> ids;
    for(unsigned i=0;i<512;++i) {auto hex=exsim::research::format_hex64(i);ids.push_back("run_"+hex);}
    const auto token=encode_run_selection(ids);CHECK_LE(token.size(),5500u);
    const auto decoded=decode_run_selection(token);REQUIRE(decoded);CHECK(*decoded==ids);
    HttpRequest request;const auto raw="GET /v1/comparisons?baselineExperimentId=exp_1111111111111111&candidateExperimentId=exp_2222222222222222&runSelection="+token+" HTTP/1.1\r\nHost: test\r\n\r\n";
    CHECK(parse_http_request(raw,HttpLimits{},request).status==HttpParseStatus::Complete);
    CHECK(!decode_run_selection("r1_!bad"));CHECK(!decode_run_selection(token+"A"));
}

TEST_CASE(execution_api, execution_semantic_checks_identify_editable_fields) {
    for(const auto& [key,value]:{std::pair{"parentQuantity","0"},{"startTimeNs","5000000000"},{"executionDurationNs","604800000000001"},{"sliceIntervalNs","1"},{"limitOffsetTicks","100001"}}) {
        auto request=parse_json(request_body(true));set(field(field(request,"config"),"strategy"),key,"\""+std::string(value)+"\"");
        const auto result=translate_experiment_config(field(request,"config"),"/config");REQUIRE(!result.translated);
        bool found=false;for(const auto& issue:result.semantic_issues)if(issue.field_path=="/config/strategy/"+std::string(key))found=true;
        CHECK(found);
    }
}

TEST_CASE(execution_api, sweep_trace_over_thousand_rows_preserves_causality_and_page_identity) {
    ExecutionService service(fresh_directory("sweep_trace_bounds"));auto request=parse_json(request_body(false,"buy","100000"));auto& config=field(request,"config");
    set(field(config,"bounds"),"endSimulationTimeNs",R"("40000000000")");
    set(field(config,"strategy"),"executionDurationNs",R"("30000000000")");
    set(field(config,"latency"),"acknowledgmentLatencyNs",R"("100000000")");
    const auto experiment=create(service,serialize_json_value(request));const auto id=run(service,experiment,"1");service.drain();
    std::string cursor;std::uint64_t seen=0,total=0;bool in_flight=false,headroom=false;
    do {
        const auto page=service.send("GET","/v1/runs/"+id+"/execution-trace?limit=1000"+(cursor.empty()?"":"&cursor="+cursor));REQUIRE_EQ(page.status,200);
        total=std::stoull(text(page.body,"totalCount"));
        for(const auto& row:page.body.find("items")->items) {
            CHECK_EQ(std::stoull(text(row,"index")),seen++);
            if(text(row,"kind")!="decision")continue;
            CHECK_LE(std::stoull(text(row,"deliveredSourceTimeNs")),std::stoull(text(row,"simulationTimeNs")));
            CHECK_EQ(text(row,"cumulativeTargetQuantity"),std::string("100000"));
            if(text(row,"reason")=="child_in_flight") {in_flight=true;CHECK_GT(std::stoull(text(row,"outstandingQuantity")),0u);}
            if(text(row,"reason")=="no_headroom_or_lot") headroom=true;
        }
        cursor=text(*page.body.find("page"),"nextCursor");
    }while(!cursor.empty());
    CHECK_EQ(seen,total);CHECK_GT(total,1000u);CHECK(in_flight);CHECK(headroom);
}

TEST_CASE(execution_api, latest_eligible_attempt_selection_and_ordinal_survive_restart) {
    const auto directory=fresh_directory("selection_restart");std::string a,b,pin,latest;
    {
        ExecutionService service(directory);a=create(service,request_body(true));b=create(service,request_body(false));
        (void)run(service,a,"1");latest=run(service,a,"1");(void)run(service,b,"1");service.drain();
        const auto comparison=service.send("GET","/v1/comparisons?baselineExperimentId="+a+"&candidateExperimentId="+b);
        REQUIRE_EQ(comparison.status,200);pin=text(comparison.body,"runSelection");
        CHECK_EQ(decode_run_selection(pin)->at(0),latest);
    }
    ExecutionService restart(directory);
    const auto comparison=restart.send("GET","/v1/comparisons?baselineExperimentId="+a+"&candidateExperimentId="+b);
    REQUIRE_EQ(comparison.status,200);CHECK_EQ(text(comparison.body,"runSelection"),pin);
    const auto pinned=restart.send("GET","/v1/comparisons?baselineExperimentId="+a+"&candidateExperimentId="+b+"&runSelection="+pin);
    REQUIRE_EQ(pinned.status,200);CHECK_EQ(text(pinned.body,"runSelection"),pin);
}
