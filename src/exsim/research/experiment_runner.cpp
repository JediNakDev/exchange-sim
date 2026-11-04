#include "exsim/research/experiment_runner.hpp"

#include "exsim/research/json.hpp"
#include "exsim/research/simulation.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/strategies/immediate_sweep_executor.hpp"
#include "exsim/research/strategies/inventory_aware_market_maker.hpp"
#include "exsim/research/strategies/twap_executor.hpp"

#include <charconv>
#include <chrono>
#include <memory>
#include <system_error>

namespace exsim::research {

namespace {

constexpr auto kStatusInterval = std::chrono::milliseconds(500);

template <typename Strategy>
RunOutcome run_strategy(const RunRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    auto simulation = std::make_unique<Simulation<Strategy>>(request.config, request.master_seed, request.config.strategy);

    RunArtifactOptions options;
    options.run_directory = request.run_directory;
    options.run_id = make_run_id(simulation->config().name, experiment_config_hash(simulation->config()), request.master_seed);
    options.parent_run_id = request.parent_run_id;
    options.config_source_text = request.config_source_text;
    options.build = request.build;
    options.overwrite = request.overwrite;
    RunArtifactWriter writer(*simulation, StrategyIdentity{Strategy::kStrategyName, Strategy::kStrategyVersion}, options);
    writer.begin();

    auto last_status = std::chrono::steady_clock::now();
    const RunBounds chunk{kNoTime, request.steps_per_chunk == 0 ? 1 : request.steps_per_chunk};
    while (true) {
        if (request.cancel_requested != nullptr && request.cancel_requested->load(std::memory_order_relaxed))
            simulation->request_stop();
        const RunStop stop = simulation->run(chunk);
        if (stop == RunStop::FlushRequired) {
            writer.drain(simulation->recorder());
            writer.write_status(true);
            last_status = std::chrono::steady_clock::now();
            continue;
        }
        if (stop == RunStop::StepBudgetExhausted || stop == RunStop::PausedAtTime) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_status >= kStatusInterval) {
                writer.write_status(true);
                last_status = now;
            }
            continue;
        }
        break;  // Completed, Failed or Cancelled
    }
    writer.finish(simulation->recorder());

    RunOutcome outcome;
    outcome.run_id = writer.run_id();
    outcome.master_seed = request.master_seed;
    outcome.run_directory = request.run_directory;
    outcome.state = simulation->state();
    outcome.termination = simulation->termination();
    outcome.replay_verified = writer.replay_verified();
    outcome.digests = simulation->digests();
    outcome.simulation_events = simulation->simulation_events_processed();
    outcome.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return outcome;
}

void prepare_sweep_directory(const std::filesystem::path& directory, bool overwrite) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        std::filesystem::create_directories(directory, error);
        if (error) throw ArtifactError("cannot create directory " + directory.string() + ": " + error.message());
        return;
    }
    if (!std::filesystem::is_directory(directory, error)) throw ArtifactError(directory.string() + " is not a directory");
    bool has_runs = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        const bool known = name == "sweep.json" || name == "sweep.json.tmp" || (entry.is_directory() && name.rfind("seed-", 0) == 0);
        if (!known) throw ArtifactError(directory.string() + " contains " + name + ", which is not part of a sweep; refusing to use it");
        has_runs = true;
    }
    if (error) throw ArtifactError("cannot list " + directory.string() + ": " + error.message());
    if (has_runs && !overwrite) throw ArtifactError(directory.string() + " already holds a sweep; pass --overwrite to replace it");
    std::filesystem::remove(directory / "sweep.json", error);
}

std::string render_sweep_json(const SweepRequest& request, const std::vector<RunOutcome>& outcomes) {
    ExperimentConfig validated = request.config;
    validate_experiment_config(validated);
    JsonWriter json;
    json.begin_object();
    json.key("schema").string(kSweepSchema);
    json.key("experiment").string(validated.name);
    json.key("config_hash").string(format_hex64(experiment_config_hash(validated)));
    json.key("strategy_kind").string(to_string(validated.strategy.kind));
    json.key("parent_run_id");
    if (request.parent_run_id.empty()) json.null();
    else json.string(request.parent_run_id);
    json.key("requested_seeds").begin_array();
    for (const std::uint64_t seed : request.master_seeds) json.integer_text(seed);
    json.end_array();
    json.key("runs").begin_array();
    for (const RunOutcome& outcome : outcomes) {
        json.begin_object();
        json.key("master_seed").integer_text(outcome.master_seed);
        json.key("run_id").string(outcome.run_id);
        json.key("directory").string(outcome.run_directory.filename().string());
        json.key("state").string(to_string(outcome.state));
        json.key("termination_reason").string(to_string(outcome.termination));
        json.key("replay_verified").boolean(outcome.replay_verified);
        json.key("event_digest").integer_text(outcome.digests.event_digest);
        json.key("ledger_digest").integer_text(outcome.digests.ledger_digest);
        json.end_object();
    }
    json.end_array();
    json.key("complete").boolean(outcomes.size() == request.master_seeds.size());
    json.end_object();
    std::string text = json.take();
    text += '\n';
    return text;
}

}  // namespace

RunOutcome execute_run(const RunRequest& request) {
    ExperimentConfig validated = request.config;
    validate_experiment_config(validated);
    switch (validated.strategy.kind) {
        case StrategyKind::FixedSpreadMarketMaker: return run_strategy<strategies::FixedSpreadMarketMaker>(request);
        case StrategyKind::InventoryAwareMarketMaker: return run_strategy<strategies::InventoryAwareMarketMaker>(request);
        case StrategyKind::TwapExecutor: return run_strategy<strategies::TwapExecutor>(request);
        case StrategyKind::ImmediateSweepExecutor: return run_strategy<strategies::ImmediateSweepExecutor>(request);
    }
    throw ConfigError(0, "unknown strategy kind");
}

std::vector<RunOutcome> execute_sweep(const SweepRequest& request) {
    if (request.master_seeds.empty()) throw ConfigError(0, "a sweep needs at least one seed");
    prepare_sweep_directory(request.sweep_directory, request.overwrite);
    std::vector<RunOutcome> outcomes;
    for (const std::uint64_t seed : request.master_seeds) {
        if (request.cancel_requested != nullptr && request.cancel_requested->load(std::memory_order_relaxed)) break;
        RunRequest run;
        run.config = request.config;
        run.config_source_text = request.config_source_text;
        run.master_seed = seed;
        run.run_directory = request.sweep_directory / ("seed-" + std::to_string(seed));
        run.parent_run_id = request.parent_run_id;
        run.build = request.build;
        run.overwrite = request.overwrite;
        run.cancel_requested = request.cancel_requested;
        outcomes.push_back(execute_run(run));
        write_file_atomically(request.sweep_directory / "sweep.json", render_sweep_json(request, outcomes));
        if (request.on_run_finished) request.on_run_finished(outcomes.back());
    }
    write_file_atomically(request.sweep_directory / "sweep.json", render_sweep_json(request, outcomes));
    return outcomes;
}

bool parse_seed_range(std::string_view text, std::vector<std::uint64_t>& seeds) {
    seeds.clear();
    const auto parse_number = [](std::string_view digits, std::uint64_t& value) {
        if (digits.empty()) return false;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        return result.ec == std::errc() && result.ptr == digits.data() + digits.size();
    };
    const std::size_t separator = text.find("..");
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (separator == std::string_view::npos) {
        if (!parse_number(text, first)) return false;
        last = first;
    } else if (!parse_number(text.substr(0, separator), first) || !parse_number(text.substr(separator + 2), last)) {
        return false;
    }
    if (last < first || last - first >= 100'000) return false;
    for (std::uint64_t seed = first;; ++seed) {
        seeds.push_back(seed);
        if (seed == last) break;
    }
    return true;
}

}  // namespace exsim::research
