#ifndef EXSIM_RESEARCH_STRATEGY_MESSAGES_HPP
#define EXSIM_RESEARCH_STRATEGY_MESSAGES_HPP

// Messages delivered to a research strategy. These are the ONLY market and
// order information a strategy receives, and each one reaches it after its
// configured latency. They deliberately omit engine internals and other
// participants' identities (a fill report carries only the strategy's own leg).

#include "types.hpp"

#include <cstdint>
#include <type_traits>

namespace exsim::research {

enum class Liquidity : std::uint8_t { Maker = 0, Taker = 1, Auction = 2 };

[[nodiscard]] constexpr const char* to_string(Liquidity liquidity) noexcept {
    switch (liquidity) {
        case Liquidity::Maker: return "maker";
        case Liquidity::Taker: return "taker";
        case Liquidity::Auction: return "auction";
    }
    return "?";
}

// Best price levels with aggregate displayed quantity.
struct TopOfBook {
    bool has_bid = false;
    bool has_ask = false;
    Price bid_price = 0;
    Price ask_price = 0;
    std::uint64_t bid_qty = 0;
    std::uint64_t ask_qty = 0;

    [[nodiscard]] bool two_sided() const noexcept { return has_bid && has_ask; }
    // bid + ask in price units (twice the mid), exact. Requires two_sided().
    [[nodiscard]] std::int64_t mid_times_two() const noexcept { return bid_price + ask_price; }

    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

// Public market data: one update per engine command that changed the top of
// book or traded. Delivered feed latency after the command's exchange time.
struct MarketDataUpdate {
    std::uint64_t sequence = 0;       // 1, 2, ... per run, no gaps
    Timestamp exchange_time = 0;      // simulated time the exchange processed the command
    TopOfBook top;                    // engine truth right after that command
    std::uint32_t trade_count = 0;    // fills executed by that command
    std::uint64_t traded_qty = 0;
    Price last_trade_price = 0;       // valid when trade_count > 0
    Aggressor last_trade_aggressor = Aggressor::None;
};

enum class ReportKind : std::uint8_t { Accepted = 0, Rejected = 1, Rested = 2, Fill = 3, Canceled = 4, Replaced = 5 };

[[nodiscard]] constexpr const char* to_string(ReportKind kind) noexcept {
    switch (kind) {
        case ReportKind::Accepted: return "accepted";
        case ReportKind::Rejected: return "rejected";
        case ReportKind::Rested: return "rested";
        case ReportKind::Fill: return "fill";
        case ReportKind::Canceled: return "canceled";
        case ReportKind::Replaced: return "replaced";
    }
    return "?";
}

// Private execution report for one of the strategy's orders. Delivered ack
// latency after the exchange event.
struct ExecutionReport {
    ReportKind kind = ReportKind::Accepted;
    Timestamp exchange_time = 0;
    ClOrdId cl_ord_id = 0;        // the strategy's order token (target token for a rejected cancel/replace)
    OrderId order_id = 0;         // exchange order id (new id after a priority-losing replace)
    Side side = Side::BUY;
    Price price = 0;              // order price; execution price for Fill
    Quantity qty = 0;             // order total (Accepted/Replaced), executed (Fill), removed (Canceled)
    Quantity leaves_qty = 0;      // after this report's event
    Quantity cum_qty = 0;         // after this report's event
    Liquidity liquidity = Liquidity::Maker;  // Fill only
    MatchId match_id = 0;                    // Fill only
    CommandType rejected_command = CommandType::NewOrder;  // Rejected only
    RejectReason reject_reason = RejectReason::None;       // Rejected only
    CancelReason cancel_reason = CancelReason::UserRequested;  // Canceled only
    bool priority_kept = false;                                // Replaced only
};

static_assert(std::is_trivially_copyable_v<MarketDataUpdate>);
static_assert(std::is_trivially_copyable_v<ExecutionReport>);

}  // namespace exsim::research

#endif
