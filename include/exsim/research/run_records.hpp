#ifndef EXSIM_RESEARCH_RUN_RECORDS_HPP
#define EXSIM_RESEARCH_RUN_RECORDS_HPP

// Plain records captured inside the simulated hot path and the bounded
// recorder that holds them until the artifact writer drains it.
//
// The recorder never grows and never drops a record: when a buffer is within
// the flush margin the run loop yields (RunStop::FlushRequired) BEFORE the
// next step, the driver drains to disk, and the run resumes. A push into a
// full buffer marks overflow and the run fails explicitly.

#include "exsim/command.hpp"
#include "exsim/research/strategy_context.hpp"
#include "exsim/research/strategy_messages.hpp"
#include "exsim/sim/canonical.hpp"
#include "exsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>

namespace exsim::research {

enum class CommandSource : std::uint8_t { Strategy = 0, BackgroundAgent = 1, Scripted = 2 };

[[nodiscard]] constexpr const char* to_string(CommandSource source) noexcept {
    switch (source) {
        case CommandSource::Strategy: return "strategy";
        case CommandSource::BackgroundAgent: return "background_agent";
        case CommandSource::Scripted: return "scripted";
    }
    return "?";
}

struct CommandRecord {
    std::uint64_t command_index = 0;  // 0, 1, ... in engine processing order
    Timestamp time = 0;               // exchange processing time (== command ts)
    CommandSource source = CommandSource::Strategy;
    std::uint16_t agent_index = 0;    // background agent index when source == BackgroundAgent
    Command command;
};

enum class LifecycleStage : std::uint8_t {
    StrategySubmitted = 0,   // decision time; request leaves after compute + order/cancel latency
    RiskRejected = 1,        // failed the pre-trade check; never sent
    ExchangeArrival = 2,     // the engine processed the request (FIFO evidence for resting orders)
    ExchangeAccepted = 3,
    ExchangeRejected = 4,
    ExchangeRested = 5,
    ExchangeFill = 6,
    ExchangeCanceled = 7,
    ExchangeReplaced = 8,
    ReportDelivered = 9,     // execution report reached the strategy
};

[[nodiscard]] constexpr const char* to_string(LifecycleStage stage) noexcept {
    switch (stage) {
        case LifecycleStage::StrategySubmitted: return "strategy_submitted";
        case LifecycleStage::RiskRejected: return "risk_rejected";
        case LifecycleStage::ExchangeArrival: return "exchange_arrival";
        case LifecycleStage::ExchangeAccepted: return "exchange_accepted";
        case LifecycleStage::ExchangeRejected: return "exchange_rejected";
        case LifecycleStage::ExchangeRested: return "exchange_rested";
        case LifecycleStage::ExchangeFill: return "exchange_fill";
        case LifecycleStage::ExchangeCanceled: return "exchange_canceled";
        case LifecycleStage::ExchangeReplaced: return "exchange_replaced";
        case LifecycleStage::ReportDelivered: return "report_delivered";
    }
    return "?";
}

inline constexpr std::uint64_t kNoCommandIndex = ~std::uint64_t{0};

struct LifecycleRecord {
    Timestamp time = 0;
    // StrategySubmitted: scheduled exchange arrival time. ExchangeArrival:
    // decision time. ReportDelivered: exchange time of the reported event.
    Timestamp reference_time = 0;
    std::uint64_t command_index = kNoCommandIndex;
    ClOrdId cl_ord_id = 0;
    OrderId order_id = 0;
    MatchId match_id = 0;
    Price price = 0;
    Quantity qty = 0;
    Quantity leaves_qty = 0;
    Quantity cum_qty = 0;
    std::uint64_t queue_ahead_qty = 0;     // engine truth, resting orders only
    std::uint64_t level_qty = 0;           // total leaves at the order's level, itself included
    std::uint32_t queue_orders_ahead = 0;
    std::int64_t fee = 0;                  // ExchangeFill: price units
    LifecycleStage stage = LifecycleStage::StrategySubmitted;
    CommandType request = CommandType::NewOrder;
    Side side = Side::BUY;
    TimeInForce tif = TimeInForce::Day;
    ReportKind report_kind = ReportKind::Accepted;
    Liquidity liquidity = Liquidity::Maker;
    RejectReason reject_reason = RejectReason::None;
    CancelReason cancel_reason = CancelReason::UserRequested;
    RiskRejectReason risk_reason = RiskRejectReason::None;
    bool has_queue_position = false;
    bool priority_kept = false;
    bool post_only = false;
};

// One strategy fill leg as recorded in the authoritative ledger.
struct FillRecord {
    std::uint64_t fill_index = 0;
    Timestamp exchange_time = 0;
    std::uint64_t command_index = 0;
    MatchId match_id = 0;
    ClOrdId cl_ord_id = 0;
    OrderId order_id = 0;
    Side side = Side::BUY;
    Liquidity liquidity = Liquidity::Maker;
    Price price = 0;
    Quantity qty = 0;
    std::int64_t fee_per_share = 0;
    std::int64_t fee = 0;
    std::int64_t cash_after = 0;
    std::int64_t position_after = 0;
    std::int64_t realized_pnl_after = 0;
    std::int64_t fees_after = 0;
    bool has_mid_before = false;
    std::int64_t mid_before_times_two = 0;
    std::int64_t spread_capture_times_two = 0;  // sign * (2 * mid_before - 2 * price) * qty
};

// Ordered-field digest of a ledger row (big-endian 64-bit fields, FNV-1a 64).
[[nodiscard]] inline std::uint64_t hash_fill_record(const FillRecord& r, std::uint64_t hash) noexcept {
    const std::uint64_t fields[] = {
        r.fill_index, r.exchange_time, r.command_index, r.match_id, r.cl_ord_id, r.order_id,
        static_cast<std::uint64_t>(r.side), static_cast<std::uint64_t>(r.liquidity),
        static_cast<std::uint64_t>(r.price), r.qty, static_cast<std::uint64_t>(r.fee_per_share),
        static_cast<std::uint64_t>(r.fee), static_cast<std::uint64_t>(r.cash_after),
        static_cast<std::uint64_t>(r.position_after), static_cast<std::uint64_t>(r.realized_pnl_after),
        static_cast<std::uint64_t>(r.fees_after)};
    unsigned char bytes[sizeof(fields)];
    for (std::size_t i = 0; i < std::size(fields); ++i) sim::store64(bytes + 8 * i, fields[i]);
    return sim::hash_bytes(bytes, hash);
}

struct SeriesSample {
    Timestamp time = 0;
    TopOfBook exchange;           // engine truth
    TopOfBook visible;            // the strategy's delivered view
    std::int64_t position = 0;    // ledger (truth)
    std::int64_t known_position = 0;  // strategy view
    std::int64_t cash = 0;
    std::int64_t realized_pnl = 0;
    std::int64_t fees = 0;
    // Valuation in doubled price units (a mid may fall on half a price unit).
    bool has_mark = false;
    std::int64_t mark_times_two = 0;
    // Equity is available with a mark, or without one while flat (no position to value).
    bool has_equity = false;
    std::int64_t unrealized_pnl_times_two = 0;
    std::int64_t equity_times_two = 0;
    std::uint64_t open_bid_qty = 0;  // strategy resting quantity, engine truth
    std::uint64_t open_ask_qty = 0;
};

enum class MarkoutStatus : std::uint8_t { Measured = 0, Unavailable = 1, Censored = 2 };

[[nodiscard]] constexpr const char* to_string(MarkoutStatus status) noexcept {
    switch (status) {
        case MarkoutStatus::Measured: return "measured";
        case MarkoutStatus::Unavailable: return "unavailable";
        case MarkoutStatus::Censored: return "censored";
    }
    return "?";
}

struct MarkoutRecord {
    std::uint64_t fill_index = 0;
    Timestamp fill_time = 0;
    Timestamp horizon_ns = 0;
    std::uint8_t horizon_index = 0;
    MarkoutStatus status = MarkoutStatus::Measured;
    Side side = Side::BUY;
    Price price = 0;
    Quantity qty = 0;
    std::int64_t mid_times_two = 0;       // Measured only
    std::int64_t markout_times_two = 0;   // sign * (2 * mid - 2 * price) * qty, Measured only
};

template <typename Record>
class BoundedRecordBuffer {
public:
    explicit BoundedRecordBuffer(std::size_t capacity) : capacity_(capacity), records_(std::make_unique<Record[]>(capacity)) {
        if (capacity == 0) throw std::invalid_argument("record buffer capacity");
    }
    [[nodiscard]] bool push(const Record& record) noexcept {
        if (size_ == capacity_) return false;
        records_[size_++] = record;
        return true;
    }
    void clear() noexcept { size_ = 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::span<const Record> records() const noexcept { return {records_.get(), size_}; }

private:
    std::size_t capacity_;
    std::unique_ptr<Record[]> records_;
    std::size_t size_ = 0;
};

class RunRecorder {
public:
    RunRecorder(std::size_t command_capacity, std::size_t record_capacity, std::size_t flush_margin)
        : commands(command_capacity), lifecycle(record_capacity), fills(record_capacity), samples(record_capacity),
          markouts(record_capacity), flush_margin_(flush_margin) {}

    // True when any buffer is within the flush margin of its capacity.
    [[nodiscard]] bool needs_flush() const noexcept {
        return near_full(commands) || near_full(lifecycle) || near_full(fills) || near_full(samples) || near_full(markouts);
    }
    [[nodiscard]] bool empty() const noexcept {
        return commands.size() + lifecycle.size() + fills.size() + samples.size() + markouts.size() == 0;
    }
    void clear() noexcept {
        commands.clear();
        lifecycle.clear();
        fills.clear();
        samples.clear();
        markouts.clear();
    }

    BoundedRecordBuffer<CommandRecord> commands;
    BoundedRecordBuffer<LifecycleRecord> lifecycle;
    BoundedRecordBuffer<FillRecord> fills;
    BoundedRecordBuffer<SeriesSample> samples;
    BoundedRecordBuffer<MarkoutRecord> markouts;

private:
    template <typename Record>
    [[nodiscard]] bool near_full(const BoundedRecordBuffer<Record>& buffer) const noexcept {
        return buffer.size() + flush_margin_ > buffer.capacity();
    }
    std::size_t flush_margin_;
};

}  // namespace exsim::research

#endif
