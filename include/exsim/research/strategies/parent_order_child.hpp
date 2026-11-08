#ifndef EXSIM_RESEARCH_STRATEGIES_PARENT_ORDER_CHILD_HPP
#define EXSIM_RESEARCH_STRATEGIES_PARENT_ORDER_CHILD_HPP

// Shared state of the execution baselines: one parent order worked through
// IOC child orders priced limit_offset_ticks through the VISIBLE opposite
// best (buy: ask + offset, sell: bid - offset), clamped to the band.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategies/price_grid.hpp"
#include "exsim/research/strategy_context.hpp"
#include "exsim/research/strategy_messages.hpp"

#include <algorithm>
#include <cstdint>

namespace exsim::research::strategies {

class ParentOrderChild {
public:
    explicit ParentOrderChild(const ParentOrderParameters& parameters) noexcept : parameters_(parameters) {}

    [[nodiscard]] const ParentOrderParameters& parameters() const noexcept { return parameters_; }
    [[nodiscard]] std::uint64_t filled_qty() const noexcept { return filled_qty_; }
    [[nodiscard]] std::uint64_t remaining_qty() const noexcept {
        return filled_qty_ >= parameters_.parent_qty ? 0 : parameters_.parent_qty - filled_qty_;
    }
    [[nodiscard]] bool child_in_flight() const noexcept { return child_order_ != 0; }

    // Sends one IOC child for up to `qty`; false when nothing was sent.
    bool send_child(StrategyContext& context, std::uint64_t qty, std::uint64_t target) noexcept {
        ExecutionDecision row;
        row.time = context.now();
        row.target = target;
        last_target_ = target;
        row.filled = filled_qty_;
        row.requested = qty;
        row.child = child_order_;
        if (const auto* order = context.find_order(child_order_)) row.outstanding = order->worst_case_remaining_qty();
        row.headroom = context.position_headroom(parameters_.side);
        row.child_cap = context.risk_limits().max_order_qty;
        row.source_time = context.last_market_data().exchange_time;
        row.source_sequence = context.last_market_data().sequence;
        const auto& instrument = context.instrument();
        const auto& visible = context.top_of_book();
        const bool buying = parameters_.side == Side::BUY;
        row.has_touch = buying ? visible.has_ask : visible.has_bid;
        row.touch = buying ? visible.ask_price : visible.bid_price;
        const Price offset = static_cast<Price>(parameters_.limit_offset_ticks) * instrument.tick_size;
        row.limit = row.has_touch ? clamp_to_band(buying ? row.touch + offset : row.touch - offset, instrument) : 0;
        if (child_order_ != 0) row.reason = "child_in_flight";
        else if (qty == 0) row.reason = "target_satisfied";
        else if (!row.has_touch) row.reason = "no_delivered_touch";
        else {
            const auto capped = std::min<std::int64_t>({static_cast<std::int64_t>(std::min(qty, remaining_qty())),
                                                       row.child_cap, row.headroom});
            row.submitted = round_down_to_lot(capped, instrument.lot_size);
            if (row.submitted == 0) row.reason = "no_headroom_or_lot";
            else {
                child_order_ = context.submit_limit_order(parameters_.side, row.limit, row.submitted, TimeInForce::IOC, false);
                row.child = child_order_;
                row.reason = child_order_ ? "submitted" : to_string(context.last_risk_reject());
                if (!child_order_) row.submitted = 0;
            }
        }
        context.record_execution_decision(row);
        return row.submitted != 0;
    }

    void observe_report(const ExecutionReport& report, StrategyContext& context) noexcept {
        if (child_order_ == 0 || report.cl_ord_id != child_order_) return;
        (void)on_report(report);
        ExecutionDecision row;
        row.time = context.now();
        row.report = true;
        row.result = report;
        row.child = report.cl_ord_id;
        row.filled = filled_qty_;
        row.target = last_target_;
        row.reason = to_string(report.kind);
        context.record_execution_decision(row);
    }

    // Tracks fills and the end of the child; returns true when the child finished.
    bool on_report(const ExecutionReport& report) noexcept {
        if (report.cl_ord_id != child_order_ || child_order_ == 0) return false;
        if (report.kind == ReportKind::Fill && report.side == parameters_.side) filled_qty_ += report.qty;
        const bool done = report.kind == ReportKind::Rejected ||
                          ((report.kind == ReportKind::Fill || report.kind == ReportKind::Canceled) && report.leaves_qty == 0);
        if (done) child_order_ = 0;
        return done;
    }

private:
    ParentOrderParameters parameters_;
    std::uint64_t filled_qty_ = 0;
    ClOrdId child_order_ = 0;
    std::uint64_t last_target_ = 0;
};

}  // namespace exsim::research::strategies

#endif
