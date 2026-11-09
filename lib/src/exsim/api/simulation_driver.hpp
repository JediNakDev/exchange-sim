#ifndef EXSIM_API_SIMULATION_DRIVER_HPP
#define EXSIM_API_SIMULATION_DRIVER_HPP

// Private to src/exsim/api: a type-erased Simulation<Strategy> plus the
// runner's own artifact writer (the runner's run directory, command journal
// and independent replay check).

#include "research/run_artifacts.hpp"
#include "research/simulation_core.hpp"

#include <memory>
#include <string>

namespace exsim::api {

class SimulationDriver {
public:
    virtual ~SimulationDriver() = default;
    // One scheduler step (Simulation::step).
    virtual research::RunStop step() = 0;
    [[nodiscard]] virtual research::SimulationCore& core() noexcept = 0;
    // Writes and clears the recorder buffers (RunStop::FlushRequired).
    virtual void drain() = 0;
    // Final drain, journal footer, replay check, runner summary.json.
    virtual void finish() = 0;
    [[nodiscard]] virtual bool replay_verified() const noexcept = 0;
};

// Builds the driver for the configured strategy kind. Throws
// research::ArtifactError or std::exception on setup failures, and
// std::invalid_argument for a strategy the API does not execute.
[[nodiscard]] std::unique_ptr<SimulationDriver> make_simulation_driver(const research::ExperimentConfig& config,
                                                                       std::uint64_t master_seed,
                                                                       research::RunArtifactOptions options);

// "<strategy name>/<strategy version>" of the compiled strategy for `kind`.
[[nodiscard]] std::string strategy_build_id(research::StrategyKind kind);

}  // namespace exsim::api

#endif
