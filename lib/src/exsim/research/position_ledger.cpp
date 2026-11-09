#include "research/position_ledger.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace exsim::research {

PositionLedger::PositionLedger(std::int64_t initial_cash, std::size_t max_open_lots)
    : initial_cash_(initial_cash), lot_capacity_(max_open_lots) {
    if (max_open_lots == 0 || max_open_lots > std::numeric_limits<std::size_t>::max() / sizeof(Lot)) {
        throw std::invalid_argument("invalid ledger lot capacity");
    }
    lots_ = std::make_unique<Lot[]>(max_open_lots);
    state_.cash = initial_cash;
}

LedgerStatus PositionLedger::record_fill(Side side, Price price, Quantity quantity,
                                         std::int64_t fee_per_share) noexcept {
    if ((side != Side::BUY && side != Side::SELL) || price <= 0 || quantity == 0) {
        return LedgerStatus::InvalidInput;
    }
    const bool buying = side == Side::BUY;
    const std::int64_t signed_quantity = static_cast<std::int64_t>(quantity);
    LedgerState updated = state_;
    std::int64_t notional = 0;
    std::int64_t fill_fee = 0;
    if (__builtin_mul_overflow(price, signed_quantity, &notional) ||
        __builtin_mul_overflow(fee_per_share, signed_quantity, &fill_fee) ||
        __builtin_add_overflow(state_.fees, fill_fee, &updated.fees) ||
        __builtin_add_overflow(state_.cash, buying ? -notional : notional, &updated.cash) ||
        __builtin_sub_overflow(updated.cash, fill_fee, &updated.cash) ||
        __builtin_add_overflow(state_.position, buying ? signed_quantity : -signed_quantity,
                               &updated.position) ||
        updated.position == std::numeric_limits<std::int64_t>::min() ||
        __builtin_add_overflow(state_.fill_count, std::uint64_t{1}, &updated.fill_count)) {
        return LedgerStatus::Overflow;
    }
    auto& traded_quantity = buying ? updated.bought_quantity : updated.sold_quantity;
    if (__builtin_add_overflow(traded_quantity, static_cast<std::uint64_t>(quantity), &traded_quantity)) {
        return LedgerStatus::Overflow;
    }

    // Preflight every lot and all arithmetic before changing the ring.
    const bool closing = (buying && state_.position < 0) || (!buying && state_.position > 0);
    std::int64_t remaining_quantity = signed_quantity;
    std::size_t remaining_lots = lot_count_;
    std::size_t lot_index = first_lot_;
    if (closing) {
        while (remaining_quantity != 0 && remaining_lots != 0) {
            const Lot& lot = lots_[lot_index];
            const auto closed_quantity = std::min(remaining_quantity, lot.quantity);
            const Price price_difference = buying ? lot.price - price : price - lot.price;
            std::int64_t realized_change = 0;
            if (__builtin_mul_overflow(price_difference, closed_quantity, &realized_change) ||
                __builtin_add_overflow(updated.gross_realized_pnl, realized_change,
                                       &updated.gross_realized_pnl)) {
                return LedgerStatus::Overflow;
            }
            remaining_quantity -= closed_quantity;
            if (closed_quantity == lot.quantity) {
                --remaining_lots;
                lot_index = next_slot(lot_index);
            } else {
                break;
            }
        }
    }
    const auto last_lot = next_lot_ == 0 ? lot_capacity_ - 1 : next_lot_ - 1;
    const bool extend_last_lot = !closing && lot_count_ != 0 && lots_[last_lot].price == price;
    if (remaining_quantity != 0 && !extend_last_lot && remaining_lots == lot_capacity_) {
        return LedgerStatus::CapacityExceeded;
    }

    remaining_quantity = signed_quantity;
    if (closing) {
        while (remaining_quantity != 0 && lot_count_ != 0) {
            Lot& lot = lots_[first_lot_];
            const auto closed_quantity = std::min(remaining_quantity, lot.quantity);
            lot.quantity -= closed_quantity;
            remaining_quantity -= closed_quantity;
            if (lot.quantity == 0) {
                first_lot_ = next_slot(first_lot_);
                --lot_count_;
            }
        }
    }
    if (remaining_quantity != 0) {
        if (extend_last_lot) {
            lots_[last_lot].quantity += remaining_quantity;
        } else {
            lots_[next_lot_] = Lot{price, remaining_quantity};
            next_lot_ = next_slot(next_lot_);
            ++lot_count_;
        }
    }
    state_ = updated;
    return LedgerStatus::Ok;
}

std::optional<LedgerValuation> PositionLedger::valuation(Price mark_price) const noexcept {
    if (mark_price <= 0) return std::nullopt;
    LedgerValuation result;
    result.mark_price = mark_price;
    std::int64_t marked_position = 0;
    if (__builtin_mul_overflow(state_.position, mark_price, &marked_position) ||
        __builtin_add_overflow(state_.cash, marked_position, &result.equity) ||
        __builtin_sub_overflow(result.equity, initial_cash_, &result.net_pnl)) {
        return std::nullopt;
    }
    auto lot_index = first_lot_;
    for (std::size_t visited = 0; visited < lot_count_; ++visited) {
        const Lot& lot = lots_[lot_index];
        const Price price_difference = state_.position > 0 ? mark_price - lot.price : lot.price - mark_price;
        std::int64_t lot_pnl = 0;
        if (__builtin_mul_overflow(price_difference, lot.quantity, &lot_pnl) ||
            __builtin_add_overflow(result.unrealized_pnl, lot_pnl, &result.unrealized_pnl)) {
            return std::nullopt;
        }
        lot_index = next_slot(lot_index);
    }
    return result;
}

}  // namespace exsim::research
