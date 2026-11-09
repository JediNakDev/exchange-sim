#include "exsim/research/run_compare.hpp"

#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <limits>
#include <set>

namespace exsim::research {

namespace {

const JsonValue& require_member(const JsonValue& object, std::string_view name, const std::filesystem::path& file) {
    const JsonValue* value = object.find(name);
    if (value == nullptr) throw ArtifactError(file.string() + ": missing \"" + std::string(name) + "\"");
    return *value;
}

std::string require_string(const JsonValue& object, std::string_view name, const std::filesystem::path& file) {
    const JsonValue& value = require_member(object, name, file);
    if (value.kind != JsonValue::Kind::String) throw ArtifactError(file.string() + ": \"" + std::string(name) + "\" is not a string");
    return value.text;
}

std::uint64_t require_integer_text(const JsonValue& object, std::string_view name, const std::filesystem::path& file) {
    const std::string text = require_string(object, name, file);
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
        throw ArtifactError(file.string() + ": \"" + std::string(name) + "\" is not a decimal integer string");
    return value;
}

JsonValue parse_json_file(const std::filesystem::path& file) {
    try {
        return parse_json(read_text_file(file));
    } catch (const JsonError& error) {
        throw ArtifactError(file.string() + ": " + error.what());
    }
}

LoadedRun load_run(const std::filesystem::path& directory) {
    const std::filesystem::path summary_path = directory / "summary.json";
    const std::filesystem::path manifest_path = directory / "manifest.json";
    const JsonValue summary = parse_json_file(summary_path);
    const JsonValue manifest = parse_json_file(manifest_path);
    if (require_string(summary, "schema", summary_path) != kSummarySchema)
        throw ArtifactError(summary_path.string() + ": unsupported schema");
    if (require_string(manifest, "schema", manifest_path) != kManifestSchema)
        throw ArtifactError(manifest_path.string() + ": unsupported schema");

    LoadedRun run;
    run.directory = directory;
    run.run_id = require_string(summary, "run_id", summary_path);
    run.master_seed = require_integer_text(summary, "master_seed", summary_path);
    run.state = require_string(summary, "state", summary_path);
    run.termination_reason = require_string(summary, "termination_reason", summary_path);
    run.config_hash = require_string(summary, "config_hash", summary_path);
    if (const JsonValue* digests = summary.find("digests"); digests != nullptr && digests->kind == JsonValue::Kind::Object) {
        const JsonValue* replay_verified = digests->find("replay_verified");
        run.replay_verified = replay_verified != nullptr && replay_verified->kind == JsonValue::Kind::Boolean && replay_verified->boolean;
    }
    run.market_world = require_string(manifest, "market_world", manifest_path);
    run.experiment_name = require_string(require_member(manifest, "experiment", manifest_path), "name", manifest_path);
    run.strategy_name = require_string(require_member(manifest, "strategy", manifest_path), "name", manifest_path);

    const JsonValue& metrics = require_member(summary, "metrics", summary_path);
    if (metrics.kind != JsonValue::Kind::Object) throw ArtifactError(summary_path.string() + ": \"metrics\" is not an object");
    for (const auto& [name, value] : metrics.members) {
        if (value.kind == JsonValue::Kind::Number) run.metrics.emplace_back(name, value.number);
        else if (value.kind == JsonValue::Kind::Null) run.metrics.emplace_back(name, std::nullopt);
        else throw ArtifactError(summary_path.string() + ": metric \"" + name + "\" is not a number or null");
    }

    const JsonValue& config = require_member(manifest, "config", manifest_path);
    const JsonValue& entries = require_member(config, "entries", manifest_path);
    const auto* strategy_kind = entries.find("strategy.kind");
    if (strategy_kind && (strategy_kind->text == "twap_executor" || strategy_kind->text == "immediate_sweep_executor")) {
        run.execution_observation_status = "unavailable";
        if (entries.find("strategy.start_ns") && entries.find("strategy.execution_duration_ns")) {
            const auto start = require_integer_text(entries, "strategy.start_ns", manifest_path);
            const auto duration = require_integer_text(entries, "strategy.execution_duration_ns", manifest_path);
            if (duration > std::numeric_limits<std::uint64_t>::max() - start)
                throw ArtifactError(manifest_path.string() + ": execution window overflows");
            const auto& simulation = require_member(summary, "simulation", summary_path);
            const auto observed = require_integer_text(simulation, "final_time_ns", summary_path);
            run.execution_observation_status = observed >= start + duration ? "window_observed" : "censored_run_end";
        }
    }
    for (const auto& [key, value] : entries.members) {
        if (value.kind != JsonValue::Kind::String) throw ArtifactError(manifest_path.string() + ": config entry is not a string");
        run.config_entries.emplace_back(key, value.text);
    }
    return run;
}

struct RunningStatistic {
    std::uint64_t count = 0;
    double mean = 0;
    double sum_of_squares = 0;  // Welford
    double min = 0;
    double max = 0;

    void add(double value) {
        ++count;
        const double delta = value - mean;
        mean += delta / static_cast<double>(count);
        sum_of_squares += delta * (value - mean);
        min = count == 1 ? value : std::min(min, value);
        max = count == 1 ? value : std::max(max, value);
    }
    [[nodiscard]] std::optional<double> sample_sd() const {
        if (count < 2) return std::nullopt;
        return std::sqrt(sum_of_squares / static_cast<double>(count - 1));
    }
};

std::string number_text(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}

std::string optional_text(const std::optional<double>& value) { return value ? number_text(*value) : "n/a"; }

std::string seed_list_text(const std::vector<std::uint64_t>& seeds) {
    if (seeds.empty()) return "none";
    std::string text;
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        if (i) text += ',';
        text += std::to_string(seeds[i]);
    }
    return text;
}

void put_optional(JsonWriter& json, std::string_view name, const std::optional<double>& value) {
    json.key(name);
    if (value) json.number(*value);
    else json.null();
}

void put_run_set(JsonWriter& json, const RunSet& set) {
    json.begin_object();
    json.key("path").string(set.path.string());
    json.key("kind").string(set.is_sweep ? "sweep" : "run");
    json.key("runs").integer_text(set.runs.size());
    json.key("experiment").string(set.runs.empty() ? "" : set.runs.front().experiment_name);
    json.key("strategy").string(set.runs.empty() ? "" : set.runs.front().strategy_name);
    json.key("config_hash").string(set.runs.empty() ? "" : set.runs.front().config_hash);
    json.key("market_world").string(set.runs.empty() ? "" : set.runs.front().market_world);
    json.end_object();
}

}  // namespace

RunSet load_run_set(const std::filesystem::path& path) {
    RunSet set;
    set.path = path;
    std::error_code error;
    if (std::filesystem::exists(path / "sweep.json", error)) {
        set.is_sweep = true;
        const std::filesystem::path sweep_path = path / "sweep.json";
        const JsonValue sweep = parse_json_file(sweep_path);
        if (require_string(sweep, "schema", sweep_path) != "exsim.research.sweep/1")
            throw ArtifactError(sweep_path.string() + ": unsupported schema");
        const JsonValue& runs = require_member(sweep, "runs", sweep_path);
        for (const JsonValue& run : runs.items) set.runs.push_back(load_run(path / require_string(run, "directory", sweep_path)));
    } else if (std::filesystem::exists(path / "summary.json", error)) {
        set.runs.push_back(load_run(path));
    } else {
        throw ArtifactError(path.string() + " has neither summary.json nor sweep.json");
    }
    std::set<std::uint64_t> seeds;
    for (const LoadedRun& run : set.runs)
        if (!seeds.insert(run.master_seed).second)
            throw ArtifactError(path.string() + ": master seed " + std::to_string(run.master_seed) + " appears twice");
    return set;
}

ComparisonReport compare_run_sets(RunSet a, RunSet b) {
    ComparisonReport report;
    report.a = std::move(a);
    report.b = std::move(b);

    std::map<std::uint64_t, const LoadedRun*> completed_a;
    std::map<std::uint64_t, const LoadedRun*> completed_b;
    report.reproduction_checked = true;
    const auto admit = [&](char side, const LoadedRun& run, std::map<std::uint64_t, const LoadedRun*>& admitted) {
        ExcludedRun excluded{side, run.master_seed, run.run_id, run.state, "", ""};
        if (run.state != "completed") {
            excluded.reason = "not_completed";
        } else if (!run.replay_verified) {
            excluded.reason = "replay_not_verified";
            excluded.detail = "summary.json does not record a matching independent replay";
        } else if (run.reproduction == ReproductionCheck::NotReproduced) {
            excluded.reason = "not_reproduced";
            excluded.detail = run.reproduction_detail;
        } else if (run.execution_observation_status == "censored_run_end") {
            excluded.reason = "execution_window_censored";
            excluded.detail = "execution observation ended before the parent window deadline";
        } else if (run.execution_observation_status == "unavailable") {
            excluded.reason = "execution_window_evidence_unavailable";
            excluded.detail = "recorded parent observation window cannot be established";
        } else {
            if (run.reproduction != ReproductionCheck::Reproduced) report.reproduction_checked = false;
            admitted[run.master_seed] = &run;
            return;
        }
        report.excluded_runs.push_back(std::move(excluded));
    };
    for (const LoadedRun& run : report.a.runs) admit('A', run, completed_a);
    for (const LoadedRun& run : report.b.runs) admit('B', run, completed_b);
    for (const auto& [seed, run] : completed_a) {
        if (completed_b.count(seed)) report.paired_seeds.push_back(seed);
        else report.unpaired_seeds_a.push_back(seed);
    }
    for (const auto& [seed, run] : completed_b)
        if (!completed_a.count(seed)) report.unpaired_seeds_b.push_back(seed);

    if (!report.a.runs.empty() && !report.b.runs.empty()) {
        const LoadedRun& first_a = report.a.runs.front();
        const LoadedRun& first_b = report.b.runs.front();
        report.market_world_differs = first_a.market_world != first_b.market_world;
        std::map<std::string, std::string> entries_b(first_b.config_entries.begin(), first_b.config_entries.end());
        std::set<std::string> keys_a;
        for (const auto& [key, value] : first_a.config_entries) {
            keys_a.insert(key);
            const auto found = entries_b.find(key);
            if (found == entries_b.end()) report.config_differences.push_back({key, value, std::nullopt});
            else if (found->second != value) report.config_differences.push_back({key, value, found->second});
        }
        for (const auto& [key, value] : first_b.config_entries)
            if (!keys_a.count(key)) report.config_differences.push_back({key, std::nullopt, value});
    }

    // Metric names in A's order, then any only B has.
    std::vector<std::string> metric_names;
    std::set<std::string> seen_names;
    for (const auto* set : {&report.a, &report.b})
        for (const LoadedRun& run : set->runs)
            for (const auto& [name, value] : run.metrics)
                if (seen_names.insert(name).second) metric_names.push_back(name);

    const auto metric_value = [](const LoadedRun& run, const std::string& name) -> std::optional<double> {
        for (const auto& [metric_name, value] : run.metrics)
            if (metric_name == name) return value;
        return std::nullopt;
    };
    for (const std::string& name : metric_names) {
        RunningStatistic side_a;
        RunningStatistic side_b;
        RunningStatistic difference;
        MetricComparison comparison;
        comparison.name = name;
        for (const std::uint64_t seed : report.paired_seeds) {
            const auto value_a = metric_value(*completed_a[seed], name);
            const auto value_b = metric_value(*completed_b[seed], name);
            if (!value_a || !value_b) {
                ++comparison.unavailable;
                continue;
            }
            side_a.add(*value_a);
            side_b.add(*value_b);
            difference.add(*value_b - *value_a);
        }
        comparison.pairs = difference.count;
        comparison.mean_a = side_a.mean;
        comparison.mean_b = side_b.mean;
        comparison.mean_difference = difference.mean;
        comparison.min_difference = difference.min;
        comparison.max_difference = difference.max;
        comparison.sd_a = side_a.sample_sd();
        comparison.sd_b = side_b.sample_sd();
        comparison.sd_difference = difference.sample_sd();
        if (comparison.sd_difference)
            comparison.standard_error_difference = *comparison.sd_difference / std::sqrt(static_cast<double>(difference.count));
        report.metrics.push_back(std::move(comparison));
    }
    return report;
}

std::string render_comparison_text(const ComparisonReport& report) {
    std::string text;
    const auto describe = [](const RunSet& set) {
        std::string line = set.path.string() + " (" + (set.is_sweep ? "sweep" : "run") + ", " + std::to_string(set.runs.size()) + " runs";
        if (!set.runs.empty())
            line += ", experiment " + set.runs.front().experiment_name + ", strategy " + set.runs.front().strategy_name +
                    ", config " + set.runs.front().config_hash;
        return line + ")";
    };
    text += "A: " + describe(report.a) + "\n";
    text += "B: " + describe(report.b) + "\n";
    text += "paired seeds (" + std::to_string(report.paired_seeds.size()) + "): " + seed_list_text(report.paired_seeds) + "\n";
    text += "unpaired seeds: A " + seed_list_text(report.unpaired_seeds_a) + "; B " + seed_list_text(report.unpaired_seeds_b) + "\n";
    if (report.excluded_runs.empty()) {
        text += "excluded runs: none\n";
    } else {
        text += "excluded runs:\n";
        for (const ExcludedRun& run : report.excluded_runs) {
            text += std::string("  ") + run.side + " seed " + std::to_string(run.master_seed) + " " + run.run_id + " state " +
                    run.state + " reason " + run.reason;
            if (!run.detail.empty()) text += " (" + run.detail + ")";
            text += "\n";
        }
    }
    text += report.reproduction_checked
                ? "reproduction check: every paired run was re-executed and matched its artifacts byte for byte\n"
                : "reproduction check: not run; summary.json values are used as written\n";
    if (report.market_world_differs) text += "WARNING: the market worlds differ; results are not directly comparable\n";
    if (report.config_differences.empty()) {
        text += "config differences: none\n";
    } else {
        text += "config differences (A -> B):\n";
        for (const ConfigDifference& difference : report.config_differences)
            text += "  " + difference.key + ": " + difference.value_a.value_or("(absent)") + " -> " +
                    difference.value_b.value_or("(absent)") + "\n";
    }
    text += "paired differences are B - A; sd is the sample standard deviation over pairs; se = sd / sqrt(n)\n";
    char line[512];
    std::snprintf(line, sizeof line, "%-36s %4s %4s %13s %13s %13s %13s %13s %13s %13s\n", "metric", "n", "na", "mean A",
                  "mean B", "mean B-A", "sd B-A", "se B-A", "min B-A", "max B-A");
    text += line;
    for (const MetricComparison& metric : report.metrics) {
        const bool any = metric.pairs > 0;
        std::snprintf(line, sizeof line, "%-36s %4llu %4llu %13s %13s %13s %13s %13s %13s %13s\n", metric.name.c_str(),
                      static_cast<unsigned long long>(metric.pairs), static_cast<unsigned long long>(metric.unavailable),
                      any ? number_text(metric.mean_a).c_str() : "n/a", any ? number_text(metric.mean_b).c_str() : "n/a",
                      any ? number_text(metric.mean_difference).c_str() : "n/a", optional_text(metric.sd_difference).c_str(),
                      optional_text(metric.standard_error_difference).c_str(),
                      any ? number_text(metric.min_difference).c_str() : "n/a",
                      any ? number_text(metric.max_difference).c_str() : "n/a");
        text += line;
    }
    if (report.paired_seeds.size() < 2)
        text += "note: fewer than two paired seeds; no dispersion. A single synthetic run is a demonstration, not evidence.\n";
    return text;
}

std::string render_comparison_json(const ComparisonReport& report) {
    JsonWriter json;
    json.begin_object();
    json.key("schema").string(kCompareSchema);
    json.key("a");
    put_run_set(json, report.a);
    json.key("b");
    put_run_set(json, report.b);
    json.key("difference_direction").string("b_minus_a");
    const auto put_seeds = [&](std::string_view name, const std::vector<std::uint64_t>& seeds) {
        json.key(name).begin_array();
        for (const std::uint64_t seed : seeds) json.integer_text(seed);
        json.end_array();
    };
    put_seeds("paired_seeds", report.paired_seeds);
    put_seeds("unpaired_seeds_a", report.unpaired_seeds_a);
    put_seeds("unpaired_seeds_b", report.unpaired_seeds_b);
    json.key("excluded_runs").begin_array();
    for (const ExcludedRun& run : report.excluded_runs) {
        json.begin_object();
        json.key("side").string(std::string(1, run.side));
        json.key("master_seed").integer_text(run.master_seed);
        json.key("run_id").string(run.run_id);
        json.key("state").string(run.state);
        json.key("reason").string(run.reason);
        json.key("detail").string(run.detail);
        json.end_object();
    }
    json.end_array();
    json.key("reproduction_checked").boolean(report.reproduction_checked);
    json.key("market_world_differs").boolean(report.market_world_differs);
    json.key("config_differences").begin_array();
    for (const ConfigDifference& difference : report.config_differences) {
        json.begin_object();
        json.key("key").string(difference.key);
        json.key("a");
        if (difference.value_a) json.string(*difference.value_a);
        else json.null();
        json.key("b");
        if (difference.value_b) json.string(*difference.value_b);
        else json.null();
        json.end_object();
    }
    json.end_array();
    json.key("metrics").begin_array();
    for (const MetricComparison& metric : report.metrics) {
        const bool any = metric.pairs > 0;
        json.begin_object();
        json.key("name").string(metric.name);
        json.key("pairs").integer_text(metric.pairs);
        json.key("unavailable_pairs").integer_text(metric.unavailable);
        put_optional(json, "mean_a", any ? std::optional<double>(metric.mean_a) : std::nullopt);
        put_optional(json, "mean_b", any ? std::optional<double>(metric.mean_b) : std::nullopt);
        put_optional(json, "sd_a", metric.sd_a);
        put_optional(json, "sd_b", metric.sd_b);
        put_optional(json, "mean_difference", any ? std::optional<double>(metric.mean_difference) : std::nullopt);
        put_optional(json, "sd_difference", metric.sd_difference);
        put_optional(json, "standard_error_difference", metric.standard_error_difference);
        put_optional(json, "min_difference", any ? std::optional<double>(metric.min_difference) : std::nullopt);
        put_optional(json, "max_difference", any ? std::optional<double>(metric.max_difference) : std::nullopt);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    std::string text = json.take();
    text += '\n';
    return text;
}

}  // namespace exsim::research
