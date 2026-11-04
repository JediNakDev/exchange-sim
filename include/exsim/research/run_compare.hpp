#ifndef EXSIM_RESEARCH_RUN_COMPARE_HPP
#define EXSIM_RESEARCH_RUN_COMPARE_HPP

// Paired comparison of two run sets (a run directory or a sweep directory
// each). Runs are paired by master seed; only completed runs take part. For
// every metric in summary.json "metrics" the report gives the number of
// pairs, per-side means and sample standard deviations, and the paired
// difference (B - A): mean, standard deviation, standard error, min, max.
// Configuration differences between the sides are listed so a comparison
// never hides a fee/model/scenario change.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::research {

inline constexpr std::string_view kCompareSchema = "exsim.research.compare/1";

struct LoadedRun {
    std::filesystem::path directory;
    std::string run_id;
    std::uint64_t master_seed = 0;
    std::string state;
    std::string termination_reason;
    std::string market_world;
    std::string experiment_name;
    std::string strategy_name;
    std::string config_hash;
    std::vector<std::pair<std::string, std::optional<double>>> metrics;
    std::vector<std::pair<std::string, std::string>> config_entries;
};

struct RunSet {
    std::filesystem::path path;
    bool is_sweep = false;
    std::vector<LoadedRun> runs;
};

// Loads <path>/summary.json + manifest.json, or every run listed in
// <path>/sweep.json. Throws ArtifactError (missing files, bad JSON, duplicate seeds).
[[nodiscard]] RunSet load_run_set(const std::filesystem::path& path);

struct MetricComparison {
    std::string name;
    std::uint64_t pairs = 0;        // pairs with a value on both sides
    std::uint64_t unavailable = 0;  // pairs where either side is null
    double mean_a = 0;
    double mean_b = 0;
    double mean_difference = 0;
    double min_difference = 0;
    double max_difference = 0;
    std::optional<double> sd_a;               // sample standard deviation (n - 1), needs 2 pairs
    std::optional<double> sd_b;
    std::optional<double> sd_difference;
    std::optional<double> standard_error_difference;
};

struct ConfigDifference {
    std::string key;
    std::optional<std::string> value_a;
    std::optional<std::string> value_b;
};

struct ExcludedRun {
    char side = 'A';
    std::uint64_t master_seed = 0;
    std::string run_id;
    std::string state;
};

struct ComparisonReport {
    RunSet a;
    RunSet b;
    std::vector<std::uint64_t> paired_seeds;
    std::vector<std::uint64_t> unpaired_seeds_a;
    std::vector<std::uint64_t> unpaired_seeds_b;
    std::vector<ExcludedRun> excluded_runs;
    std::vector<ConfigDifference> config_differences;
    bool market_world_differs = false;
    std::vector<MetricComparison> metrics;
};

[[nodiscard]] ComparisonReport compare_run_sets(RunSet a, RunSet b);
[[nodiscard]] std::string render_comparison_text(const ComparisonReport& report);
[[nodiscard]] std::string render_comparison_json(const ComparisonReport& report);

}  // namespace exsim::research

#endif
