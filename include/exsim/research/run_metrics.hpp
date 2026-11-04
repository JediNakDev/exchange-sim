#ifndef EXSIM_RESEARCH_RUN_METRICS_HPP
#define EXSIM_RESEARCH_RUN_METRICS_HPP

// Allocation-free metric accumulators updated inside the simulated hot path.
// Exact quantities are integers (price units, shares, nanoseconds); derived
// floating-point statistics are computed only when summary.json is written.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategy_context.hpp"
#include "exsim/research/strategy_messages.hpp"
#include "exsim/types.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace exsim::research {

// count/min/max/sum of nanosecond durations (sum saturates).
struct DurationStatistic {
    std::uint64_t count = 0;
    Timestamp min = std::numeric_limits<Timestamp>::max();
    Timestamp max = 0;
    std::uint64_t sum = 0;

    void add(Timestamp value) noexcept {
        ++count;
        if (value < min) min = value;
        if (value > max) max = value;
        sum = value > std::numeric_limits<std::uint64_t>::max() - sum ? std::numeric_limits<std::uint64_t>::max() : sum + value;
    }
};

struct MarkoutAccumulator {
    std::uint64_t measured = 0;
    std::uint64_t unavailable = 0;  // no two-sided exchange book at the horizon
    std::uint64_t censored = 0;     // horizon past the run end
    std::uint64_t measured_qty = 0;
    std::int64_t total_times_two = 0;  // price units * shares, doubled
};

struct ParentOrderTracking {
    bool declared = false;
    Side side = Side::BUY;
    Quantity qty = 0;
    Timestamp decision_time = 0;
    bool has_decision_mid = false;
    std::int64_t decision_mid_times_two = 0;   // exchange truth at decision time (arrival benchmark)
    TopOfBook visible_at_decision;             // what the strategy saw
    std::uint64_t market_volume_at_decision = 0;
    std::uint64_t market_volume_at_last_fill = 0;
    Timestamp first_fill_time = 0;
    Timestamp last_fill_time = 0;
    std::uint64_t filled_qty = 0;          // fills on the parent side
    std::int64_t fill_notional = 0;        // price units
    std::int64_t fees = 0;
    std::uint64_t opposite_side_qty = 0;   // fills against the parent side (should be zero)
};

struct RunMetrics {
    // ---- Engine / pipeline volume ----------------------------------------
    std::uint64_t engine_commands = 0;
    std::uint64_t strategy_commands = 0;
    std::uint64_t background_commands = 0;
    std::uint64_t scripted_commands = 0;
    std::uint64_t engine_events = 0;
    std::uint64_t market_data_published = 0;
    std::uint64_t market_data_delivered = 0;
    std::uint64_t reports_scheduled = 0;
    std::uint64_t reports_delivered = 0;
    std::uint64_t strategy_wakeups = 0;
    std::uint64_t market_trades = 0;
    std::uint64_t market_traded_qty = 0;

    // ---- Strategy requests (host) ----------------------------------------
    std::uint64_t new_orders_submitted = 0;
    std::uint64_t cancel_requests_submitted = 0;
    std::uint64_t replace_requests_submitted = 0;
    std::uint64_t risk_rejects = 0;
    std::array<std::uint64_t, kRiskRejectReasonCount> risk_rejects_by_reason{};
    std::uint64_t cancels_resent_after_ack = 0;

    // ---- Exchange outcomes for strategy orders (engine truth) -------------
    std::uint64_t orders_accepted = 0;
    std::uint64_t orders_rejected = 0;
    std::uint64_t cancel_requests_rejected = 0;
    std::uint64_t replace_requests_rejected = 0;
    std::uint64_t orders_canceled_by_request = 0;
    std::uint64_t ioc_remainders_canceled = 0;
    std::uint64_t self_trade_cancels = 0;
    std::uint64_t replaces_applied = 0;
    std::uint64_t replaces_priority_kept = 0;
    std::uint64_t orders_with_fill = 0;
    std::uint64_t orders_filled_completely = 0;
    std::uint64_t orders_ended_partially_filled = 0;  // canceled with cum > 0
    std::uint64_t accepted_order_qty = 0;              // sum of accepted NewOrder quantities
    std::uint64_t replace_added_qty = 0;               // sum of positive total-quantity increases

    // ---- Fills (engine truth) ---------------------------------------------
    std::uint64_t fill_count = 0;
    std::uint64_t filled_qty = 0;
    std::uint64_t maker_fill_count = 0;
    std::uint64_t maker_qty = 0;
    std::uint64_t taker_fill_count = 0;
    std::uint64_t taker_qty = 0;
    std::uint64_t auction_fill_count = 0;
    std::uint64_t auction_qty = 0;
    std::int64_t maker_fees = 0;
    std::int64_t taker_fees = 0;
    std::int64_t auction_fees = 0;
    std::int64_t buy_notional = 0;
    std::int64_t sell_notional = 0;
    DurationStatistic time_to_first_fill;  // decision of the order -> its first fill

    // ---- Spread capture and markouts ---------------------------------------
    std::uint64_t spread_capture_fills = 0;
    std::uint64_t spread_capture_qty = 0;
    std::uint64_t spread_capture_unavailable = 0;
    std::int64_t spread_capture_total_times_two = 0;
    std::array<MarkoutAccumulator, kMaxMarkoutHorizons> markouts{};

    // ---- Quote exposure (engine truth, strategy resting orders) ------------
    Timestamp bid_exposure_ns = 0;
    Timestamp ask_exposure_ns = 0;
    Timestamp two_sided_exposure_ns = 0;
    Timestamp any_side_exposure_ns = 0;
    bool bid_resting = false;
    bool ask_resting = false;
    Timestamp exposure_since = 0;

    // ---- Inventory and equity ----------------------------------------------
    std::int64_t max_position = 0;
    std::int64_t min_position = 0;
    std::uint64_t limit_breaches = 0;  // fills leaving the true position beyond a limit
    bool has_equity = false;
    std::int64_t peak_equity = 0;
    std::int64_t max_drawdown = 0;
    Timestamp max_drawdown_time = 0;
    std::uint64_t samples_without_mark = 0;

    // ---- Measured latency per component ------------------------------------
    DurationStatistic feed_latency;     // exchange time -> market data delivery
    DurationStatistic order_latency;    // decision -> exchange arrival, new orders
    DurationStatistic cancel_latency;   // decision -> exchange arrival, cancel/replace
    DurationStatistic ack_latency;      // exchange time -> report delivery

    ParentOrderTracking parent;
};

}  // namespace exsim::research

#endif
