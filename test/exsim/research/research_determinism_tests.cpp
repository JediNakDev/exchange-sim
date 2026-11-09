// Determinism: one manifest plus one master seed gives one run, digest for
// digest and file for file; seeds feed independent, documented streams.

#include "matching_engine.hpp"
#include "research/experiment_runner.hpp"
#include "research/json.hpp"
#include "research/run_artifacts.hpp"
#include "research/seed_derivation.hpp"
#include "research/strategies/fixed_spread_market_maker.hpp"
#include "research/strategies/twap_executor.hpp"
#include "sim/canonical.hpp"
#include "sim/journal.hpp"
#include "test/exsim/research/research_test_support.hpp"
#include "test/framework/test.hpp"

#include <filesystem>
#include <string>

using namespace research_test;

namespace {

struct RunFingerprint {
    RunDigests digests;
    std::uint64_t simulation_events = 0;
    std::int64_t cash = 0;
    std::int64_t position = 0;
    std::uint64_t ledger_chain = 14695981039346656037ull;  // over every collected FillRecord
};

template <typename Strategy, typename... StrategyArguments>
RunFingerprint fingerprint_run(const ExperimentConfig& config, std::uint64_t seed, StrategyArguments&&... arguments) {
    Simulation<Strategy> simulation(config, seed, std::forward<StrategyArguments>(arguments)...);
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    RunFingerprint fingerprint;
    fingerprint.digests = simulation.digests();
    fingerprint.simulation_events = simulation.simulation_events_processed();
    fingerprint.cash = simulation.ledger().state().cash;
    fingerprint.position = simulation.ledger().state().position;
    for (const auto& fill : records.fills) fingerprint.ledger_chain = hash_fill_record(fill, fingerprint.ledger_chain);
    return fingerprint;
}

RunRequest busy_run_request(const std::filesystem::path& directory, std::uint64_t seed) {
    RunRequest request;
    request.config = busy_market_config(2 * kNanosPerSecond);
    request.config.name = "determinism";
    request.config_source_text = canonical_config_text(request.config);
    request.master_seed = seed;
    request.run_directory = directory;
    request.build.compiler = "test";
    request.steps_per_chunk = 1'000;  // many chunks: chunking must not change anything
    return request;
}

}  // namespace

TEST_CASE(research_determinism, same_seed_reproduces_every_digest) {
    const ExperimentConfig config = busy_market_config(3 * kNanosPerSecond);
    const RunFingerprint first = fingerprint_run<strategies::FixedSpreadMarketMaker>(config, 7, config.strategy);
    const RunFingerprint second = fingerprint_run<strategies::FixedSpreadMarketMaker>(config, 7, config.strategy);
    CHECK_GT(first.digests.fill_count, 0u);
    CHECK_EQ(first.digests.command_digest, second.digests.command_digest);
    CHECK_EQ(first.digests.event_digest, second.digests.event_digest);
    CHECK_EQ(first.digests.ledger_digest, second.digests.ledger_digest);
    CHECK_EQ(first.digests.command_count, second.digests.command_count);
    CHECK_EQ(first.digests.event_count, second.digests.event_count);
    CHECK_EQ(first.simulation_events, second.simulation_events);
    CHECK_EQ(first.cash, second.cash);
    CHECK_EQ(first.position, second.position);
    CHECK_EQ(first.ledger_chain, second.ledger_chain);
    // The core's ledger digest is the same chain over the recorded fills.
    CHECK_EQ(first.ledger_chain, first.digests.ledger_digest);
}

TEST_CASE(research_determinism, different_seed_changes_the_run) {
    const ExperimentConfig config = busy_market_config(3 * kNanosPerSecond);
    const RunFingerprint seed_seven = fingerprint_run<strategies::FixedSpreadMarketMaker>(config, 7, config.strategy);
    const RunFingerprint seed_eight = fingerprint_run<strategies::FixedSpreadMarketMaker>(config, 8, config.strategy);
    CHECK_NE(seed_seven.digests.command_digest, seed_eight.digests.command_digest);
    CHECK_NE(seed_seven.digests.event_digest, seed_eight.digests.event_digest);
    CHECK_NE(seed_seven.digests.ledger_digest, seed_eight.digests.ledger_digest);
}

TEST_CASE(research_determinism, derived_seeds_follow_the_documented_formula) {
    const ExperimentConfig config = busy_market_config(1 * kNanosPerSecond);
    Simulation<HookStrategy> simulation(config, 42);
    const auto& seeds = simulation.derived_seeds();
    REQUIRE_EQ(seeds.size(), 4u);
    CHECK_EQ(seeds[0].stream, std::string("strategy"));
    CHECK_EQ(seeds[1].stream, std::string("latency"));
    CHECK_EQ(seeds[2].stream, std::string("agent.0.market_maker.liquidity"));
    CHECK_EQ(seeds[3].stream, std::string("agent.1.noise_taker.flow"));
    for (const auto& seed : seeds) CHECK_EQ(seed.seed, derive_stream_seed(42, seed.stream));
    // Hand-computed reference for the formula itself (master 42, label "strategy").
    std::uint64_t label_hash = 14695981039346656037ull;
    for (const char character : std::string_view("strategy")) {
        label_hash ^= static_cast<unsigned char>(character);
        label_hash *= 1099511628211ull;
    }
    std::uint64_t z = (42 ^ label_hash) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    CHECK_EQ(seeds[0].seed, z ^ (z >> 31));
    // Streams are distinct from each other and from the master seed.
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        CHECK_NE(seeds[i].seed, 42u);
        for (std::size_t j = i + 1; j < seeds.size(); ++j) CHECK_NE(seeds[i].seed, seeds[j].seed);
    }
}

TEST_CASE(research_determinism, agent_streams_do_not_depend_on_the_strategy) {
    ExperimentConfig market_making = busy_market_config(1 * kNanosPerSecond);
    ExperimentConfig execution = market_making;
    execution.strategy.kind = StrategyKind::TwapExecutor;
    execution.strategy.parent.start_ns = 100 * kMillisecond;
    execution.strategy.parent.execution_duration_ns = 500 * kMillisecond;
    execution.strategy.parent.slice_interval_ns = 100 * kMillisecond;
    Simulation<strategies::FixedSpreadMarketMaker> quoting(market_making, 9, market_making.strategy);
    Simulation<strategies::TwapExecutor> twap(execution, 9, execution.strategy);
    REQUIRE_EQ(quoting.derived_seeds().size(), twap.derived_seeds().size());
    for (std::size_t i = 0; i < quoting.derived_seeds().size(); ++i) {
        CHECK_EQ(quoting.derived_seeds()[i].stream, twap.derived_seeds()[i].stream);
        CHECK_EQ(quoting.derived_seeds()[i].seed, twap.derived_seeds()[i].seed);
    }
}

// A strategy that never trades cannot move the market, however many numbers
// it draws from its own stream: the exogenous agents keep their streams.
TEST_CASE(research_determinism, strategy_rng_draws_do_not_change_the_market) {
    const ExperimentConfig config = busy_market_config(2 * kNanosPerSecond);
    Simulation<HookStrategy> passive(config, 5);
    CHECK(run_to_end(passive) == RunStop::Completed);

    Simulation<HookStrategy> drawing(config, 5);
    std::uint64_t draws = 0;
    drawing.strategy().market_data = [&](const MarketDataUpdate&, StrategyContext& context) {
        for (int i = 0; i < 3; ++i) (void)context.rng().next();
        draws += 3;
    };
    CHECK(run_to_end(drawing) == RunStop::Completed);
    CHECK_GT(draws, 0u);
    CHECK_EQ(passive.digests().event_digest, drawing.digests().event_digest);
    CHECK_EQ(passive.digests().command_digest, drawing.digests().command_digest);
}

TEST_CASE(research_determinism, rerun_artifacts_are_byte_identical_and_replay_verifies) {
    const std::filesystem::path first_directory = fresh_test_directory("determinism_first");
    const std::filesystem::path second_directory = fresh_test_directory("determinism_second");
    const RunOutcome first = execute_run(busy_run_request(first_directory, 3));
    const RunOutcome second = execute_run(busy_run_request(second_directory, 3));
    CHECK(first.state == RunState::Completed);
    CHECK(first.replay_verified);
    CHECK(second.replay_verified);
    CHECK_EQ(first.run_id, second.run_id);
    CHECK_EQ(first.digests.event_digest, second.digests.event_digest);

    for (const std::string_view name : kRunArtifactFileNames) {
        if (name == "status.json" || name == "status.json.tmp") continue;
        const std::filesystem::path first_file = first_directory / name;
        REQUIRE(std::filesystem::exists(first_file));
        CHECK(read_text_file(first_file) == read_text_file(second_directory / name));
    }
    CHECK(!std::filesystem::exists(first_directory / "status.json.tmp"));

    // The command journal replays through a fresh engine to the online event digest.
    sim::JournalReader reader((first_directory / "commands.journal").string());
    MatchingEngine engine(reader.setup().engine);
    for (const auto& instrument : reader.setup().instruments) REQUIRE(engine.add_instrument(instrument));
    EventBuffer output(required_event_capacity(reader.setup().engine));
    Command command;
    std::uint64_t replay_digest = 14695981039346656037ull;
    std::uint64_t replay_commands = 0;
    std::uint64_t replay_events = 0;
    while (reader.next(command)) {
        output.clear();
        REQUIRE(engine.handle(command, output) == HandleResult::Applied);
        ++replay_commands;
        replay_events += output.size();
        for (const auto& event : output) replay_digest = sim::hash_event(event, replay_digest);
    }
    CHECK_EQ(replay_digest, first.digests.event_digest);
    CHECK_EQ(replay_commands, first.digests.command_count);
    CHECK_EQ(replay_events, first.digests.event_count);

    // summary.json and status.json agree with the outcome.
    const JsonValue summary = parse_json(read_text_file(first_directory / "summary.json"));
    const JsonValue* digests = summary.find("digests");
    REQUIRE(digests != nullptr);
    CHECK_EQ(digests->find("event_digest")->text, std::to_string(first.digests.event_digest));
    CHECK(digests->find("replay_verified")->boolean);
    const JsonValue status = parse_json(read_text_file(first_directory / "status.json"));
    CHECK_EQ(status.find("state")->text, std::string("completed"));
    CHECK(status.find("summary_written")->boolean);

    std::filesystem::remove_all(first_directory);
    std::filesystem::remove_all(second_directory);
}

TEST_CASE(research_determinism, run_directory_is_never_silently_reused) {
    const std::filesystem::path directory = fresh_test_directory("determinism_reuse");
    RunRequest request = busy_run_request(directory, 4);
    request.config.limits.duration_ns = 200 * kMillisecond;
    (void)execute_run(request);
    bool refused = false;
    try {
        (void)execute_run(request);
    } catch (const ArtifactError&) {
        refused = true;
    }
    CHECK(refused);
    request.overwrite = true;
    CHECK(execute_run(request).replay_verified);

    // A directory holding a file the runner does not own is never cleared.
    write_file_atomically(directory / "notes.txt", "keep me");
    refused = false;
    try {
        (void)execute_run(request);
    } catch (const ArtifactError&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(std::filesystem::exists(directory / "notes.txt"));
    std::filesystem::remove_all(directory);
}
