#ifndef EXSIM_RESEARCH_STRATEGIES_IMMEDIATE_SWEEP_EXECUTOR_HPP
#define EXSIM_RESEARCH_STRATEGIES_IMMEDIATE_SWEEP_EXECUTOR_HPP

// Baseline: take the whole parent immediately.
//   At start: declare the parent and send one IOC child for the full
//   remaining quantity (capped by max_order_qty and position headroom),
//   limit_offset_ticks through the visible opposite best. While quantity
//   remains, re-sweep on the next delivered market-data update after each
//   child finishes, until execution_duration has elapsed since start.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategies/parent_order_child.hpp"
#include "exsim/research/strategy_context.hpp"
#include "exsim/research/strategy_messages.hpp"

#include <cstdint>
#include <string_view>

namespace exsim::research::strategies {

class ImmediateSweepExecutor {
public:
    static constexpr std::string_view kStrategyName = "immediate_sweep_executor";
    static constexpr std::uint32_t kStrategyVersion = 1;

    explicit ImmediateSweepExecutor(const StrategyConfig& config) noexcept : child_(config.parent) {}

    void on_start(StrategyContext& context) noexcept { context.wake_at(child_.parameters().start_ns); }

    void on_wakeup(StrategyContext& context) noexcept {
        if (started_) return;
        started_ = true;
        context.declare_parent_order(child_.parameters().side, child_.parameters().parent_qty);
        sweep(context);
    }

    void on_market_data(const MarketDataUpdate&, StrategyContext& context) noexcept {
        if (started_) sweep(context);
    }

    void on_execution_report(const ExecutionReport& report, StrategyContext& context) noexcept { child_.observe_report(report, context); }

    [[nodiscard]] std::uint64_t filled_qty() const noexcept { return child_.filled_qty(); }
    [[nodiscard]] std::uint64_t sweeps_sent() const noexcept { return sweeps_sent_; }

private:
    void sweep(StrategyContext& context) noexcept {
        const auto& parameters = child_.parameters();
        if (child_.remaining_qty() == 0) return;
        if (context.now() > parameters.start_ns + parameters.execution_duration_ns) return;
        if (child_.send_child(context, child_.remaining_qty(), parameters.parent_qty)) ++sweeps_sent_;
    }

    ParentOrderChild child_;
    bool started_ = false;
    std::uint64_t sweeps_sent_ = 0;
};

}  // namespace exsim::research::strategies

#endif
