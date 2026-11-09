#ifndef EXSIM_GATEWAY_OUCH_OUCH_MAPPING_HPP
#define EXSIM_GATEWAY_OUCH_OUCH_MAPPING_HPP

// OUCH 4.2 <-> engine vocabulary, exactly as gateway mapping section 2 and
// its "Supported simulator profile" specify. Pure constexpr functions; the
// session code in ouch_server.hpp applies them.
//
// The reason maps deliberately do not use protocol::ouch42::reject_reason /
// cancel_reason: those codec helpers predate the 2026-11-02 contract repair
// and differ from GATEWAYS.md (NotAllowedInPhase must be 'R'; MassCancel and
// CancelOnDisconnect must be 'U').

#include "protocol/ouch42.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>

namespace exsim::gateway::ouch {

// ---- Wire constants --------------------------------------------------------

inline constexpr Price kOuchMarketOrderPrice = 0x7FFFFFFF;
inline constexpr std::uint32_t kOuchTimeInForceImmediateOrCancel = 0;
inline constexpr std::uint32_t kOuchTimeInForceMarketHours = 99998;
inline constexpr std::uint32_t kOuchTimeInForceSystemHours = 99999;

inline constexpr char kOuchOrderStateLive = 'L';
inline constexpr char kOuchBboWeightUnspecified = ' ';

inline constexpr char kOuchSystemEventStartOfDay = 'S';
inline constexpr char kOuchSystemEventEndOfDay = 'E';

// Order Rejected reasons used by this simulator.
inline constexpr char kOuchRejectInvalidStock = 'S';
inline constexpr char kOuchRejectInvalidPrice = 'X';
inline constexpr char kOuchRejectInvalidShares = 'Z';
inline constexpr char kOuchRejectExchangeClosed = 'C';
inline constexpr char kOuchRejectHalted = 'H';
inline constexpr char kOuchRejectNotAllowedAtThisTime = 'R';
inline constexpr char kOuchRejectOther = 'O';

// Liquidity flags.
inline constexpr char kOuchLiquidityAdded = 'A';
inline constexpr char kOuchLiquidityRemoved = 'R';
inline constexpr char kOuchLiquidityOpeningCross = 'O';
inline constexpr char kOuchLiquidityClosingCross = 'C';
inline constexpr char kOuchLiquidityHaltOrOtherCross = 'H';

// SoupBinTCP Login Rejected reasons.
inline constexpr char kSoupLoginRejectNotAuthorized = 'A';
inline constexpr char kSoupLoginRejectSessionNotAvailable = 'S';

// ---- Reason maps (GATEWAYS.md section 2, outbound table) -------------------

[[nodiscard]] constexpr char ouch_reject_reason(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::UnknownInstrument: return kOuchRejectInvalidStock;
        case RejectReason::InvalidPrice: return kOuchRejectInvalidPrice;
        case RejectReason::InvalidQuantity: return kOuchRejectInvalidShares;
        case RejectReason::MarketClosed: return kOuchRejectExchangeClosed;
        case RejectReason::InstrumentHalted: return kOuchRejectHalted;
        case RejectReason::NotAllowedInPhase: return kOuchRejectNotAllowedAtThisTime;
        case RejectReason::PostOnlyWouldCross:
        case RejectReason::DuplicateClOrdId:
        case RejectReason::InvalidClOrdId:
        case RejectReason::InvalidField:
        case RejectReason::CapacityExceeded:
        case RejectReason::UnknownOrder:
        case RejectReason::None: return kOuchRejectOther;
    }
    return kOuchRejectOther;
}

[[nodiscard]] constexpr char ouch_cancel_reason(CancelReason reason) noexcept {
    switch (reason) {
        case CancelReason::UserRequested:
        case CancelReason::MassCancel:
        case CancelReason::CancelOnDisconnect: return 'U';
        case CancelReason::ImmediateOrCancel:
        case CancelReason::FillOrKill: return 'I';
        case CancelReason::SelfTradePrevention: return 'Q';
        case CancelReason::EndOfDay: return 'E';
    }
    return 'U';
}

// Liquidity of one Fill leg. `phase_being_left` is only consulted for
// auction executions (aggressor None): the old phase of the PhaseChanged that
// follows the uncross in the same batch.
[[nodiscard]] constexpr char ouch_liquidity_flag(Aggressor aggressor, Side leg_side,
                                                 Phase phase_being_left) noexcept {
    if (aggressor == Aggressor::None) {
        if (phase_being_left == Phase::PreOpen) return kOuchLiquidityOpeningCross;
        if (phase_being_left == Phase::PreClose) return kOuchLiquidityClosingCross;
        return kOuchLiquidityHaltOrOtherCross;
    }
    const bool leg_is_aggressor = (aggressor == Aggressor::Buy) == (leg_side == Side::BUY);
    return leg_is_aggressor ? kOuchLiquidityRemoved : kOuchLiquidityAdded;
}

// ---- Inbound field mapping --------------------------------------------------

// 'B' buys; 'S' (sell), 'T' (sell short) and 'E' (sell short exempt) sell.
[[nodiscard]] constexpr Side engine_side(char ouch_side) noexcept {
    return ouch_side == 'B' ? Side::BUY : Side::SELL;
}

[[nodiscard]] constexpr bool supported_time_in_force(std::uint32_t seconds) noexcept {
    return seconds == kOuchTimeInForceImmediateOrCancel || seconds == kOuchTimeInForceMarketHours ||
           seconds == kOuchTimeInForceSystemHours;
}

[[nodiscard]] constexpr TimeInForce engine_time_in_force(std::uint32_t seconds) noexcept {
    return seconds == kOuchTimeInForceImmediateOrCancel ? TimeInForce::IOC : TimeInForce::Day;
}

// Only displayed 'Y' and post-only 'P' are supported.
[[nodiscard]] constexpr bool supported_display(char display) noexcept {
    return display == 'Y' || display == 'P';
}

[[nodiscard]] constexpr bool display_is_post_only(char display) noexcept { return display == 'P'; }

// Standard OUCH 4.2 codes for the fields the simulator echoes but does not act on.
[[nodiscard]] constexpr bool known_capacity(char capacity) noexcept {
    return capacity == 'A' || capacity == 'P' || capacity == 'R' || capacity == 'O';
}
[[nodiscard]] constexpr bool known_intermarket_sweep(char intermarket_sweep) noexcept {
    return intermarket_sweep == 'Y' || intermarket_sweep == 'N';
}
[[nodiscard]] constexpr bool known_customer_type(char customer_type) noexcept {
    return customer_type == 'R' || customer_type == 'N' || customer_type == ' ';
}

// The instructions of an order as the client entered (or last replaced) them:
// everything an Accepted / Replaced message echoes that the engine does not keep.
struct OuchOrderInstructions {
    std::array<char, 4> firm{' ', ' ', ' ', ' '};
    std::uint32_t time_in_force_seconds = kOuchTimeInForceMarketHours;
    std::uint32_t minimum_quantity = 0;
    char side = 'B';
    char display = 'Y';
    char capacity = 'A';
    char intermarket_sweep = 'N';
    char cross_type = 'N';
};

[[nodiscard]] constexpr OuchOrderInstructions instructions_of(const protocol::ouch42::EnterOrder& order) noexcept {
    OuchOrderInstructions instructions;
    instructions.firm = order.firm;
    instructions.time_in_force_seconds = order.time_in_force;
    instructions.minimum_quantity = order.minimum_quantity;
    instructions.side = order.side;
    instructions.display = order.display;
    instructions.capacity = order.capacity;
    instructions.intermarket_sweep = order.ise;
    instructions.cross_type = order.cross_type;
    return instructions;
}

// Replace keeps side, firm, capacity and cross type; it restates TIF, display,
// intermarket sweep and minimum quantity.
[[nodiscard]] constexpr OuchOrderInstructions instructions_after_replace(
    const OuchOrderInstructions& existing, const protocol::ouch42::ReplaceOrder& replace) noexcept {
    OuchOrderInstructions instructions = existing;
    instructions.time_in_force_seconds = replace.time_in_force;
    instructions.display = replace.display;
    instructions.intermarket_sweep = replace.ise;
    instructions.minimum_quantity = replace.minimum_quantity;
    return instructions;
}

// Supported simulator profile for Enter Order: TIF 0 / 99998 / 99999,
// display Y or P, minimum quantity 0, cross type N, standard codes for
// capacity, intermarket sweep and customer type. Anything else is J 'O'.
[[nodiscard]] constexpr bool enter_order_instructions_supported(const protocol::ouch42::EnterOrder& order) noexcept {
    return supported_time_in_force(order.time_in_force) && supported_display(order.display) &&
           order.minimum_quantity == 0 && order.cross_type == 'N' && known_capacity(order.capacity) &&
           known_intermarket_sweep(order.ise) && known_customer_type(order.customer_type);
}

// Replacement instruction fields get the same validation. The engine's
// ReplaceOrder cannot change time in force or post-only, so a replacement
// must keep the existing order's engine TIF (only DAY orders are ever live)
// and its post-only flag; 99998 and 99999 are interchangeable.
[[nodiscard]] constexpr bool replace_instructions_supported(const OuchOrderInstructions& existing,
                                                            const protocol::ouch42::ReplaceOrder& replace) noexcept {
    return supported_time_in_force(replace.time_in_force) && supported_display(replace.display) &&
           replace.minimum_quantity == 0 && known_intermarket_sweep(replace.ise) &&
           engine_time_in_force(replace.time_in_force) == engine_time_in_force(existing.time_in_force_seconds) &&
           display_is_post_only(replace.display) == display_is_post_only(existing.display);
}

}  // namespace exsim::gateway::ouch

#endif
