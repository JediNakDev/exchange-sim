#ifndef EXSIM_ENGINE_ORDER_ARENA_HPP
#define EXSIM_ENGINE_ORDER_ARENA_HPP

// Index-addressed order arena records.
//
// Every resting order owns one slot in two parallel arrays (structure of
// arrays) addressed by a 32-bit OrderIndex. Index 0 is the nil sentinel, so a
// zero-filled link means "none" and the arena holds max_orders + 1 slots.
//
//   OrderHot   32 B  everything a fill touches on the resting order: FIFO
//                    successor, leaves/cum, STP owner and the identifiers
//                    copied into the FillLeg. A partial fill of a resting
//                    order touches exactly one 32 B record.
//   OrderCold  32 B  everything else: FIFO predecessor, ladder position,
//                    order attributes and the two ascending-OrderId lists
//                    (engine-wide and per session). Touched on removal,
//                    cancel, replace and queries.
//
// The FIFO predecessor of a level's head is never read (removal compares the
// index against Level::head instead), so popping the head of a level never
// has to touch the next order's cold record.

#include <exsim/types.hpp>

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
    OrderIndex next;              // FIFO successor at the price level; free-list link when free
    Quantity leaves_qty;
    Quantity cum_qty;
    OwnerId owner;
    OrderId order_id;
    std::uint64_t session_token;  // make_session_token(session, cl_ord_id)
};
static_assert(sizeof(OrderHot) == 32, "two hot records per cache line");

struct OrderCold {
    OrderIndex prev;              // FIFO predecessor; meaningless while the order is the level head
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
static_assert(sizeof(OrderCold) == 32, "two cold records per cache line");

// Head and tail of an intrusive doubly linked list threaded through OrderCold.
struct OrderListEnds {
    OrderIndex head;
    OrderIndex tail;
};

}  // namespace exsim::engine

#endif
