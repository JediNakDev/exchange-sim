#ifndef EXSIM_RESEARCH_SIMULATION_HPP
#define EXSIM_RESEARCH_SIMULATION_HPP

// Simulation<Strategy>: the run loop, templated on a compiled strategy.
//
// Every strategy callback is a direct call on the concrete type (no virtual
// dispatch). The loop is resumable: run() returns at a pause point, a step
// budget, a required recorder flush, or a terminal state, and the next run()
// continues exactly where it stopped. Splitting a run into any sequence of
// run()/step() calls produces the same events, ledger and digests.

#include "research/simulation_core.hpp"
#include "research/strategy_context.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace exsim::research {

template <ResearchStrategy Strategy>
class Simulation : public SimulationCore {
public:
    template <typename... StrategyArguments>
    Simulation(const ExperimentConfig& experiment_config, std::uint64_t run_master_seed, StrategyArguments&&... strategy_arguments)
        : SimulationCore(experiment_config, run_master_seed), strategy_(std::forward<StrategyArguments>(strategy_arguments)...) {}

    // Processes events until a stop condition. Strategies must not throw.
    RunStop run(const RunBounds& bounds = {}) {
        begin_run();
        std::uint64_t steps_taken = 0;
        while (true) {
            const RunStop stop = prepare_step(bounds, steps_taken);
            if (stop != RunStop::Continue) return stop;
            ++steps_taken;
            if (next_step_is_observation()) {
                run_observation_step();
                continue;
            }
            SimulationEvent event;
            if (!pop_simulation_event(event)) continue;
            StrategyContext& context = strategy_context();
            switch (event.kind) {
                case SimulationEventKind::StrategyStart: strategy_.on_start(context); break;
                case SimulationEventKind::StrategyWakeup:
                    if (accept_wakeup(event)) strategy_.on_wakeup(context);
                    break;
                case SimulationEventKind::MarketDataDelivery:
                    deliver_market_data(event.data.market_data);
                    strategy_.on_market_data(event.data.market_data, context);
                    break;
                case SimulationEventKind::ExecutionReportDelivery:
                    deliver_execution_report(event.data.report);
                    strategy_.on_execution_report(event.data.report, context);
                    break;
                default: break;
            }
            after_strategy_callback();
        }
    }

    // Exactly one simulation or observation event (or a stop condition).
    RunStop step() { return run(RunBounds{kNoTime, 1}); }

    // Processes every event with time <= time, then pauses.
    RunStop run_until(Timestamp time) { return run(RunBounds{time, RunBounds{}.max_steps}); }

    [[nodiscard]] Strategy& strategy() noexcept { return strategy_; }
    [[nodiscard]] const Strategy& strategy() const noexcept { return strategy_; }
    [[nodiscard]] static constexpr std::string_view strategy_name() noexcept { return Strategy::kStrategyName; }
    [[nodiscard]] static constexpr std::uint32_t strategy_version() noexcept { return Strategy::kStrategyVersion; }

private:
    Strategy strategy_;
};

}  // namespace exsim::research

#endif
