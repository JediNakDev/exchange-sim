#include "research_test_support.hpp"
#include "exsim/research/process_simulation.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/strategies/inventory_aware_market_maker.hpp"
#include "exsim/research/strategies/twap_executor.hpp"
#include "exsim/research/strategies/immediate_sweep_executor.hpp"
#include "test/framework/test.hpp"
#include "test/framework/alloc_counter.hpp"
#include <csignal>
#include <fstream>
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
    for (const std::uint64_t seed : {7u,42u}) {
        Simulation<Strategy> local(config,seed,config.strategy);
        ProcessSimulation remote(config,seed);
        CHECK(remote.trader_pid() != ::getpid());
        write_run(local,root/"local");
        write_run(remote,root/"remote");
        CHECK_GT(local.metrics().new_orders_submitted,0u);
        for (const auto* file : {"commands.journal","commands.jsonl","events.jsonl","ledger.jsonl","lifecycle.jsonl","execution-decisions.jsonl","markouts.jsonl","series.csv","summary.json"})
            CHECK_EQ(read(root/"local"/file),read(root/"remote"/file));
        std::filesystem::remove_all(root);
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
    auto config = busy_market_config(100 * kMillisecond);
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
