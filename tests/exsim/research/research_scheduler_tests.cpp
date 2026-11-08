// Discrete-event scheduler: (time, enqueue sequence) ordering, capacity,
// the declared equal-time policy, and resumable pause/step/bounded runs.

#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/timed_event_queue.hpp"
#include "exsim/sim/random.hpp"
#include "tests/exsim/research/research_test_support.hpp"
#include "tests/framework/test.hpp"

#include <map>
#include <utility>

using namespace research_test;

TEST_CASE(research_scheduler, queue_orders_by_time_then_enqueue_sequence) {
    TimedEventQueue<int> queue(16);
    REQUIRE(queue.push(5, 1));
    REQUIRE(queue.push(3, 2));
    REQUIRE(queue.push(5, 3));
    REQUIRE(queue.push(3, 4));
    REQUIRE(queue.push(1, 5));
    const int expected[] = {5, 2, 4, 1, 3};
    std::uint64_t previous_sequence = 0;
    Timestamp previous_time = 0;
    for (const int payload : expected) {
        REQUIRE(!queue.empty());
        const auto entry = queue.pop();
        CHECK_EQ(entry.payload, payload);
        CHECK_GE(entry.time, previous_time);
        if (entry.time == previous_time) CHECK_GT(entry.sequence, previous_sequence);
        previous_time = entry.time;
        previous_sequence = entry.sequence;
    }
    CHECK(queue.empty());
}

TEST_CASE(research_scheduler, full_queue_rejects_without_dropping) {
    TimedEventQueue<int> queue(4);
    for (int i = 0; i < 4; ++i) REQUIRE(queue.push(10 - i, i));
    CHECK(!queue.push(0, 99));
    CHECK_EQ(queue.size(), 4u);
    CHECK_EQ(queue.pop().payload, 3);
    CHECK_EQ(queue.pop().payload, 2);
    REQUIRE(queue.push(8, 7));
    REQUIRE(queue.push(9, 8));
    CHECK_EQ(queue.high_water_mark(), 4u);
    const int expected[] = {7, 1, 8, 0};
    for (const int payload : expected) CHECK_EQ(queue.pop().payload, payload);
}

TEST_CASE(research_scheduler, randomized_operations_match_reference_order) {
    TimedEventQueue<std::uint64_t> queue(64);
    std::map<std::pair<Timestamp, std::uint64_t>, std::uint64_t> reference;
    sim::Rng rng(12345);
    std::uint64_t next_payload = 0;
    std::uint64_t next_sequence = 0;  // mirrors the queue's enqueue counter
    for (int operation = 0; operation < 20'000; ++operation) {
        const bool push = reference.empty() || (reference.size() < 64 && rng.between(0, 2) != 0);
        if (push) {
            const Timestamp time = rng.between(0, 50);
            REQUIRE(queue.push(time, next_payload));
            reference.emplace(std::make_pair(time, next_sequence++), next_payload++);
        } else {
            const auto entry = queue.pop();
            const auto expected = reference.begin();
            CHECK_EQ(entry.time, expected->first.first);
            CHECK_EQ(entry.payload, expected->second);
            reference.erase(expected);
        }
    }
    CHECK_EQ(queue.size(), reference.size());
}

// Observations run after every simulation event at the same time: the t = 0
// sample is enqueued before the strategy's zero-latency order, yet sees it.
TEST_CASE(research_scheduler, observations_follow_simulation_events_at_equal_time) {
    ExperimentConfig config = quiet_market_config();
    config.latency = LatencyModel{};
    config.latency.feed_ns = 0;
    config.latency.order_ns = 0;
    config.latency.cancel_ns = 0;
    config.latency.ack_ns = 0;
    Simulation<HookStrategy> simulation(config, 1);
    std::vector<Timestamp> report_times;
    simulation.strategy().start = [](StrategyContext& context) {
        CHECK_NE(context.submit_limit_order(Side::BUY, cents(9990), 10, TimeInForce::Day, false), 0u);
    };
    simulation.strategy().report = [&](const ExecutionReport&, StrategyContext& context) { report_times.push_back(context.now()); };
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    REQUIRE(!records.samples.empty());
    CHECK_EQ(records.samples.front().time, 0u);
    CHECK_EQ(records.samples.front().open_bid_qty, 10u);
    REQUIRE_EQ(report_times.size(), 2u);  // accepted, rested
    CHECK_EQ(report_times[0], 0u);
    CHECK_EQ(report_times[1], 0u);
}

TEST_CASE(research_scheduler, pause_step_and_bounded_runs_resume_identically) {
    const ExperimentConfig config = busy_market_config(3 * kNanosPerSecond);
    Simulation<strategies::FixedSpreadMarketMaker> reference(config, 11, config.strategy);
    CHECK(run_to_end(reference) == RunStop::Completed);

    Simulation<strategies::FixedSpreadMarketMaker> piecewise(config, 11, config.strategy);
    CollectedRecords records;
    CHECK(run_until_time(piecewise, 500 * kMillisecond, &records) == RunStop::PausedAtTime);
    CHECK_EQ(piecewise.now(), 500 * kMillisecond);
    CHECK(piecewise.state() == RunState::Paused);
    // Paused at t: everything at or before t is done, nothing after it.
    REQUIRE(!records.commands.empty());
    CHECK_LE(records.commands.back().time, 500 * kMillisecond);
    const std::size_t commands_before_pause = records.commands.size();
    CHECK(piecewise.step() == RunStop::StepBudgetExhausted);
    records.take(piecewise.recorder());
    for (std::size_t i = commands_before_pause; i < records.commands.size(); ++i)
        CHECK_GT(records.commands[i].time, 500 * kMillisecond);
    for (int i = 0; i < 200; ++i) {
        const RunStop stop = piecewise.step();
        if (stop == RunStop::FlushRequired) piecewise.recorder().clear();
        else CHECK(stop == RunStop::StepBudgetExhausted);
    }
    CHECK(run_until_time(piecewise, 1700 * kMillisecond) == RunStop::PausedAtTime);
    while (true) {
        const RunStop stop = piecewise.run(RunBounds{kNoTime, 997});
        if (stop == RunStop::FlushRequired) piecewise.recorder().clear();
        else if (stop != RunStop::StepBudgetExhausted) break;
    }
    CHECK(piecewise.state() == RunState::Completed);
    CHECK_EQ(piecewise.digests().command_digest, reference.digests().command_digest);
    CHECK_EQ(piecewise.digests().event_digest, reference.digests().event_digest);
    CHECK_EQ(piecewise.digests().ledger_digest, reference.digests().ledger_digest);
    CHECK_EQ(piecewise.digests().event_count, reference.digests().event_count);
    CHECK_EQ(piecewise.simulation_events_processed(), reference.simulation_events_processed());
}

TEST_CASE(research_scheduler, run_ends_at_end_time_and_processes_nothing_later) {
    const ExperimentConfig config = busy_market_config(1 * kNanosPerSecond);
    Simulation<strategies::FixedSpreadMarketMaker> simulation(config, 3, config.strategy);
    CollectedRecords records;
    CHECK(run_to_end(simulation, &records) == RunStop::Completed);
    CHECK(simulation.termination() == TerminationReason::EndTimeReached);
    CHECK_EQ(simulation.now(), config.limits.duration_ns);
    REQUIRE(!records.commands.empty());
    for (const auto& command : records.commands) CHECK_LE(command.time, config.limits.duration_ns);
    REQUIRE(!records.samples.empty());
    CHECK_EQ(records.samples.back().time, config.limits.duration_ns);
}

TEST_CASE(research_scheduler, event_limit_fails_the_run_explicitly) {
    ExperimentConfig config = busy_market_config(5 * kNanosPerSecond);
    config.limits.max_simulation_events = 500;
    Simulation<strategies::FixedSpreadMarketMaker> simulation(config, 3, config.strategy);
    CHECK(run_to_end(simulation) == RunStop::Failed);
    CHECK(simulation.state() == RunState::Failed);
    CHECK(simulation.termination() == TerminationReason::EventLimitReached);
    CHECK_EQ(simulation.simulation_events_processed(), 500u);
}

TEST_CASE(research_scheduler, stop_request_cancels_before_the_next_step) {
    const ExperimentConfig config = busy_market_config(5 * kNanosPerSecond);
    Simulation<strategies::FixedSpreadMarketMaker> simulation(config, 3, config.strategy);
    CHECK(run_until_time(simulation, 1 * kNanosPerSecond) == RunStop::PausedAtTime);
    const std::uint64_t processed = simulation.simulation_events_processed();
    simulation.request_stop();
    CHECK(run_to_end(simulation) == RunStop::Cancelled);
    CHECK(simulation.state() == RunState::Cancelled);
    CHECK(simulation.termination() == TerminationReason::CancelRequested);
    CHECK_EQ(simulation.simulation_events_processed(), processed);
}

TEST_CASE(research_scheduler, event_queue_exhaustion_completes_early) {
    ExperimentConfig config = quiet_market_config();
    Simulation<HookStrategy> simulation(config, 1);  // no agents, a passive strategy
    CHECK(run_to_end(simulation) == RunStop::Completed);
    CHECK(simulation.termination() == TerminationReason::EventQueueExhausted);
    CHECK_EQ(simulation.now(), config.limits.duration_ns);
}
