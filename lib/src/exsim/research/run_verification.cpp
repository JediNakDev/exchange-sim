#include "research/run_verification.hpp"

#include "research/experiment_config.hpp"
#include "research/experiment_runner.hpp"
#include "research/json.hpp"
#include "research/run_artifacts.hpp"

#include <cstdlib>
#include <vector>
#include <charconv>
#include <system_error>
#include <unistd.h>

namespace exsim::research {

namespace {

std::string optional_string_member(const JsonValue& object, std::string_view name) {
    const JsonValue* value = object.find(name);
    return value != nullptr && value->kind == JsonValue::Kind::String ? value->text : std::string();
}

// A fresh directory for one re-execution; unique within the process and
// across concurrent processes.
std::filesystem::path scratch_run_directory() {
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) throw ArtifactError("no temporary directory for run verification: " + error.message());
    const std::string pattern = (base / "exsim_research_verify_XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    if (::mkdtemp(name.data()) == nullptr) throw ArtifactError("cannot create temporary directory for run verification");
    return std::filesystem::path(name.data());
}

class ScratchDirectory {
public:
    explicit ScratchDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScratchDirectory() { remove(); }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void remove() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path path_;
};

}  // namespace

RunReproduction reproduce_run(const std::filesystem::path& run_directory) {
    RunReproduction result;
    JsonValue manifest;
    JsonValue summary;
    try {
        manifest = parse_json(read_text_file(run_directory / "manifest.json"));
        summary = parse_json(read_text_file(run_directory / "summary.json"));
    } catch (const JsonError& error) {
        result.detail = std::string("manifest.json or summary.json is not valid JSON: ") + error.what();
        return result;
    }
    if (optional_string_member(summary, "state") == "cancelled") {
        result.detail = "a cancelled run cannot be re-executed (when it was cancelled is not recorded)";
        return result;
    }

    RunRequest request;
    request.config_source_text = read_text_file(run_directory / "experiment.conf");
    try {
        request.config = parse_experiment_config(request.config_source_text);
    } catch (const ConfigError& error) {
        result.detail = std::string("experiment.conf no longer parses: ") + error.what();
        return result;
    }
    const std::string seed_text = optional_string_member(manifest, "master_seed");
    const auto parsed = std::from_chars(seed_text.data(), seed_text.data() + seed_text.size(), request.master_seed);
    if (seed_text.empty() || parsed.ec != std::errc() || parsed.ptr != seed_text.data() + seed_text.size()) {
        result.detail = "manifest.json has no decimal master_seed";
        return result;
    }
    request.parent_run_id = optional_string_member(manifest, "parent_run_id");
    // The build labels are recorded inputs of manifest.json, not of the
    // simulation; reuse them so a reproduced run's manifest is comparable.
    if (const JsonValue* build = manifest.find("build"); build != nullptr && build->kind == JsonValue::Kind::Object) {
        request.build.engine_build_id = optional_string_member(*build, "engine_build_id");
        request.build.runner_build_id = optional_string_member(*build, "runner_build_id");
        request.build.compiler = optional_string_member(*build, "compiler");
    }

    const ScratchDirectory scratch(scratch_run_directory());
    request.run_directory = scratch.path();
    RunOutcome outcome;
    try {
        outcome = execute_run(request);
    } catch (const ConfigError& error) {
        result.detail = std::string("experiment.conf is rejected by this build: ") + error.what();
        return result;
    } catch (const std::invalid_argument& error) {
        result.detail = std::string("experiment.conf is rejected by the engine: ") + error.what();
        return result;
    }
    if (!outcome.replay_verified) {
        result.detail = "the re-executed run's own independent replay did not match";
        return result;
    }

    std::string differing_files;
    for (const std::string_view name : kRunArtifactFileNames) {
        if (name == "status.json") continue;  // wall clock and process id
        std::error_code error;
        const bool recorded_exists = std::filesystem::exists(run_directory / name, error);
        const bool reproduced_exists = std::filesystem::exists(scratch.path() / name, error);
        bool equal = recorded_exists == reproduced_exists;
        if (equal && recorded_exists)
            equal = read_text_file(run_directory / name) == read_text_file(scratch.path() / name);
        if (!equal) {
            if (!differing_files.empty()) differing_files += ", ";
            differing_files += name;
        }
    }
    if (std::filesystem::exists(run_directory / kExecutionTraceFileName) &&
        read_text_file(run_directory / kExecutionTraceFileName) != read_text_file(scratch.path() / kExecutionTraceFileName)) {
        if (!differing_files.empty()) differing_files += ", ";
        differing_files += kExecutionTraceFileName;
    }
    if (!differing_files.empty()) {
        result.detail = "re-execution by this build differs in " + differing_files;
        return result;
    }
    result.reproduced = true;
    return result;
}

void check_reproduction(RunSet& set) {
    for (LoadedRun& run : set.runs) {
        if (run.state != "completed" || !run.replay_verified) continue;
        const RunReproduction reproduction = reproduce_run(run.directory);
        run.reproduction = reproduction.reproduced ? ReproductionCheck::Reproduced : ReproductionCheck::NotReproduced;
        run.reproduction_detail = reproduction.detail;
    }
}

}  // namespace exsim::research
