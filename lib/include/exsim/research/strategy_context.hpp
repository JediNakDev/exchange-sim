#ifndef EXSIM_RESEARCH_STRATEGY_CONTEXT_HPP
#define EXSIM_RESEARCH_STRATEGY_CONTEXT_HPP

// The strategy host interface.
//
// A research strategy is a compiled C++ class satisfying ResearchStrategy.
// The runner is a template over it (Simulation<Strategy>), so every callback
// is a direct, inlinable call: no virtual dispatch on the hot path.
//
// Causality: StrategyContext exposes only strategy-visible state, i.e. what
// has been DELIVERED to the strategy by its current simulated time (market
// data after feed latency, own execution reports after ack latency) plus the
// strategy's own outstanding requests. It has no route to the matching engine.
//
// Pre-trade risk: every new order and replace passes the host's risk check
// before it leaves the strategy. Position limits use the worst case over
// everything not yet confirmed: known position plus the full remaining
// quantity of every open or in-flight order on that side. The true position
// therefore cannot exceed a limit even though fills reach the strategy late.

#include "exsim/instrument.hpp"
#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategy_messages.hpp"
#include "exsim/sim/random.hpp"
#include "exsim/types.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace exsim::research {

class SimulationCore;

// Separate evidence stream: never contributes to canonical command/event identity.
struct ExecutionDecision {
    Timestamp time = 0;
    Timestamp source_time = 0;
    std::uint64_t source_sequence = 0;
    std::uint64_t target = 0;
    std::uint64_t filled = 0;
    std::uint64_t requested = 0;
    std::int64_t headroom = 0;
    Quantity child_cap = 0;
    Quantity submitted = 0;
    Quantity outstanding = 0;
    ClOrdId child = 0;
    Price touch = 0;
    Price limit = 0;
    bool has_touch = false;
    bool report = false;
    ExecutionReport result{};
    // Static strings only; copied/serialized before the recorder is cleared.
    const char* reason = "submitted";
};

enum class RiskRejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity = 1,    // zero or not a lot multiple
    InvalidPrice = 2,       // off the tick grid or outside the instrument band
    MaxOrderQuantity = 3,
    MaxOpenOrders = 4,
    MaxLongPosition = 5,
    MaxShortPosition = 6,
    UnknownOrder = 7,       // cancel/replace of an order the strategy does not hold
    RequestInFlight = 8,    // the order already has an unacknowledged cancel/replace
    NotYetAcknowledged = 9, // replace before the exchange acknowledged the order
    RunNotActive = 10,
    QueueCapacity = 11,     // the simulation queue is full; the run fails
};

inline constexpr std::size_t kRiskRejectReasonCount = 12;

[[nodiscard]] constexpr const char* to_string(RiskRejectReason reason) noexcept {
    switch (reason) {
        case RiskRejectReason::None: return "none";
        case RiskRejectReason::InvalidQuantity: return "invalid_quantity";
        case RiskRejectReason::InvalidPrice: return "invalid_price";
        case RiskRejectReason::MaxOrderQuantity: return "max_order_quantity";
        case RiskRejectReason::MaxOpenOrders: return "max_open_orders";
        case RiskRejectReason::MaxLongPosition: return "max_long_position";
        case RiskRejectReason::MaxShortPosition: return "max_short_position";
        case RiskRejectReason::UnknownOrder: return "unknown_order";
        case RiskRejectReason::RequestInFlight: return "request_in_flight";
        case RiskRejectReason::NotYetAcknowledged: return "not_yet_acknowledged";
        case RiskRejectReason::RunNotActive: return "run_not_active";
        case RiskRejectReason::QueueCapacity: return "queue_capacity";
    }
    return "?";
}

enum class OrderViewState : std::uint8_t {
    PendingNew = 0,  // sent, no acknowledgment delivered yet
    Live = 1,        // acknowledged and not known to be done
};

enum class PendingRequest : std::uint8_t { None = 0, Cancel = 1, Replace = 2 };

// The strategy's own view of one order, built only from its requests and
// delivered reports. It may lag the exchange truth.
struct StrategyOrderView {
    bool live = false;              // slot in use
    OrderViewState state = OrderViewState::PendingNew;
    PendingRequest pending = PendingRequest::None;
    bool cancel_after_ack = false;  // a cancel raced ahead of the order; re-sent on acknowledgment
    Side side = Side::BUY;
    TimeInForce tif = TimeInForce::Day;
    ClOrdId cl_ord_id = 0;
    OrderId order_id = 0;           // 0 until acknowledged
    Price price = 0;                // confirmed (or submitted) price
    Quantity total_qty = 0;         // confirmed (or submitted) total quantity
    Quantity leaves_qty = 0;        // known remaining quantity
    Quantity cum_qty = 0;           // known executed quantity
    Price requested_price = 0;      // in-flight replace
    Quantity requested_total_qty = 0;
    Timestamp submitted_at = 0;

    // Largest quantity this order could still execute, given what the
    // strategy knows (covers an in-flight replace either way).
    [[nodiscard]] Quantity worst_case_remaining_qty() const noexcept {
        Quantity remaining = leaves_qty;
        if (pending == PendingRequest::Replace && requested_total_qty > cum_qty)
            remaining = remaining > requested_total_qty - cum_qty ? remaining : requested_total_qty - cum_qty;
        return remaining;
    }
};

// Everything a strategy may observe. Owned by the core, read through the context.
struct StrategyVisibleState {
    Timestamp now = 0;
    InstrumentConfig instrument;
    RiskLimits risk;
    TopOfBook top_of_book;                // from the latest delivered market data
    MarketDataUpdate last_market_data;    // sequence 0 until the first delivery
    bool has_seen_two_sided_book = false;
    std::int64_t last_two_sided_mid_times_two = 0;
    std::int64_t known_position = 0;      // from delivered fill reports
    std::uint64_t known_bought_qty = 0;
    std::uint64_t known_sold_qty = 0;
    std::uint32_t open_order_count = 0;   // live views
    std::size_t order_capacity = 0;       // power of two
    std::unique_ptr<StrategyOrderView[]> orders;  // slot = cl_ord_id & (order_capacity - 1)
    RiskRejectReason last_risk_reject = RiskRejectReason::None;
};

// Requests made by a strategy host across the private research transport.
enum class StrategyActionKind : std::uint8_t { Headroom, NewOrder, Cancel, Replace, Wake, Parent, Decision, Session, Done };
struct StrategyAction {
    StrategyActionKind kind = StrategyActionKind::Done;
    Side side = Side::BUY;
    Price price = 0;
    Quantity qty = 0;
    TimeInForce tif = TimeInForce::Day;
    bool post_only = false;
    ClOrdId token = 0;
    Timestamp time = 0;
    ExecutionDecision decision;
};
struct StrategyRemote {
    void* host = nullptr;
    std::uint64_t (*request)(void*, const StrategyAction&) noexcept = nullptr;
    sim::Rng* random = nullptr;
};

class StrategyContext {
public:
    StrategyContext(SimulationCore& core, const StrategyVisibleState& visible) noexcept : core_(&core), visible_(visible) {}

    StrategyContext(StrategyRemote remote, const StrategyVisibleState& visible) noexcept : remote_(remote), visible_(visible) {}

    // ---- Observation (delivered state only) -----------------------------
    [[nodiscard]] Timestamp now() const noexcept { return visible_.now; }
    [[nodiscard]] const InstrumentConfig& instrument() const noexcept { return visible_.instrument; }
    [[nodiscard]] const RiskLimits& risk_limits() const noexcept { return visible_.risk; }
    [[nodiscard]] const TopOfBook& top_of_book() const noexcept { return visible_.top_of_book; }
    [[nodiscard]] const MarketDataUpdate& last_market_data() const noexcept { return visible_.last_market_data; }
    [[nodiscard]] bool has_seen_two_sided_book() const noexcept { return visible_.has_seen_two_sided_book; }
    // Twice the mid of the most recent two-sided book the strategy has seen.
    [[nodiscard]] std::int64_t last_two_sided_mid_times_two() const noexcept { return visible_.last_two_sided_mid_times_two; }
    [[nodiscard]] std::int64_t known_position() const noexcept { return visible_.known_position; }
    [[nodiscard]] std::uint32_t open_order_count() const noexcept { return visible_.open_order_count; }
    [[nodiscard]] RiskRejectReason last_risk_reject() const noexcept { return visible_.last_risk_reject; }

    [[nodiscard]] const StrategyOrderView* find_order(ClOrdId cl_ord_id) const noexcept {
        if (cl_ord_id == 0) return nullptr;
        const auto& view = visible_.orders[cl_ord_id & (visible_.order_capacity - 1)];
        return view.live && view.cl_ord_id == cl_ord_id ? &view : nullptr;
    }

    template <typename Visitor>
    void for_each_open_order(Visitor&& visitor) const {
        for (std::size_t i = 0; i < visible_.order_capacity; ++i)
            if (visible_.orders[i].live) visitor(visible_.orders[i]);
    }

    // Largest new-order quantity on `side` that passes the position limits,
    // ignoring the remaining quantity of `excluded_order` (0 = none).
    [[nodiscard]] std::int64_t position_headroom(Side side, ClOrdId excluded_order = 0) const noexcept;

    // ---- Actions (pass the risk check, then travel with order latency) ----
    // Returns the new order token, or 0 when the risk check rejected it.
    [[nodiscard]] ClOrdId submit_limit_order(Side side, Price price, Quantity qty, TimeInForce tif,
                                             bool post_only) noexcept;
    // Requests travel with the cancel latency. Return false on a host-side reject.
    bool cancel_order(ClOrdId cl_ord_id) noexcept;
    bool replace_order(ClOrdId cl_ord_id, Price price, Quantity new_total_qty) noexcept;

    // Single wakeup slot: the latest call replaces any pending wakeup.
    void wake_at(Timestamp time) noexcept;

    // Records the parent order for execution metrics: decision time is now,
    // the arrival benchmark is the exchange mid at this simulated time.
    void declare_parent_order(Side side, Quantity qty) noexcept;
    void record_execution_decision(const ExecutionDecision& row) noexcept;

    // The strategy's own seeded RNG stream ("strategy").
    [[nodiscard]] sim::Rng& rng() noexcept;

private:
    SimulationCore* core_ = nullptr;
    StrategyRemote remote_;
    const StrategyVisibleState& visible_;
};

template <typename Strategy>
concept ResearchStrategy = requires(Strategy& strategy, StrategyContext& context, const MarketDataUpdate& update,
                                    const ExecutionReport& report) {
    { Strategy::kStrategyName } -> std::convertible_to<std::string_view>;
    { Strategy::kStrategyVersion } -> std::convertible_to<std::uint32_t>;
    strategy.on_start(context);
    strategy.on_market_data(update, context);
    strategy.on_execution_report(report, context);
    strategy.on_wakeup(context);
};

}  // namespace exsim::research

#endif
