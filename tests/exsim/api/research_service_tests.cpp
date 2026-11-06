// The research service driven in process through its router: errors,
// idempotency, the run state machine, read views that reconcile with each
// other, determinism, persistence across a restart, and comparisons.
//
// The executor thread is off; tests advance runs with run_executor_pass().

#include "exsim/api/api_json.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/api/http_message.hpp"
#include "exsim/api/research_service.hpp"
#include "exsim/research/json.hpp"
#include "tests/framework/test.hpp"

#include <unistd.h>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

using namespace exsim::api;
using exsim::research::JsonValue;
using exsim::research::parse_json;

namespace {

std::filesystem::path fresh_service_directory(std::string_view name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("exsim_api_tests_" + std::to_string(::getpid()) + "_" + std::string(name));
    std::filesystem::remove_all(directory);
    return directory;
}

struct ServiceResponse {
    int status = 0;
    JsonValue body;
    std::string body_text;
    std::map<std::string, std::string> headers;
    [[nodiscard]] std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string("<missing>") : found->second;
    }

    [[nodiscard]] std::string text_at(std::initializer_list<std::string_view> path) const {
        const JsonValue* value = &body;
        for (const std::string_view name : path) {
            value = value->find(name);
            if (value == nullptr) return "<missing>";
        }
        return value->text;
    }
    [[nodiscard]] std::string error_code() const { return text_at({"error", "code"}); }
};

class ServiceUnderTest {
public:
    explicit ServiceUnderTest(const std::filesystem::path& data_directory, std::size_t max_keys = 100'000)
        : service_(options_for(data_directory, max_keys)) {}

    ServiceResponse send(std::string_view method, std::string_view target, std::string_view body = {}, std::string_view idempotency_key = {},
                         std::string_view extra_headers = {}) {
        std::string raw = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: test\r\n";
        if (!idempotency_key.empty()) raw += "Idempotency-Key: " + std::string(idempotency_key) + "\r\n";
        raw += std::string(extra_headers);
        raw += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
        HttpRequest request;
        const HttpParseResult parsed = parse_http_request(raw, HttpLimits{}, request);
        if (parsed.status != HttpParseStatus::Complete) throw std::runtime_error("test built an unparsable request: " + parsed.error_message);
        const HttpResponse response = service_.handle(request);
        ServiceResponse result;
        result.status = response.status;
        result.body_text = response.body;
        result.body = parse_json(response.body);
        for (const auto& [name, value] : response.headers) result.headers[name] = value;
        return result;
    }
    ServiceResponse get(std::string_view target) { return send("GET", target); }
    ServiceResponse post(std::string_view target, std::string_view body) { return send("POST", target, body, next_key()); }

    // Runs the executor until no run is running.
    void drain_executor() {
        for (int pass = 0; pass < 100'000; ++pass)
            if (!service_.run_executor_pass()) return;
        throw std::runtime_error("executor did not settle");
    }
    ResearchService& service() { return service_; }

    std::string next_key() { return "test-key-" + std::to_string(++key_counter_); }

private:
    static ResearchServiceOptions options_for(const std::filesystem::path& data_directory, std::size_t max_keys) {
        ResearchServiceOptions options;
        options.data_directory = data_directory;
        options.max_idempotency_keys = max_keys;
        options.start_executor = false;
        options.executor_slice_steps = 512;
        return options;
    }
    ResearchService service_;
    int key_counter_ = 0;
};

// A baseline's configuration with a shorter run and the given seeds.
std::string experiment_request_body(std::size_t baseline_index, std::string_view name, std::string_view seeds_json = R"(["1","2","3"])",
                                    std::string_view end_time_ns = "5000000000") {
    std::string config = baseline_configurations().at(baseline_index).config_json;
    const auto replace = [&config](std::string_view from, std::string_view to) {
        const std::size_t at = config.find(from);
        if (at == std::string::npos) throw std::runtime_error("baseline text changed: " + std::string(from));
        config.replace(at, from.size(), to);
    };
    replace(R"("endSimulationTimeNs":"60000000000")", std::string(R"("endSimulationTimeNs":")") + std::string(end_time_ns) + "\"");
    const std::size_t seeds_at = config.find(R"("masterSeeds":[)");
    const std::size_t seeds_end = config.find(']', seeds_at);
    config.replace(seeds_at, seeds_end - seeds_at + 1, std::string(R"("masterSeeds":)") + std::string(seeds_json));
    return std::string(R"({"name":")") + std::string(name) + R"(","description":"","derivedFromBaselineId":")" +
           baseline_configurations().at(baseline_index).baseline_id + R"(","config":)" + config + "}";
}

std::string create_experiment(ServiceUnderTest& service, std::size_t baseline_index, std::string_view name,
                              std::string_view seeds_json = R"(["1","2","3"])") {
    const ServiceResponse response = service.post("/v1/experiments", experiment_request_body(baseline_index, name, seeds_json));
    if (response.status != 201) throw std::runtime_error("experiment creation failed: " + response.body_text);
    return response.text_at({"experimentId"});
}

std::string create_run(ServiceUnderTest& service, const std::string& experiment_id, std::string_view seed, bool start_paused = false) {
    const ServiceResponse response = service.post(
        "/v1/runs", R"({"experimentId":")" + experiment_id + R"(","masterSeed":")" + std::string(seed) + R"(","startPaused":)" +
                        (start_paused ? "true" : "false") + "}");
    if (response.status != 201) throw std::runtime_error("run creation failed: " + response.body_text);
    return response.text_at({"runId"});
}

std::uint64_t unsigned_text(const std::string& text) {
    std::uint64_t value = 0;
    if (!parse_unsigned_integer_text(text, value)) throw std::runtime_error("not an unsigned integer string: " + text);
    return value;
}

}  // namespace

TEST_CASE(api_service, every_response_carries_the_envelope_and_schema_header) {
    ServiceUnderTest service(fresh_service_directory("envelope"));
    const ServiceResponse info = service.get("/v1/service");
    CHECK_EQ(info.status, 200);
    CHECK_EQ(info.header("Exsim-Schema-Version"), std::string("exsim.api/1"));
    CHECK_EQ(info.text_at({"schemaVersion"}), std::string("exsim.api/1"));

    const ServiceResponse missing_route = service.get("/v1/nothing");
    CHECK_EQ(missing_route.status, 404);
    CHECK_EQ(missing_route.error_code(), std::string("not_found"));
    CHECK_EQ(missing_route.header("Exsim-Schema-Version"), std::string("exsim.api/1"));
    CHECK(missing_route.text_at({"error", "requestId"}).rfind("req_", 0) == 0);

    CHECK_EQ(service.get("/v1/runs/run_0000000000000000").status, 404);
    CHECK_EQ(service.get("/v1/experiments/exp_0000000000000000").status, 404);
    CHECK_EQ(service.send("DELETE", "/v1/runs").error_code(), std::string("invalid_request"));

    const ServiceResponse wrong_version = service.send("GET", "/v1/service", "", "", "Exsim-Schema-Version: exsim.api/2\r\n");
    CHECK_EQ(wrong_version.status, 400);
    CHECK_EQ(wrong_version.error_code(), std::string("unsupported_schema_version"));

    const ServiceResponse connections = service.get("/v1/connections");
    CHECK_EQ(connections.text_at({"serviceState"}), std::string("not_running"));
    CHECK_EQ(service.get("/v1/baselines").body.find("items")->items.size(), baseline_configurations().size());
}

TEST_CASE(api_service, malformed_and_hostile_bodies_are_rejected) {
    ServiceUnderTest service(fresh_service_directory("hostile"));
    CHECK_EQ(service.send("POST", "/v1/experiments", "{}").error_code(), std::string("idempotency_key_required"));
    const ServiceResponse short_key = service.send("POST", "/v1/experiments", "{}", "short");
    CHECK_EQ(short_key.error_code(), std::string("validation_failed"));
    CHECK_EQ(short_key.body.find("error")->find("fieldIssues")->items.at(0).find("fieldPath")->text, std::string("Idempotency-Key"));
    CHECK_EQ(service.post("/v1/experiments", "{").error_code(), std::string("invalid_request"));
    CHECK_EQ(service.post("/v1/experiments", "").error_code(), std::string("invalid_request"));
    CHECK_EQ(service.post("/v1/experiments", std::string(100'000, '[')).error_code(), std::string("invalid_request"));
    CHECK_EQ(service.post("/v1/experiments", R"({"a":"\ud800"})").status / 100, 4);
    CHECK_EQ(service.post("/v1/experiments", "[]").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.post("/v1/experiment-previews", R"({"name":1})").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.post("/v1/runs", R"({"experimentId":"exp_0000000000000000","masterSeed":"1"})").status, 404);
    CHECK_EQ(service.post("/v1/runs", R"({"experimentId":"x","masterSeed":"01"})").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.get("/v1/runs?limit=0").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.get("/v1/runs?limit=abc").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.get("/v1/runs?state=sleeping").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.get("/v1/runs?cursor=zzz").error_code(), std::string("invalid_request"));
    CHECK_EQ(service.get("/v1/runs?experimentId=exp_0000000000000000").status, 404);
    CHECK_EQ(service.get("/v1/comparisons").error_code(), std::string("validation_failed"));
}

TEST_CASE(api_service, unsupported_world_is_reported_by_preview_and_create) {
    ServiceUnderTest service(fresh_service_directory("world"));
    std::string body = experiment_request_body(0, "historical");
    body.replace(body.find("interactive_synthetic"), std::string_view("interactive_synthetic").size(), "historical_reconstruction");
    const ServiceResponse preview = service.post("/v1/experiment-previews", body);
    CHECK_EQ(preview.status, 200);
    CHECK(!preview.body.find("valid")->boolean);
    const ServiceResponse created = service.post("/v1/experiments", body);
    CHECK_EQ(created.status, 422);
    CHECK_EQ(created.error_code(), std::string("unsupported_market_world"));

    const ServiceResponse valid_preview = service.post("/v1/experiment-previews", experiment_request_body(0, "ok"));
    CHECK(valid_preview.body.find("valid")->boolean);
    CHECK_EQ(valid_preview.text_at({"estimatedWork", "runCount"}), std::string("3"));
}

TEST_CASE(api_service, completed_run_views_reconcile_with_each_other) {
    ServiceUnderTest service(fresh_service_directory("views"));
    const std::string experiment_id = create_experiment(service, 0, "views");
    const std::string run_id = create_run(service, experiment_id, "2");
    CHECK_EQ(service.get("/v1/runs/" + run_id).text_at({"state"}), std::string("queued"));
    service.drain_executor();
    const ServiceResponse run = service.get("/v1/runs/" + run_id);
    REQUIRE_EQ(run.text_at({"state"}), std::string("completed"));
    CHECK_EQ(run.text_at({"terminalDiagnostics", "outcomeCode"}), std::string("completed_at_simulation_bound"));
    CHECK_EQ(run.text_at({"progress", "simulationTimeNs"}), std::string("5000000000"));
    CHECK_EQ(run.text_at({"digests", "canonicalEventDigest", "algorithm"}), std::string("fnv1a_64"));
    const std::uint64_t processed = unsigned_text(run.text_at({"progress", "processedCanonicalEventCount"}));
    REQUIRE(processed > 100);

    // Events: contiguous positions, ordered by time, every page linked by its cursor.
    std::uint64_t expected_position = 0;
    std::uint64_t previous_time = 0;
    std::map<std::uint64_t, const JsonValue*> unused;
    std::map<std::uint64_t, std::pair<std::string, std::string>> kind_and_match_by_position;
    std::string cursor;
    for (;;) {
        const ServiceResponse page = service.get("/v1/runs/" + run_id + "/events?limit=1000" + (cursor.empty() ? "" : "&cursor=" + cursor));
        REQUIRE_EQ(page.status, 200);
        CHECK_EQ(page.text_at({"totalEventCount"}), std::to_string(processed));
        for (const JsonValue& event : page.body.find("items")->items) {
            CHECK_EQ(unsigned_text(event.find("eventPosition")->text), expected_position);
            const std::uint64_t time = unsigned_text(event.find("simulationTimeNs")->text);
            CHECK_GE(time, previous_time);
            previous_time = time;
            kind_and_match_by_position[expected_position] = {event.find("eventKind")->text, event.find("matchId")->text};
            ++expected_position;
        }
        const JsonValue* next = page.body.find("page")->find("nextCursor");
        if (next->is_null()) break;
        cursor = next->text;
    }
    CHECK_EQ(expected_position, processed);

    // Fills: every fill points at an order_filled event with the same match id, and the count matches the summary.
    std::uint64_t fill_count = 0;
    std::map<std::string, std::string> client_order_id_by_filled_order_id;
    cursor.clear();
    for (;;) {
        const ServiceResponse page = service.get("/v1/runs/" + run_id + "/fills?limit=1000" + (cursor.empty() ? "" : "&cursor=" + cursor));
        REQUIRE_EQ(page.status, 200);
        for (const JsonValue& fill : page.body.find("items")->items) {
            const auto& [kind, match_id] = kind_and_match_by_position[unsigned_text(fill.find("eventPosition")->text)];
            CHECK_EQ(kind, std::string("order_filled"));
            CHECK_EQ(match_id, fill.find("matchId")->text);
            client_order_id_by_filled_order_id[fill.find("orderId")->text] = fill.find("clientOrderId")->text;
            ++fill_count;
        }
        const JsonValue* next = page.body.find("page")->find("nextCursor");
        if (next->is_null()) break;
        cursor = next->text;
    }
    const ServiceResponse summary = service.get("/v1/runs/" + run_id + "/summary");
    CHECK_EQ(summary.text_at({"availability"}), std::string("final"));
    CHECK_EQ(summary.text_at({"orders", "fillCount"}), std::to_string(fill_count));
    REQUIRE(fill_count > 0);

    // Orders and lifecycle.
    const ServiceResponse orders = service.get("/v1/runs/" + run_id + "/orders?limit=1000");
    REQUIRE_EQ(orders.status, 200);
    REQUIRE(!orders.body.find("items")->items.empty());
    const JsonValue& first_order = orders.body.find("items")->items.front();
    const ServiceResponse lifecycle = service.get("/v1/runs/" + run_id + "/orders/" + first_order.find("orderId")->text + "/lifecycle");
    REQUIRE_EQ(lifecycle.status, 200);
    CHECK_EQ(lifecycle.body.find("stages")->items.front().find("stageKind")->text, std::string("strategy_submitted"));
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/orders/999999999/lifecycle").status, 404);
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/orders/abc/lifecycle").status, 404);
    // The orderId filter selects every fill of the own order that has had that
    // exchange order id; a replace gives the order a new id, so the fills share
    // the client order id rather than the exchange order id.
    const auto& [filled_order, filled_client_order] = *client_order_id_by_filled_order_id.begin();
    const ServiceResponse fills_of_order = service.get("/v1/runs/" + run_id + "/fills?orderId=" + filled_order);
    REQUIRE_EQ(fills_of_order.status, 200);
    bool queried_order_id_seen = false;
    for (const JsonValue& fill : fills_of_order.body.find("items")->items) {
        CHECK_EQ(fill.find("clientOrderId")->text, filled_client_order);
        if (fill.find("orderId")->text == filled_order) queried_order_id_seen = true;
    }
    CHECK(queried_order_id_seen);

    // Book views at the last event.
    const std::string last_position = std::to_string(processed - 1);
    const ServiceResponse truth = service.get("/v1/runs/" + run_id + "/book?view=exchange_truth&depth=3&eventPosition=" + last_position);
    REQUIRE_EQ(truth.status, 200);
    CHECK_LE(truth.body.find("bids")->items.size(), 3u);
    CHECK(truth.body.find("lastDeliveredSourceEventPosition")->is_null());
    const ServiceResponse visible = service.get("/v1/runs/" + run_id + "/book?view=strategy_visible&eventPosition=" + last_position);
    REQUIRE_EQ(visible.status, 200);
    CHECK_LE(visible.body.find("bids")->items.size(), 1u);
    if (!visible.body.find("bids")->items.empty()) CHECK(visible.body.find("bids")->items.front().find("orderCount")->is_null());
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/book?view=exchange_truth&eventPosition=" + std::to_string(processed + 10)).status, 409);
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/book?view=exchange_truth").error_code(), std::string("validation_failed"));

    // Series and manifest.
    const ServiceResponse series = service.get("/v1/runs/" + run_id + "/series?seriesKinds=equity,inventory&maxPoints=10");
    REQUIRE_EQ(series.status, 200);
    REQUIRE_EQ(series.body.find("series")->items.size(), 2u);
    CHECK_LE(series.body.find("series")->items.front().find("points")->items.size(), 10u);
    const ServiceResponse manifest = service.get("/v1/runs/" + run_id + "/manifest");
    CHECK_EQ(manifest.text_at({"configDigest", "algorithm"}), std::string("sha256"));
    CHECK_EQ(manifest.text_at({"seeds", "masterSeed"}), std::string("2"));

    // A completed run accepts no controls.
    const ServiceResponse step = service.post("/v1/runs/" + run_id + "/step", R"({"canonicalEventCount":1})");
    CHECK_EQ(step.status, 409);
    CHECK_EQ(step.error_code(), std::string("invalid_state_transition"));
    CHECK_EQ(step.text_at({"error", "currentRunState"}), std::string("completed"));
}

TEST_CASE(api_service, runs_are_deterministic_per_seed) {
    ServiceUnderTest service(fresh_service_directory("determinism"));
    const std::string first_experiment = create_experiment(service, 0, "first");
    const std::string second_experiment = create_experiment(service, 0, "second");
    const std::string first_run = create_run(service, first_experiment, "3");
    const std::string twin_run = create_run(service, second_experiment, "3");
    const std::string other_seed_run = create_run(service, first_experiment, "1");
    service.drain_executor();
    const auto digest_of = [&](const std::string& run_id) {
        const ServiceResponse run = service.get("/v1/runs/" + run_id);
        return run.text_at({"digests", "canonicalEventDigest", "hexDigest"}) + "/" + run.text_at({"digests", "ledgerDigest", "hexDigest"});
    };
    CHECK_EQ(digest_of(first_run), digest_of(twin_run));
    CHECK_NE(digest_of(first_run), digest_of(other_seed_run));
    CHECK_EQ(service.get("/v1/runs/" + first_run + "/summary").text_at({"accounting", "netPnl"}),
             service.get("/v1/runs/" + twin_run + "/summary").text_at({"accounting", "netPnl"}));
}

TEST_CASE(api_service, pause_step_resume_and_cancel_follow_the_state_machine) {
    ServiceUnderTest service(fresh_service_directory("controls"));
    const std::string experiment_id = create_experiment(service, 1, "controls");
    const std::string run_id = create_run(service, experiment_id, "1", true);
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/resume", "{}").error_code(), std::string("invalid_state_transition"));
    service.drain_executor();
    const ServiceResponse paused = service.get("/v1/runs/" + run_id);
    REQUIRE_EQ(paused.text_at({"state"}), std::string("paused"));
    CHECK_EQ(paused.text_at({"progress", "processedCanonicalEventCount"}), std::string("0"));

    std::uint64_t processed = 0;
    for (const std::uint64_t requested : {1u, 7u, 250u}) {
        const ServiceResponse stepped = service.post("/v1/runs/" + run_id + "/step", R"({"canonicalEventCount":)" + std::to_string(requested) + "}");
        REQUIRE_EQ(stepped.status, 200);
        CHECK_EQ(stepped.text_at({"state"}), std::string("paused"));
        const std::uint64_t now_processed = unsigned_text(stepped.text_at({"progress", "processedCanonicalEventCount"}));
        CHECK_GE(now_processed, processed + requested);
        CHECK_LE(now_processed, processed + requested + 64);  // one scheduler event never appends this many
        processed = now_processed;
    }
    const ServiceResponse partial_events = service.get("/v1/runs/" + run_id + "/events?limit=1000");
    CHECK_EQ(partial_events.body.find("items")->items.size(), processed);
    CHECK(!partial_events.body.find("page")->find("nextCursor")->is_null());  // a live run always has more to come
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/summary").text_at({"availability"}), std::string("partial"));
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/step", R"({"canonicalEventCount":0})").status / 100, 4);
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/step", R"({"canonicalEventCount":1,"extra":true})").error_code(), std::string("validation_failed"));
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/pause", "{}").text_at({"error", "currentRunState"}), std::string("paused"));

    CHECK_EQ(service.post("/v1/runs/" + run_id + "/resume", R"({"reason":"go on"})").text_at({"state"}), std::string("running"));
    CHECK(service.service().run_executor_pass());  // one slice, still running
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/pause", "").text_at({"state"}), std::string("paused"));
    const ServiceResponse cancelled = service.post("/v1/runs/" + run_id + "/cancel", R"({"reason":"enough"})");
    REQUIRE_EQ(cancelled.status, 200);
    CHECK_EQ(cancelled.text_at({"state"}), std::string("cancelled"));
    CHECK_EQ(cancelled.text_at({"terminalDiagnostics", "outcomeCode"}), std::string("cancelled_by_request"));
    const JsonValue& history = *cancelled.body.find("stateHistory");
    CHECK_EQ(history.items.back().find("reason")->text, std::string("enough"));
    CHECK_EQ(service.post("/v1/runs/" + run_id + "/cancel", "{}").error_code(), std::string("invalid_state_transition"));
    CHECK_EQ(service.get("/v1/runs/" + run_id + "/summary").text_at({"availability"}), std::string("final"));

    // A queued run can be cancelled before it ever starts.
    const std::string queued_run = create_run(service, experiment_id, "2");
    CHECK_EQ(service.post("/v1/runs/" + queued_run + "/cancel", "{}").text_at({"state"}), std::string("cancelled"));
    const ServiceResponse run_list = service.get("/v1/runs?experimentId=" + experiment_id + "&state=cancelled");
    CHECK_EQ(run_list.body.find("items")->items.size(), 2u);
}

TEST_CASE(api_service, idempotency_replays_successes_and_rejects_reuse) {
    ServiceUnderTest service(fresh_service_directory("idempotency"));
    const std::string body = experiment_request_body(0, "idempotent");
    // A failed request is not stored, so the same key can still succeed later.
    CHECK_EQ(service.send("POST", "/v1/experiments", "{}", "key-reused-000").status, 422);
    const ServiceResponse first = service.send("POST", "/v1/experiments", body, "key-reused-000");
    CHECK_EQ(first.status, 201);
    CHECK_EQ(first.header("Idempotent-Replayed"), std::string("false"));
    const ServiceResponse replay = service.send("POST", "/v1/experiments", body, "key-reused-000");
    CHECK_EQ(replay.status, 201);
    CHECK_EQ(replay.header("Idempotent-Replayed"), std::string("true"));
    CHECK_EQ(replay.body_text, first.body_text);
    // The same key with a different request body is a conflict.
    const ServiceResponse different = service.send("POST", "/v1/experiments", experiment_request_body(1, "idempotent"), "key-reused-000");
    CHECK_EQ(different.status, 409);
    CHECK_EQ(different.error_code(), std::string("idempotency_key_reused_with_different_request"));
    CHECK_EQ(service.get("/v1/experiments").body.find("items")->items.size(), 1u);
}

TEST_CASE(api_service, a_restarted_service_serves_stored_runs_and_fails_interrupted_ones) {
    const std::filesystem::path directory = fresh_service_directory("restart");
    std::string completed_run;
    std::string interrupted_run;
    std::string cancelled_run;
    std::string summary_before;
    std::string fills_before;
    {
        ServiceUnderTest service(directory);
        const std::string experiment_id = create_experiment(service, 0, "restart");
        completed_run = create_run(service, experiment_id, "1");
        interrupted_run = create_run(service, experiment_id, "2", true);
        service.drain_executor();
        CHECK_EQ(service.post("/v1/runs/" + interrupted_run + "/step", R"({"canonicalEventCount":10})").status, 200);
        cancelled_run = create_run(service, experiment_id, "3", true);
        CHECK_EQ(service.post("/v1/runs/" + cancelled_run + "/cancel", R"({"reason":"stopped before restart"})").status, 200);
        summary_before = service.get("/v1/runs/" + completed_run + "/summary").body_text;
        fills_before = service.get("/v1/runs/" + completed_run + "/fills?limit=1000").body_text;
        service.service().shutdown();
    }
    ServiceUnderTest restarted(directory);
    CHECK_EQ(restarted.get("/v1/runs/" + completed_run).text_at({"state"}), std::string("completed"));
    CHECK_EQ(restarted.get("/v1/runs/" + completed_run + "/summary").body_text, summary_before);
    CHECK_EQ(restarted.get("/v1/runs/" + completed_run + "/fills?limit=1000").body_text, fills_before);
    const ServiceResponse interrupted = restarted.get("/v1/runs/" + interrupted_run);
    CHECK_EQ(interrupted.text_at({"state"}), std::string("failed"));
    CHECK_EQ(interrupted.text_at({"terminalDiagnostics", "outcomeCode"}), std::string("failed_internal_error"));
    const ServiceResponse cancelled = restarted.get("/v1/runs/" + cancelled_run);
    CHECK_EQ(cancelled.text_at({"state"}), std::string("cancelled"));
    CHECK_EQ(cancelled.body.find("stateHistory")->items.back().find("reason")->text, std::string("stopped before restart"));
    CHECK_EQ(restarted.get("/v1/experiments").body.find("items")->items.size(), 1u);
}

TEST_CASE(api_service, comparison_pairs_completed_runs_by_seed) {
    ServiceUnderTest service(fresh_service_directory("comparison"));
    const std::string baseline_id = create_experiment(service, 0, "baseline", R"(["1","2","3"])");
    const std::string candidate_id = create_experiment(service, 1, "candidate", R"(["2","3","4"])");
    for (const char* seed : {"1", "2", "3"}) (void)create_run(service, baseline_id, seed);
    for (const char* seed : {"2", "3", "4"}) (void)create_run(service, candidate_id, seed);
    service.drain_executor();
    const ServiceResponse comparison =
        service.get("/v1/comparisons?baselineExperimentId=" + baseline_id + "&candidateExperimentId=" + candidate_id + "&maxPoints=20");
    REQUIRE_EQ(comparison.status, 200);
    CHECK(comparison.body.find("comparable")->boolean);
    const JsonValue& pairing = *comparison.body.find("seedPairing");
    CHECK_EQ(pairing.find("pairedSeeds")->items.size(), 2u);
    CHECK_EQ(pairing.find("baselineOnlySeeds")->items.at(0).text, std::string("1"));
    CHECK_EQ(pairing.find("candidateOnlySeeds")->items.at(0).text, std::string("4"));
    const JsonValue& metrics = *comparison.body.find("metricComparisons");
    REQUIRE(!metrics.items.empty());
    for (const JsonValue& metric : metrics.items) {
        CHECK_EQ(metric.find("perSeed")->items.size(), 2u);
        const JsonValue& dispersion = *metric.find("dispersion");
        const std::uint64_t counted = unsigned_text(dispersion.find("candidateBetterCount")->text) +
                                      unsigned_text(dispersion.find("candidateWorseCount")->text) + unsigned_text(dispersion.find("tiedCount")->text);
        CHECK_EQ(counted, unsigned_text(dispersion.find("sampleCount")->text));
    }
    for (const JsonValue& aligned : comparison.body.find("alignedSeries")->items) {
        const std::size_t bucket_count = aligned.find("bucketEndTimesNs")->items.size();
        CHECK_LE(bucket_count, 20u);
        for (const JsonValue& seed_values : aligned.find("baselineBySeed")->items) CHECK_EQ(seed_values.find("values")->items.size(), bucket_count);
    }
    CHECK(!comparison.body.find("assumptionDifferences")->items.empty());
    CHECK_EQ(service.get("/v1/comparisons?baselineExperimentId=" + baseline_id + "&candidateExperimentId=" + baseline_id).error_code(),
             std::string("comparison_not_paired"));
}

TEST_CASE(api_service, idempotency_capacity_rejects_before_mutation_and_preserves_replay) {
    ServiceUnderTest service(fresh_service_directory("key_capacity"), 1);
    const std::string body = experiment_request_body(0, "capacity");
    const ServiceResponse first = service.send("POST", "/v1/experiments", body, "capacity-first");
    REQUIRE_EQ(first.status, 201);
    CHECK_EQ(service.send("POST", "/v1/experiments", body, "capacity-second").status, 503);
    const ServiceResponse replay = service.send("POST", "/v1/experiments", body, "capacity-first");
    CHECK_EQ(replay.status, 201);
    CHECK_EQ(replay.body_text, first.body_text);
    CHECK_EQ(service.get("/v1/experiments").body.find("items")->items.size(), 1u);
}
