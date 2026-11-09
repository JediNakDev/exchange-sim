#ifndef EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_MAPPING_HPP
#define EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_MAPPING_HPP

// Pure value mappings between the SBE order entry schema
// (config/protocols/exsim_order_entry_sbe.xml) and the protocol-neutral engine
// types (lib/include/types.hpp). Field semantics follow the FIX 4.4 rows of
// gateway mapping section 3; differences are listed in historical design notes.

#include "protocol/sbe_order_entry.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace exsim::gateway::fixp_sbe {

namespace sbe = protocol::sbe_order_entry;

// Client tokens are uint64 on the wire. The TokenTable interns their 8
// little-endian bytes, so ClOrdId -> wire token is a table lookup.
using TokenBytes = std::array<char, 8>;

[[nodiscard]] constexpr TokenBytes token_bytes_from_wire_cl_ord_id(std::uint64_t wire_cl_ord_id) noexcept {
    TokenBytes bytes{};
    sbe::little_endian::store64(bytes.data(), wire_cl_ord_id);
    return bytes;
}
[[nodiscard]] constexpr std::uint64_t wire_cl_ord_id_from_token_bytes(std::string_view token) noexcept {
    return token.size() == sizeof(std::uint64_t) ? sbe::little_endian::load64(token.data()) : 0;
}

[[nodiscard]] constexpr bool exsim_side_from_wire(sbe::SideCode wire_side, Side& side) noexcept {
    switch (wire_side) {
        case sbe::SideCode::Buy: side = Side::BUY; return true;
        case sbe::SideCode::Sell: side = Side::SELL; return true;
    }
    return false;
}
[[nodiscard]] constexpr sbe::SideCode wire_side_from_exsim(Side side) noexcept {
    return side == Side::BUY ? sbe::SideCode::Buy : sbe::SideCode::Sell;
}

[[nodiscard]] constexpr bool exsim_ord_type_from_wire(sbe::OrdTypeCode wire_ord_type, OrdType& ord_type) noexcept {
    switch (wire_ord_type) {
        case sbe::OrdTypeCode::Market: ord_type = OrdType::Market; return true;
        case sbe::OrdTypeCode::Limit: ord_type = OrdType::Limit; return true;
    }
    return false;
}
[[nodiscard]] constexpr sbe::OrdTypeCode wire_ord_type_from_exsim(OrdType ord_type) noexcept {
    return ord_type == OrdType::Market ? sbe::OrdTypeCode::Market : sbe::OrdTypeCode::Limit;
}

[[nodiscard]] constexpr bool exsim_time_in_force_from_wire(sbe::TimeInForceCode wire_time_in_force,
                                                           TimeInForce& time_in_force) noexcept {
    switch (wire_time_in_force) {
        case sbe::TimeInForceCode::Day: time_in_force = TimeInForce::Day; return true;
        case sbe::TimeInForceCode::GoodTillCancel: time_in_force = TimeInForce::GTC; return true;
        case sbe::TimeInForceCode::ImmediateOrCancel: time_in_force = TimeInForce::IOC; return true;
        case sbe::TimeInForceCode::FillOrKill: time_in_force = TimeInForce::FOK; return true;
    }
    return false;
}
[[nodiscard]] constexpr sbe::TimeInForceCode wire_time_in_force_from_exsim(TimeInForce time_in_force) noexcept {
    switch (time_in_force) {
        case TimeInForce::Day: return sbe::TimeInForceCode::Day;
        case TimeInForce::GTC: return sbe::TimeInForceCode::GoodTillCancel;
        case TimeInForce::IOC: return sbe::TimeInForceCode::ImmediateOrCancel;
        case TimeInForce::FOK: return sbe::TimeInForceCode::FillOrKill;
    }
    return sbe::TimeInForceCode::Day;
}

// NUL selects the user's configured default (gateway mapping section 1).
[[nodiscard]] constexpr bool exsim_stp_mode_from_wire(sbe::SelfTradePreventionCode wire_stp, StpMode user_default,
                                                      StpMode& stp_mode) noexcept {
    switch (wire_stp) {
        case sbe::SelfTradePreventionCode::UseUserDefault: stp_mode = user_default; return true;
        case sbe::SelfTradePreventionCode::None: stp_mode = StpMode::None; return true;
        case sbe::SelfTradePreventionCode::CancelResting: stp_mode = StpMode::CancelResting; return true;
        case sbe::SelfTradePreventionCode::CancelAggressor: stp_mode = StpMode::CancelAggressor; return true;
        case sbe::SelfTradePreventionCode::CancelBoth: stp_mode = StpMode::CancelBoth; return true;
    }
    return false;
}
[[nodiscard]] constexpr sbe::SelfTradePreventionCode wire_stp_from_exsim(StpMode stp_mode) noexcept {
    switch (stp_mode) {
        case StpMode::None: return sbe::SelfTradePreventionCode::None;
        case StpMode::CancelResting: return sbe::SelfTradePreventionCode::CancelResting;
        case StpMode::CancelAggressor: return sbe::SelfTradePreventionCode::CancelAggressor;
        case StpMode::CancelBoth: return sbe::SelfTradePreventionCode::CancelBoth;
    }
    return sbe::SelfTradePreventionCode::None;
}

[[nodiscard]] constexpr sbe::ExsimRejectReasonCode wire_reject_reason_from_exsim(RejectReason reason) noexcept {
    return static_cast<sbe::ExsimRejectReasonCode>(static_cast<std::uint8_t>(reason));
}
[[nodiscard]] constexpr sbe::ExsimCancelReasonCode wire_cancel_reason_from_exsim(CancelReason reason) noexcept {
    return static_cast<sbe::ExsimCancelReasonCode>(static_cast<std::uint8_t>(reason));
}

// OrdRejReason(103) exactly as the FIX 4.4 gateway maps it; SBE carries the
// precise exsim reason in ExsimRejectReason(7902) instead of Text(58).
[[nodiscard]] constexpr sbe::OrdRejReasonCode ord_rej_reason_for(sbe::ExsimRejectReasonCode reason) noexcept {
    switch (reason) {
        case sbe::ExsimRejectReasonCode::UnknownInstrument: return sbe::OrdRejReasonCode::UnknownSymbol;
        case sbe::ExsimRejectReasonCode::MarketClosed: return sbe::OrdRejReasonCode::ExchangeClosed;
        case sbe::ExsimRejectReasonCode::UnknownOrder: return sbe::OrdRejReasonCode::UnknownOrder;
        case sbe::ExsimRejectReasonCode::DuplicateClOrdId:
        case sbe::ExsimRejectReasonCode::ClOrdIdAlreadyUsedToday: return sbe::OrdRejReasonCode::DuplicateOrder;
        default: return sbe::OrdRejReasonCode::Other;
    }
}

// CxlRejReason(102): 1 UnknownOrder, 6 duplicate ClOrdID, else 99.
[[nodiscard]] constexpr sbe::CxlRejReasonCode cxl_rej_reason_for(sbe::ExsimRejectReasonCode reason) noexcept {
    switch (reason) {
        case sbe::ExsimRejectReasonCode::UnknownOrder: return sbe::CxlRejReasonCode::UnknownOrder;
        case sbe::ExsimRejectReasonCode::DuplicateClOrdId:
        case sbe::ExsimRejectReasonCode::ClOrdIdAlreadyUsedToday: return sbe::CxlRejReasonCode::DuplicateClOrdId;
        default: return sbe::CxlRejReasonCode::Other;
    }
}

// MassCancelRejectReason(532).
[[nodiscard]] constexpr sbe::MassCancelRejectReasonCode mass_cancel_reject_reason_for(
    sbe::ExsimRejectReasonCode reason) noexcept {
    switch (reason) {
        case sbe::ExsimRejectReasonCode::UnknownInstrument: return sbe::MassCancelRejectReasonCode::InvalidOrUnknownSecurity;
        case sbe::ExsimRejectReasonCode::InvalidField: return sbe::MassCancelRejectReasonCode::MassCancelNotSupported;
        default: return sbe::MassCancelRejectReasonCode::Other;
    }
}

// LastLiquidityInd(851): auction executions have no aggressor.
[[nodiscard]] constexpr sbe::LastLiquidityIndicator liquidity_indicator_for(Aggressor aggressor, Side leg_side) noexcept {
    if (aggressor == Aggressor::None) return sbe::LastLiquidityIndicator::Auction;
    const bool leg_is_aggressor = (aggressor == Aggressor::Buy) == (leg_side == Side::BUY);
    return leg_is_aggressor ? sbe::LastLiquidityIndicator::RemovedLiquidity : sbe::LastLiquidityIndicator::AddedLiquidity;
}

// OrdStatus(39) of an order that is still live.
[[nodiscard]] constexpr sbe::OrdStatusCode live_order_status(Quantity cum_qty) noexcept {
    return cum_qty == 0 ? sbe::OrdStatusCode::New : sbe::OrdStatusCode::PartiallyFilled;
}

}  // namespace exsim::gateway::fixp_sbe

#endif
