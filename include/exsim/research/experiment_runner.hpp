#ifndef EXSIM_RESEARCH_EXPERIMENT_RUNNER_HPP
#define EXSIM_RESEARCH_EXPERIMENT_RUNNER_HPP

// Drives one bounded research run, or a seed sweep, to completion and writes
// its artifacts. Selects the compiled strategy from the configuration (one
// switch at setup; the run loop itself is a template, no virtual dispatch).
//
// Single run:  <out>/            (manifest.json, ..., summary.json)
// Seed sweep:  <out>/sweep.json and <out>/seed-<N>/ per master seed.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/simulation_core.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::research {

inline constexpr std::string_view kSweepSchema = "exsim.research.sweep/1";

struct RunRequest {
    ExperimentConfig config;
    std::string config_source_text;  // copied to experiment.conf
    std::uint64_t master_seed = 1;
    std::filesystem::path run_directory;
    std::string parent_run_id;
    BuildInfo build;
    bool overwrite = false;
    // Checked between step chunks; a true value cancels the run (state cancelled,
    // artifacts and diagnostics still written).
    const std::atomic<bool>* cancel_requested = nullptr;
    std::uint64_t steps_per_chunk = 1u << 16;
};

struct RunOutcome {
    std::string run_id;
    std::uint64_t master_seed = 0;
    std::filesystem::path run_directory;
    RunState state = RunState::Queued;
    TerminationReason termination = TerminationReason::None;
    bool replay_verified = false;
    RunDigests digests;
    std::uint64_t simulation_events = 0;
    double wall_seconds = 0;
};

// Throws ConfigError, ArtifactError, or std::invalid_argument for a config the engine refuses.
[[nodiscard]] RunOutcome execute_run(const RunRequest& request);

struct SweepRequest {
    ExperimentConfig config;
    std::string config_source_text;
    std::vector<std::uint64_t> master_seeds;
    std::filesystem::path sweep_directory;
    std::string parent_run_id;
    BuildInfo build;
    bool overwrite = false;
    const std::atomic<bool>* cancel_requested = nullptr;
    std::function<void(const RunOutcome&)> on_run_finished;  // progress reporting (optional)
};

// Runs every seed in order (stops early when cancelled) and writes sweep.json.
[[nodiscard]] std::vector<RunOutcome> execute_sweep(const SweepRequest& request);

// Parses "N" or "A..B" (inclusive, A <= B, at most 100000 seeds). False on error.
[[nodiscard]] bool parse_seed_range(std::string_view text, std::vector<std::uint64_t>& seeds);

}  // namespace exsim::research

#endif
