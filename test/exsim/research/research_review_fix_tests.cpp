// Follow-up tests for the research runner QA review (2026-11-05): the cases
// the reviewer's reproducers (research_review_tests.cpp) do not pin, and the
// plausible findings P1-P3 that the fix confirmed.

#include "research/experiment_runner.hpp"
#include "research/json.hpp"
#include "research/run_artifacts.hpp"
#include "research/run_compare.hpp"
#include "research/run_verification.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace research_test;

namespace {

RunRequest followup_run_request(const std::filesystem::path& directory, std::uint64_t seed) {
    RunRequest request;
    request.config = busy_market_config(200 * kMillisecond);
    request.config.name = "research_review_followup";
    request.config_source_text = canonical_config_text(request.config);
    request.master_seed = seed;
    request.run_directory = directory;
    request.build.compiler = "test";
    return request;
}

SweepRequest followup_sweep_request(const std::filesystem::path& directory, std::vector<std::uint64_t> seeds) {
    SweepRequest request;
    request.config = busy_market_config(200 * kMillisecond);
    request.config.name = "research_review_followup";
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

bool config_is_rejected(ExperimentConfig config) {
    try {
        validate_experiment_config(config);
    } catch (const ConfigError&) {
        return true;
    }
    return false;
}

bool config_text_is_rejected(std::string_view text) {
    try {
        (void)parse_experiment_config(text);
    } catch (const ConfigError&) {
        return true;
    }
    return false;
}

std::size_t config_error_line(std::string_view text) {
    try {
        (void)parse_experiment_config(text);
    } catch (const ConfigError& error) {
        return error.line();
    }
    return 0;
}

const LifecycleRecord* find_arrival(const CollectedRecords& records, CommandType request) {
    for (const LifecycleRecord& row : records.lifecycle)
        if (row.stage == LifecycleStage::ExchangeArrival && row.request == request) return &row;
    return nullptr;
}

// A run of a shipped experiment file, as the CLI writes it (reproduce_run
// re-parses experiment.conf, so it must be the INI source).
RunRequest shipped_config_run_request(const std::filesystem::path& directory, std::uint64_t seed) {
    RunRequest request;
    request.config_source_text = read_text_file("config/research/fixed_spread_narrow.conf");
    request.config = parse_experiment_config(request.config_source_text);
    request.master_seed = seed;
    request.run_directory = directory;
    request.build.compiler = "test";
    return request;
}

}  // namespace

// R1: an untouched run reproduces byte for byte; an edited summary.json
// metric (with replay_verified left true) or a changed journal byte does not,
// and compare excludes the run once check_reproduction ran.
TEST_CASE(research_review_fix, reproduction_check_catches_edited_artifacts) {
    const std::filesystem::path baseline_directory = fresh_test_directory("fix_reproduce_baseline");
    const std::filesystem::path candidate_directory = fresh_test_directory("fix_reproduce_candidate");
    REQUIRE(execute_run(shipped_config_run_request(baseline_directory, 1)).replay_verified);
    REQUIRE(execute_run(shipped_config_run_request(candidate_directory, 1)).replay_verified);

    const RunReproduction untouched = reproduce_run(candidate_directory);
    CHECK(untouched.reproduced);
    CHECK(untouched.detail.empty());

    const std::filesystem::path candidate_summary = candidate_directory / "summary.json";
    std::string summary_text = read_whole_file(candidate_summary);
    const std::size_t net_pnl_metric = summary_text.find("\"net_pnl\":", summary_text.find("\"metrics\":"));
    REQUIRE(net_pnl_metric != std::string::npos);
    const std::size_t value_start = net_pnl_metric + std::string("\"net_pnl\":").size();
    const std::size_t value_end = summary_text.find_first_of(",}", value_start);
    summary_text.replace(value_start, value_end - value_start, "99999999");
    write_whole_file(candidate_summary, summary_text);

    const RunReproduction edited = reproduce_run(candidate_directory);
    CHECK(!edited.reproduced);
    CHECK(edited.detail.find("summary.json") != std::string::npos);

    RunSet baseline = load_run_set(baseline_directory);
    RunSet candidate = load_run_set(candidate_directory);
    REQUIRE(candidate.runs.front().replay_verified);
    check_reproduction(baseline);
    check_reproduction(candidate);
    const ComparisonReport report = compare_run_sets(std::move(baseline), std::move(candidate));
    CHECK(report.paired_seeds.empty());
    REQUIRE_EQ(report.excluded_runs.size(), 1u);
    CHECK_EQ(report.excluded_runs.front().side, 'B');
    CHECK_EQ(report.excluded_runs.front().reason, std::string("not_reproduced"));
    CHECK(render_comparison_json(report).find("\"reason\":\"not_reproduced\"") != std::string::npos);

    // Restore the summary, then change one journal byte.
    REQUIRE(execute_run([&] {
                RunRequest request = shipped_config_run_request(candidate_directory, 1);
                request.overwrite = true;
                return request;
            }())
                .replay_verified);
    std::string journal = read_whole_file(candidate_directory / "commands.journal");
    REQUIRE(journal.size() > 64);
    journal[journal.size() / 2] = static_cast<char>(journal[journal.size() / 2] ^ 0x01);
    write_whole_file(candidate_directory / "commands.journal", journal);
    const RunReproduction journal_edited = reproduce_run(candidate_directory);
    CHECK(!journal_edited.reproduced);
    CHECK(journal_edited.detail.find("commands.journal") != std::string::npos);

    // Without the reproduction check the comparison says so.
    const ComparisonReport unchecked = compare_run_sets(load_run_set(baseline_directory), load_run_set(baseline_directory));
    CHECK(!unchecked.reproduction_checked);
    CHECK(render_comparison_text(unchecked).find("reproduction check: not run") != std::string::npos);

    std::filesystem::remove_all(baseline_directory);
    std::filesystem::remove_all(candidate_directory);
}

// R2 and R3: a foreign file in a stale seed directory refuses the overwrite
// before anything changes; with only run artifacts there, the stale seed
// directory is removed.
TEST_CASE(research_review_fix, refused_sweep_overwrite_changes_nothing_on_disk) {
    const std::filesystem::path sweep_directory = fresh_test_directory("fix_sweep_stale_foreign");
    REQUIRE_EQ(execute_sweep(followup_sweep_request(sweep_directory, {1, 2, 3})).size(), 3u);
    const std::string sweep_index = read_whole_file(sweep_directory / "sweep.json");
    const std::string seed_one_summary = read_whole_file(sweep_directory / "seed-1" / "summary.json");
    write_whole_file(sweep_directory / "seed-3" / "notes.txt", "keep me");

    SweepRequest overwrite_request = followup_sweep_request(sweep_directory, {1, 2});
    overwrite_request.overwrite = true;
    bool refused = false;
    try {
        (void)execute_sweep(overwrite_request);
    } catch (const ArtifactError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK_EQ(read_whole_file(sweep_directory / "sweep.json"), sweep_index);
    CHECK_EQ(read_whole_file(sweep_directory / "seed-1" / "summary.json"), seed_one_summary);
    CHECK(std::filesystem::exists(sweep_directory / "seed-3" / "summary.json"));
    CHECK_EQ(load_run_set(sweep_directory).runs.size(), 3u);

    std::filesystem::remove(sweep_directory / "seed-3" / "notes.txt");
    write_whole_file(sweep_directory / "seed-3" / "summary.json.tmp", "{");
    REQUIRE_EQ(execute_sweep(overwrite_request).size(), 2u);
    CHECK(!std::filesystem::exists(sweep_directory / "seed-3"));
    CHECK_EQ(load_run_set(sweep_directory).runs.size(), 2u);

    std::filesystem::remove_all(sweep_directory);
}

// R4: every artifact's temporary is accepted and removed; other names are
// still refused.
TEST_CASE(research_review_fix, overwrite_removes_every_artifact_temporary_but_refuses_other_files) {
    CHECK(is_run_artifact_file_name("manifest.json.tmp"));
    CHECK(is_run_artifact_file_name("experiment.conf.tmp"));
    CHECK(is_run_artifact_file_name("status.json.tmp"));
    CHECK(!is_run_artifact_file_name("notes.tmp"));
    CHECK(!is_run_artifact_file_name("summary.json.tmp.tmp"));

    const std::filesystem::path run_directory = fresh_test_directory("fix_overwrite_temporaries");
    RunRequest request = followup_run_request(run_directory, 1);
    REQUIRE(execute_run(request).replay_verified);
    write_whole_file(run_directory / "manifest.json.tmp", "{");
    write_whole_file(run_directory / "experiment.conf.tmp", "[");
    request.overwrite = true;
    REQUIRE(execute_run(request).replay_verified);
    CHECK(!std::filesystem::exists(run_directory / "manifest.json.tmp"));
    CHECK(!std::filesystem::exists(run_directory / "experiment.conf.tmp"));

    write_whole_file(run_directory / "notes.tmp", "keep me");
    bool refused = false;
    try {
        (void)execute_run(request);
    } catch (const ArtifactError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(std::filesystem::exists(run_directory / "notes.tmp"));

    std::filesystem::remove_all(run_directory);
}

// R5: horizons must be strictly increasing, in code and in config files.
TEST_CASE(research_review_fix, markout_horizons_must_increase) {
    ExperimentConfig config = quiet_market_config();
    config.metrics.markout_horizon_count = 2;
    config.metrics.markout_horizons_ns[0] = 5 * kNanosPerSecond;
    config.metrics.markout_horizons_ns[1] = 1 * kNanosPerSecond;
    CHECK(config_is_rejected(config));
    config.metrics.markout_horizons_ns[1] = 6 * kNanosPerSecond;
    CHECK(!config_is_rejected(config));
    CHECK(config_text_is_rejected("[metrics]\nmarkout_horizons = 1s, 1000ms\n"));
    CHECK(!config_text_is_rejected("[metrics]\nmarkout_horizons = 100ms, 1s, 5s\n"));
}

// R6: exact at the int64 extremes and unchanged for ordinary values.
TEST_CASE(research_review_fix, doubled_price_formatting_is_exact_over_the_int64_range) {
    CHECK_EQ(format_doubled_price(0), std::string("0.00"));
    CHECK_EQ(format_doubled_price(1), std::string("0.00005"));
    CHECK_EQ(format_doubled_price(-1), std::string("-0.00005"));
    CHECK_EQ(format_doubled_price(200), std::string("0.01"));
    CHECK_EQ(format_doubled_price(2'000'001), std::string("100.00005"));
    CHECK_EQ(format_doubled_price(-2'002'470), std::string("-100.1235"));
    CHECK_EQ(format_doubled_price(std::numeric_limits<std::int64_t>::max()), std::string("461168601842738.79035"));
    CHECK_EQ(format_doubled_price(std::numeric_limits<std::int64_t>::min()), std::string("-461168601842738.7904"));
}

// R7: a replace arrival shows the position the order held at its old price,
// and a new order arrival shows the queue it joins (its own size excluded).
TEST_CASE(research_review_fix, arrival_queue_evidence_is_the_book_before_the_command) {
    const ExperimentConfig config = quiet_market_config();
    Simulation<HookStrategy> simulation(config, 1);
    REQUIRE(simulation.schedule_scripted_command(1 * kMicrosecond, scripted_limit_order(1, Side::BUY, cents(9900), 5)));
    ClOrdId resting_order = 0;
    bool replace_sent = false;
    simulation.strategy().start = [&](StrategyContext& context) {
        resting_order = context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
    };
    simulation.strategy().report = [&](const ExecutionReport& report, StrategyContext& context) {
        if (report.kind == ReportKind::Rested && !replace_sent) {
            replace_sent = true;
            CHECK(context.replace_order(resting_order, cents(9800), 10));
        }
    };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE(replace_sent);

    const LifecycleRecord* new_arrival = find_arrival(records, CommandType::NewOrder);
    REQUIRE(new_arrival != nullptr);
    CHECK(new_arrival->has_queue_position);
    CHECK_EQ(new_arrival->queue_orders_ahead, 1u);
    CHECK_EQ(new_arrival->queue_ahead_qty, 5u);
    CHECK_EQ(new_arrival->level_qty, 5u);
    CHECK_EQ(new_arrival->order_id, 0u);  // assigned by the engine on acceptance

    const LifecycleRecord* replace_arrival = find_arrival(records, CommandType::Replace);
    REQUIRE(replace_arrival != nullptr);
    CHECK(replace_arrival->has_queue_position);
    CHECK_EQ(replace_arrival->queue_orders_ahead, 1u);
    CHECK_EQ(replace_arrival->queue_ahead_qty, 5u);
    CHECK_EQ(replace_arrival->level_qty, 15u);
    CHECK_EQ(replace_arrival->leaves_qty, 10u);
    CHECK_EQ(replace_arrival->price, cents(9800));  // the requested price
}

// P1: one background command that fills every resting strategy order emits
// more lifecycle rows and fills than a 16-record margin. A config that
// validates must never end in recorder_overflow, whatever the capacity.
TEST_CASE(research_review_fix, validated_recorder_limits_cover_the_largest_step) {
    constexpr std::uint32_t kRestingOrders = 64;
    ExperimentConfig base = quiet_market_config();
    base.limits.duration_ns = 10 * kMillisecond;
    base.metrics.sample_interval_ns = 5 * kMillisecond;
    base.risk.max_open_orders = kRestingOrders;
    base.risk.max_order_qty = 10;
    base.risk.max_long_position = 10 * kRestingOrders;
    base.limits.recorder_flush_margin = 16;
    CHECK(recorder_step_bound(base).records >= 2 * kRestingOrders);

    std::size_t validated_configs = 0;
    for (std::size_t capacity = 64; capacity <= 1024; capacity += 8) {
        ExperimentConfig config = base;
        config.limits.recorder_command_capacity = capacity;
        config.limits.recorder_record_capacity = capacity;
        if (config_is_rejected(config)) continue;
        ++validated_configs;
        Simulation<HookStrategy> simulation(config, 1);
        REQUIRE(simulation.schedule_scripted_command(
            2 * kMillisecond, scripted_limit_order(1, Side::SELL, cents(9900), 10 * kRestingOrders, TimeInForce::IOC)));
        simulation.strategy().start = [&](StrategyContext& context) {
            for (std::uint32_t order = 0; order < kRestingOrders; ++order)
                (void)context.submit_limit_order(Side::BUY, cents(9900), 10, TimeInForce::Day, false);
        };
        CollectedRecords records;
        const RunStop stop = run_to_end(simulation, &records);
        CHECK(simulation.termination() != TerminationReason::RecorderOverflow);
        CHECK(stop == RunStop::Completed);
        CHECK_EQ(records.fills.size(), static_cast<std::size_t>(kRestingOrders));
    }
    CHECK(validated_configs > 0);
}

// P2: superseded wakeups are not simulation events.
TEST_CASE(research_review_fix, superseded_wakeups_do_not_count_as_events) {
    const auto run_with_wakeups = [](std::vector<Timestamp> wake_times) {
        Simulation<HookStrategy> simulation(quiet_market_config(), 1);
        simulation.strategy().start = [wake_times](StrategyContext& context) {
            for (const Timestamp time : wake_times) context.wake_at(time);
        };
        std::uint64_t wakeups = 0;
        simulation.strategy().wakeup = [&wakeups](StrategyContext&) { ++wakeups; };
        REQUIRE(run_to_end(simulation) == RunStop::Completed);
        CHECK_EQ(wakeups, 1u);
        return simulation.simulation_events_processed();
    };
    const std::uint64_t single = run_with_wakeups({500 * kMillisecond});
    const std::uint64_t rescheduled =
        run_with_wakeups({100 * kMillisecond, 200 * kMillisecond, 300 * kMillisecond, 400 * kMillisecond, 500 * kMillisecond});
    CHECK_EQ(rescheduled, single);

    // The event limit no longer depends on timer churn: a limit that fits the
    // real events completes even with many superseded wakeups.
    ExperimentConfig limited = quiet_market_config();
    limited.limits.max_simulation_events = single;
    Simulation<HookStrategy> simulation(limited, 1);
    simulation.strategy().start = [](StrategyContext& context) {
        for (Timestamp time = 1; time <= 500; ++time) context.wake_at(time * kMillisecond);
    };
    CHECK(run_to_end(simulation) == RunStop::Completed);
}

// P3: '#' and ';' start a comment only as the first non-blank character.
TEST_CASE(research_review_fix, comment_characters_inside_values_are_text) {
    const ExperimentConfig config = parse_experiment_config(
        "  # leading comment\n"
        "; another comment\n"
        "[experiment]\n"
        "name = comments\n"
        "description = spread 2; inventory skew #3\n");
    CHECK_EQ(config.description, std::string("spread 2; inventory skew #3"));
    // A trailing comment after a typed value is now an error, not ignored.
    CHECK_EQ(config_error_line("[experiment]\nduration = 1s # one second\n"), 2u);
}
