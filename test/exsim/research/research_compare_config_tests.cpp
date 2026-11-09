// Config parsing of the shipped examples and the config identity, and the
// compare tool over real sweep directories and hand-built run sets.

#include "research/experiment_runner.hpp"
#include "research/run_artifacts.hpp"
#include "research/run_compare.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

#include <filesystem>

using namespace research_test;

namespace {

std::size_t config_error_line(std::string_view text) {
    try {
        (void)parse_experiment_config(text);
    } catch (const ConfigError& error) {
        return error.line();
    }
    return 0;
}

LoadedRun completed_run(std::uint64_t seed, std::optional<double> net_pnl) {
    LoadedRun run;
    run.master_seed = seed;
    run.run_id = "run-" + std::to_string(seed);
    run.state = "completed";
    run.replay_verified = true;
    run.market_world = "synthetic_interactive";
    run.metrics = {{"net_pnl", net_pnl}};
    return run;
}

}  // namespace

TEST_CASE(research_config, shipped_example_configs_parse_and_validate) {
    const std::filesystem::path directory = "config/research";
    REQUIRE(std::filesystem::is_directory(directory));
    std::size_t parsed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".conf") continue;
        bool ok = true;
        try {
            (void)parse_experiment_config(read_text_file(entry.path()));
        } catch (const std::exception&) {
            ok = false;
        }
        CHECK(ok);
        ++parsed;
    }
    CHECK_GE(parsed, 4u);

    // narrow and wide differ only in the half spread, so their identities differ.
    const ExperimentConfig narrow = parse_experiment_config(read_text_file(directory / "fixed_spread_narrow.conf"));
    const ExperimentConfig wide = parse_experiment_config(read_text_file(directory / "fixed_spread_wide.conf"));
    CHECK_NE(narrow.strategy.quoting.half_spread_ticks, wide.strategy.quoting.half_spread_ticks);
    CHECK_NE(experiment_config_hash(narrow), experiment_config_hash(wide));
}

TEST_CASE(research_config, seed_is_not_part_of_the_config_identity) {
    ExperimentConfig config = quiet_market_config();
    const auto before = experiment_config_hash(config);
    config.default_seed = 99;
    CHECK_EQ(before, experiment_config_hash(config));
    const auto first = parse_experiment_config("[experiment]\nname = a\nseed = 1\n");
    const auto second = parse_experiment_config("[experiment]\nname = a\nseed = 2\n");
    CHECK_EQ(first.default_seed, 1u);
    CHECK_EQ(second.default_seed, 2u);
    CHECK_EQ(experiment_config_hash(first), experiment_config_hash(second));
}

TEST_CASE(research_config, errors_report_the_offending_line) {
    CHECK_EQ(config_error_line("[experiment]\nname = a\nbogus = 1\n"), 3u);
    CHECK_EQ(config_error_line("# comment\n\n[nowhere]\n"), 3u);
    CHECK_EQ(config_error_line("[latency]\nfeed = 10 parsecs\n"), 2u);
    CHECK_EQ(config_error_line("[strategy]\nkind = fixed_spread_market_maker\nslice_interval = 1s\n"), 3u);
    CHECK_NE(config_error_line("[accounting]\nmark = strategy_visible_mid\n"), 0u);  // not supported in v1
}

TEST_CASE(research_config, mark_conventions_use_the_api_names) {
    CHECK(parse_experiment_config("[accounting]\nmark = last_trade_price\n").accounting.mark == MarkConvention::LastTrade);
    CHECK(parse_experiment_config("[accounting]\nmark = exchange_mid\n").accounting.mark == MarkConvention::ExchangeMid);
    CHECK(to_string(MarkConvention::LastTrade) == "last_trade_price");
    CHECK(to_string(MarkConvention::ExchangeMid) == "exchange_mid");
    CHECK_NE(config_error_line("[accounting]\nmark = last_trade\n"), 0u);
}

TEST_CASE(research_compare, sweeps_round_trip_through_the_compare_tool) {
    const std::filesystem::path narrow_directory = fresh_test_directory("compare_narrow");
    const std::filesystem::path wide_directory = fresh_test_directory("compare_wide");
    const auto run_sweep = [](const std::filesystem::path& directory, std::uint32_t half_spread_ticks,
                              std::vector<std::uint64_t> seeds) {
        SweepRequest request;
        request.config = busy_market_config(1 * kNanosPerSecond);
        request.config.name = "compare";
        request.config.strategy.kind = StrategyKind::FixedSpreadMarketMaker;
        request.config.strategy.quoting.half_spread_ticks = half_spread_ticks;
        request.config.strategy.quoting.quote_size = 10;
        request.config_source_text = canonical_config_text(request.config);
        request.master_seeds = std::move(seeds);
        request.sweep_directory = directory;
        request.build.compiler = "test";
        return execute_sweep(request);
    };
    const auto narrow_outcomes = run_sweep(narrow_directory, 1, {1, 2});
    const auto wide_outcomes = run_sweep(wide_directory, 3, {2, 3});
    REQUIRE_EQ(narrow_outcomes.size(), 2u);
    REQUIRE_EQ(wide_outcomes.size(), 2u);
    for (const auto& outcome : narrow_outcomes) CHECK(outcome.state == RunState::Completed);
    CHECK(std::filesystem::exists(narrow_directory / "sweep.json"));
    CHECK(std::filesystem::exists(narrow_directory / "seed-1" / "summary.json"));

    const RunSet narrow = load_run_set(narrow_directory);
    const RunSet wide = load_run_set(wide_directory);
    CHECK(narrow.is_sweep);
    REQUIRE_EQ(narrow.runs.size(), 2u);
    CHECK(narrow.runs[0].state == "completed");
    CHECK(!narrow.runs[0].metrics.empty());

    const ComparisonReport report = compare_run_sets(narrow, wide);
    REQUIRE_EQ(report.paired_seeds.size(), 1u);
    CHECK_EQ(report.paired_seeds[0], 2u);
    REQUIRE_EQ(report.unpaired_seeds_a.size(), 1u);
    CHECK_EQ(report.unpaired_seeds_a[0], 1u);
    REQUIRE_EQ(report.unpaired_seeds_b.size(), 1u);
    CHECK_EQ(report.unpaired_seeds_b[0], 3u);
    CHECK(!report.market_world_differs);
    bool half_spread_listed = false;
    for (const auto& difference : report.config_differences)
        if (difference.key.find("half_spread_ticks") != std::string::npos) half_spread_listed = true;
    CHECK(half_spread_listed);
    CHECK(!report.metrics.empty());
    for (const auto& metric : report.metrics) CHECK_LE(metric.pairs + metric.unavailable, 1u);
    const std::string text = render_comparison_text(report);
    CHECK(text.find("half_spread_ticks") != std::string::npos);
    CHECK(render_comparison_json(report).find("exsim.research.compare/1") != std::string::npos);

    std::filesystem::remove_all(narrow_directory);
    std::filesystem::remove_all(wide_directory);
}

TEST_CASE(research_compare, unfinished_runs_are_excluded_and_nulls_are_unavailable) {
    RunSet a;
    RunSet b;
    a.runs = {completed_run(1, 5.0), completed_run(2, std::nullopt), completed_run(3, 1.0)};
    b.runs = {completed_run(1, 7.0), completed_run(2, 4.0), completed_run(3, 2.0)};
    b.runs[2].state = "cancelled";
    const ComparisonReport report = compare_run_sets(a, b);
    REQUIRE_EQ(report.excluded_runs.size(), 1u);
    CHECK_EQ(report.excluded_runs[0].side, 'B');
    CHECK_EQ(report.excluded_runs[0].master_seed, 3u);
    CHECK(report.excluded_runs[0].state == "cancelled");
    REQUIRE_EQ(report.unpaired_seeds_a.size(), 1u);
    CHECK_EQ(report.unpaired_seeds_a[0], 3u);
    REQUIRE_EQ(report.metrics.size(), 1u);
    CHECK_EQ(report.metrics[0].pairs, 1u);
    CHECK_EQ(report.metrics[0].unavailable, 1u);
    CHECK_EQ(report.metrics[0].mean_difference, 2.0);
    CHECK(!report.metrics[0].sd_difference.has_value());  // needs two pairs

    // A different market world is flagged, not silently compared.
    b.runs[0].market_world = "historical_replay";
    CHECK(compare_run_sets(a, b).market_world_differs);
}

TEST_CASE(research_config, selected_full_session_protocol_is_validated_and_part_of_identity) {
    auto ouch = parse_experiment_config("[strategy]\nprotocol = ouch42\n");
    auto fix = parse_experiment_config("[strategy]\nprotocol = fix44\n");
    auto sbe = parse_experiment_config("[strategy]\nprotocol = fixp_sbe\n");
    CHECK_EQ(ouch.order_entry_protocol,server::Protocol::Ouch42);
    CHECK_EQ(fix.order_entry_protocol,server::Protocol::Fix44);
    CHECK_EQ(sbe.order_entry_protocol,server::Protocol::FixpSbe);
    CHECK_NE(experiment_config_hash(ouch),experiment_config_hash(fix));
    CHECK_NE(experiment_config_hash(fix),experiment_config_hash(sbe));
    CHECK_EQ(config_error_line("[strategy]\nprotocol = imaginary\n"),2u);
    CHECK(canonical_config_text(sbe).find("strategy.protocol=fixp_sbe\n") != std::string::npos);
}
