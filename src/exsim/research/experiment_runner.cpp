#include "exsim/research/experiment_runner.hpp"

#include "exsim/research/json.hpp"
#include "exsim/research/simulation.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/strategies/immediate_sweep_executor.hpp"
#include "exsim/research/strategies/inventory_aware_market_maker.hpp"
#include "exsim/research/strategies/twap_executor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>
#include <set>
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

std::filesystem::path seed_directory(const std::filesystem::path& sweep_directory, std::uint64_t master_seed) {
    return sweep_directory / ("seed-" + std::to_string(master_seed));
}

// Checks the whole sweep directory before anything is changed, so a refused
// overwrite leaves the previous sweep (sweep.json and every seed-N) intact.
// Returns the seed-N directories of an earlier sweep that the new seed list
// does not include; the caller removes them once the checks have passed.
std::vector<std::filesystem::path> check_sweep_directory(const SweepRequest& request) {
    const std::filesystem::path& directory = request.sweep_directory;
    std::vector<std::filesystem::path> stale_seed_directories;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) throw ArtifactError("cannot inspect " + directory.string() + ": " + error.message());
        return stale_seed_directories;
    }
    if (!std::filesystem::is_directory(directory, error)) throw ArtifactError(directory.string() + " is not a directory");
    std::set<std::string> requested_directory_names;
    for (const std::uint64_t seed : request.master_seeds)
        requested_directory_names.insert(seed_directory(directory, seed).filename().string());
    bool has_runs = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        const bool is_sweep_index = !entry.is_directory() && (name == "sweep.json" || name == "sweep.json.tmp");
        const bool is_seed_directory = entry.is_directory() && name.rfind("seed-", 0) == 0;
        if (!is_sweep_index && !is_seed_directory)
            throw ArtifactError(directory.string() + " contains " + name + ", which is not part of a sweep; refusing to use it");
        has_runs = true;
        if (is_seed_directory && requested_directory_names.count(name) == 0) stale_seed_directories.push_back(entry.path());
    }
    if (error) throw ArtifactError("cannot list " + directory.string() + ": " + error.message());
    if (has_runs && !request.overwrite)
        throw ArtifactError(directory.string() + " already holds a sweep; pass --overwrite to replace it");
    for (const std::uint64_t seed : request.master_seeds) check_run_directory_reusable(seed_directory(directory, seed), request.overwrite);
    for (const std::filesystem::path& stale : stale_seed_directories) check_run_directory_reusable(stale, true);
    std::sort(stale_seed_directories.begin(), stale_seed_directories.end());
    return stale_seed_directories;
}

void remove_stale_seed_directory(const std::filesystem::path& directory) {
    prepare_run_directory(directory, true);  // removes only run artifact files
    std::error_code error;
    std::filesystem::remove(directory, error);
    if (error) throw ArtifactError("cannot remove " + directory.string() + ": " + error.message());
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
    ExperimentConfig validated = request.config;
    validate_experiment_config(validated);  // an invalid config changes nothing on disk
    const std::vector<std::filesystem::path> stale_seed_directories = check_sweep_directory(request);
    std::error_code error;
    std::filesystem::create_directories(request.sweep_directory, error);
    if (error) throw ArtifactError("cannot create directory " + request.sweep_directory.string() + ": " + error.message());
    // Replace (never delete) the index first: from here on it describes the
    // new sweep, with no runs yet, so it never lists runs being overwritten.
    std::vector<RunOutcome> outcomes;
    write_file_atomically(request.sweep_directory / "sweep.json", render_sweep_json(request, outcomes));
    for (const std::filesystem::path& stale : stale_seed_directories) remove_stale_seed_directory(stale);
    for (const std::uint64_t seed : request.master_seeds) {
        if (request.cancel_requested != nullptr && request.cancel_requested->load(std::memory_order_relaxed)) break;
        RunRequest run;
        run.config = request.config;
        run.config_source_text = request.config_source_text;
        run.master_seed = seed;
        run.run_directory = seed_directory(request.sweep_directory, seed);
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
