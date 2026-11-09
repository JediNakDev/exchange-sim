#ifndef EXSIM_RESEARCH_PROCESS_SIMULATION_HPP
#define EXSIM_RESEARCH_PROCESS_SIMULATION_HPP
#include "research/simulation_core.hpp"
#include "ipc/strategy_wire.hpp"
#include "research/run_artifacts.hpp"
#include "research/exchange_session.hpp"

namespace exsim::research {
// Exchange-owned scheduling and matching. All strategy callbacks execute in
// exsim_traderd. The socket is the only route for observations and actions.
class ProcessSimulation final : public SimulationCore {
public:
    ProcessSimulation(const ExperimentConfig& config, std::uint64_t seed);
    RunStop run(const RunBounds& bounds = {});
    RunStop step() { return run(RunBounds{kNoTime,1}); }
    // Deterministic boundary interventions for native connectivity/recovery
    // research. They never restart a crashed trader or bypass a session.
    void reconnect_order_entry() noexcept { reconnect_ = true; }
    void drop_next_protocol_report() noexcept { session_.drop_next_report(); }
    void drop_next_market_packet() noexcept { session_.drop_next_market_packet(); }
    SessionStatistics session_statistics() const noexcept { return session_.statistics(); }
    int trader_pid() const noexcept { return trader_.pid(); }
private:
    void callback(const SimulationEvent& event);
    bool reconnect_ = false;
    ExchangeSession session_;
    ipc::TraderProcess trader_;
};
StrategyIdentity builtin_strategy_identity(StrategyKind kind);
} // namespace exsim::research
#endif
