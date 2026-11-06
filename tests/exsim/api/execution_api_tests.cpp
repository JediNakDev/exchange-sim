// Parent execution through the real HTTP router, scheduler, artifact writer and verifier.
// Executor threads are disabled; each test owns an isolated temporary data directory.
#include "exsim/api/api_json.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/api/http_message.hpp"
#include "exsim/api/research_service.hpp"
#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/run_verification.hpp"
#include "tests/framework/test.hpp"

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
    explicit ExecutionService(const std::filesystem::path& directory) : service(options(directory)) {}
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
    static ResearchServiceOptions options(const std::filesystem::path& directory) {
        ResearchServiceOptions result;
        result.data_directory = directory;
        result.start_executor = false;
        result.executor_slice_steps = 128;
        return result;
    }
    unsigned key = 0;
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
