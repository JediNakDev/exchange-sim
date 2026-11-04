// exsim_research: deterministic research runs, seed sweeps and paired comparison.
//
//   exsim_research run <experiment.conf> --out <dir> [--seed N | --seeds A..B]
//                      [--overwrite] [--parent-run <run id>] [--quiet]
//   exsim_research compare <runA> <runB> [--json]
//   exsim_research validate <experiment.conf>
//
// Exit codes (docs/RESEARCH_RUNNER.md):
//   0 success                     every run completed and its replay matched
//   1 run not successful          a run failed or was cancelled (artifacts still written)
//   2 usage error
//   3 configuration error
//   4 I/O or artifact error       unreadable input, unusable output directory, bad artifact files
//   5 replay mismatch             the independent replay did not reproduce the online event digest
//   6 nothing to compare          no completed run is paired by master seed

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/experiment_runner.hpp"
#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/run_compare.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace exsim::research;

enum ExitCode : int {
    kExitSuccess = 0,
    kExitRunNotSuccessful = 1,
    kExitUsage = 2,
    kExitConfig = 3,
    kExitInputOutput = 4,
    kExitReplayMismatch = 5,
    kExitNothingToCompare = 6,
};

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> cancel_requested{false};

extern "C" void handle_interrupt(int) { cancel_requested.store(true, std::memory_order_relaxed); }

constexpr std::string_view kUsage =
    "usage:\n"
    "  exsim_research run <experiment.conf> --out <dir> [--seed N | --seeds A..B] [--overwrite]\n"
    "                     [--parent-run <run id>] [--quiet]\n"
    "  exsim_research compare <runA> <runB> [--json]\n"
    "  exsim_research validate <experiment.conf>\n"
    "exit codes: 0 ok, 1 run failed/cancelled, 2 usage, 3 config, 4 i/o, 5 replay mismatch, 6 nothing to compare\n";

int usage_error(std::string_view message) {
    std::cerr << "exsim_research: " << message << "\n" << kUsage;
    return kExitUsage;
}

ExperimentConfig load_config(const std::string& path, std::string& source_text) {
    source_text = read_text_file(path);
    return parse_experiment_config(source_text);
}

void print_outcome(const RunOutcome& outcome) {
    std::cout << "run " << outcome.run_id << " seed=" << outcome.master_seed << " state=" << to_string(outcome.state)
              << " termination=" << to_string(outcome.termination) << " events=" << outcome.simulation_events
              << " commands=" << outcome.digests.command_count << " fills=" << outcome.digests.fill_count
              << " event_digest=" << outcome.digests.event_digest << " ledger_digest=" << outcome.digests.ledger_digest
              << " replay=" << (outcome.replay_verified ? "verified" : "MISMATCH") << " wall_s=" << outcome.wall_seconds
              << " dir=" << outcome.run_directory.string() << "\n";
}

int exit_code_for(const std::vector<RunOutcome>& outcomes, std::size_t requested) {
    for (const RunOutcome& outcome : outcomes)
        if (!outcome.replay_verified) return kExitReplayMismatch;
    if (outcomes.size() != requested) return kExitRunNotSuccessful;
    for (const RunOutcome& outcome : outcomes)
        if (outcome.state != RunState::Completed) return kExitRunNotSuccessful;
    return kExitSuccess;
}

int command_run(const std::vector<std::string>& arguments, const std::string& executable_path) {
    std::string config_path;
    std::string out_directory;
    std::string seed_text;
    bool sweep = false;
    bool overwrite = false;
    bool quiet = false;
    std::string parent_run_id;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        const auto value = [&](std::string& target) {
            if (i + 1 >= arguments.size()) return false;
            target = arguments[++i];
            return true;
        };
        if (argument == "--out") {
            if (!value(out_directory)) return usage_error("--out needs a directory");
        } else if (argument == "--seed") {
            if (!seed_text.empty() || !value(seed_text)) return usage_error("--seed needs one value (and only one seed option)");
        } else if (argument == "--seeds") {
            if (!seed_text.empty() || !value(seed_text)) return usage_error("--seeds needs A..B (and only one seed option)");
            sweep = true;
        } else if (argument == "--parent-run") {
            if (!value(parent_run_id)) return usage_error("--parent-run needs a run id");
        } else if (argument == "--overwrite") {
            overwrite = true;
        } else if (argument == "--quiet") {
            quiet = true;
        } else if (!argument.empty() && argument[0] == '-') {
            return usage_error("unknown option " + argument);
        } else if (config_path.empty()) {
            config_path = argument;
        } else {
            return usage_error("unexpected argument " + argument);
        }
    }
    if (config_path.empty()) return usage_error("run needs an experiment file");
    if (out_directory.empty()) return usage_error("run needs --out <dir>");

    std::string source_text;
    ExperimentConfig config;
    try {
        config = load_config(config_path, source_text);
    } catch (const ConfigError& error) {
        std::cerr << "exsim_research: " << config_path << ": " << error.what() << "\n";
        return kExitConfig;
    }
    std::vector<std::uint64_t> seeds;
    if (seed_text.empty()) seeds.push_back(config.default_seed);
    else if (!parse_seed_range(seed_text, seeds)) return usage_error("bad seed value " + seed_text);
    if (!sweep && seeds.size() != 1) return usage_error("--seed takes one seed; use --seeds A..B for a sweep");

    std::signal(SIGINT, handle_interrupt);
    std::signal(SIGTERM, handle_interrupt);
    const BuildInfo build = current_build_info(executable_path);
    if (!sweep) {
        RunRequest request;
        request.config = config;
        request.config_source_text = source_text;
        request.master_seed = seeds.front();
        request.run_directory = out_directory;
        request.parent_run_id = parent_run_id;
        request.build = build;
        request.overwrite = overwrite;
        request.cancel_requested = &cancel_requested;
        const RunOutcome outcome = execute_run(request);
        if (!quiet) print_outcome(outcome);
        return exit_code_for({outcome}, 1);
    }
    SweepRequest request;
    request.config = config;
    request.config_source_text = source_text;
    request.master_seeds = seeds;
    request.sweep_directory = out_directory;
    request.parent_run_id = parent_run_id;
    request.build = build;
    request.overwrite = overwrite;
    request.cancel_requested = &cancel_requested;
    if (!quiet) request.on_run_finished = print_outcome;
    const std::vector<RunOutcome> outcomes = execute_sweep(request);
    if (!quiet)
        std::cout << "sweep " << out_directory << " runs=" << outcomes.size() << "/" << seeds.size() << "\n";
    return exit_code_for(outcomes, seeds.size());
}

int command_compare(const std::vector<std::string>& arguments) {
    std::vector<std::string> paths;
    bool as_json = false;
    for (const std::string& argument : arguments) {
        if (argument == "--json") as_json = true;
        else if (!argument.empty() && argument[0] == '-') return usage_error("unknown option " + argument);
        else paths.push_back(argument);
    }
    if (paths.size() != 2) return usage_error("compare needs exactly two run or sweep directories");
    const ComparisonReport report = compare_run_sets(load_run_set(paths[0]), load_run_set(paths[1]));
    std::cout << (as_json ? render_comparison_json(report) : render_comparison_text(report));
    return report.paired_seeds.empty() ? kExitNothingToCompare : kExitSuccess;
}

int command_validate(const std::vector<std::string>& arguments) {
    if (arguments.size() != 1) return usage_error("validate needs exactly one experiment file");
    std::string source_text;
    ExperimentConfig config;
    try {
        config = load_config(arguments[0], source_text);
    } catch (const ConfigError& error) {
        std::cerr << "exsim_research: " << arguments[0] << ": " << error.what() << "\n";
        return kExitConfig;
    }
    std::cout << canonical_config_text(config);
    std::cout << "config_hash=" << format_hex64(experiment_config_hash(config)) << "\n";
    return kExitSuccess;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage_error("missing command");
    const std::string command = argv[1];
    const std::vector<std::string> arguments(argv + 2, argv + argc);
    try {
        if (command == "run") return command_run(arguments, argv[0]);
        if (command == "compare") return command_compare(arguments);
        if (command == "validate") return command_validate(arguments);
        if (command == "help" || command == "--help" || command == "-h") {
            std::cout << kUsage;
            return kExitSuccess;
        }
        return usage_error("unknown command " + command);
    } catch (const ConfigError& error) {
        std::cerr << "exsim_research: configuration error: " << error.what() << "\n";
        return kExitConfig;
    } catch (const ArtifactError& error) {
        std::cerr << "exsim_research: " << error.what() << "\n";
        return kExitInputOutput;
    } catch (const std::invalid_argument& error) {
        std::cerr << "exsim_research: configuration rejected: " << error.what() << "\n";
        return kExitConfig;
    } catch (const std::exception& error) {
        std::cerr << "exsim_research: " << error.what() << "\n";
        return kExitInputOutput;
    }
}
