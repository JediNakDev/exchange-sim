#ifndef EXSIM_RESEARCH_POSITION_LEDGER_HPP
#define EXSIM_RESEARCH_POSITION_LEDGER_HPP

#include "exsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace exsim::research {

enum class LedgerStatus : std::uint8_t { Ok, InvalidInput, CapacityExceeded, Overflow };

struct LedgerState {
    std::int64_t cash = 0;
    std::int64_t position = 0;
    std::int64_t gross_realized_pnl = 0;
    std::int64_t fees = 0;  // Negative fees are rebates, charged on every fill.
    std::uint64_t fill_count = 0;
    std::uint64_t bought_quantity = 0;
    std::uint64_t sold_quantity = 0;
};

struct LedgerValuation {
    Price mark_price = 0;
    std::int64_t equity = 0;
    std::int64_t unrealized_pnl = 0;
    std::int64_t net_pnl = 0;
};

// One instrument, initially flat, FIFO cost basis. Cash, fees and P&L use
// price units (1/10000 currency); quantity is shares, multiplier is one.
// No allocation after construction. Failed fills leave all state unchanged.
// The caller must fail the run if an executed fill cannot be recorded;
// CapacityExceeded/Overflow are not exchange-order rejections.
class PositionLedger {
public:
    explicit PositionLedger(std::int64_t initial_cash, std::size_t max_open_lots = 4096);
    PositionLedger(const PositionLedger&) = delete;
    PositionLedger& operator=(const PositionLedger&) = delete;
    PositionLedger(PositionLedger&&) = delete;
    PositionLedger& operator=(PositionLedger&&) = delete;

    [[nodiscard]] LedgerStatus record_fill(Side side, Price price, Quantity quantity,
                                           std::int64_t fee_per_share = 0) noexcept;
    [[nodiscard]] const LedgerState& state() const noexcept { return state_; }
    [[nodiscard]] std::size_t open_lot_count() const noexcept { return lot_count_; }

    // No mark is invented or stored. The caller chooses an available mark
    // at its observation time. Invalid/unrepresentable valuations are absent.
    [[nodiscard]] std::optional<LedgerValuation> valuation(Price mark_price) const noexcept;

private:
    struct Lot {
        Price price = 0;
        std::int64_t quantity = 0;
    };

    [[nodiscard]] std::size_t next_slot(std::size_t slot) const noexcept {
        return slot + 1 == lot_capacity_ ? 0 : slot + 1;
    }

    std::int64_t initial_cash_;
    LedgerState state_;
    std::size_t lot_capacity_;
    std::unique_ptr<Lot[]> lots_;
    std::size_t first_lot_ = 0;
    std::size_t lot_count_ = 0;
    std::size_t next_lot_ = 0;
};

}  // namespace exsim::research

#endif
