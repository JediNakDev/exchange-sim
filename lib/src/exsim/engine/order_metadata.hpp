#ifndef EXSIM_ENGINE_ORDER_METADATA_HPP
#define EXSIM_ENGINE_ORDER_METADATA_HPP

// Exchange metadata indexed by the original LOB order's context_index.
// The LOB owns resting orders, price levels, FIFO links and remaining shares.
// These preallocated arrays retain session/STP attributes, cumulative fills,
// and engine/session/instrument query lists. Index 0 means no metadata slot.

#include <types.hpp>

#include <cstdint>

namespace exsim::engine {

using OrderIndex = std::uint32_t;
inline constexpr OrderIndex kNilOrder = 0;

// (session << 48) | cl_ord_id: the key of the (session, token) map and the
// way FillLeg/Canceled recover both identifiers from one hot field.
inline constexpr unsigned kSessionTokenShift = 48;

[[nodiscard]] constexpr std::uint64_t make_session_token(SessionId session, ClOrdId cl_ord_id) noexcept {
    return (static_cast<std::uint64_t>(session) << kSessionTokenShift) | cl_ord_id;
}

[[nodiscard]] constexpr SessionId session_of(std::uint64_t session_token) noexcept {
    return static_cast<SessionId>(session_token >> kSessionTokenShift);
}

[[nodiscard]] constexpr ClOrdId cl_ord_id_of(std::uint64_t session_token) noexcept {
    return session_token & kMaxClOrdId;
}

struct OrderHot {
    OrderIndex next;              // metadata free-list link when the slot is free
    Quantity leaves_qty;
    Quantity cum_qty;
    OwnerId owner;
    OrderId order_id;
    std::uint64_t session_token;  // make_session_token(session, cl_ord_id)
};
static_assert(sizeof(OrderHot) == 32, "two hot records per cache line");

struct OrderCold {
    std::uint32_t tick;           // ladder index: price = min_price + tick * tick_size
    InstrumentId instrument;
    Side side;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
    std::uint16_t reserved;
    OrderIndex next_by_order_id;  // engine-wide list, ascending OrderId
    OrderIndex prev_by_order_id;
    OrderIndex next_in_session;   // per-session list, ascending OrderId
    OrderIndex prev_in_session;
};
static_assert(sizeof(OrderCold) == 28);

// Head and tail of an intrusive doubly linked list of orders.
struct OrderListEnds {
    OrderIndex head;
    OrderIndex tail;
};

// Per-instrument list links, kept apart from OrderCold (which is full at 32
// bytes) and touched only on add and remove.
struct InstrumentListLinks {
    OrderIndex next;  // per-instrument list, ascending OrderId
    OrderIndex prev;
};
static_assert(sizeof(InstrumentListLinks) == 8, "eight instrument links per cache line");

}  // namespace exsim::engine

#endif
