// Independent QA review of the deterministic research runner (2026-11-05).
//
// Each test reproduces one review finding and FAILS against the reviewed code
// (4783f45). The expected behaviour comes from research runner or from
// the stated contract of the function under test. See the review report for
// severity, scenario and suggested fix.

#include "exsim/research/experiment_runner.hpp"
#include "exsim/research/json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "exsim/research/run_compare.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace research_test;

namespace {

ExperimentConfig short_review_config() {
    ExperimentConfig config = busy_market_config(200 * kMillisecond);
    config.name = "research_review";
    return config;
}

RunRequest short_run_request(const std::filesystem::path& directory, std::uint64_t seed) {
    RunRequest request;
    request.config = short_review_config();
    request.config_source_text = canonical_config_text(request.config);
    request.master_seed = seed;
    request.run_directory = directory;
    request.build.compiler = "test";
    return request;
}

SweepRequest short_sweep_request(const std::filesystem::path& directory, std::vector<std::uint64_t> seeds) {
    SweepRequest request;
    request.config = short_review_config();
    request.config_source_text = canonical_config_text(request.config);
    request.master_seeds = std::move(seeds);
    request.sweep_directory = directory;
    request.build.compiler = "test";
    return request;
}

std::string read_whole_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_whole_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

}  // namespace

// Finding R1. compare pairs a run whose own summary says the independent
// replay did NOT reproduce it. load_run never reads digests.replay_verified,
// so a mismatched (or edited) run feeds the paired statistics.
TEST_CASE(research_review, compare_excludes_runs_whose_replay_was_not_verified) {
    const std::filesystem::path baseline_directory = fresh_test_directory("review_compare_baseline");
    const std::filesystem::path candidate_directory = fresh_test_directory("review_compare_candidate");
    REQUIRE(execute_run(short_run_request(baseline_directory, 1)).replay_verified);
    REQUIRE(execute_run(short_run_request(candidate_directory, 1)).replay_verified);

    const std::filesystem::path candidate_summary = candidate_directory / "summary.json";
    std::string summary_text = read_whole_file(candidate_summary);
    const std::string verified_marker = "\"replay_verified\":true";
    const std::size_t marker_position = summary_text.find(verified_marker);
    REQUIRE(marker_position != std::string::npos);
    summary_text.replace(marker_position, verified_marker.size(), "\"replay_verified\":false");
    write_whole_file(candidate_summary, summary_text);

    const ComparisonReport report =
        compare_run_sets(load_run_set(baseline_directory), load_run_set(candidate_directory));
    CHECK(report.paired_seeds.empty());
    CHECK_EQ(report.excluded_runs.size(), 1u);

    std::filesystem::remove_all(baseline_directory);
    std::filesystem::remove_all(candidate_directory);
}

// Finding R2. A refused --overwrite of a sweep deletes the old sweep.json
// before any seed directory is checked, so the previous sweep can no longer be
// loaded or compared even though nothing new was written.
TEST_CASE(research_review, refused_sweep_overwrite_keeps_the_previous_sweep_index) {
    const std::filesystem::path sweep_directory = fresh_test_directory("review_sweep_refused_overwrite");
    REQUIRE_EQ(execute_sweep(short_sweep_request(sweep_directory, {1, 2})).size(), 2u);
    REQUIRE(std::filesystem::exists(sweep_directory / "sweep.json"));

    // A file the runner does not own makes the seed-1 run refuse its directory.
    write_whole_file(sweep_directory / "seed-1" / "notes.txt", "keep me");
    SweepRequest overwrite_request = short_sweep_request(sweep_directory, {1, 2});
    overwrite_request.overwrite = true;
    bool refused = false;
    try {
        (void)execute_sweep(overwrite_request);
    } catch (const ArtifactError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(std::filesystem::exists(sweep_directory / "sweep.json"));
    bool previous_sweep_loads = false;
    try {
        previous_sweep_loads = load_run_set(sweep_directory).runs.size() == 2;
    } catch (const std::exception&) {
        previous_sweep_loads = false;
    }
    CHECK(previous_sweep_loads);

    std::filesystem::remove_all(sweep_directory);
}

// Finding R3. Overwriting a sweep with fewer seeds leaves the old seed-N
// directories in place, so the sweep directory holds complete runs that
// sweep.json does not list.
TEST_CASE(research_review, sweep_overwrite_with_fewer_seeds_removes_stale_seed_directories) {
    const std::filesystem::path sweep_directory = fresh_test_directory("review_sweep_stale_seeds");
    REQUIRE_EQ(execute_sweep(short_sweep_request(sweep_directory, {1, 2, 3})).size(), 3u);
    SweepRequest overwrite_request = short_sweep_request(sweep_directory, {1, 2});
    overwrite_request.overwrite = true;
    REQUIRE_EQ(execute_sweep(overwrite_request).size(), 2u);
    CHECK(!std::filesystem::exists(sweep_directory / "seed-3"));

    std::filesystem::remove_all(sweep_directory);
}

// Finding R4. write_file_atomically leaves <name>.tmp behind when a process is
// killed mid write, but kRunArtifactFileNames only knows status.json.tmp. A
// leftover summary.json.tmp (or manifest.json.tmp, experiment.conf.tmp) then
// makes every later --overwrite of that run directory fail with exit 4.
TEST_CASE(research_review, overwrite_tolerates_leftover_atomic_write_temporaries) {
    const std::filesystem::path run_directory = fresh_test_directory("review_overwrite_tmp");
    RunRequest request = short_run_request(run_directory, 1);
    REQUIRE(execute_run(request).replay_verified);

    write_whole_file(run_directory / "summary.json.tmp", "{\"partial\":");
    request.overwrite = true;
    bool overwrite_succeeded = false;
    try {
        overwrite_succeeded = execute_run(request).replay_verified;
    } catch (const ArtifactError&) {
        overwrite_succeeded = false;
    }
    CHECK(overwrite_succeeded);

    std::filesystem::remove_all(run_directory);
}

// Finding R5. Two distinct horizon entries that render to the same text (or
// a repeated value) pass validation and produce duplicate metric keys in
// summary.json, e.g. "markout_1s_per_share" twice.
TEST_CASE(research_review, duplicate_markout_horizons_are_rejected) {
    ExperimentConfig config = quiet_market_config();
    config.metrics.markout_horizon_count = 2;
    config.metrics.markout_horizons_ns[0] = 1 * kNanosPerSecond;     // "1s"
    config.metrics.markout_horizons_ns[1] = 1000 * kMillisecond;     // "1000ms", same value
    CHECK_EQ(format_duration(config.metrics.markout_horizons_ns[0]),
             format_duration(config.metrics.markout_horizons_ns[1]));
    bool rejected = false;
    try {
        validate_experiment_config(config);
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

// Finding R6. format_doubled_price multiplies the magnitude by 5 in uint64
// and wraps for |value_times_two| > 2^64 / 5 (about 3.69e18). Validation only
// bounds cumulative money at INT64_MAX / 4 (single units), so doubled values up
// to about 4.6e18 are allowed through and would print as garbage.
TEST_CASE(research_review, doubled_price_formatting_does_not_wrap_for_validated_magnitudes) {
    CHECK_EQ(format_doubled_price(4'000'000'000'000'000'000), std::string("200000000000000.00"));
    CHECK_EQ(format_doubled_price(-4'000'000'000'000'000'000), std::string("-200000000000000.00"));
}

// Finding R7. research runner: lifecycle queue evidence is "exact
// engine truth at arrival, before command application". record_strategy_arrival
// runs after engine_.handle, so a cancel arrival never carries the position of
// the order it cancels, and a replace arrival shows the post-replace queue.
TEST_CASE(research_review, cancel_arrival_queue_evidence_is_taken_before_application) {
    const ExperimentConfig config = quiet_market_config();
    Simulation<HookStrategy> simulation(config, 1);
    ClOrdId resting_order = 0;
    bool cancel_sent = false;
    simulation.strategy().start = [&](StrategyContext& context) {
        resting_order = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        if (report.kind == ReportKind::Rested && !cancel_sent) {
            cancel_sent = true;
            CHECK(context.cancel_order(resting_order));
        }
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE(cancel_sent);

    const LifecycleRecord* cancel_arrival = nullptr;
    for (const LifecycleRecord& row : records.lifecycle)
        if (row.stage == LifecycleStage::ExchangeArrival && row.request == CommandType::Cancel) cancel_arrival = &row;
    REQUIRE(cancel_arrival != nullptr);
    CHECK(cancel_arrival->has_queue_position);
    CHECK_EQ(cancel_arrival->level_qty, 10u);
    CHECK_EQ(cancel_arrival->queue_orders_ahead, 0u);
}
