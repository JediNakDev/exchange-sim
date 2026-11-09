#ifndef EXSIM_RESEARCH_PROCESS_SIMULATION_HPP
#define EXSIM_RESEARCH_PROCESS_SIMULATION_HPP
#include "exsim/research/simulation_core.hpp"
#include "exsim/ipc/strategy_wire.hpp"
#include "exsim/research/run_artifacts.hpp"

namespace exsim::research {
// Exchange-owned scheduling and matching. All strategy callbacks execute in
// exsim_traderd. The socket is the only route for observations and actions.
class ProcessSimulation final : public SimulationCore {
public:
    ProcessSimulation(const ExperimentConfig& config, std::uint64_t seed);
    RunStop run(const RunBounds& bounds = {});
    RunStop step() { return run(RunBounds{kNoTime,1}); }
    int trader_pid() const noexcept { return trader_.pid(); }
private:
    void callback(const SimulationEvent& event);
    ipc::TraderProcess trader_;
};
StrategyIdentity builtin_strategy_identity(StrategyKind kind);
} // namespace exsim::research
#endif
