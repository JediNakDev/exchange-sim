#include "research_test_support.hpp"
#include "research/process_simulation.hpp"
#include "research/strategies/fixed_spread_market_maker.hpp"
#include "research/strategies/inventory_aware_market_maker.hpp"
#include "research/strategies/twap_executor.hpp"
#include "research/strategies/immediate_sweep_executor.hpp"
#include "test/framework/test.hpp"
#include "test/framework/alloc_counter.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace {
using namespace research_test;
template<class SimulationType>
void write_run(SimulationType& simulation, const std::filesystem::path& directory) {
    RunArtifactOptions options;
    options.run_directory = directory;
    options.run_id = "process-equivalence";
    RunArtifactWriter writer(simulation,builtin_strategy_identity(simulation.config().strategy.kind),options);
    writer.begin();
    for (;;) {
        const auto stop = simulation.run();
        if (stop == RunStop::FlushRequired) { writer.drain(simulation.recorder()); continue; }
        REQUIRE_EQ(stop,RunStop::Completed);
        break;
    }
    writer.finish(simulation.recorder());
    CHECK(writer.replay_verified());
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw std::runtime_error("missing equivalence artifact");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
template<class Strategy>
void equivalent(StrategyKind kind) {
    auto config = busy_market_config(500 * kMillisecond);
    config.strategy.kind = kind;
    config.strategy.parent.start_ns = 0;
    config.strategy.parent.parent_qty = 100;
    config.strategy.parent.execution_duration_ns = 200 * kMillisecond;
    config.strategy.parent.slice_interval_ns = 20 * kMillisecond;
    validate_experiment_config(config);
    const auto root = std::filesystem::temp_directory_path()/("exsim-process-equality-"+std::to_string(::getpid())+"-"+std::to_string(static_cast<int>(kind)));
    std::filesystem::remove_all(root);
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
    config.order_entry_protocol = protocol;
    for (const std::uint64_t seed : {7u,42u}) {
        Simulation<Strategy> local(config,seed,config.strategy);
        ProcessSimulation remote(config,seed);
        CHECK(remote.trader_pid() != ::getpid());
        write_run(local,root/"local");
        write_run(remote,root/"remote");
        CHECK_GT(local.metrics().new_orders_submitted,0u);
        CHECK_GT(remote.metrics().new_orders_submitted,0u);
        CHECK(remote.session_statistics().established);
        CHECK_GT(remote.session_statistics().decoded_orders,0u);
        CHECK_GT(remote.session_statistics().report_bytes,0u);
        CHECK_GT(remote.session_statistics().itch_messages,0u);
        bool equal = true;
        for (const auto* file : {"commands.journal","commands.jsonl","events.jsonl","ledger.jsonl","lifecycle.jsonl","execution-decisions.jsonl","markouts.jsonl","series.csv","summary.json"}) {
            const bool same = read(root/"local"/file) == read(root/"remote"/file);
            if (!same) std::cerr << "artifact mismatch: " << root << "/" << file << '\n';
            CHECK(same);
            equal &= same;
        }
        if (equal) std::filesystem::remove_all(root);
        else return;
    }
    }
}
}
TEST_CASE(process_simulation, fixed_spread_matches_in_process_artifacts) { equivalent<strategies::FixedSpreadMarketMaker>(StrategyKind::FixedSpreadMarketMaker); }
TEST_CASE(process_simulation, inventory_aware_matches_in_process_artifacts) { equivalent<strategies::InventoryAwareMarketMaker>(StrategyKind::InventoryAwareMarketMaker); }
TEST_CASE(process_simulation, twap_matches_in_process_artifacts) { equivalent<strategies::TwapExecutor>(StrategyKind::TwapExecutor); }
TEST_CASE(process_simulation, sweep_matches_in_process_artifacts) { equivalent<strategies::ImmediateSweepExecutor>(StrategyKind::ImmediateSweepExecutor); }
TEST_CASE(process_simulation, killed_trader_fails_next_callback_and_is_reaped) {
    int child = -1;
    {
        auto config = quiet_market_config();
        ProcessSimulation simulation(config,1);
        child = simulation.trader_pid();
        REQUIRE_EQ(::kill(child,SIGKILL),0);
        bool failed = false;
        try { (void)simulation.run(); } catch (const std::exception&) { failed = true; }
        CHECK(failed);
    }
    CHECK_EQ(::waitpid(child,nullptr,WNOHANG),-1);
    CHECK_EQ(errno,ECHILD);
}
TEST_CASE(process_simulation, stepping_and_resuming_matches_uninterrupted_run) {
    auto config = busy_market_config(100 * kMillisecond);
    ProcessSimulation stepped(config,99);
    REQUIRE_EQ(stepped.step(),RunStop::StepBudgetExhausted);
    REQUIRE_EQ(stepped.step(),RunStop::StepBudgetExhausted);
    REQUIRE_EQ(stepped.run(),RunStop::Completed);
    ProcessSimulation uninterrupted(config,99);
    REQUIRE_EQ(uninterrupted.run(),RunStop::Completed);
    CHECK_EQ(stepped.digests().command_digest,uninterrupted.digests().command_digest);
    CHECK_EQ(stepped.digests().event_digest,uninterrupted.digests().event_digest);
    CHECK_EQ(stepped.digests().ledger_digest,uninterrupted.digests().ledger_digest);
}
TEST_CASE(process_simulation, exchange_callback_transport_does_not_allocate_after_startup) {
    if (!testfw::allocation_counting_available()) return;
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
        auto config = busy_market_config(100 * kMillisecond);
        config.order_entry_protocol = protocol;
        ProcessSimulation simulation(config,73);
        RunStop stop;
        std::size_t allocations;
        {
            testfw::AllocationScope scope;
            stop = simulation.run();
            allocations = scope.count();
        }
        CHECK_EQ(stop,RunStop::Completed);
        CHECK_EQ(allocations,0u);
    }
}

TEST_CASE(process_sessions, reconnect_and_recover_reports_and_itch_without_changing_outcomes) {
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
        auto config = busy_market_config(500 * kMillisecond);
        config.order_entry_protocol = protocol;
        ProcessSimulation baseline(config,17);
        REQUIRE_EQ(baseline.run(RunBounds{150*kMillisecond}),RunStop::PausedAtTime);
        const auto baseline_position = baseline.strategy_view().known_position;
        const auto baseline_open = baseline.strategy_view().open_order_count;
        const auto baseline_top = baseline.strategy_view().top_of_book;
        REQUIRE_EQ(baseline.run(),RunStop::Completed);
        ProcessSimulation interrupted(config,17);
        interrupted.drop_next_protocol_report();
        interrupted.drop_next_market_packet();
        interrupted.reconnect_order_entry();
        REQUIRE_EQ(interrupted.run(RunBounds{150*kMillisecond}),RunStop::PausedAtTime);
        CHECK_EQ(interrupted.strategy_view().known_position,baseline_position);
        CHECK_EQ(interrupted.strategy_view().open_order_count,baseline_open);
        CHECK(interrupted.strategy_view().top_of_book == baseline_top);
        const auto paused_position = interrupted.strategy_view().known_position;
        CHECK_EQ(paused_position,static_cast<std::int64_t>(interrupted.strategy_view().known_bought_qty)-static_cast<std::int64_t>(interrupted.strategy_view().known_sold_qty));
        interrupted.reconnect_order_entry();
        REQUIRE_EQ(interrupted.run(),RunStop::Completed);
        CHECK_EQ(interrupted.digests().command_digest,baseline.digests().command_digest);
        CHECK_EQ(interrupted.digests().event_digest,baseline.digests().event_digest);
        CHECK_EQ(interrupted.digests().ledger_digest,baseline.digests().ledger_digest);
        const auto stats = interrupted.session_statistics();
        CHECK_GE(stats.reconnects,3u);
        CHECK_EQ(stats.reports_dropped,1u);
        CHECK_EQ(stats.packets_dropped,1u);
        CHECK_GT(stats.retransmission_requests,0u);
    }
}
TEST_CASE(process_sessions, idle_sessions_use_simulated_heartbeats_and_survive_pauses) {
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
        auto config = quiet_market_config();
        config.limits.duration_ns = 61*kNanosPerSecond;
        config.order_entry_protocol = protocol;
        ProcessSimulation simulation(config,18);
        REQUIRE_EQ(simulation.run(RunBounds{31*kNanosPerSecond}),RunStop::PausedAtTime);
        REQUIRE_EQ(simulation.run(),RunStop::Completed);
        CHECK(simulation.session_statistics().established);
    }
}

TEST_CASE(process_sessions, busy_sessions_cross_heartbeat_boundaries_without_reordering_reports) {
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
        auto config = busy_market_config(2200*kMillisecond);
        config.latency.ack_ns = 50*kMillisecond;
        config.order_entry_protocol = protocol;
        Simulation<strategies::FixedSpreadMarketMaker> reference(config,41,config.strategy);
        ProcessSimulation remote(config,41);
        REQUIRE_EQ(reference.run(),RunStop::Completed);
        REQUIRE_EQ(remote.run(),RunStop::Completed);
        CHECK_EQ(remote.digests().command_digest,reference.digests().command_digest);
        CHECK_EQ(remote.digests().event_digest,reference.digests().event_digest);
        CHECK_EQ(remote.digests().ledger_digest,reference.digests().ledger_digest);
    }
}

TEST_CASE(process_sessions, session_control_never_overtakes_delayed_reports) {
    for (const auto protocol : {server::Protocol::Ouch42,server::Protocol::Fix44,server::Protocol::FixpSbe}) {
        auto config = busy_market_config(2200*kMillisecond);
        config.order_entry_protocol = protocol;
        config.agents.resize(1);
        config.agents[0].timer_interval_ns = 10*kNanosPerSecond;
        config.strategy.quoting.requote_interval_ns = 5*kNanosPerSecond;
        config.latency = LatencyModel{};
        config.latency.feed_ns = config.latency.order_ns = config.latency.cancel_ns = 0;
        config.latency.ack_ns = 1500*kMillisecond;
        Simulation<strategies::FixedSpreadMarketMaker> reference(config,41,config.strategy);
        ProcessSimulation remote(config,41);
        REQUIRE_EQ(reference.run(RunBounds{kNanosPerSecond}),RunStop::PausedAtTime);
        REQUIRE_EQ(remote.run(RunBounds{kNanosPerSecond}),RunStop::PausedAtTime);
        CHECK_EQ(remote.metrics().reports_delivered,0u);
        CHECK_EQ(remote.strategy_view().known_position,0);
        CHECK_EQ(remote.strategy_view().open_order_count,reference.strategy_view().open_order_count);
        REQUIRE_EQ(reference.run(),RunStop::Completed);
        REQUIRE_EQ(remote.run(),RunStop::Completed);
        CHECK_EQ(remote.session_statistics().reconnects,0u);
        CHECK_EQ(remote.digests().command_digest,reference.digests().command_digest);
        CHECK_EQ(remote.digests().event_digest,reference.digests().event_digest);
        CHECK_EQ(remote.digests().ledger_digest,reference.digests().ledger_digest);
    }
}
