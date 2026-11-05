#include "exsim/api/research_service.hpp"

#include "exsim/api/api_json.hpp"
#include "exsim/api/sha256.hpp"
#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>

namespace exsim::api {

namespace {

constexpr int kMaxJsonNestingDepth = 64;
constexpr std::size_t kMaxIdempotencyKeys = 100'000;

ApiFailure validation_failure(const std::string& message, std::vector<FieldIssue> issues) {
    ApiFailure failure(422, "validation_failed", message);
    failure.field_issues = std::move(issues);
    return failure;
}

ApiFailure invalid_request(const std::string& message) { return ApiFailure(400, "invalid_request", message); }

ApiFailure not_found(const std::string& message) { return ApiFailure(404, "not_found", message); }

// parse_json recurses; refuse bodies that nest deeper than any valid request.
bool json_nesting_within_limit(std::string_view text) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char character : text) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
            continue;
        }
        if (character == '"') in_string = true;
        else if (character == '[' || character == '{') {
            if (++depth > kMaxJsonNestingDepth) return false;
        } else if (character == ']' || character == '}') {
            --depth;
        }
    }
    return true;
}

research::JsonValue parse_request_body(const std::string& body) {
    if (!json_nesting_within_limit(body)) throw invalid_request("request body nests deeper than " + std::to_string(kMaxJsonNestingDepth) + " levels");
    try {
        return research::parse_json(body);
    } catch (const research::JsonError& error) {
        throw invalid_request(std::string("request body is not valid JSON: ") + error.what());
    }
}

bool body_is_blank(const std::string& body) {
    return std::all_of(body.begin(), body.end(), [](char character) { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; });
}

std::optional<std::string> query_value(const HttpRequest& request, std::string_view name) {
    const auto values = request.query_values(name);
    if (values.empty()) return std::nullopt;
    return std::string(values.front());
}

std::uint64_t integer_query(const HttpRequest& request, std::string_view name, std::uint64_t default_value, std::uint64_t minimum,
                            std::uint64_t maximum) {
    const auto text = query_value(request, name);
    if (!text) return default_value;
    std::uint64_t value = 0;
    if (!parse_unsigned_integer_text(*text, value) || value < minimum || value > maximum)
        throw validation_failure(std::string(name) + " must be an integer from " + std::to_string(minimum) + " to " + std::to_string(maximum),
                                 {{"?" + std::string(name), "out of range"}});
    return value;
}

std::optional<std::uint64_t> unsigned_query(const HttpRequest& request, std::string_view name) {
    const auto text = query_value(request, name);
    if (!text) return std::nullopt;
    std::uint64_t value = 0;
    if (!parse_unsigned_integer_text(*text, value))
        throw validation_failure(std::string(name) + " must be an unsigned decimal string", {{"?" + std::string(name), "not an unsigned integer string"}});
    return value;
}

std::vector<std::string> enum_list_query(const HttpRequest& request, std::string_view name, const std::vector<std::string_view>& allowed) {
    std::vector<std::string> values;
    for (const std::string_view text : request.query_values(name)) {
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t comma = std::min(text.find(',', start), text.size());
            const std::string_view value = text.substr(start, comma - start);
            if (!value.empty()) {
                if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
                    throw validation_failure(std::string(name) + " contains unknown value " + std::string(value),
                                             {{"?" + std::string(name), "unknown value " + std::string(value)}});
                values.emplace_back(value);
            }
            start = comma + 1;
        }
    }
    return values;
}

std::uint64_t cursor_start(const HttpRequest& request, const std::string& fingerprint) {
    const auto cursor = query_value(request, "cursor");
    if (!cursor) return 0;
    const auto index = decode_page_cursor(*cursor, fingerprint);
    if (!index) throw invalid_request("cursor is invalid or was issued for different filters");
    return *index;
}

std::string join(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values) text += value + ",";
    return text;
}

std::vector<std::string_view> path_segments(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') {
            ++start;
            continue;
        }
        const std::size_t end = std::min(path.find('/', start), path.size());
        segments.push_back(path.substr(start, end - start));
        start = end;
    }
    return segments;
}

bool valid_idempotency_key(std::string_view key) {
    if (key.size() < 8 || key.size() > 128) return false;
    return std::all_of(key.begin(), key.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '-';
    });
}

const std::vector<std::string_view> kRunStateNames = {"queued", "running", "paused", "completed", "cancelled", "failed"};
const std::vector<std::string_view> kSeriesKinds = {"equity", "inventory", "exchange_mid", "quoted_spread", "drawdown"};
const std::vector<std::string_view> kVisibilities = {"exchange_truth", "strategy_visible"};
const std::vector<std::string_view> kActors = {"research_strategy", "background_market_maker", "background_noise_taker", "exchange", "runner"};
const std::vector<std::string_view> kMarketWorlds = {"interactive_synthetic", "historical_reconstruction", "historical_counterfactual_replay"};
const std::vector<std::string_view> kOrderFinalStatuses = {"open_at_run_end", "filled", "canceled", "partially_filled_then_canceled", "rejected"};

std::vector<std::string_view> canonical_event_kind_names() {
    std::vector<std::string_view> names;
    for (std::size_t index = 0; index < kCanonicalEventKindCount; ++index) names.emplace_back(to_api_string(static_cast<CanonicalEventKind>(index)));
    return names;
}

const char* better_direction(std::string_view metric_key) {
    static const std::set<std::string_view> higher = {"net_pnl", "equity", "gross_realized_pnl", "unrealized_pnl", "spread_capture_total", "markout_total"};
    static const std::set<std::string_view> lower = {"net_fees", "max_drawdown", "cancel_race_lost_count"};
    if (higher.count(metric_key)) return "higher";
    if (lower.count(metric_key)) return "lower";
    return "neither";
}

std::string read_whole_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

// ---- Lifetime ----------------------------------------------------------------------

ResearchService::ResearchService(ResearchServiceOptions options) : options_(std::move(options)) {
    std::error_code error;
    std::filesystem::create_directories(options_.data_directory / "experiments", error);
    std::filesystem::create_directories(options_.data_directory / "runs", error);
    if (error) throw std::runtime_error("cannot create data directory " + options_.data_directory.string() + ": " + error.message());
    load_data_directory();
    if (options_.start_executor) executor_thread_ = std::thread([this] { executor_loop(); });
}

ResearchService::~ResearchService() { shutdown(); }

void ResearchService::shutdown() {
    {
        std::lock_guard lock(executor_mutex_);
        if (executor_stop_requested_ && !executor_thread_.joinable()) return;
        executor_stop_requested_ = true;
    }
    executor_wakeup_.notify_all();
    if (executor_thread_.joinable()) executor_thread_.join();
    std::vector<std::shared_ptr<ResearchRun>> runs;
    {
        std::lock_guard lock(state_mutex_);
        runs = runs_in_creation_order_;
    }
    for (const auto& run : runs) run->abandon("the service stopped before the run finished");
}

bool ResearchService::run_executor_pass() {
    std::vector<std::shared_ptr<ResearchRun>> runs;
    {
        std::lock_guard lock(state_mutex_);
        runs = runs_in_creation_order_;
    }
    std::size_t live = 0;
    for (const auto& run : runs)
        if (run->holds_simulation()) ++live;
    for (const auto& run : runs) {
        if (live >= options_.max_live_simulations) break;
        if (run->state() == ApiRunState::Queued && run->activate()) ++live;
    }
    bool still_running = false;
    for (const auto& run : runs)
        if (run->state() == ApiRunState::Running && run->advance_running_slice(options_.executor_slice_steps)) still_running = true;
    return still_running;
}

void ResearchService::executor_loop() {
    while (true) {
        bool busy = false;
        try {
            busy = run_executor_pass();
        } catch (const std::exception& error) {
            std::cerr << "exsim_api: executor pass failed: " << error.what() << "\n";
        }
        std::unique_lock lock(executor_mutex_);
        if (executor_stop_requested_) return;
        if (!busy) executor_wakeup_.wait_for(lock, std::chrono::milliseconds(200));
        if (executor_stop_requested_) return;
    }
}

std::string ResearchService::new_identifier(std::string_view prefix) {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::lock_guard lock(state_mutex_);
    const std::uint64_t value = generator() ^ (++identifier_counter_ * 0x9E3779B97F4A7C15ull);
    return std::string(prefix) + research::format_hex64(value);
}

// ---- Persistence --------------------------------------------------------------------

void ResearchService::persist_experiment(const StoredExperiment& experiment) const {
    ApiJsonWriter json;
    json.begin_object();
    json.key("schema").string("exsim.api.experiment-record/1");
    json.key("experimentId").string(experiment.experiment_id);
    json.key("name").string(experiment.name);
    json.key("description").string(experiment.description);
    json.key("createdAtUnixMs").signed_text(experiment.created_at_unix_ms);
    json.key("derivedFromBaselineId").string(experiment.derived_from_baseline_id);
    json.key("parentExperimentId").optional_string(experiment.parent_experiment_id);
    json.key("config").raw(experiment.config_json);
    json.end_object();
    research::write_file_atomically(options_.data_directory / "experiments" / (experiment.experiment_id + ".json"), json.take() + "\n");
}

void ResearchService::append_idempotency_record(const std::string& key, const StoredIdempotentResponse& response) const {
    ApiJsonWriter json;
    json.begin_object();
    json.key("key").string(key);
    json.key("fingerprint").string(response.request_fingerprint);
    json.key("status").integer_number(response.status);
    json.key("body").string(response.body);
    json.end_object();
    std::ofstream out(options_.data_directory / "idempotency.jsonl", std::ios::app);
    out << json.text() << "\n";
}

void ResearchService::load_data_directory() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(options_.data_directory / "experiments", error)) {
        if (entry.path().extension() != ".json") continue;
        try {
            const research::JsonValue record = research::parse_json(read_whole_file(entry.path()));
            auto experiment = std::make_shared<StoredExperiment>();
            experiment->experiment_id = record.find("experimentId")->text;
            experiment->name = record.find("name")->text;
            experiment->description = record.find("description")->text;
            (void)parse_signed_integer_text(record.find("createdAtUnixMs")->text, experiment->created_at_unix_ms);
            experiment->derived_from_baseline_id = record.find("derivedFromBaselineId")->text;
            const research::JsonValue* parent = record.find("parentExperimentId");
            if (parent != nullptr && !parent->is_null()) experiment->parent_experiment_id = parent->text;
            const research::JsonValue* config = record.find("config");
            experiment->config_json = serialize_json_value(*config);
            ConfigTranslationResult translation = translate_experiment_config(*config, "/config");
            if (!translation.translated) throw std::runtime_error("stored configuration no longer translates");
            experiment->translated = std::make_shared<const TranslatedExperimentConfig>(std::move(*translation.translated));
            experiments_[experiment->experiment_id] = std::move(experiment);
        } catch (const std::exception& load_error) {
            std::cerr << "exsim_api: skipping experiment " << entry.path() << ": " << load_error.what() << "\n";
        }
    }
    std::vector<std::shared_ptr<ResearchRun>> loaded;
    for (const auto& entry : std::filesystem::directory_iterator(options_.data_directory / "runs", error)) {
        if (!std::filesystem::exists(entry.path() / "run.json")) continue;
        try {
            const research::JsonValue record = research::parse_json(read_whole_file(entry.path() / "run.json"));
            const auto experiment = experiments_.find(record.find("experimentId")->text);
            if (experiment == experiments_.end()) throw std::runtime_error("its experiment is missing");
            loaded.push_back(std::shared_ptr<ResearchRun>(ResearchRun::load(entry.path(), experiment->second->translated)));
        } catch (const std::exception& load_error) {
            std::cerr << "exsim_api: skipping run " << entry.path() << ": " << load_error.what() << "\n";
        }
    }
    std::sort(loaded.begin(), loaded.end(), [](const auto& left, const auto& right) {
        return left->created_at_unix_ms() != right->created_at_unix_ms() ? left->created_at_unix_ms() < right->created_at_unix_ms()
                                                                         : left->run_id() < right->run_id();
    });
    for (const auto& run : loaded) {
        runs_by_id_[run->run_id()] = run;
        runs_in_creation_order_.push_back(run);
    }
    std::ifstream idempotency(options_.data_directory / "idempotency.jsonl");
    std::string line;
    while (std::getline(idempotency, line)) {
        try {
            const research::JsonValue record = research::parse_json(line);
            StoredIdempotentResponse response;
            response.request_fingerprint = record.find("fingerprint")->text;
            response.status = static_cast<int>(record.find("status")->number);
            response.body = record.find("body")->text;
            idempotent_responses_[record.find("key")->text] = std::move(response);
        } catch (const std::exception&) {
            // A torn last line from a crash: ignore it.
        }
    }
}

std::shared_ptr<ResearchRun> ResearchService::find_run(const std::string& run_id) const {
    std::lock_guard lock(state_mutex_);
    const auto found = runs_by_id_.find(run_id);
    if (found == runs_by_id_.end()) throw not_found("run " + run_id + " does not exist");
    return found->second;
}

std::shared_ptr<const ResearchService::StoredExperiment> ResearchService::find_experiment(const std::string& experiment_id) const {
    std::lock_guard lock(state_mutex_);
    const auto found = experiments_.find(experiment_id);
    if (found == experiments_.end()) throw not_found("experiment " + experiment_id + " does not exist");
    return found->second;
}

// ---- Responses ------------------------------------------------------------------------

namespace {

HttpResponse json_response(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Exsim-Schema-Version", std::string(kApiSchemaVersion));
    response.set_header("Cache-Control", "no-store");
    return response;
}

std::string render_error_body(const ApiFailure& failure, const std::string& request_id) {
    ApiJsonWriter json;
    json.begin_object().key("error").begin_object();
    json.key("code").string(failure.code);
    json.key("message").string(failure.what());
    json.key("requestId").string(request_id);
    json.key("retryable").boolean(failure.retryable || failure.code == "service_unavailable" || failure.code == "internal_error");
    json.key("fieldIssues").begin_array();
    for (const FieldIssue& issue : failure.field_issues)
        json.begin_object().key("fieldPath").string(issue.field_path).key("issue").string(issue.issue).end_object();
    json.end_array();
    if (failure.current_run_state) json.key("currentRunState").string(*failure.current_run_state);
    json.end_object().end_object();
    return json.take();
}

}  // namespace

HttpResponse ResearchService::rejection_response(int http_status, std::string_view code, std::string_view message) {
    const std::string request_id = "req_" + std::to_string(next_request_number_++);
    ApiFailure failure(http_status, std::string(code), std::string(message));
    HttpResponse response = json_response(http_status, render_error_body(failure, request_id));
    response.close_connection = true;
    return response;
}

HttpResponse ResearchService::handle(const HttpRequest& request) {
    const std::string request_id = "req_" + std::to_string(next_request_number_++);
    try {
        return route(request, request_id);
    } catch (const ApiFailure& failure) {
        return json_response(failure.http_status, render_error_body(failure, request_id));
    } catch (const std::exception& error) {
        std::cerr << "exsim_api: " << request_id << " " << request.method << " " << request.path << " failed: " << error.what() << "\n";
        const ApiFailure failure(500, "internal_error", "internal error; see the service log for " + request_id);
        return json_response(500, render_error_body(failure, request_id));
    }
}

HttpResponse ResearchService::route(const HttpRequest& request, const std::string& request_id) {
    (void)request_id;
    if (const std::string* version = request.header("exsim-schema-version"); version != nullptr && *version != kApiSchemaVersion) {
        ApiFailure failure(400, "unsupported_schema_version", "this service speaks " + std::string(kApiSchemaVersion) + ", not " + *version);
        failure.field_issues.push_back({"Exsim-Schema-Version", "unsupported version"});
        throw failure;
    }
    if (request.method == "POST") return route_post(request);
    if (request.method != "GET") throw invalid_request("method " + request.method + " is not supported");
    return json_response(200, route_get(request));
}

// ---- GET --------------------------------------------------------------------------------

std::string ResearchService::route_get(const HttpRequest& request) {
    const auto segments = path_segments(request.path);
    if (segments.size() < 2 || segments[0] != "v1") throw not_found("no route for GET " + request.path);
    const std::string_view resource = segments[1];
    if (segments.size() == 2) {
        if (resource == "service") return render_service_info();
        if (resource == "baselines") return render_baselines();
        if (resource == "experiments") return list_experiments(request);
        if (resource == "runs") return list_runs(request);
        if (resource == "comparisons") return render_comparison(request);
        if (resource == "connections") return render_connections();
    }
    if (resource == "experiments" && segments.size() == 3) {
        const auto experiment = find_experiment(std::string(segments[2]));
        return render_experiment(*experiment);
    }
    if (resource == "runs" && segments.size() == 3) return route_run_get(request, std::string(segments[2]), "", "");
    if (resource == "runs" && segments.size() == 4) return route_run_get(request, std::string(segments[2]), std::string(segments[3]), "");
    if (resource == "runs" && segments.size() == 6 && segments[3] == "orders" && segments[5] == "lifecycle")
        return route_run_get(request, std::string(segments[2]), "lifecycle", std::string(segments[4]));
    throw not_found("no route for GET " + request.path);
}

std::string ResearchService::render_service_info() const {
    ApiJsonWriter json;
    json.begin_object();
    json.key("apiVersion").string("v1");
    json.key("schemaVersion").string(kApiSchemaVersion);
    json.key("serviceBuildId").string(options_.service_build_id);
    json.key("engineBuildId").string(research::kEngineBuildId);
    json.key("executableMarketWorlds").begin_array().string(kExecutableMarketWorld).end_array();
    json.key("executableStrategyKinds").begin_array().string("fixed_spread_market_maker").string("inventory_aware_market_maker").end_array();
    json.key("realTimeProtocolModeAvailable").boolean(false);
    json.end_object();
    return json.take();
}

std::string ResearchService::render_baselines() const {
    ApiJsonWriter json;
    json.begin_object().key("items").begin_array();
    for (const BaselineConfiguration& baseline : baseline_configurations()) {
        json.begin_object();
        json.key("baselineId").string(baseline.baseline_id);
        json.key("name").string(baseline.name);
        json.key("description").string(baseline.description);
        json.key("config").raw(baseline.config_json);
        json.end_object();
    }
    json.end_array().end_object();
    return json.take();
}

std::string ResearchService::render_experiment(const StoredExperiment& experiment) const {
    std::map<ApiRunState, std::uint64_t> counts;
    {
        std::lock_guard lock(state_mutex_);
        for (const auto& run : runs_in_creation_order_)
            if (run->experiment_id() == experiment.experiment_id) ++counts[run->state()];
    }
    ApiJsonWriter json;
    json.begin_object();
    json.key("experimentId").string(experiment.experiment_id);
    json.key("name").string(experiment.name);
    json.key("description").string(experiment.description);
    json.key("createdAt").string(format_wall_clock_time(experiment.created_at_unix_ms));
    json.key("derivedFromBaselineId").string(experiment.derived_from_baseline_id);
    json.key("parentExperimentId").optional_string(experiment.parent_experiment_id);
    json.key("marketWorld").string(experiment.translated->market_world);
    json.key("config").raw(experiment.config_json);
    json.key("configDigest").begin_object().key("algorithm").string("sha256").key("hexDigest").string(experiment.translated->config_digest_hex).end_object();
    json.key("runStateCounts").begin_object();
    for (const ApiRunState state : {ApiRunState::Queued, ApiRunState::Running, ApiRunState::Paused, ApiRunState::Completed,
                                    ApiRunState::Cancelled, ApiRunState::Failed})
        json.key(to_api_string(state)).integer_number(static_cast<std::int64_t>(counts[state]));
    json.end_object();
    json.end_object();
    return json.take();
}

std::string ResearchService::list_experiments(const HttpRequest& request) {
    const auto worlds = enum_list_query(request, "marketWorld", kMarketWorlds);
    const std::string world = worlds.empty() ? "" : worlds.front();
    const std::uint64_t limit = integer_query(request, "limit", 100, 1, 1000);
    const std::string fingerprint = "experiments|" + world;
    const std::uint64_t start = cursor_start(request, fingerprint);
    std::vector<std::shared_ptr<const StoredExperiment>> experiments;
    {
        std::lock_guard lock(state_mutex_);
        for (const auto& [identifier, experiment] : experiments_)
            if (world.empty() || experiment->translated->market_world == world) experiments.push_back(experiment);
    }
    std::sort(experiments.begin(), experiments.end(), [](const auto& left, const auto& right) {
        return left->created_at_unix_ms != right->created_at_unix_ms ? left->created_at_unix_ms > right->created_at_unix_ms
                                                                     : left->experiment_id < right->experiment_id;
    });
    ApiJsonWriter json;
    json.begin_object().key("items").begin_array();
    std::uint64_t index = start;
    for (; index < experiments.size() && index < start + limit; ++index) json.raw(render_experiment(*experiments[index]));
    json.end_array();
    json.key("page").begin_object();
    json.key("nextCursor");
    if (index < experiments.size()) json.string(encode_page_cursor(index, fingerprint));
    else json.null();
    json.key("limit").integer_number(static_cast<std::int64_t>(limit));
    json.end_object().end_object();
    return json.take();
}

std::string ResearchService::list_runs(const HttpRequest& request) {
    const auto experiment_id = query_value(request, "experimentId");
    if (experiment_id) (void)find_experiment(*experiment_id);
    const auto states = enum_list_query(request, "state", kRunStateNames);
    const auto master_seed = query_value(request, "masterSeed");
    const std::uint64_t limit = integer_query(request, "limit", 100, 1, 1000);
    const std::string fingerprint = "runs|" + experiment_id.value_or("") + "|" + join(states) + "|" + master_seed.value_or("");
    const std::uint64_t start = cursor_start(request, fingerprint);
    std::vector<std::shared_ptr<ResearchRun>> runs;
    {
        std::lock_guard lock(state_mutex_);
        runs = runs_in_creation_order_;
    }
    std::vector<std::shared_ptr<ResearchRun>> matching;
    for (const auto& run : runs) {
        if (experiment_id && run->experiment_id() != *experiment_id) continue;
        if (master_seed && std::to_string(run->master_seed()) != *master_seed) continue;
        if (!states.empty() && std::find(states.begin(), states.end(), to_api_string(run->state())) == states.end()) continue;
        matching.push_back(run);
    }
    std::stable_sort(matching.begin(), matching.end(), [](const auto& left, const auto& right) {
        return left->created_at_unix_ms() != right->created_at_unix_ms() ? left->created_at_unix_ms() > right->created_at_unix_ms()
                                                                         : left->run_id() < right->run_id();
    });
    ApiJsonWriter json;
    json.begin_object().key("items").begin_array();
    std::uint64_t index = start;
    for (; index < matching.size() && index < start + limit; ++index) json.raw(matching[index]->render_run_json());
    json.end_array();
    json.key("page").begin_object();
    json.key("nextCursor");
    if (index < matching.size()) json.string(encode_page_cursor(index, fingerprint));
    else json.null();
    json.key("limit").integer_number(static_cast<std::int64_t>(limit));
    json.end_object().end_object();
    return json.take();
}

std::string ResearchService::route_run_get(const HttpRequest& request, const std::string& run_id, const std::string& view,
                                           const std::string& order_segment) {
    const std::shared_ptr<ResearchRun> run = find_run(run_id);
    if (view.empty()) return run->render_run_json();
    if (view == "manifest") return run->render_manifest_json();
    if (view == "summary") return run->render_summary_json();
    if (view == "series") {
        SeriesQuery query;
        const auto kinds = enum_list_query(request, "seriesKinds", kSeriesKinds);
        query.kinds = kinds.empty() ? std::vector<std::string>(kSeriesKinds.begin(), kSeriesKinds.end()) : kinds;
        query.from_time_ns = unsigned_query(request, "fromSimulationTimeNs");
        query.to_time_ns = unsigned_query(request, "toSimulationTimeNs");
        query.max_points = static_cast<std::uint32_t>(integer_query(request, "maxPoints", 500, 2, 5000));
        return run->render_series_json(query);
    }
    if (view == "events") {
        EventPageQuery query;
        const auto from = unsigned_query(request, "fromEventPosition");
        query.to_position = unsigned_query(request, "toEventPosition");
        for (const std::string& kind : enum_list_query(request, "eventKinds", canonical_event_kind_names()))
            query.kinds.push_back(*parse_canonical_event_kind(kind));
        const auto visibility = enum_list_query(request, "visibility", kVisibilities);
        if (!visibility.empty()) query.visibility = parse_event_visibility(visibility.front());
        const auto actor = enum_list_query(request, "actor", kActors);
        if (!actor.empty()) query.actor = parse_event_actor(actor.front());
        query.order_id = unsigned_query(request, "orderId");
        query.limit = integer_query(request, "limit", 100, 1, 1000);
        const std::string fingerprint = "events|" + std::to_string(from.value_or(0)) + "|" + query_value(request, "toEventPosition").value_or("") + "|" +
                                        query_value(request, "eventKinds").value_or("") + "|" + join(visibility) + "|" + join(actor) + "|" +
                                        query_value(request, "orderId").value_or("");
        const auto cursor = query_value(request, "cursor");
        if (cursor) {
            const auto position = decode_page_cursor(*cursor, fingerprint);
            if (!position) throw invalid_request("cursor is invalid or was issued for different filters");
            query.start_position = *position;
        } else {
            query.start_position = from.value_or(0);
        }
        return run->render_events_json(query, fingerprint);
    }
    if (view == "orders") {
        const auto statuses = enum_list_query(request, "finalStatus", kOrderFinalStatuses);
        const std::optional<std::string> status = statuses.empty() ? std::nullopt : std::optional<std::string>(statuses.front());
        PageQuery page;
        page.limit = integer_query(request, "limit", 100, 1, 1000);
        const std::string fingerprint = "orders|" + status.value_or("");
        page.start_index = cursor_start(request, fingerprint);
        return run->render_orders_json(page, status, fingerprint);
    }
    if (view == "fills") {
        const auto order_id = unsigned_query(request, "orderId");
        PageQuery page;
        page.limit = integer_query(request, "limit", 100, 1, 1000);
        const std::string fingerprint = "fills|" + query_value(request, "orderId").value_or("");
        page.start_index = cursor_start(request, fingerprint);
        return run->render_fills_json(page, order_id, fingerprint);
    }
    if (view == "lifecycle") {
        std::uint64_t order_id = 0;
        if (!parse_unsigned_integer_text(order_segment, order_id)) throw not_found("order " + order_segment + " does not exist in this run");
        return run->render_order_lifecycle_json(order_id);
    }
    if (view == "book") {
        const auto position = unsigned_query(request, "eventPosition");
        const auto views = enum_list_query(request, "view", kVisibilities);
        if (!position || views.empty())
            throw validation_failure("eventPosition and view are required", {{"?eventPosition", "required"}, {"?view", "required"}});
        const std::size_t depth = integer_query(request, "depth", 5, 1, 50);
        return run->render_book_json(position, *parse_event_visibility(views.front()), depth);
    }
    throw not_found("no route for GET " + request.path);
}

std::string ResearchService::render_connections() const {
    ApiJsonWriter json;
    json.begin_object();
    json.key("mode").string("real_time_protocol");
    json.key("serviceState").string("not_running");
    json.key("observedAt").string(format_wall_clock_time(current_unix_milliseconds()));
    json.key("telemetryDroppedUpdateCount").string("0");
    json.key("sessions").begin_array().end_array();
    json.key("feeds").begin_array().end_array();
    json.key("faults").begin_array().end_array();
    json.end_object();
    return json.take();
}

// ---- Comparisons --------------------------------------------------------------------------

std::string ResearchService::render_comparison(const HttpRequest& request) {
    const auto baseline_id = query_value(request, "baselineExperimentId");
    const auto candidate_id = query_value(request, "candidateExperimentId");
    if (!baseline_id || !candidate_id)
        throw validation_failure("baselineExperimentId and candidateExperimentId are required",
                                 {{"?baselineExperimentId", "required"}, {"?candidateExperimentId", "required"}});
    if (*baseline_id == *candidate_id) throw ApiFailure(409, "comparison_not_paired", "baseline and candidate must be different experiments");
    const std::uint32_t max_points = static_cast<std::uint32_t>(integer_query(request, "maxPoints", 200, 2, 2000));
    const auto baseline = find_experiment(*baseline_id);
    const auto candidate = find_experiment(*candidate_id);

    std::vector<std::shared_ptr<ResearchRun>> runs;
    {
        std::lock_guard lock(state_mutex_);
        runs = runs_in_creation_order_;
    }
    // Completed runs by seed; a later completed run of the same seed wins.
    const auto completed_by_seed = [&](const std::string& experiment_id) {
        std::map<std::uint64_t, RunComparisonInputs> by_seed;
        for (const auto& run : runs) {
            if (run->experiment_id() != experiment_id) continue;
            if (auto inputs = run->comparison_inputs()) by_seed[run->master_seed()] = std::move(*inputs);
        }
        return by_seed;
    };
    const auto baseline_runs = completed_by_seed(*baseline_id);
    const auto candidate_runs = completed_by_seed(*candidate_id);
    std::vector<std::uint64_t> paired;
    for (const auto& [seed, inputs] : baseline_runs)
        if (candidate_runs.count(seed)) paired.push_back(seed);

    std::vector<std::string> reasons;
    if (baseline->translated->market_world != candidate->translated->market_world)
        reasons.push_back("market worlds differ: " + baseline->translated->market_world + " vs " + candidate->translated->market_world);
    if (paired.empty()) reasons.push_back("no master seed has a completed run in both experiments");

    ApiJsonWriter json;
    json.begin_object();
    json.key("schemaVersion").string("exsim.comparison/1");
    const auto reference = [&](const StoredExperiment& experiment, const std::map<std::uint64_t, RunComparisonInputs>& completed) {
        json.begin_object();
        json.key("experimentId").string(experiment.experiment_id);
        json.key("name").string(experiment.name);
        json.key("marketWorld").string(experiment.translated->market_world);
        json.key("configDigest").begin_object().key("algorithm").string("sha256").key("hexDigest").string(experiment.translated->config_digest_hex).end_object();
        json.key("completedSeeds").begin_array();
        for (const auto& [seed, inputs] : completed) json.unsigned_text(seed);
        json.end_array();
        json.end_object();
    };
    json.key("baseline");
    reference(*baseline, baseline_runs);
    json.key("candidate");
    reference(*candidate, candidate_runs);
    json.key("comparable").boolean(reasons.empty());
    json.key("incomparabilityReasons").begin_array();
    for (const std::string& reason : reasons) json.string(reason);
    json.end_array();
    json.key("seedPairing").begin_object();
    json.key("pairedSeeds").begin_array();
    for (const std::uint64_t seed : paired) json.unsigned_text(seed);
    json.end_array();
    json.key("baselineOnlySeeds").begin_array();
    for (const auto& [seed, inputs] : baseline_runs)
        if (!candidate_runs.count(seed)) json.unsigned_text(seed);
    json.end_array();
    json.key("candidateOnlySeeds").begin_array();
    for (const auto& [seed, inputs] : candidate_runs)
        if (!baseline_runs.count(seed)) json.unsigned_text(seed);
    json.end_array();
    json.end_object();

    json.key("assumptionDifferences").begin_array();
    for (const AssumptionDifference& difference :
         list_assumption_differences(research::parse_json(baseline->config_json), research::parse_json(candidate->config_json))) {
        json.begin_object();
        json.key("category").string(difference.reference.category);
        json.key("fieldPath").string(difference.reference.field_path);
        json.key("label").string(difference.reference.label);
        json.key("baselineValue").string(difference.baseline_value);
        json.key("candidateValue").string(difference.candidate_value);
        json.key("unit").optional_string(difference.reference.unit);
        json.end_object();
    }
    json.end_array();

    json.key("metricComparisons").begin_array();
    if (!paired.empty()) {
        const auto metric_identity = [](const RunComparisonInputs::Metric& metric) {
            return metric.metric_key + "@" + (metric.horizon_ns ? std::to_string(*metric.horizon_ns) : std::string());
        };
        const auto find_value = [&](const RunComparisonInputs& inputs, const std::string& identity) -> std::optional<std::string> {
            for (const auto& metric : inputs.metrics)
                if (metric_identity(metric) == identity) return metric.value;
            return std::nullopt;
        };
        for (const RunComparisonInputs::Metric& metric_template : baseline_runs.at(paired.front()).metrics) {
            const std::string identity = metric_identity(metric_template);
            const char* direction = better_direction(metric_template.metric_key);
            json.begin_object();
            json.key("metricKey").string(metric_template.metric_key);
            json.key("horizonNs");
            if (metric_template.horizon_ns) json.unsigned_text(*metric_template.horizon_ns);
            else json.null();
            json.key("unit").string(metric_template.unit);
            json.key("exactness").string(metric_template.exactness);
            json.key("betterDirection").string(direction);
            json.key("perSeed").begin_array();
            std::vector<ScaledDecimal> differences;
            for (const std::uint64_t seed : paired) {
                const RunComparisonInputs& baseline_run = baseline_runs.at(seed);
                const RunComparisonInputs& candidate_run = candidate_runs.at(seed);
                const auto baseline_value = find_value(baseline_run, identity);
                const auto candidate_value = find_value(candidate_run, identity);
                std::optional<ScaledDecimal> difference;
                if (baseline_value && candidate_value) {
                    const auto left = parse_scaled_decimal(*candidate_value);
                    const auto right = parse_scaled_decimal(*baseline_value);
                    if (left && right) difference = subtract_scaled_decimals(*left, *right);
                }
                json.begin_object();
                json.key("masterSeed").unsigned_text(seed);
                json.key("baselineRunId").string(baseline_run.run_id);
                json.key("candidateRunId").string(candidate_run.run_id);
                json.key("baselineValue").optional_string(baseline_value);
                json.key("candidateValue").optional_string(candidate_value);
                json.key("difference");
                if (difference) {
                    json.string(format_scaled_decimal(*difference));
                    differences.push_back(*difference);
                } else {
                    json.null();
                }
                json.end_object();
            }
            json.end_array();
            // Dispersion (derived floats, rounded half to even to 6 digits).
            const std::size_t count = differences.size();
            double mean = 0;
            for (const ScaledDecimal& difference : differences) mean += scaled_decimal_to_double(difference);
            if (count > 0) mean /= static_cast<double>(count);
            std::optional<double> deviation;
            if (count >= 2) {
                double sum_of_squares = 0;
                for (const ScaledDecimal& difference : differences) {
                    const double delta = scaled_decimal_to_double(difference) - mean;
                    sum_of_squares += delta * delta;
                }
                deviation = std::sqrt(sum_of_squares / static_cast<double>(count - 1));
            }
            std::optional<ScaledDecimal> minimum;
            std::optional<ScaledDecimal> maximum;
            std::int64_t positive = 0;
            std::int64_t negative = 0;
            const ScaledDecimal zero{};
            for (const ScaledDecimal& difference : differences) {
                if (!minimum || compare_scaled_decimals(difference, *minimum) < 0) minimum = difference;
                if (!maximum || compare_scaled_decimals(difference, *maximum) > 0) maximum = difference;
                const int sign = compare_scaled_decimals(difference, zero);
                if (sign > 0) ++positive;
                if (sign < 0) --negative;
            }
            negative = -negative;
            const std::int64_t tied = static_cast<std::int64_t>(count) - positive - negative;
            const std::string_view better = direction;
            json.key("dispersion").begin_object();
            json.key("sampleCount").integer_number(static_cast<std::int64_t>(count));
            json.key("meanDifference");
            if (count > 0) json.string(format_derived_decimal(mean));
            else json.null();
            json.key("sampleStandardDeviation");
            if (deviation) json.string(format_derived_decimal(*deviation));
            else json.null();
            json.key("standardErrorOfMean");
            if (deviation) json.string(format_derived_decimal(*deviation / std::sqrt(static_cast<double>(count))));
            else json.null();
            json.key("minimumDifference");
            if (minimum) json.string(format_scaled_decimal(*minimum));
            else json.null();
            json.key("maximumDifference");
            if (maximum) json.string(format_scaled_decimal(*maximum));
            else json.null();
            json.key("candidateBetterCount").integer_number(better == "higher" ? positive : better == "lower" ? negative : 0);
            json.key("candidateWorseCount").integer_number(better == "higher" ? negative : better == "lower" ? positive : 0);
            json.key("tiedCount").integer_number(better == "neither" ? static_cast<std::int64_t>(count) : tied);
            json.end_object();
            json.end_object();
        }
    }
    json.end_array();

    // Aligned series over [0, latest end], last value in each bucket.
    Timestamp aligned_end = 0;
    for (const auto* completed : {&baseline_runs, &candidate_runs})
        for (const auto& [seed, inputs] : *completed) aligned_end = std::max(aligned_end, inputs.end_time_ns);
    Timestamp bucket_width = (aligned_end + max_points - 1) / max_points;
    if (bucket_width == 0) bucket_width = 1;
    std::vector<Timestamp> bucket_ends;
    for (Timestamp bucket_end = bucket_width;; bucket_end += bucket_width) {
        const Timestamp capped = std::min(bucket_end, aligned_end);
        bucket_ends.push_back(capped);
        if (capped >= aligned_end) break;
    }
    json.key("alignedSeries").begin_array();
    for (const std::string_view series_kind : {std::string_view("equity"), std::string_view("inventory"), std::string_view("drawdown")}) {
        const auto aligned_values = [&](const RunComparisonInputs& inputs) {
            std::vector<std::optional<std::string>> values;
            std::size_t sample_index = 0;
            std::optional<std::string> last_value;
            bool has_peak = false;
            std::int64_t peak = 0;
            for (const Timestamp bucket_end : bucket_ends) {
                while (sample_index < inputs.samples.size() && inputs.samples[sample_index].time_ns <= bucket_end) {
                    const auto& sample = inputs.samples[sample_index++];
                    if (series_kind == "inventory") {
                        last_value = std::to_string(sample.position);
                    } else if (!sample.has_mark) {
                        last_value.reset();
                    } else if (series_kind == "equity") {
                        last_value = format_doubled_price_units(sample.equity_times_two);
                    } else {
                        if (!has_peak || sample.equity_times_two > peak) peak = sample.equity_times_two;
                        has_peak = true;
                        last_value = format_doubled_price_units(peak - sample.equity_times_two);
                    }
                }
                values.push_back(bucket_end > inputs.end_time_ns ? std::nullopt : last_value);
            }
            return values;
        };
        json.begin_object();
        json.key("seriesKind").string(series_kind);
        json.key("unit").string(series_kind == "inventory" ? "shares" : "currency");
        json.key("aggregation").string("last_value_in_bucket");
        json.key("bucketEndTimesNs").begin_array();
        for (const Timestamp bucket_end : bucket_ends) json.unsigned_text(bucket_end);
        json.end_array();
        for (const auto& [key, mean_key, completed] : {std::tuple{"baselineBySeed", "baselineMean", &baseline_runs},
                                                       std::tuple{"candidateBySeed", "candidateMean", &candidate_runs}}) {
            std::vector<std::vector<std::optional<std::string>>> by_seed;
            json.key(key).begin_array();
            for (const std::uint64_t seed : paired) {
                const RunComparisonInputs& inputs = completed->at(seed);
                by_seed.push_back(aligned_values(inputs));
                json.begin_object();
                json.key("masterSeed").unsigned_text(seed);
                json.key("runId").string(inputs.run_id);
                json.key("values").begin_array();
                for (const auto& value : by_seed.back()) json.optional_string(value);
                json.end_array();
                json.end_object();
            }
            json.end_array();
            json.key(mean_key).begin_array();
            for (std::size_t bucket = 0; bucket < bucket_ends.size(); ++bucket) {
                double total = 0;
                std::size_t present = 0;
                for (const auto& values : by_seed)
                    if (values[bucket]) {
                        if (const auto parsed = parse_scaled_decimal(*values[bucket])) {
                            total += scaled_decimal_to_double(*parsed);
                            ++present;
                        }
                    }
                if (present == 0) json.null();
                else json.string(format_derived_decimal(total / static_cast<double>(present)));
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_array();
    json.end_object();
    return json.take();
}

// ---- POST -------------------------------------------------------------------------------

HttpResponse ResearchService::route_post(const HttpRequest& request) {
    if (request.path == "/v1/experiment-previews") {
        if (body_is_blank(request.body)) throw invalid_request("a JSON request body is required");
        const research::JsonValue body = parse_request_body(request.body);
        return json_response(200, render_preview(body));
    }
    const std::string* key = request.header("idempotency-key");
    if (key == nullptr) throw ApiFailure(400, "idempotency_key_required", "this operation requires an Idempotency-Key header");
    if (!valid_idempotency_key(*key))
        throw validation_failure("Idempotency-Key must be 8-128 characters of [A-Za-z0-9_-]", {{"Idempotency-Key", "invalid format"}});
    std::optional<research::JsonValue> body;
    if (!body_is_blank(request.body)) body = parse_request_body(request.body);
    const std::string fingerprint = sha256_hex(request.path + "\n" + (body ? stable_serialize_json_value(*body) : std::string("null")));

    std::lock_guard mutation_lock(mutation_mutex_);
    {
        std::lock_guard lock(state_mutex_);
        const auto stored = idempotent_responses_.find(*key);
        if (stored != idempotent_responses_.end()) {
            if (stored->second.request_fingerprint != fingerprint)
                throw ApiFailure(409, "idempotency_key_reused_with_different_request", "this Idempotency-Key was used for a different request");
            HttpResponse response = json_response(stored->second.status, stored->second.body);
            response.set_header("Idempotent-Replayed", "true");
            return response;
        }
    }
    const auto [status, response_body] = execute_mutation(request, body ? &*body : nullptr);
    StoredIdempotentResponse stored{fingerprint, status, response_body};
    {
        std::lock_guard lock(state_mutex_);
        if (idempotent_responses_.size() < kMaxIdempotencyKeys) idempotent_responses_[*key] = stored;
    }
    try {
        append_idempotency_record(*key, stored);
    } catch (const std::exception&) {
    }
    HttpResponse response = json_response(status, response_body);
    response.set_header("Idempotent-Replayed", "false");
    return response;
}

std::pair<int, std::string> ResearchService::execute_mutation(const HttpRequest& request, const research::JsonValue* body) {
    const auto segments = path_segments(request.path);
    if (request.path == "/v1/experiments") {
        if (body == nullptr) throw invalid_request("a JSON request body is required");
        return {201, create_experiment(*body)};
    }
    if (request.path == "/v1/runs") {
        if (body == nullptr) throw invalid_request("a JSON request body is required");
        return {201, create_run(*body)};
    }
    if (segments.size() == 4 && segments[0] == "v1" && segments[1] == "runs") {
        const std::string_view action = segments[3];
        if (action != "cancel" && action != "pause" && action != "resume" && action != "step") throw not_found("no route for POST " + request.path);
        const std::shared_ptr<ResearchRun> run = find_run(std::string(segments[2]));
        std::string reason;
        std::uint64_t step_count = 0;
        if (action == "step") {
            if (body == nullptr) throw validation_failure("step requires a body", {{"/canonicalEventCount", "required"}});
            if (body->kind != research::JsonValue::Kind::Object) throw validation_failure("request body must be an object", {{"", "must be an object"}});
            for (const research::JsonMember& member : body->members)
                if (member.name != "canonicalEventCount") throw validation_failure("unknown field " + member.name, {{"/" + member.name, "unknown field"}});
            const research::JsonValue* count = body->find("canonicalEventCount");
            if (count == nullptr) throw validation_failure("canonicalEventCount is required", {{"/canonicalEventCount", "required"}});
            if (count->kind != research::JsonValue::Kind::Number || !parse_unsigned_integer_text(count->text, step_count))
                throw validation_failure("canonicalEventCount must be an integer", {{"/canonicalEventCount", "must be an integer"}});
            if (step_count < 1 || step_count > 100'000) throw invalid_request("canonicalEventCount must be from 1 to 100000");
        } else if (body != nullptr) {
            if (body->kind != research::JsonValue::Kind::Object) throw validation_failure("request body must be an object", {{"", "must be an object"}});
            for (const research::JsonMember& member : body->members) {
                if (member.name != "reason") throw validation_failure("unknown field " + member.name, {{"/" + member.name, "unknown field"}});
                if (member.value.kind != research::JsonValue::Kind::String || member.value.text.size() > 500)
                    throw validation_failure("reason must be a string of at most 500 characters", {{"/reason", "string of at most 500 characters"}});
                reason = member.value.text;
            }
        }
        if (action == "cancel") run->cancel(reason);
        else if (action == "pause") run->pause(reason);
        else if (action == "resume") run->resume(reason);
        else run->step(step_count, reason);
        executor_wakeup_.notify_all();
        return {200, run->render_run_json()};
    }
    throw not_found("no route for POST " + request.path);
}

std::string ResearchService::render_preview(const research::JsonValue& body) {
    std::vector<FieldIssue> issues;
    const ExperimentCreateFields fields = read_experiment_create_request(body, issues);
    if (!issues.empty() || fields.config == nullptr) throw validation_failure("request body does not match ExperimentCreateRequest", issues);
    ConfigTranslationResult translation = translate_experiment_config(*fields.config, "/config");
    if (!translation.shape_issues.empty()) throw validation_failure("request body does not match ExperimentCreateRequest", translation.shape_issues);
    std::vector<FieldIssue> semantic = translation.semantic_issues;
    const BaselineConfiguration* baseline = find_baseline(fields.derived_from_baseline_id);
    if (baseline == nullptr) semantic.push_back({"/derivedFromBaselineId", "unknown baseline"});
    if (fields.parent_experiment_id) {
        std::lock_guard lock(state_mutex_);
        if (!experiments_.count(*fields.parent_experiment_id)) semantic.push_back({"/parentExperimentId", "unknown experiment"});
    }
    std::map<std::string, std::string> baseline_values;
    if (baseline != nullptr)
        for (const AssumptionLine& line : flatten_experiment_config(research::parse_json(baseline->config_json))) baseline_values[line.field_path] = line.value;

    const research::JsonValue& config = *fields.config;
    const std::size_t run_count = config.find("seedSet")->find("masterSeeds")->items.size();
    const std::string duration_text = config.find("bounds")->find("endSimulationTimeNs")->text;
    std::uint64_t estimated_events = 0;
    if (translation.translated) estimated_events = estimate_canonical_events_per_run(translation.translated->runner_config);

    ApiJsonWriter json;
    json.begin_object();
    json.key("valid").boolean(semantic.empty() && translation.translated.has_value());
    json.key("fieldIssues").begin_array();
    for (const FieldIssue& issue : semantic) json.begin_object().key("fieldPath").string(issue.field_path).key("issue").string(issue.issue).end_object();
    json.end_array();
    json.key("configDigest").begin_object().key("algorithm").string("sha256").key("hexDigest").string(configuration_digest_hex(config)).end_object();
    json.key("assumptions").begin_array();
    for (const AssumptionLine& line : flatten_experiment_config(config)) {
        const auto baseline_value = baseline_values.find(line.field_path);
        json.begin_object();
        json.key("category").string(line.category);
        json.key("fieldPath").string(line.field_path);
        json.key("label").string(line.label);
        json.key("value").string(line.value);
        json.key("unit").optional_string(line.unit);
        json.key("differsFromBaseline").boolean(baseline_value == baseline_values.end() || baseline_value->second != line.value);
        json.end_object();
    }
    json.end_array();
    json.key("estimatedWork").begin_object();
    json.key("runCount").integer_number(static_cast<std::int64_t>(run_count));
    json.key("simulatedDurationPerRunNs").string(duration_text);
    json.key("estimatedCanonicalEventsPerRun").unsigned_text(estimated_events);
    json.key("estimatedWallClockSecondsTotal")
        .string(format_derived_decimal(static_cast<double>(run_count) * estimate_wall_clock_seconds_per_run(estimated_events)));
    json.end_object();
    json.end_object();
    return json.take();
}

std::string ResearchService::create_experiment(const research::JsonValue& body) {
    std::vector<FieldIssue> issues;
    const ExperimentCreateFields fields = read_experiment_create_request(body, issues);
    if (!issues.empty() || fields.config == nullptr) throw validation_failure("request body does not match ExperimentCreateRequest", issues);
    ConfigTranslationResult translation = translate_experiment_config(*fields.config, "/config");
    if (!translation.shape_issues.empty()) throw validation_failure("request body does not match ExperimentCreateRequest", translation.shape_issues);
    if (!translation.market_world_supported) {
        ApiFailure failure(422, "unsupported_market_world", "this market world is not executable in v1");
        failure.field_issues.push_back({"/config/marketWorld", "only interactive_synthetic is executable"});
        throw failure;
    }
    std::vector<FieldIssue> semantic = translation.semantic_issues;
    if (find_baseline(fields.derived_from_baseline_id) == nullptr) semantic.push_back({"/derivedFromBaselineId", "unknown baseline"});
    if (fields.parent_experiment_id) {
        std::lock_guard lock(state_mutex_);
        if (!experiments_.count(*fields.parent_experiment_id)) semantic.push_back({"/parentExperimentId", "unknown experiment"});
    }
    if (!semantic.empty() || !translation.translated) throw validation_failure("experiment configuration is not executable", semantic);

    auto experiment = std::make_shared<StoredExperiment>();
    experiment->experiment_id = new_identifier("exp_");
    experiment->name = fields.name;
    experiment->description = fields.description;
    experiment->created_at_unix_ms = current_unix_milliseconds();
    experiment->derived_from_baseline_id = fields.derived_from_baseline_id;
    experiment->parent_experiment_id = fields.parent_experiment_id;
    experiment->config_json = serialize_json_value(*fields.config);
    experiment->translated = std::make_shared<const TranslatedExperimentConfig>(std::move(*translation.translated));
    persist_experiment(*experiment);
    {
        std::lock_guard lock(state_mutex_);
        experiments_[experiment->experiment_id] = experiment;
    }
    return render_experiment(*experiment);
}

std::string ResearchService::create_run(const research::JsonValue& body) {
    std::vector<FieldIssue> issues;
    if (body.kind != research::JsonValue::Kind::Object) throw validation_failure("request body does not match RunCreateRequest", {{"", "must be an object"}});
    for (const research::JsonMember& member : body.members)
        if (member.name != "experimentId" && member.name != "masterSeed" && member.name != "startPaused")
            issues.push_back({"/" + member.name, "unknown field"});
    const research::JsonValue* experiment_id = body.find("experimentId");
    const research::JsonValue* master_seed = body.find("masterSeed");
    const research::JsonValue* start_paused = body.find("startPaused");
    std::uint64_t seed = 0;
    if (experiment_id == nullptr || experiment_id->kind != research::JsonValue::Kind::String) issues.push_back({"/experimentId", "required string"});
    if (master_seed == nullptr || master_seed->kind != research::JsonValue::Kind::String || !parse_unsigned_integer_text(master_seed->text, seed))
        issues.push_back({"/masterSeed", "required unsigned integer string"});
    if (start_paused != nullptr && start_paused->kind != research::JsonValue::Kind::Boolean) issues.push_back({"/startPaused", "must be a boolean"});
    if (!issues.empty()) throw validation_failure("request body does not match RunCreateRequest", issues);

    const auto experiment = find_experiment(experiment_id->text);
    const auto& seeds = experiment->translated->master_seeds;
    if (std::find(seeds.begin(), seeds.end(), seed) == seeds.end())
        throw validation_failure("masterSeed is not in the experiment's seed set", {{"/masterSeed", "not a member of config.seedSet.masterSeeds"}});

    RunCreateParameters parameters;
    parameters.run_id = new_identifier("run_");
    parameters.experiment_id = experiment->experiment_id;
    parameters.config = experiment->translated;
    parameters.master_seed = seed;
    parameters.start_paused = start_paused != nullptr && start_paused->boolean;
    parameters.created_at_unix_ms = current_unix_milliseconds();
    parameters.run_directory = options_.data_directory / "runs" / parameters.run_id;
    parameters.service_build_id = options_.service_build_id;
    auto run = std::make_shared<ResearchRun>(std::move(parameters));
    {
        std::lock_guard lock(state_mutex_);
        runs_by_id_[run->run_id()] = run;
        runs_in_creation_order_.push_back(run);
    }
    executor_wakeup_.notify_all();
    return run->render_run_json();
}

}  // namespace exsim::api
