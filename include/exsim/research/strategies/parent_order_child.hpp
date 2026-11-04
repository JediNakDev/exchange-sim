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
    bool send_child(StrategyContext& context, std::uint64_t qty) noexcept {
        if (child_order_ != 0 || qty == 0) return false;
        const auto& instrument = context.instrument();
        const TopOfBook& visible = context.top_of_book();
        const bool buying = parameters_.side == Side::BUY;
        if (buying ? !visible.has_ask : !visible.has_bid) return false;
        const Price offset = static_cast<Price>(parameters_.limit_offset_ticks) * instrument.tick_size;
        const Price limit = clamp_to_band(buying ? visible.ask_price + offset : visible.bid_price - offset, instrument);
        const std::int64_t capped = std::min<std::int64_t>(
            {static_cast<std::int64_t>(std::min<std::uint64_t>(qty, remaining_qty())),
             static_cast<std::int64_t>(context.risk_limits().max_order_qty), context.position_headroom(parameters_.side)});
        const Quantity child_qty = round_down_to_lot(capped, instrument.lot_size);
        if (child_qty == 0) return false;
        child_order_ = context.submit_limit_order(parameters_.side, limit, child_qty, TimeInForce::IOC, false);
        return child_order_ != 0;
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
};

}  // namespace exsim::research::strategies

#endif
