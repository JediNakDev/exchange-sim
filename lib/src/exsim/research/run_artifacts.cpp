#include "exsim/research/run_artifacts.hpp"

#include "exsim/research/json.hpp"
#include "exsim/research/seed_derivation.hpp"
#include "exsim/sim/canonical.hpp"
#include "exsim/sim/fields.hpp"
#include "exsim/sim/journal.hpp"
#include "exsim/sim/price_text.hpp"

#include <bit>
#include <ctime>
#include <iterator>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace exsim::research {

namespace {

constexpr std::string_view kEventOrderPolicy =
    "Simulation events run in (simulated time, enqueue sequence) order. Observations (series samples, markout probes) "
    "run after every simulation event at the same or an earlier time and never schedule simulation events. Initial "
    "events at t=0 are enqueued as: background agent timers in configuration order, strategy start, first series "
    "sample. For one engine command the strategy's execution reports are enqueued in engine event order, then at most "
    "one market-data update. Background agents act synchronously at their timers and observe every engine event batch "
    "immediately. Full order-entry sessions use simulated one-second heartbeat ticks; every nonempty engine batch "
    "produces a feed delivery, including deeper-book changes.";

constexpr std::string_view kSeriesHeader =
    "time_ns,exchange_bid,exchange_bid_qty,exchange_ask,exchange_ask_qty,visible_bid,visible_bid_qty,visible_ask,"
    "visible_ask_qty,position,known_position,cash,realized_pnl,fees,mark_price,unrealized_pnl,equity,open_bid_qty,"
    "open_ask_qty\n";

std::string utc_timestamp(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm calendar{};
    gmtime_r(&seconds, &calendar);
    char text[32];
    const std::size_t length = std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &calendar);
    return std::string(text, length);
}

void put_entries_object(JsonWriter& json, const ConfigEntries& entries) {
    json.begin_object();
    for (const auto& [key, value] : entries) json.key(key).string(value);
    json.end_object();
}

// One engine field as JSON: prices as decimal strings, enums by name, flags as
// booleans, every other integer as a decimal string.
void put_engine_field(JsonWriter& json, const sim::Field& field) {
    json.key(field.name);
    if (field.kind == sim::FieldKind::Price) {
        json.price_text(std::bit_cast<Price>(field.raw));
    } else if (field.kind == sim::FieldKind::Bool) {
        json.boolean(field.raw != 0);
    } else if (sim::is_enum_kind(field.kind)) {
        const std::string_view name = sim::enum_value_name(field.kind, field.raw);
        if (name.empty()) json.integer_text(field.raw);
        else json.string(name);
    } else {
        json.integer_text(field.raw);
    }
}

void append_csv_price(std::string& line, bool present, Price price) {
    if (present) line += sim::format_price(price);
    line += ',';
}

void append_csv_doubled_price(std::string& line, bool present, std::int64_t value_times_two) {
    if (present) line += format_doubled_price(value_times_two);
    line += ',';
}

void append_csv_integer(std::string& line, std::uint64_t value) {
    line += std::to_string(value);
    line += ',';
}

void append_csv_signed(std::string& line, std::int64_t value) {
    line += std::to_string(value);
    line += ',';
}

std::ofstream open_stream(const std::filesystem::path& path, std::string_view header = {}) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw ArtifactError("cannot create " + path.string());
    if (!header.empty()) stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    return stream;
}

void put_file_description(JsonWriter& json, std::string_view name, std::string_view format, std::string_view schema,
                          std::string_view description) {
    json.begin_object();
    json.key("name").string(name);
    json.key("format").string(format);
    json.key("schema").string(schema);
    json.key("description").string(description);
    json.end_object();
}

}  // namespace

BuildInfo current_build_info(const std::string& executable_path) {
    BuildInfo info;
#if defined(__VERSION__)
    info.compiler = __VERSION__;
#else
    info.compiler = "unknown";
#endif
    std::ifstream executable(executable_path, std::ios::binary);
    if (executable) {
        std::uint64_t hash = 14695981039346656037ull;
        char block[1 << 16];
        while (executable) {
            executable.read(block, sizeof block);
            const auto count = executable.gcount();
            for (std::streamsize i = 0; i < count; ++i) {
                hash ^= static_cast<unsigned char>(block[i]);
                hash *= 1099511628211ull;
            }
        }
        info.executable_fnv1a64 = format_hex64(hash);
    }
    return info;
}

std::string make_run_id(std::string_view experiment_name, std::uint64_t config_hash, std::uint64_t master_seed) {
    std::string run_id;
    for (const char character : experiment_name) {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
        run_id += allowed ? character : '_';
    }
    if (run_id.empty()) run_id = "experiment";
    run_id += '-';
    run_id += format_hex64(config_hash).substr(0, 12);
    run_id += "-seed";
    run_id += std::to_string(master_seed);
    return run_id;
}

bool is_run_artifact_file_name(std::string_view name) noexcept {
    if (name == kExecutionTraceFileName || name == "execution-decisions.jsonl.tmp" || name == "provenance.json" || name == "provenance.json.tmp") return true;
    for (const std::string_view artifact_name : kRunArtifactFileNames) {
        if (name == artifact_name) return true;
        if (name.size() == artifact_name.size() + kAtomicWriteSuffix.size() && name.substr(0, artifact_name.size()) == artifact_name &&
            name.substr(artifact_name.size()) == kAtomicWriteSuffix)
            return true;
    }
    return false;
}

void check_run_directory_reusable(const std::filesystem::path& directory, bool overwrite) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) throw ArtifactError("cannot inspect " + directory.string() + ": " + error.message());
        return;
    }
    if (!std::filesystem::is_directory(directory, error)) throw ArtifactError(directory.string() + " is not a directory");
    bool has_artifacts = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (!is_run_artifact_file_name(name) || entry.is_directory())
            throw ArtifactError(directory.string() + " contains " + name + ", which is not a run artifact; refusing to use it");
        has_artifacts = true;
    }
    if (error) throw ArtifactError("cannot list " + directory.string() + ": " + error.message());
    if (has_artifacts && !overwrite)
        throw ArtifactError(directory.string() + " already holds a run; pass --overwrite to replace it");
}

void prepare_run_directory(const std::filesystem::path& directory, bool overwrite) {
    check_run_directory_reusable(directory, overwrite);
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        std::filesystem::create_directories(directory, error);
        if (error) throw ArtifactError("cannot create directory " + directory.string() + ": " + error.message());
        return;
    }
    for (const auto optional_name : {kExecutionTraceFileName, std::string_view("provenance.json")}) {
        std::filesystem::remove(directory / optional_name, error);
        if (error) throw ArtifactError("cannot remove optional artifact: " + error.message());
        std::filesystem::remove(directory / (std::string(optional_name) + ".tmp"), error);
        if (error) throw ArtifactError("cannot remove optional artifact temporary: " + error.message());
    }
    for (const std::string_view artifact_name : kRunArtifactFileNames) {
        std::filesystem::path artifact_path = directory / artifact_name;
        std::filesystem::path temporary_path = artifact_path;
        temporary_path += kAtomicWriteSuffix;
        for (const std::filesystem::path& path : {artifact_path, temporary_path}) {
            std::filesystem::remove(path, error);
            if (error) throw ArtifactError("cannot remove " + path.string() + ": " + error.message());
        }
    }
}

void write_file_atomically(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::path temporary = path;
    temporary += kAtomicWriteSuffix;
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw ArtifactError("cannot create " + temporary.string());
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) throw ArtifactError("cannot write " + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) throw ArtifactError("cannot rename " + temporary.string() + ": " + error.message());
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ArtifactError("cannot open " + path.string());
    std::ostringstream text;
    text << stream.rdbuf();
    if (stream.bad()) throw ArtifactError("cannot read " + path.string());
    return text.str();
}

std::string render_manifest_json(const SimulationCore& core, const StrategyIdentity& strategy, const RunArtifactOptions& options) {
    const ExperimentConfig& config = core.config();
    JsonWriter json;
    json.begin_object();
    json.key("schema").string(kManifestSchema);
    json.key("artifact_schema_version").integer_text(kArtifactSchemaVersion);
    json.key("run_id").string(options.run_id);
    json.key("parent_run_id");
    if (options.parent_run_id.empty()) json.null();
    else json.string(options.parent_run_id);
    json.key("mode").string("deterministic_research");
    json.key("experiment").begin_object().key("name").string(config.name).key("description").string(config.description).end_object();

    json.key("market_world").string("synthetic_interactive");
    json.key("market_model").begin_object();
    json.key("description")
        .string("Seeded background agents (market makers, noise takers) trade in the same order book as the research "
                "strategy. Orders consume liquidity and move the simulated book; agents react only through their "
                "declared models. A controlled synthetic experiment, not calibrated real-world market impact.");
    json.key("background_agent_latency").string("zero: agents act at their timers and observe every engine event batch synchronously");
    json.key("background_agent_count").integer_text(config.agents.size());
    json.end_object();

    json.key("config_hash").string(format_hex64(experiment_config_hash(config)));
    json.key("config_hash_algorithm").string("fnv1a64 of the canonical configuration text (exsim.research.config/1); master seed excluded");
    json.key("exchange_session").begin_object();
    json.key("order_entry_protocol").string(server::to_string(config.order_entry_protocol));
    json.key("market_data_protocol").string("itch50_moldudp64");
    json.key("transport").string("private_unix_domain_stream_with_datagram_envelopes");
    json.key("cancel_on_disconnect").boolean(false);
    json.key("max_request_tokens").integer_text(65535);
    json.key("recovery_delivery").string("Recovered reports wait for their original scheduled delivery; ITCH recovery requests never extend beyond the delivered feed head.");
    json.end_object();
    json.key("data_hash").null();
    json.key("data_hash_note").string("synthetic market world: the run reads no input data");
    json.key("config").begin_object();
    json.key("format").string("exsim.research.config/1");
    json.key("entries");
    put_entries_object(json, canonical_config_entries(config));
    json.end_object();

    json.key("master_seed").integer_text(core.master_seed());
    json.key("seed_derivation").begin_object();
    json.key("formula").string(kSeedDerivationDescription);
    json.key("streams").begin_array();
    for (const DerivedSeed& derived : core.derived_seeds())
        json.begin_object().key("stream").string(derived.stream).key("seed").integer_text(derived.seed).end_object();
    json.end_array();
    json.end_object();

    json.key("build").begin_object();
    json.key("engine_build_id").string(options.build.engine_build_id);
    json.key("runner_build_id").string(options.build.runner_build_id);
    json.key("canonical_encoding_schema").integer_text(sim::kSchema);
    json.key("compiler").string(options.build.compiler);
    json.end_object();

    json.key("strategy").begin_object();
    json.key("kind").string(to_string(config.strategy.kind));
    json.key("name").string(strategy.name);
    json.key("version").integer_text(strategy.version);
    json.key("build_id").string(std::string(strategy.name) + "/" + std::to_string(strategy.version) + "+" +
                                options.build.runner_build_id);
    json.key("session_id").integer_text(kStrategySessionId);
    json.key("owner_id").integer_text(kStrategyOwnerId);
    json.key("parameters");
    put_entries_object(json, strategy_parameter_entries(config.strategy));
    json.end_object();

    const auto agents = core.background_agent_summaries();
    json.key("background_agents").begin_array();
    for (std::size_t index = 0; index < config.agents.size(); ++index) {
        const BackgroundAgentConfig& agent = config.agents[index];
        json.begin_object();
        json.key("index").integer_text(index);
        json.key("name").string(agent.name);
        json.key("kind").string(to_string(agent.kind));
        json.key("session_id").integer_text(background_agent_session(index));
        json.key("seed_stream").string("agent." + std::to_string(index) + "." + std::string(to_string(agent.kind)) + "." + agent.name);
        json.key("seed").integer_text(index < agents.size() ? agents[index].seed : 0);
        json.key("parameters");
        put_entries_object(json, background_agent_parameter_entries(agent));
        json.end_object();
    }
    json.end_array();

    const InstrumentConfig& instrument = config.instrument;
    json.key("instrument").begin_object();
    json.key("instrument_id").integer_text(core.instrument_id());
    json.key("symbol").string(instrument.symbol.view());
    json.key("tick_size").price_text(instrument.tick_size);
    json.key("lot_size").integer_text(instrument.lot_size);
    json.key("max_order_qty").integer_text(instrument.max_order_qty);
    json.key("min_price").price_text(instrument.min_price);
    json.key("max_price").price_text(instrument.max_price);
    json.key("reference_price").price_text(instrument.reference_price);
    json.key("initial_phase").string(exsim::to_string(instrument.initial_phase));
    json.key("engine_max_orders").integer_text(config.engine.max_orders);
    json.end_object();

    const LatencyModel& latency = config.latency;
    json.key("latency_model").begin_object();
    json.key("units").string("nanoseconds of simulated time");
    json.key("feed_ns").integer_text(latency.feed_ns);
    json.key("order_ns").integer_text(latency.order_ns);
    json.key("cancel_ns").integer_text(latency.cancel_ns);
    json.key("ack_ns").integer_text(latency.ack_ns);
    json.key("strategy_compute_ns").integer_text(latency.strategy_compute_ns);
    json.key("feed_jitter_ns").integer_text(latency.feed_jitter_ns);
    json.key("order_jitter_ns").integer_text(latency.order_jitter_ns);
    json.key("cancel_jitter_ns").integer_text(latency.cancel_jitter_ns);
    json.key("ack_jitter_ns").integer_text(latency.ack_jitter_ns);
    json.key("order_channel_fifo").boolean(latency.order_channel_fifo);
    json.key("jitter_distribution").string("uniform integer in [0, jitter_ns], drawn from the latency seed stream");
    json.key("channel_ordering")
        .string("feed and report channels are FIFO (delivery time never decreases); the order channel is FIFO when "
                "order_channel_fifo is true, otherwise each request arrives after its own latency");
    json.end_object();

    json.key("queue_model").begin_object();
    json.key("kind").string("exact_engine_price_time_fifo");
    json.key("description")
        .string("Strategy orders rest in the real matching engine beside background orders; queue position is engine "
                "truth (recorded in lifecycle.jsonl), never an estimate.");
    json.end_object();

    json.key("fee_schedule").begin_object();
    json.key("units").string("currency per share; negative is a rebate");
    json.key("maker_fee_per_share").price_text(config.fees.maker_fee_per_share);
    json.key("taker_fee_per_share").price_text(config.fees.taker_fee_per_share);
    json.key("auction_fee_per_share").price_text(config.fees.auction_fee_per_share);
    json.key("fee_rule").string("fee = fee_per_share * filled qty, charged per fill leg by liquidity (maker, taker, auction)");
    json.end_object();

    json.key("risk_limits").begin_object();
    json.key("max_long_position").signed_integer_text(config.risk.max_long_position);
    json.key("max_short_position").signed_integer_text(config.risk.max_short_position);
    json.key("max_order_qty").integer_text(config.risk.max_order_qty);
    json.key("max_open_orders").integer_text(config.risk.max_open_orders);
    json.key("check")
        .string("before a request leaves the strategy: known position plus the worst-case remaining quantity of every "
                "live same-side order plus the new quantity must stay within the side's limit");
    json.end_object();

    json.key("accounting").begin_object();
    json.key("price_scale").integer_text(static_cast<std::uint64_t>(kPriceScale));
    json.key("money_units").string("decimal currency strings; integer price units are 1/10000 currency");
    json.key("initial_cash").price_text(config.accounting.initial_cash);
    json.key("initial_position").signed_integer_text(0);
    json.key("mark_convention").string(to_string(config.accounting.mark));
    json.key("mark_definition")
        .string(config.accounting.mark == MarkConvention::ExchangeMid
                    ? "(best_bid + best_ask) / 2 of the last two-sided engine-truth book at or before the mark time, "
                      "exact to half a price unit; a one-sided book does not clear it; unavailable before the first "
                      "two-sided book"
                    : "engine last trade price; unavailable before the first trade");
    json.key("cost_basis").string("FIFO lots");
    json.key("realized_pnl").string("gross of fees, FIFO lot matching");
    json.key("equity").string("cash + position * mark (fees already deducted from cash), exact to half a price unit; "
                              "unavailable without a mark unless flat");
    json.key("arithmetic").string("checked 64-bit integers in price units; floating point only in derived summary statistics");
    json.end_object();

    json.key("metrics").begin_object();
    json.key("markout_horizons_ns").begin_array();
    for (std::size_t horizon = 0; horizon < config.metrics.markout_horizon_count; ++horizon)
        json.integer_text(config.metrics.markout_horizons_ns[horizon]);
    json.end_array();
    json.key("markout_reference").string("reference_exchange_mid_at_horizon");
    json.key("spread_capture_reference").string("reference_exchange_mid_before_the_filling_command");
    json.key("execution_benchmark").string("reference_exchange_mid_at_decision");
    json.key("reference_mid_definition")
        .string("last two-sided engine-truth (best_bid + best_ask) / 2 at or before the time, carried across "
                "one-sided books, exact to half a price unit; unavailable before the first two-sided book");
    json.key("sample_interval_ns").integer_text(config.metrics.sample_interval_ns);
    json.end_object();

    const RunnerLimits& limits = config.limits;
    json.key("simulation").begin_object();
    json.key("start_time_ns").integer_text(0);
    json.key("end_time_ns").integer_text(limits.duration_ns);
    json.key("max_simulation_events").integer_text(limits.max_simulation_events);
    json.key("simulation_queue_capacity").integer_text(limits.simulation_queue_capacity);
    json.key("observation_queue_capacity").integer_text(limits.observation_queue_capacity);
    json.key("recorder_command_capacity").integer_text(limits.recorder_command_capacity);
    json.key("recorder_record_capacity").integer_text(limits.recorder_record_capacity);
    json.key("recorder_flush_margin").integer_text(limits.recorder_flush_margin);
    json.key("recorder_effective_flush_margin").integer_text(effective_recorder_flush_margin(config));
    json.key("successful_completion")
        .string("every simulation event with time <= end_time_ns processed (end_time_reached), or no events left "
                "(event_queue_exhausted)");
    json.key("time_limit").string("events after end_time_ns are never processed; exceeding max_simulation_events fails the run");
    json.end_object();

    json.key("event_order_policy").string(kEventOrderPolicy);

    json.key("sessions").begin_object();
    json.key("strategy_session_id").integer_text(kStrategySessionId);
    json.key("background_agent_session_ids").string("2 + agent index");
    json.key("scripted_session_ids").string("any other session id (recorded interventions)");
    json.end_object();

    json.key("files").begin_array();
    put_file_description(json, "manifest.json", "json", kManifestSchema, "this file; immutable once written");
    put_file_description(json, "experiment.conf", "ini", "exsim.research.experiment/1", "the experiment file exactly as given");
    put_file_description(json, "status.json", "json", kStatusSchema, "run state and wall-clock progress; not deterministic");
    put_file_description(json, "commands.journal", "binary", "exsim.sim.journal/1",
                         "canonical command journal; `exsim_replay commands.journal` prints the event digest");
    put_file_description(json, "commands.jsonl", "jsonl", "exsim.research.command/1", "every engine command in processing order");
    put_file_description(json, "events.jsonl", "jsonl", "exsim.research.event/1",
                         "every engine event, regenerated by an independent replay of commands");
    put_file_description(json, "lifecycle.jsonl", "jsonl", "exsim.research.lifecycle/1",
                         "strategy order lifecycle with engine-truth FIFO queue evidence");
    put_file_description(json, "ledger.jsonl", "jsonl", "exsim.research.ledger/1", "strategy fills with running accounting");
    put_file_description(json, "markouts.jsonl", "jsonl", "exsim.research.markout/1", "per fill and horizon markouts");
    put_file_description(json, "series.csv", "csv", "exsim.research.series/1", "sampled book, inventory and equity series");
    put_file_description(json, "summary.json", "json", kSummarySchema, "end-of-run accounting and metrics");
    json.end_array();

    json.end_object();
    std::string text = json.take();
    text += '\n';
    return text;
}

struct RunArtifactWriter::ReplayState {
    explicit ReplayState(const ExperimentConfig& config)
        : engine(research_engine_config(config)), output(required_event_capacity(research_engine_config(config))) {
        if (engine.add_instrument(config.instrument) != kResearchInstrumentId)
            throw ArtifactError("replay engine rejected the instrument");
    }
    MatchingEngine engine;
    EventBuffer output;
    std::uint64_t event_digest = 14695981039346656037ull;
    std::uint64_t event_count = 0;
    std::uint64_t command_count = 0;
    bool failed = false;
};

RunArtifactWriter::RunArtifactWriter(const SimulationCore& core, StrategyIdentity strategy, RunArtifactOptions options)
    : core_(core), strategy_(strategy), options_(std::move(options)) {}

RunArtifactWriter::~RunArtifactWriter() = default;

void RunArtifactWriter::begin() {
    if (begun_) throw ArtifactError("artifact writer already started");
    wall_started_ = std::chrono::system_clock::now();
    steady_started_ = std::chrono::steady_clock::now();
    prepare_run_directory(options_.run_directory, options_.overwrite);
    const auto& directory = options_.run_directory;
    write_file_atomically(directory / "manifest.json", render_manifest_json(core_, strategy_, options_));
    write_file_atomically(directory / "experiment.conf", options_.config_source_text);

    sim::JournalSetup setup;
    setup.engine = research_engine_config(core_.config());
    setup.instruments.push_back(core_.config().instrument);
    journal_ = std::make_unique<sim::JournalWriter>((directory / "commands.journal").string(), setup);
    replay_ = std::make_unique<ReplayState>(core_.config());

    commands_jsonl_ = open_stream(directory / "commands.jsonl");
    events_jsonl_ = open_stream(directory / "events.jsonl");
    lifecycle_jsonl_ = open_stream(directory / "lifecycle.jsonl");
    ledger_jsonl_ = open_stream(directory / "ledger.jsonl");
    markouts_jsonl_ = open_stream(directory / "markouts.jsonl");
    series_csv_ = open_stream(directory / "series.csv", kSeriesHeader);
    decisions_jsonl_ = open_stream(directory / kExecutionTraceFileName);
    begun_ = true;
    write_status();
}

void RunArtifactWriter::drain(RunRecorder& recorder) {
    if (!begun_ || finished_) throw ArtifactError("artifact writer is not running");
    write_command_records(recorder);
    write_lifecycle_records(recorder);
    write_fill_records(recorder);
    write_markout_records(recorder);
    write_series_samples(recorder);
    write_execution_decisions(recorder);
    recorder.clear();
    check_streams();
}

void RunArtifactWriter::check_streams() {
    const bool good = commands_jsonl_.good() && events_jsonl_.good() && lifecycle_jsonl_.good() && ledger_jsonl_.good() &&
                      markouts_jsonl_.good() && series_csv_.good() && decisions_jsonl_.good();
    if (!good) throw ArtifactError("write failed in " + options_.run_directory.string());
}

void RunArtifactWriter::write_command_records(const RunRecorder& recorder) {
    JsonWriter json;
    for (const CommandRecord& record : recorder.commands.records()) {
        if (!journal_->append(record.command)) throw ArtifactError("command journal write failed");
        ++commands_written_;

        json.clear();
        json.begin_object();
        json.key("command_index").integer_text(record.command_index);
        json.key("time_ns").integer_text(record.time);
        json.key("source").string(to_string(record.source));
        json.key("agent_index");
        if (record.source == CommandSource::BackgroundAgent) json.integer_text(record.agent_index);
        else json.null();
        json.key("command").string(exsim::to_string(record.command.type));
        sim::visit_fields(record.command, [&](const sim::Field& field) { put_engine_field(json, field); });
        json.end_object();
        commands_jsonl_ << json.text() << '\n';

        // Independent replay: a second engine fed the recorded commands
        // regenerates the event stream, which must match the online digest.
        ReplayState& replay = *replay_;
        replay.output.clear();
        if (replay.engine.handle(record.command, replay.output) != HandleResult::Applied) {
            replay.failed = true;
            continue;
        }
        ++replay.command_count;
        std::uint64_t event_index_in_command = 0;
        for (const Event& event : replay.output) {
            replay.event_digest = sim::hash_event(event, replay.event_digest);
            json.clear();
            json.begin_object();
            json.key("event_index").integer_text(replay.event_count);
            json.key("command_index").integer_text(record.command_index);
            json.key("index_in_command").integer_text(event_index_in_command);
            json.key("event").string(sim::event_type_name(event.type));
            sim::visit_fields(event, [&](const sim::Field& field) { put_engine_field(json, field); });
            json.end_object();
            events_jsonl_ << json.text() << '\n';
            ++replay.event_count;
            ++event_index_in_command;
        }
    }
}

void RunArtifactWriter::write_lifecycle_records(const RunRecorder& recorder) {
    JsonWriter json;
    for (const LifecycleRecord& record : recorder.lifecycle.records()) {
        json.clear();
        json.begin_object();
        json.key("time_ns").integer_text(record.time);
        json.key("stage").string(to_string(record.stage));
        json.key("cl_ord_id").integer_text(record.cl_ord_id);
        json.key("command_index");
        if (record.command_index == kNoCommandIndex) json.null();
        else json.integer_text(record.command_index);
        const bool request_stage = record.stage == LifecycleStage::StrategySubmitted ||
                                   record.stage == LifecycleStage::RiskRejected ||
                                   record.stage == LifecycleStage::ExchangeArrival;
        json.key("request");
        if (request_stage) json.string(exsim::to_string(record.request));
        else json.null();
        json.key("reference_time_ns");
        if (record.stage == LifecycleStage::StrategySubmitted || record.stage == LifecycleStage::ExchangeArrival ||
            record.stage == LifecycleStage::ReportDelivered)
            json.integer_text(record.reference_time);
        else
            json.null();
        json.key("order_id").integer_text(record.order_id);
        json.key("side").string(exsim::to_string(record.side));
        json.key("price").price_text(record.price);
        json.key("qty").integer_text(record.qty);
        json.key("leaves_qty").integer_text(record.leaves_qty);
        json.key("cum_qty").integer_text(record.cum_qty);
        json.key("tif");
        if (record.stage == LifecycleStage::StrategySubmitted || record.stage == LifecycleStage::ExchangeArrival)
            json.string(exsim::to_string(record.tif));
        else
            json.null();
        json.key("post_only").boolean(record.post_only);
        json.key("report_kind");
        if (record.stage == LifecycleStage::ReportDelivered) json.string(to_string(record.report_kind));
        else json.null();
        const bool is_fill = record.stage == LifecycleStage::ExchangeFill ||
                             (record.stage == LifecycleStage::ReportDelivered && record.report_kind == ReportKind::Fill);
        json.key("match_id");
        if (is_fill) json.integer_text(record.match_id);
        else json.null();
        json.key("liquidity");
        if (is_fill) json.string(to_string(record.liquidity));
        else json.null();
        json.key("fee");
        if (record.stage == LifecycleStage::ExchangeFill) json.price_text(record.fee);
        else json.null();
        const bool is_reject = record.stage == LifecycleStage::ExchangeRejected ||
                               (record.stage == LifecycleStage::ReportDelivered && record.report_kind == ReportKind::Rejected);
        json.key("reject_reason");
        if (is_reject) json.string(exsim::to_string(record.reject_reason));
        else json.null();
        const bool is_cancel = record.stage == LifecycleStage::ExchangeCanceled ||
                               (record.stage == LifecycleStage::ReportDelivered && record.report_kind == ReportKind::Canceled);
        json.key("cancel_reason");
        if (is_cancel) json.string(exsim::to_string(record.cancel_reason));
        else json.null();
        json.key("risk_reason");
        if (record.stage == LifecycleStage::RiskRejected) json.string(to_string(record.risk_reason));
        else json.null();
        const bool is_replace = record.stage == LifecycleStage::ExchangeReplaced ||
                                (record.stage == LifecycleStage::ReportDelivered && record.report_kind == ReportKind::Replaced);
        json.key("priority_kept");
        if (is_replace) json.boolean(record.priority_kept);
        else json.null();
        json.key("queue");
        if (record.has_queue_position) {
            json.begin_object();
            json.key("source").string("engine_truth");
            json.key("orders_ahead").integer_text(record.queue_orders_ahead);
            json.key("qty_ahead").integer_text(record.queue_ahead_qty);
            json.key("level_qty").integer_text(record.level_qty);
            json.end_object();
        } else {
            json.null();
        }
        json.end_object();
        lifecycle_jsonl_ << json.text() << '\n';
    }
}

void RunArtifactWriter::write_fill_records(const RunRecorder& recorder) {
    JsonWriter json;
    for (const FillRecord& record : recorder.fills.records()) {
        json.clear();
        json.begin_object();
        json.key("fill_index").integer_text(record.fill_index);
        json.key("time_ns").integer_text(record.exchange_time);
        json.key("command_index").integer_text(record.command_index);
        json.key("match_id").integer_text(record.match_id);
        json.key("cl_ord_id").integer_text(record.cl_ord_id);
        json.key("order_id").integer_text(record.order_id);
        json.key("side").string(exsim::to_string(record.side));
        json.key("liquidity").string(to_string(record.liquidity));
        json.key("price").price_text(record.price);
        json.key("qty").integer_text(record.qty);
        json.key("fee_per_share").price_text(record.fee_per_share);
        json.key("fee").price_text(record.fee);
        json.key("cash_after").price_text(record.cash_after);
        json.key("position_after").signed_integer_text(record.position_after);
        json.key("realized_pnl_after").price_text(record.realized_pnl_after);
        json.key("fees_after").price_text(record.fees_after);
        json.key("mid_before");
        if (record.has_mid_before) json.doubled_price_text(record.mid_before_times_two);
        else json.null();
        json.key("spread_capture");
        if (record.has_mid_before) json.doubled_price_text(record.spread_capture_times_two);
        else json.null();
        json.end_object();
        ledger_jsonl_ << json.text() << '\n';
    }
}

void RunArtifactWriter::write_markout_records(const RunRecorder& recorder) {
    JsonWriter json;
    for (const MarkoutRecord& record : recorder.markouts.records()) {
        json.clear();
        json.begin_object();
        json.key("fill_index").integer_text(record.fill_index);
        json.key("fill_time_ns").integer_text(record.fill_time);
        json.key("horizon_ns").integer_text(record.horizon_ns);
        json.key("status").string(to_string(record.status));
        json.key("side").string(exsim::to_string(record.side));
        json.key("price").price_text(record.price);
        json.key("qty").integer_text(record.qty);
        json.key("mid");
        if (record.status == MarkoutStatus::Measured) json.doubled_price_text(record.mid_times_two);
        else json.null();
        json.key("markout");
        if (record.status == MarkoutStatus::Measured) json.doubled_price_text(record.markout_times_two);
        else json.null();
        json.end_object();
        markouts_jsonl_ << json.text() << '\n';
    }
}

void RunArtifactWriter::write_series_samples(const RunRecorder& recorder) {
    for (const SeriesSample& sample : recorder.samples.records()) {
        series_line_buffer_.clear();
        append_csv_integer(series_line_buffer_, sample.time);
        append_csv_price(series_line_buffer_, sample.exchange.has_bid, sample.exchange.bid_price);
        append_csv_integer(series_line_buffer_, sample.exchange.has_bid ? sample.exchange.bid_qty : 0);
        append_csv_price(series_line_buffer_, sample.exchange.has_ask, sample.exchange.ask_price);
        append_csv_integer(series_line_buffer_, sample.exchange.has_ask ? sample.exchange.ask_qty : 0);
        append_csv_price(series_line_buffer_, sample.visible.has_bid, sample.visible.bid_price);
        append_csv_integer(series_line_buffer_, sample.visible.has_bid ? sample.visible.bid_qty : 0);
        append_csv_price(series_line_buffer_, sample.visible.has_ask, sample.visible.ask_price);
        append_csv_integer(series_line_buffer_, sample.visible.has_ask ? sample.visible.ask_qty : 0);
        append_csv_signed(series_line_buffer_, sample.position);
        append_csv_signed(series_line_buffer_, sample.known_position);
        append_csv_price(series_line_buffer_, true, sample.cash);
        append_csv_price(series_line_buffer_, true, sample.realized_pnl);
        append_csv_price(series_line_buffer_, true, sample.fees);
        append_csv_doubled_price(series_line_buffer_, sample.has_mark, sample.mark_times_two);
        append_csv_doubled_price(series_line_buffer_, sample.has_equity, sample.unrealized_pnl_times_two);
        append_csv_doubled_price(series_line_buffer_, sample.has_equity, sample.equity_times_two);
        append_csv_integer(series_line_buffer_, sample.open_bid_qty);
        series_line_buffer_ += std::to_string(sample.open_ask_qty);
        series_line_buffer_ += '\n';
        series_csv_ << series_line_buffer_;
    }
}

void RunArtifactWriter::write_status(bool driver_active) {
    if (!begun_) return;
    const RunState state = driver_active && core_.state() == RunState::Paused ? RunState::Running : core_.state();
    const auto wall_now = std::chrono::system_clock::now();
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - steady_started_).count();
    const Timestamp end_time = core_.end_time();
    JsonWriter json;
    json.begin_object();
    json.key("schema").string(kStatusSchema);
    json.key("run_id").string(options_.run_id);
    json.key("state").string(to_string(state));
    json.key("termination_reason").string(to_string(core_.termination()));
    json.key("failure_detail").integer_text(core_.failure_detail());
    json.key("sim_time_ns").integer_text(core_.now());
    json.key("end_time_ns").integer_text(end_time);
    json.key("progress").number(end_time == 0 ? 1.0 : static_cast<double>(core_.now()) / static_cast<double>(end_time));
    json.key("simulation_events").integer_text(core_.simulation_events_processed());
    json.key("commands_written").integer_text(commands_written_);
    json.key("summary_written").boolean(finished_);
    json.key("replay_verified");
    if (finished_) json.boolean(replay_verified_);
    else json.null();
    json.key("wall_clock_started_utc").string(utc_timestamp(wall_started_));
    json.key("wall_clock_updated_utc").string(utc_timestamp(wall_now));
    json.key("wall_elapsed_seconds").number(elapsed_seconds);
    json.key("process_id").integer_text(static_cast<std::uint64_t>(::getpid()));
    json.end_object();
    std::string text = json.take();
    text += '\n';
    write_file_atomically(options_.run_directory / "status.json", text);
}

void RunArtifactWriter::finish(RunRecorder& recorder) {
    if (!begun_ || finished_) throw ArtifactError("artifact writer is not running");
    drain(recorder);
    if (!journal_->flush()) throw ArtifactError("command journal flush failed");
    journal_.reset();  // writes the journal footer
    commands_jsonl_.close();
    events_jsonl_.close();
    lifecycle_jsonl_.close();
    ledger_jsonl_.close();
    markouts_jsonl_.close();
    series_csv_.close();
    decisions_jsonl_.close();
    if (commands_jsonl_.fail() || events_jsonl_.fail() || lifecycle_jsonl_.fail() || ledger_jsonl_.fail() ||
        markouts_jsonl_.fail() || series_csv_.fail() || decisions_jsonl_.fail())
        throw ArtifactError("closing artifact streams failed in " + options_.run_directory.string());

    const RunDigests& digests = core_.digests();
    replay_event_digest_ = replay_->event_digest;
    replay_verified_ = !replay_->failed && replay_->event_digest == digests.event_digest &&
                       replay_->event_count == digests.event_count && replay_->command_count == digests.command_count;
    write_file_atomically(options_.run_directory / "summary.json",
                          render_summary_json(core_, strategy_, options_.run_id, replay_verified_));
    finished_ = true;
    write_status();
}

void RunArtifactWriter::write_execution_decisions(const RunRecorder& recorder) {
    JsonWriter json;
    for (const auto& row : recorder.decisions.records()) {
        json.clear();
        json.begin_object().key("index").integer_text(decisions_written_++);
        json.key("time_ns").integer_text(row.time).key("kind").string(row.report ? "child_report" : "decision");
        json.key("target_qty").integer_text(row.target).key("known_filled_qty").integer_text(row.filled);
        json.key("requested_qty").integer_text(row.requested).key("outstanding_qty").integer_text(row.outstanding);
        json.key("headroom").signed_integer_text(row.headroom).key("child_cap").integer_text(row.child_cap);
        json.key("submitted_qty").integer_text(row.submitted).key("child_client_order_id").integer_text(row.child);
        json.key("source_time_ns").integer_text(row.source_time).key("source_sequence").integer_text(row.source_sequence);
        json.key("has_touch").boolean(row.has_touch).key("touch").signed_integer_text(row.touch).key("limit").signed_integer_text(row.limit);
        json.key("reason").string(row.reason);
        json.key("exchange_time_ns").integer_text(row.result.exchange_time).key("order_id").integer_text(row.result.order_id);
        json.key("match_id").integer_text(row.result.match_id).key("report_qty").integer_text(row.result.qty);
        json.key("report_leaves_qty").integer_text(row.result.leaves_qty).key("report_cum_qty").integer_text(row.result.cum_qty);
        json.key("reject_reason").string(to_string(row.result.reject_reason)).key("cancel_reason").string(to_string(row.result.cancel_reason));
        json.end_object();
        decisions_jsonl_ << json.text() << '\n';
    }
}

}  // namespace exsim::research
