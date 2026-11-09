#ifndef EXSIM_TYPES_HPP
#define EXSIM_TYPES_HPP

// Core vocabulary of the exchange simulator. Every protocol gateway
// translates its wire format into these types, and every subsystem
// (engine, market data, journal, tests) speaks only these types.
// See matching engine contract for the normative semantics.

#include <lob/types.hpp>
#include <cstdint>
#include <string_view>

namespace exsim {

using lob::Side;  // Side::BUY / Side::SELL

// Fixed-point price with 4 implied decimals (ITCH / OUCH convention):
// $150.25 == 1'502'500.
using Price = std::int64_t;
inline constexpr Price kPriceScale = 10'000;

using Quantity = std::uint32_t;       // shares
using Timestamp = std::uint64_t;      // nanoseconds; opaque to the engine
using OrderId = std::uint64_t;        // exchange-assigned, starts at 1
using ClOrdId = std::uint64_t;        // client token, unique per session among live orders
using SessionId = std::uint16_t;      // 0 = system / invalid
using InstrumentId = std::uint16_t;   // 1..N, doubles as ITCH stock locate; 0 = all/none
using OwnerId = std::uint32_t;        // self-trade-prevention group; 0 = none
using MatchId = std::uint64_t;        // starts at 1, one per Fill and AuctionSummary

// Client tokens are packed with the session into one 64-bit map key.
inline constexpr ClOrdId kMaxClOrdId = (ClOrdId{1} << 48) - 1;

enum class OrdType : std::uint8_t { Limit = 0, Market = 1 };

enum class TimeInForce : std::uint8_t { Day = 0, GTC = 1, IOC = 2, FOK = 3 };

// Self-trade prevention mode, taken from the aggressing order.
enum class StpMode : std::uint8_t {
    None = 0,
    CancelResting = 1,    // cancel the resting order, keep matching
    CancelAggressor = 2,  // cancel the remainder of the incoming order
    CancelBoth = 3,       // cancel both
};

enum class Phase : std::uint8_t {
    Closed = 0,      // reject new orders/replaces; cancels allowed
    PreOpen = 1,     // opening auction call: orders rest without matching
    Continuous = 2,  // continuous price-time matching
    Halted = 3,      // reject new orders/replaces; cancels allowed
    PreClose = 4,    // closing auction call: orders rest without matching
};

enum class CommandType : std::uint8_t {
    NewOrder = 0,
    Cancel = 1,
    Replace = 2,
    MassCancel = 3,
    SetPhase = 4,
};

enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownInstrument = 1,
    InvalidClOrdId = 2,
    DuplicateClOrdId = 3,
    InvalidQuantity = 4,
    InvalidPrice = 5,
    InvalidField = 6,
    MarketClosed = 7,
    InstrumentHalted = 8,
    NotAllowedInPhase = 9,
    PostOnlyWouldCross = 10,
    CapacityExceeded = 11,
    UnknownOrder = 12,
};

enum class CancelReason : std::uint8_t {
    UserRequested = 0,
    ImmediateOrCancel = 1,    // unfilled remainder of IOC / market order
    FillOrKill = 2,           // FOK could not be filled in full
    SelfTradePrevention = 3,
    MassCancel = 4,
    CancelOnDisconnect = 5,
    EndOfDay = 6,             // DAY order expired on transition to Closed
};

// Which leg of a Fill was the incoming (liquidity-taking) order.
// None for auction (uncross) executions.
enum class Aggressor : std::uint8_t { None = 0, Buy = 1, Sell = 2 };

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

[[nodiscard]] constexpr std::string_view to_string(OrdType t) noexcept {
    switch (t) {
        case OrdType::Limit: return "LIMIT";
        case OrdType::Market: return "MARKET";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::Day: return "DAY";
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(StpMode m) noexcept {
    switch (m) {
        case StpMode::None: return "NONE";
        case StpMode::CancelResting: return "CANCEL_RESTING";
        case StpMode::CancelAggressor: return "CANCEL_AGGRESSOR";
        case StpMode::CancelBoth: return "CANCEL_BOTH";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Phase p) noexcept {
    switch (p) {
        case Phase::Closed: return "CLOSED";
        case Phase::PreOpen: return "PRE_OPEN";
        case Phase::Continuous: return "CONTINUOUS";
        case Phase::Halted: return "HALTED";
        case Phase::PreClose: return "PRE_CLOSE";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(CommandType c) noexcept {
    switch (c) {
        case CommandType::NewOrder: return "NEW_ORDER";
        case CommandType::Cancel: return "CANCEL";
        case CommandType::Replace: return "REPLACE";
        case CommandType::MassCancel: return "MASS_CANCEL";
        case CommandType::SetPhase: return "SET_PHASE";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "NONE";
        case RejectReason::UnknownInstrument: return "UNKNOWN_INSTRUMENT";
        case RejectReason::InvalidClOrdId: return "INVALID_CLORDID";
        case RejectReason::DuplicateClOrdId: return "DUPLICATE_CLORDID";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::InvalidField: return "INVALID_FIELD";
        case RejectReason::MarketClosed: return "MARKET_CLOSED";
        case RejectReason::InstrumentHalted: return "INSTRUMENT_HALTED";
        case RejectReason::NotAllowedInPhase: return "NOT_ALLOWED_IN_PHASE";
        case RejectReason::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
        case RejectReason::CapacityExceeded: return "CAPACITY_EXCEEDED";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::UserRequested: return "USER_REQUESTED";
        case CancelReason::ImmediateOrCancel: return "IMMEDIATE_OR_CANCEL";
        case CancelReason::FillOrKill: return "FILL_OR_KILL";
        case CancelReason::SelfTradePrevention: return "SELF_TRADE_PREVENTION";
        case CancelReason::MassCancel: return "MASS_CANCEL";
        case CancelReason::CancelOnDisconnect: return "CANCEL_ON_DISCONNECT";
        case CancelReason::EndOfDay: return "END_OF_DAY";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view to_string(Aggressor a) noexcept {
    switch (a) {
        case Aggressor::None: return "NONE";
        case Aggressor::Buy: return "BUY";
        case Aggressor::Sell: return "SELL";
    }
    return "?";
}

}  // namespace exsim

#endif
