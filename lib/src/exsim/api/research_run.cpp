#include "api/research_run.hpp"
#include "api/postgres_store.hpp"

#include "api/api_json.hpp"
#include "api/sha256.hpp"
#include "research/json.hpp"
#include "research/run_artifacts.hpp"
#include "research/seed_derivation.hpp"
#include "research/process_simulation.hpp"
#include "simulation_driver.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>

namespace exsim::api {

using research::RunStop;

// ---- Driver -----------------------------------------------------------------

namespace {

void sync_artifact(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) throw research::ArtifactError("cannot open artifact for sync: " + path.string());
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    if (result != 0) throw research::ArtifactError("cannot sync artifact: " + path.string());
}

class ProcessSimulationDriver final : public SimulationDriver {
public:
    ProcessSimulationDriver(const research::ExperimentConfig& config, std::uint64_t master_seed, research::RunArtifactOptions options)
        : simulation_(std::make_unique<research::ProcessSimulation>(config, master_seed)) {
        writer_ = std::make_unique<research::RunArtifactWriter>(
            *simulation_, research::builtin_strategy_identity(config.strategy.kind), std::move(options));
        writer_->begin();
    }
    RunStop step() override { return simulation_->step(); }
    research::SimulationCore& core() noexcept override { return *simulation_; }
    void drain() override { writer_->drain(simulation_->recorder()); }
    void finish() override { writer_->finish(simulation_->recorder()); }
    bool replay_verified() const noexcept override { return writer_->replay_verified(); }

private:
    std::unique_ptr<research::ProcessSimulation> simulation_;
    std::unique_ptr<research::RunArtifactWriter> writer_;
};

}  // namespace

std::unique_ptr<SimulationDriver> make_simulation_driver(const research::ExperimentConfig& config, std::uint64_t master_seed,
                                                         research::RunArtifactOptions options) {
    return std::make_unique<ProcessSimulationDriver>(config, master_seed, std::move(options));
}

std::string strategy_build_id(research::StrategyKind kind) {
    const auto identity = research::builtin_strategy_identity(kind);
    return std::string(identity.name) + "/" + std::to_string(identity.version);
}

std::vector<research::DerivedSeed> derive_run_seeds(const research::ExperimentConfig& config, std::uint64_t master_seed) {
    std::vector<research::DerivedSeed> seeds;
    seeds.push_back({"strategy", research::derive_stream_seed(master_seed, "strategy")});
    seeds.push_back({"latency", research::derive_stream_seed(master_seed, "latency")});
    for (std::size_t index = 0; index < config.agents.size(); ++index) {
        const auto& agent = config.agents[index];
        const std::string label = "agent." + std::to_string(index) + "." + std::string(research::to_string(agent.kind)) + "." + agent.name;
        seeds.push_back({label, research::derive_stream_seed(master_seed, label)});
    }
    return seeds;
}

// ---- States -------------------------------------------------------------------

const char* to_api_string(ApiRunState state) noexcept {
    switch (state) {
        case ApiRunState::Queued: return "queued";
        case ApiRunState::Running: return "running";
        case ApiRunState::Paused: return "paused";
        case ApiRunState::Completed: return "completed";
        case ApiRunState::Cancelled: return "cancelled";
        case ApiRunState::Failed: return "failed";
    }
    return "failed";
}

bool is_terminal(ApiRunState state) noexcept {
    return state == ApiRunState::Completed || state == ApiRunState::Cancelled || state == ApiRunState::Failed;
}

std::optional<ApiRunState> parse_api_run_state(std::string_view text) noexcept {
    for (const ApiRunState state : {ApiRunState::Queued, ApiRunState::Running, ApiRunState::Paused, ApiRunState::Completed,
                                    ApiRunState::Cancelled, ApiRunState::Failed})
        if (text == to_api_string(state)) return state;
    return std::nullopt;
}

namespace {

ApiFailure invalid_transition(ApiRunState state, const std::string& action) {
    ApiFailure failure(409, "invalid_state_transition", action + " is not allowed while the run is " + to_api_string(state));
    failure.current_run_state = to_api_string(state);
    return failure;
}

// ---- Binary artifact files ------------------------------------------------------

// Maximum payload of each API inspection cache, on both write and lazy read.
constexpr std::uint64_t kMaxArtifactPayloadBytes = 256ull * 1024 * 1024;

constexpr std::uint64_t kArtifactFileMagic = 0x6578'7369'6d61'7069ull;  // "exsimapi"

template <typename Record>
void write_records(const std::filesystem::path& path, const std::vector<Record>& records) {
    static_assert(std::is_trivially_copyable_v<Record>);
    if (records.size() > kMaxArtifactPayloadBytes / sizeof(Record))
        throw research::ArtifactError("inspection cache exceeds its 256 MiB payload limit: " + path.string());
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::uint64_t header[3] = {kArtifactFileMagic, sizeof(Record), records.size()};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        if (!records.empty())
            out.write(reinterpret_cast<const char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(Record)));
        out.flush();
        if (!out) throw research::ArtifactError("cannot write " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) throw research::ArtifactError("cannot rename " + temporary.string() + ": " + error.message());
}

template <typename Record>
std::vector<Record> read_records(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path))
        throw research::ArtifactError("unreadable artifact file " + path.string());
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto file_end = in.tellg();
    std::uint64_t header[3] = {};
    if (!in || file_end < static_cast<std::streamoff>(sizeof(header)))
        throw research::ArtifactError("unreadable artifact file " + path.string());
    in.seekg(0);
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || header[0] != kArtifactFileMagic || header[1] != sizeof(Record))
        throw research::ArtifactError("unreadable artifact file " + path.string());
    // Check the opened file's length before trusting its count or allocating.
    const auto payload_bytes = static_cast<std::uint64_t>(file_end) - sizeof(header);
    if (payload_bytes % sizeof(Record) != 0 || header[2] != payload_bytes / sizeof(Record))
        throw research::ArtifactError("artifact file length does not match its record count: " + path.string());
    if (payload_bytes > kMaxArtifactPayloadBytes)
        throw research::ArtifactError("inspection cache exceeds its 256 MiB payload limit: " + path.string());
    std::vector<Record> records(static_cast<std::size_t>(header[2]));
    if (!records.empty()) in.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(payload_bytes));
    if (!in) throw research::ArtifactError("truncated artifact file " + path.string());
    return records;
}

std::string outcome_message(const std::string& outcome_code) {
    if (outcome_code == "completed_at_simulation_bound") return "the run processed every event up to its simulation bound";
    if (outcome_code == "cancelled_by_request") return "cancelled by request; the residual position and open orders are kept";
    if (outcome_code == "failed_event_bound_exceeded") return "the run exceeded its event bound before the end time";
    if (outcome_code == "failed_wall_clock_limit_exceeded") return "the run exceeded its wall-clock time limit";
    if (outcome_code == "failed_journal_backpressure") return "the runner's record buffer overflowed";
    if (outcome_code == "failed_ledger_overflow") return "the position ledger rejected a fill";
    if (outcome_code == "failed_capacity_exceeded") return "a fixed runner capacity (event queue or engine output) was exceeded";
    if (outcome_code == "failed_replay_mismatch") return "the independent replay did not reproduce the run's event digest";
    return "internal error";
}

}  // namespace

// ---- Own-order index ----------------------------------------------------------------

void RunArtifacts::index_event(std::uint64_t position) {
    const CanonicalEventRecord& event = events[position];
    if (!event.has(kIsOwnOrder) || !event.has(kHasClientOrderId)) return;
    if (event.kind == CanonicalEventKind::StrategyOrderSubmitted) {
        OwnOrderRecord order;
        order.client_order_id = event.client_order_id;
        order.side = event.side;
        order.price = event.price;
        order.quantity = event.quantity;
        order.submit_time_ns = event.simulation_time_ns;
        order.submit_position = position;
        order.last_position = position;
        order.event_positions.push_back(position);
        order_by_client_order_id[event.client_order_id] = orders.size();
        orders.push_back(std::move(order));
        return;
    }
    const auto found = order_by_client_order_id.find(event.client_order_id);
    if (found == order_by_client_order_id.end()) return;
    const std::size_t order_index = found->second;
    OwnOrderRecord& order = orders[order_index];
    order.event_positions.push_back(position);
    order.last_position = position;
    const auto alias = [&](OrderId order_id) {
        if (order_id == 0) return;
        if (order.first_order_id == 0) order.first_order_id = order_id;
        order.current_order_id = order_id;
        order_by_exchange_order_id[order_id] = order_index;
    };
    switch (event.kind) {
        case CanonicalEventKind::OrderArrivedAtExchange:
            if (order.arrival_position == kNoEventPosition) order.arrival_position = position;
            break;
        case CanonicalEventKind::OrderAccepted:
            order.accepted_position = position;
            alias(event.order_id);
            break;
        case CanonicalEventKind::OrderRejected:
            if (event.rejected_command == CommandType::NewOrder) {
                order.rejected = true;
                order.done = true;
            }
            break;
        case CanonicalEventKind::OrderFilled:
            alias(event.order_id);
            order.cumulative_quantity += event.quantity;
            ++order.fill_count;
            if (event.leaves_quantity == 0) order.done = true;
            if (event.has(kHasMatchId)) {
                // fill_by_match_id is filled from the ledger rows (own fills only).
            }
            break;
        case CanonicalEventKind::OrderCanceled:
            if (!(event.has(kHasLeavesQuantity) && event.leaves_quantity > 0)) {
                order.canceled = true;
                order.done = true;
            }
            break;
        case CanonicalEventKind::OrderReplaced:
            alias(event.order_id);
            if (event.has(kHasPrice)) order.price = event.price;
            if (event.has(kHasQuantity)) order.quantity = event.quantity;
            break;
        case CanonicalEventKind::ExecutionReportDelivered:
            if (event.report_kind == DeliveredReportKind::Accepted && order.acknowledgment_position == kNoEventPosition)
                order.acknowledgment_position = position;
            break;
        default: break;
    }
}

// Fills are recorded densely from fill_index 0, so a row whose fill is not
// recorded (or whose horizon is out of range) can match no fill and is skipped.
void RunArtifacts::index_markout(std::size_t row) {
    const research::MarkoutRecord& markout = markouts[row];
    if (markout.fill_index >= fills.size() || markout.horizon_index >= research::kMaxMarkoutHorizons) return;
    const std::size_t slot = markout.fill_index * research::kMaxMarkoutHorizons + markout.horizon_index;
    if (slot >= markout_row_by_fill_horizon.size())
        markout_row_by_fill_horizon.resize((markout.fill_index + 1) * research::kMaxMarkoutHorizons, kNoMarkoutRow);
    markout_row_by_fill_horizon[slot] = row;
}

std::size_t RunArtifacts::markout_row(std::uint64_t fill_index, std::size_t horizon_index) const noexcept {
    if (fill_index >= markout_row_by_fill_horizon.size() / research::kMaxMarkoutHorizons || horizon_index >= research::kMaxMarkoutHorizons)
        return kNoMarkoutRow;
    return markout_row_by_fill_horizon[fill_index * research::kMaxMarkoutHorizons + horizon_index];
}

void RunArtifacts::rebuild_indexes() {
    orders.clear();
    order_by_client_order_id.clear();
    order_by_exchange_order_id.clear();
    fill_by_match_id.clear();
    markout_row_by_fill_horizon.clear();
    for (std::uint64_t position = 0; position < events.size(); ++position) index_event(position);
    for (std::size_t index = 0; index < fills.size(); ++index) fill_by_match_id[fills[index].match_id] = index;
    for (std::size_t row = 0; row < markouts.size(); ++row) index_markout(row);
}

// ---- Construction and persistence ------------------------------------------------------

void preserve_interrupted_caches(const std::filesystem::path& directory) {
    std::filesystem::path forensic;
    for (const auto* name : {"events.bin", "fills.bin", "fill_positions.bin", "markouts.bin", "samples.bin", "summary.json"}) {
        if (!std::filesystem::exists(directory / name)) continue;
        if (forensic.empty()) {
            for (std::size_t suffix = 0;; ++suffix) {
                forensic = directory / ("interrupted_" + std::to_string(suffix));
                if (std::filesystem::create_directory(forensic)) break;
            }
        }
        std::filesystem::rename(directory / name, forensic / name);
    }
    if (!forensic.empty()) {
        sync_artifact(forensic);
        sync_artifact(directory);
    }
}

namespace {

std::string render_manifest(const RunCreateParameters& parameters) {
    const TranslatedExperimentConfig& translated = *parameters.config;
    const research::JsonValue config = research::parse_json(translated.config_json);
    const auto member = [&](std::string_view name) {
        const research::JsonValue* value = config.find(name);
        return value == nullptr ? std::string("null") : serialize_json_value(*value);
    };
    ApiJsonWriter json;
    json.begin_object();
    json.key("schemaVersion").string("exsim.run-manifest/1");
    json.key("runId").string(parameters.run_id);
    json.key("experimentId").string(parameters.experiment_id);
    json.key("provenance").raw(parameters.provenance_json);
    json.key("parentRunId").null();
    json.key("mode").string("deterministic_research");
    json.key("marketWorld").string(translated.market_world);
    json.key("engineBuildId").string(research::kEngineBuildId);
    json.key("runnerBuildId").string(research::kRunnerBuildId);
    json.key("strategyBuildId").string(strategy_build_id(translated.runner_config.strategy.kind));
    json.key("configDigest").begin_object().key("algorithm").string("sha256").key("hexDigest").string(translated.config_digest_hex).end_object();
    json.key("inputDataDigest").null();
    json.key("instrument").raw(member("instrument"));
    json.key("scenario").raw(member("scenario"));
    json.key("strategy").raw(member("strategy"));
    json.key("seeds").begin_object();
    json.key("masterSeed").unsigned_text(parameters.master_seed);
    json.key("derivationFunction").string(research::kSeedDerivationDescription);
    json.key("derivedSeeds").begin_array();
    for (const research::DerivedSeed& seed : derive_run_seeds(translated.runner_config, parameters.master_seed)) {
        json.begin_object().key("streamName").string(seed.stream).key("seed").unsigned_text(seed.seed).end_object();
    }
    json.end_array();
    json.end_object();
    json.key("latency").raw(member("latency"));
    json.key("queueModel").string("exact_simulated_fifo");
    json.key("fees").raw(member("fees"));
    json.key("risk").raw(member("risk"));
    json.key("initialCash").raw(member("initialCash"));
    json.key("initialPositionShares").string("0");
    json.key("markConvention").raw(member("markConvention"));
    json.key("markoutHorizonsNs").raw(member("markoutHorizonsNs"));
    json.key("bounds").raw(member("bounds"));
    json.key("eventOrderPolicy").begin_object();
    json.key("primaryKey").string("simulation_time_ns");
    json.key("tieBreaker").string("enqueue_sequence");
    json.key("stageOrderForEqualTime").begin_array();
    for (const char* stage : {"feed_delivery", "strategy_processing", "outbound_order", "exchange_processing", "report_delivery", "runner_control"})
        json.string(stage);
    json.end_array();
    json.end_object();
    json.key("accounting").begin_object();
    json.key("cashScale").string("0.0001");
    json.key("costBasisMethod").string("fifo");
    json.key("feeSign").string("positive_paid_negative_rebate");
    json.key("equityDefinition").string("cash_plus_position_times_mark");
    json.key("netPnlDefinition").string("equity_minus_initial_cash");
    json.key("missingMarkPolicy").string("report_unavailable");
    json.end_object();
    json.key("artifactEncodings").begin_object();
    json.key("canonicalEventEncoding").string("exsim.canonical-event/1");
    json.key("lifecycleLogEncoding").string("exsim.order-lifecycle/1");
    json.key("ledgerEncoding").string("exsim.ledger/1");
    json.key("summaryEncoding").string("exsim.run-summary/1");
    json.key("seriesEncoding").string("exsim.series/1");
    json.end_object();
    json.key("matchingRule").string("price_time_fifo");
    json.end_object();
    return json.take();
}

std::uint64_t unsigned_member(const research::JsonValue& object, std::string_view name) {
    const research::JsonValue* value = object.find(name);
    std::uint64_t number = 0;
    if (value == nullptr || value->kind != research::JsonValue::Kind::String || !parse_unsigned_integer_text(value->text, number))
        throw research::ArtifactError("run record field " + std::string(name) + " is missing or invalid");
    return number;
}

std::string text_member(const research::JsonValue& object, std::string_view name) {
    const research::JsonValue* value = object.find(name);
    if (value == nullptr || value->kind != research::JsonValue::Kind::String)
        throw research::ArtifactError("run record field " + std::string(name) + " is missing or invalid");
    return value->text;
}

}  // namespace

ResearchRun::ResearchRun(RunCreateParameters parameters) : parameters_(std::move(parameters)) {
    manifest_json_ = render_manifest(parameters_);
    manifest_digest_hex_ = sha256_hex(manifest_json_);
    std::error_code error;
    std::filesystem::create_directories(parameters_.run_directory, error);
    if (error) throw research::ArtifactError("cannot create " + parameters_.run_directory.string() + ": " + error.message());
    research::write_file_atomically(parameters_.run_directory / "manifest.json", manifest_json_);
    if (parameters_.metadata_store) {
        sync_artifact(parameters_.run_directory / "manifest.json");
        sync_artifact(parameters_.run_directory);
        sync_artifact(parameters_.run_directory.parent_path());
    }
    std::lock_guard lock(mutex_);
    history_.push_back({std::nullopt, ApiRunState::Queued, parameters_.created_at_unix_ms, 0, 0, "created"});
    persist_record_locked();
}

ResearchRun::~ResearchRun() = default;

void ResearchRun::persist_record_locked() const {
    const auto record = render_record_locked();
    if (parameters_.metadata_store) parameters_.metadata_store->put(PostgresStore::Records::Runs, record);
    else research::write_file_atomically(parameters_.run_directory / (parameters_.worker_artifacts ? ".worker-record.json" : "run.json"), record + "\n");
}

std::pair<std::string, std::string> ResearchRun::worker_snapshot() const {
    std::lock_guard lock(mutex_);
    return {render_record_locked(), render_run_locked()};
}

std::string ResearchRun::render_record_locked() const {
    ApiJsonWriter json;
    json.begin_object();
    json.key("schema").string("exsim.api.run-record/1");
    json.key("runId").string(parameters_.run_id);
    json.key("experimentId").string(parameters_.experiment_id);
    json.key("masterSeed").unsigned_text(parameters_.master_seed);
    json.key("startPaused").boolean(parameters_.start_paused);
    json.key("createdAtUnixMs").signed_text(parameters_.created_at_unix_ms);
    json.key("creationOrdinal").unsigned_text(parameters_.creation_ordinal);
    json.key("state").string(to_api_string(state_));
    json.key("finalSimulationTimeNs").unsigned_text(simulation_time_locked());
    json.key("processedEventCount").unsigned_text(processed_event_count_locked());
    json.key("lastKnownPositionShares").signed_text(driver_ ? driver_->core().ledger().state().position :
        diagnostics_ ? diagnostics_->residual_position_shares : last_worker_position_);
    json.key("history").begin_array();
    for (const RunStateTransitionRecord& transition : history_) {
        json.begin_object();
        json.key("from").string(transition.from_state ? to_api_string(*transition.from_state) : "");
        json.key("to").string(to_api_string(transition.to_state));
        json.key("atUnixMs").signed_text(transition.transitioned_at_unix_ms);
        json.key("atEventPosition").unsigned_text(transition.at_event_position);
        json.key("atSimulationTimeNs").unsigned_text(transition.at_simulation_time_ns);
        json.key("reason").string(transition.reason);
        json.end_object();
    }
    json.end_array();
    json.key("diagnostics");
    if (diagnostics_) {
        json.begin_object();
        json.key("outcomeCode").string(diagnostics_->outcome_code);
        json.key("message").string(diagnostics_->message);
        json.key("residualPositionShares").signed_text(diagnostics_->residual_position_shares);
        json.key("residualOpenOrderCount").unsigned_text(diagnostics_->residual_open_order_count);
        json.end_object();
    } else {
        json.null();
    }
    json.key("canonicalEventDigest").optional_string(canonical_event_digest_hex_);
    json.key("ledgerDigest").optional_string(ledger_digest_hex_);
    json.end_object();
    return json.take();
}

void ResearchRun::persist_artifacts_locked() const {
    const std::filesystem::path& directory = parameters_.run_directory;
    write_records(directory / "events.bin", artifacts_.events);
    write_records(directory / "fills.bin", artifacts_.fills);
    write_records(directory / "fill_positions.bin", artifacts_.fill_event_positions);
    write_records(directory / "markouts.bin", artifacts_.markouts);
    write_records(directory / "samples.bin", artifacts_.samples);
    research::write_file_atomically(directory / "summary.json", final_summary_json_);
    if (parameters_.metadata_store || parameters_.worker_artifacts) {
        // Local artifacts must be durable before PostgreSQL publishes a terminal
        // state. This executes only at finalization, outside simulation steps.
        std::vector<std::filesystem::path> directories;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_symlink()) throw research::ArtifactError("refusing symbolic artifact path");
            if (entry.is_regular_file()) sync_artifact(entry.path());
            else if (entry.is_directory()) directories.push_back(entry.path());
        }
        for (auto item = directories.rbegin(); item != directories.rend(); ++item) sync_artifact(*item);
        sync_artifact(directory);
    }
}

void ResearchRun::load_artifacts_locked() const {
    if (artifacts_loaded_) return;
    const std::filesystem::path& directory = parameters_.run_directory;
    artifacts_.events = read_records<CanonicalEventRecord>(directory / "events.bin");
    artifacts_.fills = read_records<research::FillRecord>(directory / "fills.bin");
    artifacts_.fill_event_positions = read_records<std::uint64_t>(directory / "fill_positions.bin");
    artifacts_.markouts = read_records<research::MarkoutRecord>(directory / "markouts.bin");
    artifacts_.samples = read_records<research::SeriesSample>(directory / "samples.bin");
    if (artifacts_.fill_event_positions.size() != artifacts_.fills.size())
        throw research::ArtifactError("fill positions do not match fills in " + directory.string());
    artifacts_.rebuild_indexes();
    artifacts_loaded_ = true;
}

std::unique_ptr<ResearchRun> ResearchRun::load(const std::filesystem::path& run_directory,
                                               std::shared_ptr<const TranslatedExperimentConfig> config,
                                               std::shared_ptr<PostgresStore> metadata_store, std::string record_json,
                                               std::string executable_path, std::string current_provenance, bool snapshot,
                                               bool worker_artifacts) {
    const research::JsonValue record = research::parse_json(record_json.empty() ? research::read_text_file(run_directory / "run.json") : record_json);
    std::unique_ptr<ResearchRun> run(new ResearchRun());
    run->parameters_.run_id = text_member(record, "runId");
    run->parameters_.experiment_id = text_member(record, "experimentId");
    run->parameters_.master_seed = unsigned_member(record, "masterSeed");
    if (record.find("creationOrdinal")) run->parameters_.creation_ordinal = unsigned_member(record, "creationOrdinal");
    run->parameters_.config = std::move(config);
    run->parameters_.metadata_store = std::move(metadata_store);
    run->parameters_.executable_path = std::move(executable_path);
    run->parameters_.worker_artifacts = worker_artifacts;
    run->parameters_.run_directory = run_directory;
    std::int64_t created = 0;
    if (!parse_signed_integer_text(text_member(record, "createdAtUnixMs"), created))
        throw research::ArtifactError("run record createdAtUnixMs is invalid");
    run->parameters_.created_at_unix_ms = created;
    const research::JsonValue* start_paused = record.find("startPaused");
    run->parameters_.start_paused = start_paused != nullptr && start_paused->boolean;
    run->manifest_json_ = research::read_text_file(run_directory / "manifest.json");
    run->manifest_digest_hex_ = sha256_hex(run->manifest_json_);
    const auto state = parse_api_run_state(text_member(record, "state"));
    if (!state) throw research::ArtifactError("run record state is invalid");
    run->state_ = *state;
    run->final_simulation_time_ns_ = unsigned_member(record, "finalSimulationTimeNs");
    if (record.find("processedEventCount")) run->last_worker_event_count_ = unsigned_member(record, "processedEventCount");
    if (const auto* position = record.find("lastKnownPositionShares"))
        (void)parse_signed_integer_text(position->text, run->last_worker_position_);
    const research::JsonValue* history = record.find("history");
    if (history != nullptr) {
        for (const research::JsonValue& item : history->items) {
            RunStateTransitionRecord transition;
            const std::string from = text_member(item, "from");
            if (!from.empty()) transition.from_state = parse_api_run_state(from);
            transition.to_state = parse_api_run_state(text_member(item, "to")).value_or(ApiRunState::Failed);
            std::int64_t at = 0;
            (void)parse_signed_integer_text(text_member(item, "atUnixMs"), at);
            transition.transitioned_at_unix_ms = at;
            transition.at_event_position = unsigned_member(item, "atEventPosition");
            transition.at_simulation_time_ns = unsigned_member(item, "atSimulationTimeNs");
            transition.reason = text_member(item, "reason");
            run->history_.push_back(std::move(transition));
        }
    }
    if (!record.find("processedEventCount") && !run->history_.empty())
        run->last_worker_event_count_ = run->history_.back().at_event_position;
    const research::JsonValue* diagnostics = record.find("diagnostics");
    if (diagnostics != nullptr && diagnostics->kind == research::JsonValue::Kind::Object) {
        TerminalDiagnosticsRecord loaded;
        loaded.outcome_code = text_member(*diagnostics, "outcomeCode");
        loaded.message = text_member(*diagnostics, "message");
        (void)parse_signed_integer_text(text_member(*diagnostics, "residualPositionShares"), loaded.residual_position_shares);
        loaded.residual_open_order_count = unsigned_member(*diagnostics, "residualOpenOrderCount");
        run->diagnostics_ = loaded;
    }
    for (const auto& [name, target] : {std::pair{"canonicalEventDigest", &run->canonical_event_digest_hex_},
                                       std::pair{"ledgerDigest", &run->ledger_digest_hex_}}) {
        const research::JsonValue* digest = record.find(name);
        if (digest != nullptr && digest->kind == research::JsonValue::Kind::String) *target = digest->text;
    }

    std::lock_guard lock(run->mutex_);
    if (is_terminal(run->state_)) {
        if (std::filesystem::exists(run_directory / "events.bin")) {
            run->final_summary_json_ = research::read_text_file(run_directory / "summary.json");
            run->artifacts_loaded_ = false;
        } else if (run->state_ != ApiRunState::Failed) {
            throw research::ArtifactError("terminal run is missing its inspection artifacts");
        } else {
            const auto summary_path = run_directory / "summary.json";
            if (std::filesystem::is_regular_file(summary_path)) run->final_summary_json_ = research::read_text_file(summary_path);
            // Preserve recorded progress, but missing inspection data is not an empty log.
            run->artifacts_loaded_ = false;
        }
        // A failed startup/shutdown or artifact write may have no cache. Retain
        // its original diagnostics/history instead of rewriting it on every load.
        return run;
    }
    // PostgreSQL queues survive API restarts. Running/paused simulations still
    // fail explicitly until worker ownership and checkpoint recovery exist.
    if (snapshot) {
        const auto manifest = research::parse_json(run->manifest_json_);
        const auto* provenance = manifest.find("provenance");
        if (provenance) run->parameters_.provenance_json = serialize_json_value(*provenance);
        // The API holds metadata, never the worker's in-memory event arrays.
        // A queued worker starts with genuinely empty, local arrays.
        if (run->state_ != ApiRunState::Queued) run->artifacts_loaded_ = false;
        return run;
    }
    const bool queued = run->state_ == ApiRunState::Queued && run->parameters_.metadata_store;
    if (queued) {
        const auto manifest = research::parse_json(run->manifest_json_);
        const auto* saved_provenance = manifest.find("provenance");
        const auto expected = research::parse_json(current_provenance);
        if (saved_provenance && !run->parameters_.executable_path.empty() &&
            stable_serialize_json_value(*saved_provenance) == stable_serialize_json_value(expected) &&
            text_member(manifest, "engineBuildId") == research::kEngineBuildId &&
            text_member(manifest, "runnerBuildId") == research::kRunnerBuildId &&
            text_member(manifest, "strategyBuildId") == strategy_build_id(run->parameters_.config->runner_config.strategy.kind)) {
            run->parameters_.provenance_json = serialize_json_value(*saved_provenance);
            return run;
        }
    }
    // The service stopped while the run was live: its simulation is gone.
    // A worker may have died between cache renames. Never promote those bytes
    // into a supposedly complete failed-run inspection snapshot on restart.
    preserve_interrupted_caches(run_directory);
    run->artifacts_loaded_ = false;
    run->final_summary_json_.clear();
    const ApiRunState previous = run->state_;
    run->state_ = ApiRunState::Failed;
    const std::string interruption = queued ? "queued run requires its original executable and release provenance; create a new run for this release"
                                            : "the service stopped before the run finished";
    run->diagnostics_ = TerminalDiagnosticsRecord{"failed_internal_error", interruption, run->last_worker_position_, 0};
    run->history_.push_back({previous, ApiRunState::Failed, current_unix_milliseconds(),
        run->last_worker_event_count_, run->final_simulation_time_ns_, "service restarted; final state unavailable"});
    run->final_summary_json_.clear();
    run->persist_record_locked();
    return run;
}

// ---- State machine ---------------------------------------------------------------------

ApiRunState ResearchRun::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool ResearchRun::holds_simulation() const {
    std::lock_guard lock(mutex_);
    return driver_ != nullptr && !is_terminal(state_);
}

Timestamp ResearchRun::simulation_time_locked() const noexcept {
    if (driver_) return driver_->core().now();
    return final_simulation_time_ns_;
}

void ResearchRun::transition_locked(ApiRunState to_state, const std::string& reason) {
    history_.push_back({state_, to_state, current_unix_milliseconds(), processed_event_count_locked(), simulation_time_locked(), reason});
    state_ = to_state;
    // In-memory effects cannot be rolled back. Propagate the write failure so
    // the service retains the mutation's idempotency reservation.
    persist_record_locked();
}

void ResearchRun::ensure_driver_locked() {
    if (driver_) return;
    research::RunArtifactOptions options;
    options.run_directory = parameters_.run_directory / "runner";
    // Keep native artifacts identical to a CLI run of the saved config.
    // The service ID remains the identity of the enclosing API run.
    options.run_id = research::make_run_id(parameters_.config->runner_config.name,
                                         research::experiment_config_hash(parameters_.config->runner_config), parameters_.master_seed);
    options.config_source_text = parameters_.config->runner_config_text;
    options.build = research::current_build_info(parameters_.executable_path);
    options.overwrite = true;
    driver_ = make_simulation_driver(parameters_.config->runner_config, parameters_.master_seed, std::move(options));
    research::write_file_atomically(parameters_.run_directory / "runner" / "provenance.json", parameters_.provenance_json + "\n");
    builder_ = std::make_unique<CanonicalEventBuilder>(parameters_.config->runner_config);
}

bool ResearchRun::activate() {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Queued) return false;
    try {
        ensure_driver_locked();
        transition_locked(ApiRunState::Running, "started");
        if (parameters_.start_paused) transition_locked(ApiRunState::Paused, "startPaused: paused before the first event");
        return true;
    } catch (const std::exception& error) {
        fail_execution_locked(error.what());
        return false;
    } catch (...) {
        fail_execution_locked("unknown simulation startup failure");
        return false;
    }
}

research::RunStop ResearchRun::step_driver_locked() {
    research::SimulationCore& core = driver_->core();
    research::RunRecorder& recorder = core.recorder();
    while (true) {
        const RunStop stop = driver_->step();
        const std::size_t first_new_event = artifacts_.events.size();
        const auto commands = recorder.commands.records().subspan(consumed_commands_);
        const auto lifecycle = recorder.lifecycle.records().subspan(consumed_lifecycle_);
        builder_->append_step(core, commands, lifecycle, core.simulation_events_processed() + core.observation_events_processed(),
                              artifacts_.events);
        consumed_commands_ = recorder.commands.size();
        consumed_lifecycle_ = recorder.lifecycle.size();
        for (std::size_t position = first_new_event; position < artifacts_.events.size(); ++position) artifacts_.index_event(position);
        const auto& fill_positions = builder_->own_fill_event_positions();
        for (const research::FillRecord& fill : recorder.fills.records().subspan(consumed_fills_)) {
            artifacts_.fill_by_match_id[fill.match_id] = artifacts_.fills.size();
            const std::size_t ordinal = artifacts_.fills.size();
            artifacts_.fill_event_positions.push_back(ordinal < fill_positions.size() ? fill_positions[ordinal] : kNoEventPosition);
            artifacts_.fills.push_back(fill);
        }
        consumed_fills_ = recorder.fills.size();
        for (const research::MarkoutRecord& markout : recorder.markouts.records().subspan(consumed_markouts_)) {
            artifacts_.markouts.push_back(markout);
            artifacts_.index_markout(artifacts_.markouts.size() - 1);
        }
        consumed_markouts_ = recorder.markouts.size();
        for (const research::SeriesSample& sample : recorder.samples.records().subspan(consumed_samples_)) artifacts_.samples.push_back(sample);
        consumed_samples_ = recorder.samples.size();
        if (stop == RunStop::FlushRequired) {
            driver_->drain();
            consumed_commands_ = consumed_lifecycle_ = consumed_fills_ = consumed_markouts_ = consumed_samples_ = 0;
            continue;
        }
        if (stop == RunStop::StepBudgetExhausted || stop == RunStop::PausedAtTime) check_limits_locked();
        return stop;
    }
}

void ResearchRun::check_limits_locked() {
    if (pending_limit_failure_code_) return;
    if (!builder_->consistency_error().empty()) {
        pending_limit_failure_code_ = "failed_internal_error";
        pending_limit_failure_message_ = "canonical log consistency check failed: " + builder_->consistency_error();
    } else if (artifacts_.events.size() > parameters_.config->max_canonical_event_count) {
        pending_limit_failure_code_ = "failed_event_bound_exceeded";
        pending_limit_failure_message_ = "the canonical event log exceeded bounds.maxCanonicalEventCount (" +
                                         std::to_string(parameters_.config->max_canonical_event_count) + ")";
    } else if (running_wall_seconds_ > parameters_.config->wall_clock_time_limit_seconds) {
        pending_limit_failure_code_ = "failed_wall_clock_limit_exceeded";
        pending_limit_failure_message_ = "the run exceeded bounds.wallClockTimeLimitSeconds (" +
                                         std::to_string(parameters_.config->wall_clock_time_limit_seconds) + " s)";
    }
    if (pending_limit_failure_code_) driver_->core().request_stop();
}

void ResearchRun::fail_locked(const std::string& outcome_code, const std::string& message) {
    diagnostics_ = TerminalDiagnosticsRecord{outcome_code, message, last_worker_position_, 0};
    if (driver_) {
        diagnostics_->residual_position_shares = driver_->core().ledger().state().position;
        final_simulation_time_ns_ = driver_->core().now();
    }
    transition_locked(ApiRunState::Failed, message);
}

// Preserve available evidence without invoking a writer that already failed.
// Cleanup and terminal state must survive even an unwritable failure record.
void ResearchRun::fail_execution_locked(std::string_view error) noexcept {
    const ApiRunState previous = state_;
    state_ = ApiRunState::Failed;
    try {
        std::uint64_t open_orders = 0;
        for (const OwnOrderRecord& order : artifacts_.orders)
            if (order.accepted_position != kNoEventPosition && !order.done) ++open_orders;
        const auto position = driver_ ? driver_->core().ledger().state().position : 0;
        final_simulation_time_ns_ = driver_ ? driver_->core().now() : final_simulation_time_ns_;
        diagnostics_ = TerminalDiagnosticsRecord{"failed_internal_error", "run execution failed: " + std::string(error), position, open_orders};
        if (driver_) {
            canonical_event_digest_hex_ = research::format_hex64(driver_->core().digests().event_digest);
            ledger_digest_hex_ = research::format_hex64(driver_->core().digests().ledger_digest);
        }
        history_.push_back({previous, state_, current_unix_milliseconds(), processed_event_count_locked(), final_simulation_time_ns_, diagnostics_->message});
        final_summary_json_ = render_summary_locked();
    } catch (...) {
        // Allocation/formatting failure cannot retain a live simulation either.
    }
    if (!final_summary_json_.empty()) {
        try {
            persist_artifacts_locked();
        } catch (...) {
            // A small summary can survive even when large cache writes fail.
            try { research::write_file_atomically(parameters_.run_directory / "summary.json", final_summary_json_); } catch (...) {}
        }
    }
    try { persist_record_locked(); } catch (...) {}
    driver_.reset();
    builder_.reset();
}

void ResearchRun::finish_locked(research::RunStop stop, const std::string& requester_reason) {
    research::SimulationCore& core = driver_->core();
    const std::uint64_t step_sequence = core.simulation_events_processed() + core.observation_events_processed();
    if (stop == RunStop::Completed) {
        builder_->append_simulation_bounds_reached(core.now(), step_sequence, artifacts_.events);
        artifacts_.index_event(artifacts_.events.size() - 1);
    }
    bool replay_verified = false;
    std::string writer_error;
    try {
        driver_->finish();
        replay_verified = driver_->replay_verified();
    } catch (const std::exception& error) {
        writer_error = error.what();
    }

    std::string outcome_code;
    ApiRunState terminal_state = ApiRunState::Failed;
    std::string message;
    using research::TerminationReason;
    switch (core.termination()) {
        case TerminationReason::EndTimeReached:
        case TerminationReason::EventQueueExhausted:
            outcome_code = "completed_at_simulation_bound";
            terminal_state = ApiRunState::Completed;
            break;
        case TerminationReason::CancelRequested:
            outcome_code = "cancelled_by_request";
            terminal_state = ApiRunState::Cancelled;
            break;
        case TerminationReason::EventLimitReached: outcome_code = "failed_event_bound_exceeded"; break;
        case TerminationReason::SimulationQueueFull:
        case TerminationReason::ObservationQueueFull:
        case TerminationReason::EngineOutputCapacity: outcome_code = "failed_capacity_exceeded"; break;
        case TerminationReason::RecorderOverflow: outcome_code = "failed_journal_backpressure"; break;
        case TerminationReason::LedgerRejectedFill: outcome_code = "failed_ledger_overflow"; break;
        default: outcome_code = "failed_internal_error"; break;
    }
    message = outcome_message(outcome_code) + " (runner termination: " + research::to_string(core.termination()) + ")";
    if (pending_limit_failure_code_) {
        outcome_code = *pending_limit_failure_code_;
        terminal_state = ApiRunState::Failed;
        message = pending_limit_failure_message_;
    } else if (!builder_->consistency_error().empty()) {
        outcome_code = "failed_internal_error";
        terminal_state = ApiRunState::Failed;
        message = "canonical log consistency check failed: " + builder_->consistency_error();
    } else if (!writer_error.empty()) {
        outcome_code = "failed_internal_error";
        terminal_state = ApiRunState::Failed;
        message = "runner artifacts could not be written: " + writer_error;
    } else if (!replay_verified) {
        outcome_code = "failed_replay_mismatch";
        terminal_state = ApiRunState::Failed;
        message = outcome_message(outcome_code);
    }

    std::uint64_t open_orders = 0;
    for (const OwnOrderRecord& order : artifacts_.orders)
        if (order.accepted_position != kNoEventPosition && !order.done) ++open_orders;
    diagnostics_ = TerminalDiagnosticsRecord{outcome_code, message, core.ledger().state().position, open_orders};
    canonical_event_digest_hex_ = research::format_hex64(core.digests().event_digest);
    ledger_digest_hex_ = research::format_hex64(core.digests().ledger_digest);
    final_simulation_time_ns_ = core.now();
    const ApiRunState previous_state = state_;
    state_ = terminal_state;  // the summary is rendered as final
    final_summary_json_ = render_summary_locked();
    state_ = previous_state;
    const bool use_requester_reason = terminal_state == ApiRunState::Cancelled && !requester_reason.empty();
    history_.push_back({state_, terminal_state, current_unix_milliseconds(), processed_event_count_locked(), simulation_time_locked(),
                        use_requester_reason ? requester_reason : message});
    state_ = terminal_state;
    try {
        persist_artifacts_locked();
        // Publish the terminal record only after every inspection cache exists.
        persist_record_locked();
    } catch (const std::exception& error) {
        const std::string persistence_error = "run artifacts could not be persisted: " + std::string(error.what());
        const std::string failure_message = terminal_state == ApiRunState::Failed
                                                ? diagnostics_->message + "; " + persistence_error : persistence_error;
        diagnostics_->outcome_code = "failed_internal_error";
        diagnostics_->message = failure_message;
        history_.push_back({state_, ApiRunState::Failed, current_unix_milliseconds(), processed_event_count_locked(),
                            simulation_time_locked(), failure_message});
        state_ = ApiRunState::Failed;
        // Keep accurate accounting available in memory even if its cache failed.
        final_summary_json_ = render_summary_locked();
        try {
            persist_record_locked();
        } catch (const std::exception&) {
            // Preserve the first failure; the service/executor reports it.
        }
        driver_.reset();
        builder_.reset();
        throw research::ArtifactError(failure_message);
    }
    driver_.reset();
    builder_.reset();
}

bool ResearchRun::advance_running_slice(std::uint64_t max_steps) {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Running || !driver_) return false;
    const auto started = std::chrono::steady_clock::now();
    bool time_accounted = false;
    const auto account_time = [&] {
        if (!time_accounted) {
            running_wall_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            time_accounted = true;
        }
    };
    try {
        for (std::uint64_t steps = 0; steps < max_steps; ++steps) {
            const RunStop stop = step_driver_locked();
            if (stop == RunStop::Completed || stop == RunStop::Failed || stop == RunStop::Cancelled) {
                account_time();
                finish_locked(stop);
                return false;
            }
        }
        account_time();
        check_limits_locked();
        return state_ == ApiRunState::Running;
    } catch (const std::exception& error) {
        account_time();
        if (driver_) fail_execution_locked(error.what());
        throw;
    } catch (...) {
        account_time();
        if (driver_) fail_execution_locked("unknown simulation execution failure");
        throw;
    }
}

void ResearchRun::pause(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Running) throw invalid_transition(state_, "pause");
    try {
        transition_locked(ApiRunState::Paused, reason.empty() ? "paused by request" : reason);
    } catch (const std::exception& error) {
        if (driver_) fail_execution_locked(error.what());
        throw;
    } catch (...) {
        if (driver_) fail_execution_locked("unknown simulation control failure");
        throw;
    }
}

void ResearchRun::resume(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Paused) throw invalid_transition(state_, "resume");
    try {
        transition_locked(ApiRunState::Running, reason.empty() ? "resumed by request" : reason);
    } catch (const std::exception& error) {
        if (driver_) fail_execution_locked(error.what());
        throw;
    } catch (...) {
        if (driver_) fail_execution_locked("unknown simulation control failure");
        throw;
    }
}

void ResearchRun::step(std::uint64_t canonical_event_count, const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Paused || !driver_) throw invalid_transition(state_, "step");
    try {
        const std::uint64_t target = artifacts_.events.size() + canonical_event_count;
        while (artifacts_.events.size() < target) {
            const RunStop stop = step_driver_locked();
            if (stop == RunStop::Completed || stop == RunStop::Failed || stop == RunStop::Cancelled) {
                finish_locked(stop);
                return;
            }
        }
        history_.push_back({ApiRunState::Paused, ApiRunState::Paused, current_unix_milliseconds(), processed_event_count_locked(),
                            simulation_time_locked(), reason.empty() ? "stepped " + std::to_string(canonical_event_count) + " canonical events" : reason});
        persist_record_locked();
    } catch (const std::exception& error) {
        if (driver_) fail_execution_locked(error.what());
        throw;
    } catch (...) {
        if (driver_) fail_execution_locked("unknown simulation control failure");
        throw;
    }
}

void ResearchRun::cancel(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (is_terminal(state_)) throw invalid_transition(state_, "cancel");
    try {
        ensure_driver_locked();
        driver_->core().request_stop();
        while (true) {
            const RunStop stop = step_driver_locked();
            if (stop == RunStop::Completed || stop == RunStop::Failed || stop == RunStop::Cancelled) {
                finish_locked(stop, stop == RunStop::Cancelled ? reason : std::string());
                break;
            }
        }
    } catch (const std::exception& error) {
        if (driver_ || !is_terminal(state_)) fail_execution_locked(error.what());
        throw;
    } catch (...) {
        if (driver_ || !is_terminal(state_)) fail_execution_locked("unknown simulation control failure");
        throw;
    }
}

void ResearchRun::cancel_before_execution(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Queued || driver_) throw invalid_transition(state_, "cancel");
    diagnostics_ = TerminalDiagnosticsRecord{"cancelled_by_request", "cancelled before simulation execution", 0, 0};
    state_ = ApiRunState::Cancelled;
    history_.push_back({ApiRunState::Queued, state_, current_unix_milliseconds(), 0, 0,
                        reason.empty() ? diagnostics_->message : reason});
    final_summary_json_ = render_summary_locked();
    persist_artifacts_locked();
    persist_record_locked();
}

void ResearchRun::reserve_worker_launch() {
    std::lock_guard lock(mutex_);
    if (state_ != ApiRunState::Queued) throw invalid_transition(state_, "worker launch");
    transition_locked(ApiRunState::Running, "simulation worker launch reserved");
}

void ResearchRun::abandon(const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (is_terminal(state_)) return;
    try {
        fail_locked("failed_internal_error", reason);
    } catch (...) {
        driver_.reset();
        builder_.reset();
        throw;
    }
    driver_.reset();
    builder_.reset();
}

const OwnOrderRecord* ResearchRun::find_order_locked(OrderId order_id) const {
    const auto found = artifacts_.order_by_exchange_order_id.find(order_id);
    if (found == artifacts_.order_by_exchange_order_id.end()) return nullptr;
    return &artifacts_.orders[found->second];
}

void ResearchRun::require_artifacts_ready_locked() const {
    if (!artifacts_loaded_) {
        try {
            load_artifacts_locked();
        } catch (const std::exception& error) {
            throw ApiFailure(500, "internal_error", std::string("stored run artifacts are unreadable: ") + error.what());
        }
    }
}

}  // namespace exsim::api
