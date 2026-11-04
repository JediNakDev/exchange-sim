#ifndef EXSIM_RESEARCH_STRATEGIES_TWAP_EXECUTOR_HPP
#define EXSIM_RESEARCH_STRATEGIES_TWAP_EXECUTOR_HPP

// Baseline: time-weighted parent executor.
//   slices = execution_duration / slice_interval; slice k (k = 0..slices-1)
//   fires at start + k * slice_interval with cumulative target
//   parent_qty * (k + 1) / slices. Each slice sends one IOC child for
//   target - filled (known fills), unless a child is still in flight or the
//   opposite side is not visible; later slices catch up. After the last
//   slice nothing more is sent: any unfilled quantity is the residual.
// The parent order (and its arrival benchmark) is declared at start.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategies/parent_order_child.hpp"
#include "exsim/research/strategy_context.hpp"
#include "exsim/research/strategy_messages.hpp"

#include <cstdint>
#include <string_view>

namespace exsim::research::strategies {

class TwapExecutor {
public:
    static constexpr std::string_view kStrategyName = "twap_executor";
    static constexpr std::uint32_t kStrategyVersion = 1;

    explicit TwapExecutor(const StrategyConfig& config) noexcept
        : child_(config.parent),
          slice_count_(std::max<std::uint64_t>(1, config.parent.execution_duration_ns / config.parent.slice_interval_ns)) {}

    void on_start(StrategyContext& context) noexcept { context.wake_at(child_.parameters().start_ns); }
    void on_market_data(const MarketDataUpdate&, StrategyContext&) noexcept {}
    void on_execution_report(const ExecutionReport& report, StrategyContext&) noexcept { (void)child_.on_report(report); }

    void on_wakeup(StrategyContext& context) noexcept {
        const auto& parameters = child_.parameters();
        if (next_slice_ == 0) context.declare_parent_order(parameters.side, parameters.parent_qty);
        if (next_slice_ >= slice_count_) return;
        const std::uint64_t target = std::uint64_t{parameters.parent_qty} * (next_slice_ + 1) / slice_count_;
        if (target > child_.filled_qty()) (void)child_.send_child(context, target - child_.filled_qty());
        ++next_slice_;
        if (next_slice_ < slice_count_) context.wake_at(parameters.start_ns + next_slice_ * parameters.slice_interval_ns);
    }

    [[nodiscard]] std::uint64_t filled_qty() const noexcept { return child_.filled_qty(); }
    [[nodiscard]] std::uint64_t slices_fired() const noexcept { return next_slice_; }
    [[nodiscard]] std::uint64_t slice_count() const noexcept { return slice_count_; }

private:
    ParentOrderChild child_;
    std::uint64_t slice_count_;
    std::uint64_t next_slice_ = 0;
};

}  // namespace exsim::research::strategies

#endif
