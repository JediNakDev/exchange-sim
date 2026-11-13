#ifndef EXSIM_SIM_FIELDS_HPP
#define EXSIM_SIM_FIELDS_HPP

// Field-by-field reflection over Event, Command and OrderInfo.
//
// Events and commands are unions, so their padding bytes (and the bytes of
// inactive members) are indeterminate: they must never be memcmp'd, hashed
// or written out as raw struct bytes. Everything that needs to look at
// "all the fields" - equality, formatting, the canonical event-stream hash,
// the scenario DSL - goes through these visitors instead, so they can never
// disagree about what a field is.
//
// visit_fields(x, f) calls f(Field{name, kind, raw}) for every field of the
// active member in declaration order, after the header fields. `raw` holds
// the value widened to 64 bits (signed values as two's complement, enums
// as their underlying value so out-of-range values survive, bools as 0/1).
//
// Allocation-free; usable from tools and the replay path.

#include <engine/command.hpp>
#include <engine/event.hpp>
#include <engine/matching_engine.hpp>
#include <engine/types.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace exsim::sim {

// What a field means; drives formatting and DSL parsing.
enum class FieldKind : std::uint8_t {
    Instrument,
    Timestamp,
    OrderId,
    MatchId,
    Token,  // ClOrdId
    Session,
    Owner,
    Price,
    Quantity,
    Count,
    Side,
    OrdType,
    TimeInForce,
    StpMode,
    Bool,
    Phase,
    CommandType,
    RejectReason,
    CancelReason,
    Aggressor,
};

struct Field {
    std::string_view name;
    FieldKind kind;
    std::uint64_t raw;
};

template <typename T>
[[nodiscard]] constexpr std::uint64_t to_raw(T value) noexcept {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<std::uint64_t>(static_cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? 1u : 0u;
    } else {
        return static_cast<std::uint64_t>(value);
    }
}

[[nodiscard]] constexpr std::string_view event_type_name(EventType type) noexcept {
    switch (type) {
        case EventType::Accepted: return "accepted";
        case EventType::Rejected: return "rejected";
        case EventType::Rested: return "rested";
        case EventType::Fill: return "fill";
        case EventType::Canceled: return "canceled";
        case EventType::Replaced: return "replaced";
        case EventType::MassCancelDone: return "mass_cancel_done";
        case EventType::PhaseChanged: return "phase_changed";
        case EventType::AuctionSummary: return "auction_summary";
    }
    return "?";
}

[[nodiscard]] constexpr bool is_enum_kind(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::Side:
        case FieldKind::OrdType:
        case FieldKind::TimeInForce:
        case FieldKind::StpMode:
        case FieldKind::Phase:
        case FieldKind::CommandType:
        case FieldKind::RejectReason:
        case FieldKind::CancelReason:
        case FieldKind::Aggressor: return true;
        default: return false;
    }
}

// Upper-case name (engine/types.hpp to_string) of an enum-kind value; "" when the
// kind is not an enum or the value is out of range.
[[nodiscard]] constexpr std::string_view enum_value_name(FieldKind kind, std::uint64_t raw) noexcept {
    if (raw > 0xFF) {
        return {};
    }
    const auto byte = static_cast<std::uint8_t>(raw);
    std::string_view name;
    switch (kind) {
        case FieldKind::Side: return byte <= 1 ? to_string(static_cast<Side>(byte)) : std::string_view{};
        case FieldKind::OrdType: name = to_string(static_cast<OrdType>(byte)); break;
        case FieldKind::TimeInForce: name = to_string(static_cast<TimeInForce>(byte)); break;
        case FieldKind::StpMode: name = to_string(static_cast<StpMode>(byte)); break;
        case FieldKind::Phase: name = to_string(static_cast<Phase>(byte)); break;
        case FieldKind::CommandType: name = to_string(static_cast<CommandType>(byte)); break;
        case FieldKind::RejectReason: name = to_string(static_cast<RejectReason>(byte)); break;
        case FieldKind::CancelReason: name = to_string(static_cast<CancelReason>(byte)); break;
        case FieldKind::Aggressor: name = to_string(static_cast<Aggressor>(byte)); break;
        default: return {};
    }
    return name == "?" ? std::string_view{} : name;
}

inline constexpr EventType kAllEventTypes[] = {
    EventType::Accepted, EventType::Rejected,       EventType::Rested,
    EventType::Fill,     EventType::Canceled,       EventType::Replaced,
    EventType::MassCancelDone, EventType::PhaseChanged, EventType::AuctionSummary,
};

namespace detail {

template <typename F>
constexpr void visit_fill_leg(const FillLeg& leg, bool is_buy, F& f) {
    f(Field{is_buy ? "buy.order_id" : "sell.order_id", FieldKind::OrderId, to_raw(leg.order_id)});
    f(Field{is_buy ? "buy.cl_ord_id" : "sell.cl_ord_id", FieldKind::Token, to_raw(leg.cl_ord_id)});
    f(Field{is_buy ? "buy.session" : "sell.session", FieldKind::Session, to_raw(leg.session)});
    f(Field{is_buy ? "buy.owner" : "sell.owner", FieldKind::Owner, to_raw(leg.owner)});
    f(Field{is_buy ? "buy.leaves_qty" : "sell.leaves_qty", FieldKind::Quantity, to_raw(leg.leaves_qty)});
    f(Field{is_buy ? "buy.cum_qty" : "sell.cum_qty", FieldKind::Quantity, to_raw(leg.cum_qty)});
}

}  // namespace detail

// Header fields (instrument, ts) then the active member's fields. An
// out-of-range EventType visits the header fields only.
template <typename F>
constexpr void visit_fields(const Event& e, F&& f) {
    f(Field{"instrument", FieldKind::Instrument, to_raw(e.instrument)});
    f(Field{"ts", FieldKind::Timestamp, to_raw(e.ts)});
    switch (e.type) {
        case EventType::Accepted: {
            const AcceptedEvent& a = e.accepted;
            f(Field{"order_id", FieldKind::OrderId, to_raw(a.order_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(a.cl_ord_id)});
            f(Field{"session", FieldKind::Session, to_raw(a.session)});
            f(Field{"owner", FieldKind::Owner, to_raw(a.owner)});
            f(Field{"side", FieldKind::Side, to_raw(a.side)});
            f(Field{"ord_type", FieldKind::OrdType, to_raw(a.ord_type)});
            f(Field{"tif", FieldKind::TimeInForce, to_raw(a.tif)});
            f(Field{"stp", FieldKind::StpMode, to_raw(a.stp)});
            f(Field{"post_only", FieldKind::Bool, to_raw(a.post_only)});
            f(Field{"price", FieldKind::Price, to_raw(a.price)});
            f(Field{"qty", FieldKind::Quantity, to_raw(a.qty)});
            return;
        }
        case EventType::Rejected: {
            const RejectedEvent& r = e.rejected;
            f(Field{"cmd", FieldKind::CommandType, to_raw(r.cmd)});
            f(Field{"reason", FieldKind::RejectReason, to_raw(r.reason)});
            f(Field{"session", FieldKind::Session, to_raw(r.session)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(r.cl_ord_id)});
            f(Field{"orig_cl_ord_id", FieldKind::Token, to_raw(r.orig_cl_ord_id)});
            f(Field{"order_id", FieldKind::OrderId, to_raw(r.order_id)});
            return;
        }
        case EventType::Rested: {
            const RestedEvent& r = e.rested;
            f(Field{"order_id", FieldKind::OrderId, to_raw(r.order_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(r.cl_ord_id)});
            f(Field{"session", FieldKind::Session, to_raw(r.session)});
            f(Field{"side", FieldKind::Side, to_raw(r.side)});
            f(Field{"price", FieldKind::Price, to_raw(r.price)});
            f(Field{"leaves_qty", FieldKind::Quantity, to_raw(r.leaves_qty)});
            return;
        }
        case EventType::Fill: {
            const FillEvent& x = e.fill;
            f(Field{"match_id", FieldKind::MatchId, to_raw(x.match_id)});
            f(Field{"price", FieldKind::Price, to_raw(x.price)});
            f(Field{"qty", FieldKind::Quantity, to_raw(x.qty)});
            f(Field{"aggressor", FieldKind::Aggressor, to_raw(x.aggressor)});
            detail::visit_fill_leg(x.buy, true, f);
            detail::visit_fill_leg(x.sell, false, f);
            return;
        }
        case EventType::Canceled: {
            const CanceledEvent& c = e.canceled;
            f(Field{"order_id", FieldKind::OrderId, to_raw(c.order_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(c.cl_ord_id)});
            f(Field{"request_cl_ord_id", FieldKind::Token, to_raw(c.request_cl_ord_id)});
            f(Field{"session", FieldKind::Session, to_raw(c.session)});
            f(Field{"side", FieldKind::Side, to_raw(c.side)});
            f(Field{"price", FieldKind::Price, to_raw(c.price)});
            f(Field{"canceled_qty", FieldKind::Quantity, to_raw(c.canceled_qty)});
            f(Field{"cum_qty", FieldKind::Quantity, to_raw(c.cum_qty)});
            f(Field{"reason", FieldKind::CancelReason, to_raw(c.reason)});
            f(Field{"was_resting", FieldKind::Bool, to_raw(c.was_resting)});
            f(Field{"leaves_qty", FieldKind::Quantity, to_raw(c.leaves_qty)});
            return;
        }
        case EventType::Replaced: {
            const ReplacedEvent& r = e.replaced;
            f(Field{"old_order_id", FieldKind::OrderId, to_raw(r.old_order_id)});
            f(Field{"new_order_id", FieldKind::OrderId, to_raw(r.new_order_id)});
            f(Field{"orig_cl_ord_id", FieldKind::Token, to_raw(r.orig_cl_ord_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(r.cl_ord_id)});
            f(Field{"session", FieldKind::Session, to_raw(r.session)});
            f(Field{"side", FieldKind::Side, to_raw(r.side)});
            f(Field{"old_price", FieldKind::Price, to_raw(r.old_price)});
            f(Field{"price", FieldKind::Price, to_raw(r.price)});
            f(Field{"old_leaves_qty", FieldKind::Quantity, to_raw(r.old_leaves_qty)});
            f(Field{"qty", FieldKind::Quantity, to_raw(r.qty)});
            f(Field{"leaves_qty", FieldKind::Quantity, to_raw(r.leaves_qty)});
            f(Field{"cum_qty", FieldKind::Quantity, to_raw(r.cum_qty)});
            f(Field{"priority_kept", FieldKind::Bool, to_raw(r.priority_kept)});
            return;
        }
        case EventType::MassCancelDone: {
            const MassCancelDoneEvent& m = e.mass_cancel_done;
            f(Field{"session", FieldKind::Session, to_raw(m.session)});
            f(Field{"request_cl_ord_id", FieldKind::Token, to_raw(m.request_cl_ord_id)});
            f(Field{"canceled_count", FieldKind::Count, to_raw(m.canceled_count)});
            f(Field{"reason", FieldKind::CancelReason, to_raw(m.reason)});
            return;
        }
        case EventType::PhaseChanged: {
            f(Field{"old_phase", FieldKind::Phase, to_raw(e.phase_changed.old_phase)});
            f(Field{"new_phase", FieldKind::Phase, to_raw(e.phase_changed.new_phase)});
            return;
        }
        case EventType::AuctionSummary: {
            f(Field{"price", FieldKind::Price, to_raw(e.auction_summary.price)});
            f(Field{"matched_qty", FieldKind::Count, to_raw(e.auction_summary.matched_qty)});
            f(Field{"match_id", FieldKind::MatchId, to_raw(e.auction_summary.match_id)});
            return;
        }
    }
}

// Fields of the active command member. An out-of-range CommandType visits nothing.
template <typename F>
constexpr void visit_fields(const Command& c, F&& f) {
    switch (c.type) {
        case CommandType::NewOrder: {
            const NewOrder& n = c.new_order;
            f(Field{"ts", FieldKind::Timestamp, to_raw(n.ts)});
            f(Field{"instrument", FieldKind::Instrument, to_raw(n.instrument)});
            f(Field{"session", FieldKind::Session, to_raw(n.session)});
            f(Field{"owner", FieldKind::Owner, to_raw(n.owner)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(n.cl_ord_id)});
            f(Field{"side", FieldKind::Side, to_raw(n.side)});
            f(Field{"type", FieldKind::OrdType, to_raw(n.type)});
            f(Field{"tif", FieldKind::TimeInForce, to_raw(n.tif)});
            f(Field{"stp", FieldKind::StpMode, to_raw(n.stp)});
            f(Field{"post_only", FieldKind::Bool, to_raw(n.post_only)});
            f(Field{"price", FieldKind::Price, to_raw(n.price)});
            f(Field{"qty", FieldKind::Quantity, to_raw(n.qty)});
            return;
        }
        case CommandType::Cancel: {
            const CancelOrder& x = c.cancel;
            f(Field{"ts", FieldKind::Timestamp, to_raw(x.ts)});
            f(Field{"session", FieldKind::Session, to_raw(x.session)});
            f(Field{"orig_cl_ord_id", FieldKind::Token, to_raw(x.orig_cl_ord_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(x.cl_ord_id)});
            f(Field{"reduce_to_total_qty", FieldKind::Quantity, to_raw(x.reduce_to_total_qty)});
            return;
        }
        case CommandType::Replace: {
            const ReplaceOrder& r = c.replace;
            f(Field{"ts", FieldKind::Timestamp, to_raw(r.ts)});
            f(Field{"session", FieldKind::Session, to_raw(r.session)});
            f(Field{"orig_cl_ord_id", FieldKind::Token, to_raw(r.orig_cl_ord_id)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(r.cl_ord_id)});
            f(Field{"price", FieldKind::Price, to_raw(r.price)});
            f(Field{"qty", FieldKind::Quantity, to_raw(r.qty)});
            f(Field{"force_priority_loss", FieldKind::Bool, to_raw(r.force_priority_loss)});
            return;
        }
        case CommandType::MassCancel: {
            const MassCancel& m = c.mass_cancel;
            f(Field{"ts", FieldKind::Timestamp, to_raw(m.ts)});
            f(Field{"session", FieldKind::Session, to_raw(m.session)});
            f(Field{"instrument", FieldKind::Instrument, to_raw(m.instrument)});
            f(Field{"cl_ord_id", FieldKind::Token, to_raw(m.cl_ord_id)});
            f(Field{"cancel_on_disconnect", FieldKind::Bool, to_raw(m.cancel_on_disconnect)});
            return;
        }
        case CommandType::SetPhase: {
            const SetPhase& s = c.set_phase;
            f(Field{"ts", FieldKind::Timestamp, to_raw(s.ts)});
            f(Field{"instrument", FieldKind::Instrument, to_raw(s.instrument)});
            f(Field{"phase", FieldKind::Phase, to_raw(s.phase)});
            return;
        }
    }
}

template <typename F>
constexpr void visit_fields(const OrderInfo& o, F&& f) {
    f(Field{"order_id", FieldKind::OrderId, to_raw(o.order_id)});
    f(Field{"cl_ord_id", FieldKind::Token, to_raw(o.cl_ord_id)});
    f(Field{"instrument", FieldKind::Instrument, to_raw(o.instrument)});
    f(Field{"session", FieldKind::Session, to_raw(o.session)});
    f(Field{"owner", FieldKind::Owner, to_raw(o.owner)});
    f(Field{"side", FieldKind::Side, to_raw(o.side)});
    f(Field{"tif", FieldKind::TimeInForce, to_raw(o.tif)});
    f(Field{"stp", FieldKind::StpMode, to_raw(o.stp)});
    f(Field{"post_only", FieldKind::Bool, to_raw(o.post_only)});
    f(Field{"price", FieldKind::Price, to_raw(o.price)});
    f(Field{"qty", FieldKind::Quantity, to_raw(o.qty)});
    f(Field{"leaves_qty", FieldKind::Quantity, to_raw(o.leaves_qty)});
    f(Field{"cum_qty", FieldKind::Quantity, to_raw(o.cum_qty)});
}

}  // namespace exsim::sim

#endif
