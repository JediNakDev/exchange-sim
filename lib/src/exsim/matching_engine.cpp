// exsim::MatchingEngine, variant C: index-addressed order arena with a
// hot/cold field split. Semantics: matching engine contract. Hot-path rules:
// architecture (no allocation, exceptions, virtuals or strings after
// construction).
//
// Layout summary
//   orders      two parallel arrays OrderHot[] / OrderCold[] of max_orders + 1
//               slots, addressed by uint32 OrderIndex (0 = nil). Free slots
//               form an index stack threaded through OrderHot::next.
//   books       one InstrumentBook per instrument: tick-indexed Level arrays
//               per side, a 3-level occupancy bitmap per side, cached best
//               tick per side.
//   lookups     lob::FlatHashMap<uint32_t> keyed by (session << 48 | token),
//               sized for max_orders at <= 50% load, growth disabled. There
//               is no OrderId map: find_order(OrderId) is a query and walks
//               the ascending-OrderId list instead.
//   ordering    engine-wide, per-session and per-instrument intrusive lists
//               in ascending OrderId order (append on rest; OrderIds are
//               monotonic). for_each_order walks the engine-wide list,
//               MassCancel the session list, EndOfDay expiry and the
//               indicative imbalance the instrument list.

#include <exsim/matching_engine.hpp>

#include "engine/fixed_array.hpp"
#include "engine/order_arena.hpp"
#include "engine/price_ladder.hpp"

#include <lob/compiler.hpp>
#include <lob/flat_hash_map.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <vector>

namespace exsim {

namespace {

using engine::cl_ord_id_of;
using engine::FixedArray;
using engine::InstrumentBook;
using engine::InstrumentListLinks;
using engine::kNilOrder;
using engine::kNoAskTick;
using engine::kNoBidTick;
using engine::Level;
using engine::make_session_token;
using engine::OccupancyBitmap;
using engine::OrderCold;
using engine::OrderHot;
using engine::OrderIndex;
using engine::OrderListEnds;
using engine::PriceLadder;
using engine::session_of;

constexpr std::size_t kSessionSlots = std::size_t{std::numeric_limits<SessionId>::max()} + 1;

template <typename Enum>
[[nodiscard]] constexpr auto raw_value(Enum value) noexcept {
    return static_cast<std::underlying_type_t<Enum>>(value);
}

[[nodiscard]] constexpr Side opposite(Side side) noexcept { return side == Side::BUY ? Side::SELL : Side::BUY; }

[[nodiscard]] constexpr Aggressor aggressor_of(Side side) noexcept {
    return side == Side::BUY ? Aggressor::Buy : Aggressor::Sell;
}

// Initialize mandatory active fields; padding is never compared or serialized.
[[nodiscard]] inline Event blank_event(EventType type, InstrumentId instrument, Timestamp ts) noexcept {
    switch (type) {
#define ACTIVE(tag, member) case EventType::tag: return Event{type, instrument, ts, {.member = {}}};
        ACTIVE(Accepted, accepted)
        ACTIVE(Rejected, rejected)
        ACTIVE(Rested, rested)
        ACTIVE(Fill, fill)
        ACTIVE(Canceled, canceled)
        ACTIVE(Replaced, replaced)
        ACTIVE(MassCancelDone, mass_cancel_done)
        ACTIVE(PhaseChanged, phase_changed)
        ACTIVE(AuctionSummary, auction_summary)
#undef ACTIVE
    }
    std::abort();
}

inline void append_event(EventBuffer& out, const Event& event) noexcept {
    if (!out.push(event)) std::abort(); // preflight failure is an implementation defect
}

std::size_t validated_instruments(const EngineConfig& config) {
    (void)required_event_capacity(config);
    return config.max_instruments;
}

inline void set_fill_leg(FillLeg& leg, OrderId order_id, ClOrdId cl_ord_id, SessionId session, OwnerId owner,
                         Quantity leaves_qty, Quantity cum_qty) noexcept {
    leg.order_id = order_id;
    leg.cl_ord_id = cl_ord_id;
    leg.session = session;
    leg.owner = owner;
    leg.leaves_qty = leaves_qty;
    leg.cum_qty = cum_qty;
}

// The order currently being matched (a new order, or a replace that lost
// priority). It lives on the stack until it rests.
struct IncomingOrder {
    OrderId order_id;
    ClOrdId cl_ord_id;
    Price price;               // as given; for market orders the command price, verbatim
    std::int32_t limit_tick;   // market orders: the most aggressive tick of the ladder
    Quantity qty;
    Quantity leaves_qty;
    Quantity cum_qty;
    OwnerId owner;
    SessionId session;
    InstrumentId instrument;
    Side side;
    OrdType type;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
};

// Result of the uncross price search (SPEC 9).
struct UncrossPlan {
    bool crossed = false;
    std::int32_t tick = 0;
    std::uint64_t matched_qty = 0;  // V(p*)
    std::uint64_t buy_qty = 0;      // B(p*)
    std::uint64_t sell_qty = 0;     // S(p*)
};

}  // namespace

struct MatchingEngine::Impl {
    explicit Impl(const EngineConfig& config);

    // ---- configuration --------------------------------------------------
    std::size_t max_instruments;
    std::uint32_t max_orders;

    // ---- state ----------------------------------------------------------
    std::vector<InstrumentBook> books;  // books[id - 1]; reserved up front so pointers stay stable
    FixedArray<OrderHot> hot;
    FixedArray<OrderCold> cold;
    OrderIndex free_head = kNilOrder;
    std::uint32_t live_count = 0;
    lob::FlatHashMap<std::uint32_t> order_by_session_token;
    FixedArray<OrderListEnds> orders_by_session;         // per SessionId, ascending OrderId
    FixedArray<InstrumentListLinks> instrument_links;    // per order, threads orders_by_instrument
    FixedArray<OrderListEnds> orders_by_instrument;      // per InstrumentId, ascending OrderId
    OrderListEnds all_orders{};                          // engine-wide, ascending OrderId
    OrderId next_order_id = 1;
    MatchId next_match_id = 1;

    // ---- command handlers -----------------------------------------------
    void on_new_order(const NewOrder& cmd, EventBuffer& out);
    void on_cancel(const CancelOrder& cmd, EventBuffer& out);
    void on_replace(const ReplaceOrder& cmd, EventBuffer& out);
    void on_mass_cancel(const MassCancel& cmd, EventBuffer& out);
    void on_set_phase(const SetPhase& cmd, EventBuffer& out);

    // ---- helpers --------------------------------------------------------
    [[nodiscard]] InstrumentBook* find_book(InstrumentId id) noexcept {
        return (id == 0 || id > books.size()) ? nullptr : &books[id - 1];
    }
    [[nodiscard]] const InstrumentBook* find_book(InstrumentId id) const noexcept {
        return (id == 0 || id > books.size()) ? nullptr : &books[id - 1];
    }
    [[nodiscard]] OrderIndex find_live_order(SessionId session, ClOrdId cl_ord_id) const noexcept {
        if (session == 0 || cl_ord_id == 0 || cl_ord_id > kMaxClOrdId) {
            return kNilOrder;
        }
        const std::uint32_t* slot = order_by_session_token.find(make_session_token(session, cl_ord_id));
        return slot == nullptr ? kNilOrder : *slot;
    }

    void reject_new_order(const NewOrder& cmd, RejectReason reason, EventBuffer& out);
    void reject_replace(const ReplaceOrder& cmd, RejectReason reason, OrderId order_id, InstrumentId instrument,
                        EventBuffer& out);

    void emit_accepted(const IncomingOrder& order, Timestamp ts, EventBuffer& out);
    void emit_incoming_canceled(const IncomingOrder& order, CancelReason reason, Timestamp ts, EventBuffer& out);
    void emit_resting_canceled(const InstrumentBook& book, OrderIndex index, CancelReason reason,
                               ClOrdId request_cl_ord_id, Timestamp ts, EventBuffer& out);

    [[nodiscard]] bool fill_or_kill_can_fill(const InstrumentBook& book, const IncomingOrder& order) const noexcept;
    template <Side kAggressorSide>
    bool match(InstrumentBook& book, IncomingOrder& order, Timestamp ts, EventBuffer& out);
    void match_and_dispose(InstrumentBook& book, IncomingOrder& order, Timestamp ts, EventBuffer& out);
    void rest(InstrumentBook& book, const IncomingOrder& order, Timestamp ts, EventBuffer& out);

    void pop_level_head(InstrumentBook& book, Side side, Level& level, std::int32_t tick, OrderIndex index);
    void remove_resting(OrderIndex index);
    void level_emptied(InstrumentBook& book, Side side, std::int32_t tick) noexcept;
    void retire(OrderIndex index);

    void list_append_by_order_id(OrderIndex index) noexcept;
    void list_unlink_by_order_id(OrderIndex index) noexcept;
    void list_append_in_session(SessionId session, OrderIndex index) noexcept;
    void list_unlink_in_session(SessionId session, OrderIndex index) noexcept;
    void list_append_in_instrument(InstrumentId instrument, OrderIndex index) noexcept;
    void list_unlink_in_instrument(InstrumentId instrument, OrderIndex index) noexcept;

    [[nodiscard]] UncrossPlan plan_uncross(const InstrumentBook& book) const noexcept;
    void uncross(InstrumentBook& book, Timestamp ts, EventBuffer& out);
    void expire_day_orders(InstrumentBook& book, Timestamp ts, EventBuffer& out);
    void apply_phase(InstrumentBook& book, Phase new_phase, Timestamp ts, EventBuffer& out);

    [[nodiscard]] OrderInfo order_info(OrderIndex index) const noexcept;
};

// ===========================================================================
// Construction
// ===========================================================================

MatchingEngine::Impl::Impl(const EngineConfig& config)
    : max_instruments(validated_instruments(config)),
      max_orders(static_cast<std::uint32_t>(config.max_orders)),
      hot(std::size_t{max_orders} + 1),
      cold(std::size_t{max_orders} + 1),
      order_by_session_token(max_orders),
      orders_by_session(kSessionSlots),
      instrument_links(std::size_t{max_orders} + 1),
      orders_by_instrument(max_instruments + 1) {
    books.reserve(max_instruments);
    // Free-index stack: 1, 2, ..., max_orders (first allocations are sequential).
    for (std::uint32_t i = 1; i < max_orders; ++i) {
        hot[i].next = i + 1;
    }
    free_head = max_orders == 0 ? kNilOrder : 1;
}

// ===========================================================================
// Event emission
// ===========================================================================

void MatchingEngine::Impl::reject_new_order(const NewOrder& cmd, RejectReason reason, EventBuffer& out) {
    Event event = blank_event(EventType::Rejected, cmd.instrument, cmd.ts);
    event.rejected.cmd = CommandType::NewOrder;
    event.rejected.reason = reason;
    event.rejected.session = cmd.session;
    event.rejected.cl_ord_id = cmd.cl_ord_id;
    append_event(out, event);
}

void MatchingEngine::Impl::reject_replace(const ReplaceOrder& cmd, RejectReason reason, OrderId order_id,
                                          InstrumentId instrument, EventBuffer& out) {
    Event event = blank_event(EventType::Rejected, instrument, cmd.ts);
    event.rejected.cmd = CommandType::Replace;
    event.rejected.reason = reason;
    event.rejected.session = cmd.session;
    event.rejected.cl_ord_id = cmd.cl_ord_id;
    event.rejected.orig_cl_ord_id = cmd.orig_cl_ord_id;
    event.rejected.order_id = order_id;
    append_event(out, event);
}

void MatchingEngine::Impl::emit_accepted(const IncomingOrder& order, Timestamp ts, EventBuffer& out) {
    Event event = blank_event(EventType::Accepted, order.instrument, ts);
    AcceptedEvent& accepted = event.accepted;
    accepted.order_id = order.order_id;
    accepted.cl_ord_id = order.cl_ord_id;
    accepted.session = order.session;
    accepted.owner = order.owner;
    accepted.side = order.side;
    accepted.ord_type = order.type;
    accepted.tif = order.tif;
    accepted.stp = order.stp;
    accepted.post_only = order.post_only;
    accepted.price = order.price;
    accepted.qty = order.qty;
    append_event(out, event);
}

void MatchingEngine::Impl::emit_incoming_canceled(const IncomingOrder& order, CancelReason reason, Timestamp ts,
                                                  EventBuffer& out) {
    Event event = blank_event(EventType::Canceled, order.instrument, ts);
    CanceledEvent& canceled = event.canceled;
    canceled.order_id = order.order_id;
    canceled.cl_ord_id = order.cl_ord_id;
    canceled.session = order.session;
    canceled.side = order.side;
    canceled.price = order.price;
    canceled.canceled_qty = order.leaves_qty;
    canceled.cum_qty = order.cum_qty;
    canceled.reason = reason;
    canceled.was_resting = false;
    append_event(out, event);
}

void MatchingEngine::Impl::emit_resting_canceled(const InstrumentBook& book, OrderIndex index, CancelReason reason,
                                                 ClOrdId request_cl_ord_id, Timestamp ts, EventBuffer& out) {
    const OrderHot& order_hot = hot[index];
    const OrderCold& order_cold = cold[index];
    Event event = blank_event(EventType::Canceled, order_cold.instrument, ts);
    CanceledEvent& canceled = event.canceled;
    canceled.order_id = order_hot.order_id;
    canceled.cl_ord_id = cl_ord_id_of(order_hot.session_token);
    canceled.request_cl_ord_id = request_cl_ord_id;
    canceled.session = session_of(order_hot.session_token);
    canceled.side = order_cold.side;
    canceled.price = book.price_of(static_cast<std::int32_t>(order_cold.tick));
    canceled.canceled_qty = order_hot.leaves_qty;
    canceled.cum_qty = order_hot.cum_qty;
    canceled.reason = reason;
    canceled.was_resting = true;
    append_event(out, event);
}

// ===========================================================================
// Intrusive ascending-OrderId lists
// ===========================================================================

void MatchingEngine::Impl::list_append_by_order_id(OrderIndex index) noexcept {
    OrderCold& order_cold = cold[index];
    order_cold.next_by_order_id = kNilOrder;
    order_cold.prev_by_order_id = all_orders.tail;
    if (all_orders.tail != kNilOrder) {
        cold[all_orders.tail].next_by_order_id = index;
    } else {
        all_orders.head = index;
    }
    all_orders.tail = index;
}

void MatchingEngine::Impl::list_unlink_by_order_id(OrderIndex index) noexcept {
    const OrderCold& order_cold = cold[index];
    if (order_cold.prev_by_order_id != kNilOrder) {
        cold[order_cold.prev_by_order_id].next_by_order_id = order_cold.next_by_order_id;
    } else {
        all_orders.head = order_cold.next_by_order_id;
    }
    if (order_cold.next_by_order_id != kNilOrder) {
        cold[order_cold.next_by_order_id].prev_by_order_id = order_cold.prev_by_order_id;
    } else {
        all_orders.tail = order_cold.prev_by_order_id;
    }
}

void MatchingEngine::Impl::list_append_in_session(SessionId session, OrderIndex index) noexcept {
    OrderListEnds& list = orders_by_session[session];
    OrderCold& order_cold = cold[index];
    order_cold.next_in_session = kNilOrder;
    order_cold.prev_in_session = list.tail;
    if (list.tail != kNilOrder) {
        cold[list.tail].next_in_session = index;
    } else {
        list.head = index;
    }
    list.tail = index;
}

void MatchingEngine::Impl::list_unlink_in_session(SessionId session, OrderIndex index) noexcept {
    OrderListEnds& list = orders_by_session[session];
    const OrderCold& order_cold = cold[index];
    if (order_cold.prev_in_session != kNilOrder) {
        cold[order_cold.prev_in_session].next_in_session = order_cold.next_in_session;
    } else {
        list.head = order_cold.next_in_session;
    }
    if (order_cold.next_in_session != kNilOrder) {
        cold[order_cold.next_in_session].prev_in_session = order_cold.prev_in_session;
    } else {
        list.tail = order_cold.prev_in_session;
    }
}

void MatchingEngine::Impl::list_append_in_instrument(InstrumentId instrument, OrderIndex index) noexcept {
    OrderListEnds& list = orders_by_instrument[instrument];
    InstrumentListLinks& links = instrument_links[index];
    links.next = kNilOrder;
    links.prev = list.tail;
    if (list.tail != kNilOrder) {
        instrument_links[list.tail].next = index;
    } else {
        list.head = index;
    }
    list.tail = index;
}

void MatchingEngine::Impl::list_unlink_in_instrument(InstrumentId instrument, OrderIndex index) noexcept {
    OrderListEnds& list = orders_by_instrument[instrument];
    const InstrumentListLinks& links = instrument_links[index];
    if (links.prev != kNilOrder) {
        instrument_links[links.prev].next = links.next;
    } else {
        list.head = links.next;
    }
    if (links.next != kNilOrder) {
        instrument_links[links.next].prev = links.prev;
    } else {
        list.tail = links.prev;
    }
}

// ===========================================================================
// Book maintenance
// ===========================================================================

// Recomputes the cached best tick after the level at `tick` became empty.
void MatchingEngine::Impl::level_emptied(InstrumentBook& book, Side side, std::int32_t tick) noexcept {
    PriceLadder& ladder = book.ladder(side);
    ladder.occupied.clear(static_cast<std::uint32_t>(tick));
    if (side == Side::BUY) {
        if (tick == book.best_bid_tick) {
            const std::uint32_t found =
                tick == 0 ? OccupancyBitmap::kNotFound
                          : ladder.occupied.find_at_or_below(static_cast<std::uint32_t>(tick - 1));
            book.best_bid_tick = found == OccupancyBitmap::kNotFound ? kNoBidTick : static_cast<std::int32_t>(found);
        }
    } else {
        if (tick == book.best_ask_tick) {
            const std::uint32_t found = ladder.occupied.find_at_or_above(static_cast<std::uint32_t>(tick + 1));
            book.best_ask_tick = found == OccupancyBitmap::kNotFound ? kNoAskTick : static_cast<std::int32_t>(found);
        }
    }
}

// Releases everything an order holds outside its price level: list links,
// the (session, token) map entry and the arena slot.
void MatchingEngine::Impl::retire(OrderIndex index) {
    OrderHot& order_hot = hot[index];
    const SessionId session = session_of(order_hot.session_token);
    list_unlink_by_order_id(index);
    list_unlink_in_session(session, index);
    list_unlink_in_instrument(cold[index].instrument, index);
    const bool erased = order_by_session_token.erase(order_hot.session_token);
    assert(erased);
    (void)erased;
    order_hot.next = free_head;
    free_head = index;
    --live_count;
}

// Removes the head order of `level` (whose total_qty the caller already
// adjusted) and retires it.
void MatchingEngine::Impl::pop_level_head(InstrumentBook& book, Side side, Level& level, std::int32_t tick,
                                          OrderIndex index) {
    assert(level.head == index);
    level.head = hot[index].next;
    if (--level.order_count == 0) {
        level.tail = kNilOrder;
        level_emptied(book, side, tick);
    }
    retire(index);
}

// Removes a resting order from anywhere in its FIFO and retires it.
void MatchingEngine::Impl::remove_resting(OrderIndex index) {
    const OrderHot& order_hot = hot[index];
    const OrderCold& order_cold = cold[index];
    InstrumentBook& book = books[order_cold.instrument - 1];
    const std::int32_t tick = static_cast<std::int32_t>(order_cold.tick);
    Level& level = book.ladder(order_cold.side).levels[order_cold.tick];
    level.total_qty -= order_hot.leaves_qty;
    if (--level.order_count == 0) {
        level.head = kNilOrder;
        level.tail = kNilOrder;
        level_emptied(book, order_cold.side, tick);
    } else if (level.head == index) {
        level.head = order_hot.next;
    } else {
        hot[order_cold.prev].next = order_hot.next;
        if (level.tail == index) {
            level.tail = order_cold.prev;
        } else {
            cold[order_hot.next].prev = order_cold.prev;
        }
    }
    retire(index);
}

void MatchingEngine::Impl::rest(InstrumentBook& book, const IncomingOrder& order, Timestamp ts, EventBuffer& out) {
    assert(free_head != kNilOrder);
    const OrderIndex index = free_head;
    OrderHot& order_hot = hot[index];
    free_head = order_hot.next;

    order_hot.next = kNilOrder;
    order_hot.leaves_qty = order.leaves_qty;
    order_hot.cum_qty = order.cum_qty;
    order_hot.owner = order.owner;
    order_hot.order_id = order.order_id;
    order_hot.session_token = make_session_token(order.session, order.cl_ord_id);

    OrderCold& order_cold = cold[index];
    order_cold.tick = static_cast<std::uint32_t>(order.limit_tick);
    order_cold.instrument = order.instrument;
    order_cold.side = order.side;
    order_cold.tif = order.tif;
    order_cold.stp = order.stp;
    order_cold.post_only = order.post_only;

    PriceLadder& ladder = book.ladder(order.side);
    Level& level = ladder.levels[order_cold.tick];
    if (level.order_count == 0) {
        order_cold.prev = kNilOrder;
        level.head = index;
        ladder.occupied.set(order_cold.tick);
        if (order.side == Side::BUY) {
            if (order.limit_tick > book.best_bid_tick) {
                book.best_bid_tick = order.limit_tick;
            }
        } else if (order.limit_tick < book.best_ask_tick) {
            book.best_ask_tick = order.limit_tick;
        }
    } else {
        order_cold.prev = level.tail;
        hot[level.tail].next = index;
    }
    level.tail = index;
    ++level.order_count;
    level.total_qty += order.leaves_qty;

    list_append_by_order_id(index);
    list_append_in_session(order.session, index);
    list_append_in_instrument(order.instrument, index);
    const bool inserted = order_by_session_token.insert(order_hot.session_token, index);
    assert(inserted);
    (void)inserted;
    ++live_count;

    Event event = blank_event(EventType::Rested, order.instrument, ts);
    RestedEvent& rested = event.rested;
    rested.order_id = order.order_id;
    rested.cl_ord_id = order.cl_ord_id;
    rested.session = order.session;
    rested.side = order.side;
    rested.price = order.price;
    rested.leaves_qty = order.leaves_qty;
    append_event(out, event);
}

// ===========================================================================
// Matching (SPEC 4.3, 4.5, 4.6)
// ===========================================================================

bool MatchingEngine::Impl::fill_or_kill_can_fill(const InstrumentBook& book, const IncomingOrder& order) const noexcept {
    const Side resting_side = opposite(order.side);
    const PriceLadder& ladder = book.ladder(resting_side);
    const bool stp_active = order.stp != StpMode::None && order.owner != 0;
    const std::uint64_t needed = order.leaves_qty;
    std::uint64_t available = 0;
    std::int32_t tick = order.side == Side::BUY ? book.best_ask_tick : book.best_bid_tick;
    while (book.crosses_opposite_best(order.side, order.limit_tick) &&
           (order.side == Side::BUY ? order.limit_tick >= tick : order.limit_tick <= tick)) {
        const Level& level = ladder.levels[static_cast<std::uint32_t>(tick)];
        if (!stp_active) {
            available += level.total_qty;
            if (available >= needed) {
                return true;
            }
        } else {
            for (OrderIndex index = level.head; index != kNilOrder; index = hot[index].next) {
                const OrderHot& resting = hot[index];
                if (resting.owner == order.owner) {
                    if (order.stp == StpMode::CancelResting) {
                        continue;
                    }
                    return false;  // walk stops at the self-match with available < needed
                }
                available += resting.leaves_qty;
                if (available >= needed) {
                    return true;
                }
            }
        }
        std::uint32_t next_tick;
        if (order.side == Side::BUY) {
            next_tick = ladder.occupied.find_at_or_above(static_cast<std::uint32_t>(tick) + 1);
        } else {
            next_tick = tick == 0 ? OccupancyBitmap::kNotFound
                                  : ladder.occupied.find_at_or_below(static_cast<std::uint32_t>(tick) - 1);
        }
        if (next_tick == OccupancyBitmap::kNotFound) {
            return false;
        }
        tick = static_cast<std::int32_t>(next_tick);
    }
    return false;
}

// Matches `order` against the opposite side (SPEC 4.5). Returns true if the
// order was cancelled by self-trade prevention.
template <Side kAggressorSide>
bool MatchingEngine::Impl::match(InstrumentBook& book, IncomingOrder& order, Timestamp ts, EventBuffer& out) {
    constexpr Side kRestingSide = opposite(kAggressorSide);
    PriceLadder& ladder = book.ladder(kRestingSide);
    const bool stp_active = order.stp != StpMode::None && order.owner != 0;

    while (order.leaves_qty != 0) {
        const std::int32_t best_tick = kAggressorSide == Side::BUY ? book.best_ask_tick : book.best_bid_tick;
        if (kAggressorSide == Side::BUY ? order.limit_tick < best_tick : order.limit_tick > best_tick) {
            return false;  // opposite side empty or no longer crossing
        }
        Level& level = ladder.levels[static_cast<std::uint32_t>(best_tick)];
        const Price level_price = book.price_of(best_tick);
        do {
            const OrderIndex index = level.head;
            OrderHot& resting = hot[index];
            __builtin_prefetch(&hot[resting.next]);

            if (LOB_UNLIKELY(stp_active && resting.owner == order.owner)) {
                const StpMode mode = order.stp;
                if (mode != StpMode::CancelAggressor) {
                    emit_resting_canceled(book, index, CancelReason::SelfTradePrevention, 0, ts, out);
                    level.total_qty -= resting.leaves_qty;
                    pop_level_head(book, kRestingSide, level, best_tick, index);
                }
                if (mode != StpMode::CancelResting) {
                    emit_incoming_canceled(order, CancelReason::SelfTradePrevention, ts, out);
                    return true;
                }
                continue;
            }

            const Quantity fill_qty = std::min(order.leaves_qty, resting.leaves_qty);
            resting.leaves_qty -= fill_qty;
            resting.cum_qty += fill_qty;
            order.leaves_qty -= fill_qty;
            order.cum_qty += fill_qty;
            level.total_qty -= fill_qty;

            Event event = blank_event(EventType::Fill, order.instrument, ts);
            FillEvent& fill = event.fill;
            fill.match_id = next_match_id++;
            fill.price = level_price;
            fill.qty = fill_qty;
            fill.aggressor = aggressor_of(kAggressorSide);
            FillLeg& aggressor_leg = kAggressorSide == Side::BUY ? fill.buy : fill.sell;
            FillLeg& resting_leg = kAggressorSide == Side::BUY ? fill.sell : fill.buy;
            set_fill_leg(aggressor_leg, order.order_id, order.cl_ord_id, order.session, order.owner, order.leaves_qty,
                         order.cum_qty);
            set_fill_leg(resting_leg, resting.order_id, cl_ord_id_of(resting.session_token),
                         session_of(resting.session_token), resting.owner, resting.leaves_qty, resting.cum_qty);
            append_event(out, event);

            book.last_trade_price = level_price;
            book.has_last_trade_price = true;

            if (resting.leaves_qty == 0) {
                pop_level_head(book, kRestingSide, level, best_tick, index);
            }
        } while (order.leaves_qty != 0 && level.order_count != 0);
    }
    return false;
}

void MatchingEngine::Impl::match_and_dispose(InstrumentBook& book, IncomingOrder& order, Timestamp ts,
                                             EventBuffer& out) {
    const bool canceled_by_stp =
        order.side == Side::BUY ? match<Side::BUY>(book, order, ts, out) : match<Side::SELL>(book, order, ts, out);
    if (canceled_by_stp || order.leaves_qty == 0) {
        return;
    }
    if (order.type == OrdType::Market || order.tif == TimeInForce::IOC || order.tif == TimeInForce::FOK) {
        emit_incoming_canceled(order, CancelReason::ImmediateOrCancel, ts, out);
        return;
    }
    rest(book, order, ts, out);
}

// ===========================================================================
// NewOrder (SPEC 4)
// ===========================================================================

void MatchingEngine::Impl::on_new_order(const NewOrder& cmd, EventBuffer& out) {
    InstrumentBook* const book_ptr = find_book(cmd.instrument);
    if (LOB_UNLIKELY(book_ptr == nullptr)) {
        return reject_new_order(cmd, RejectReason::UnknownInstrument, out);
    }
    InstrumentBook& book = *book_ptr;
    if (LOB_UNLIKELY(cmd.session == 0 || cmd.cl_ord_id == 0 || cmd.cl_ord_id > kMaxClOrdId)) {
        return reject_new_order(cmd, RejectReason::InvalidClOrdId, out);
    }
    if (LOB_UNLIKELY(order_by_session_token.find(make_session_token(cmd.session, cmd.cl_ord_id)) != nullptr)) {
        return reject_new_order(cmd, RejectReason::DuplicateClOrdId, out);
    }
    const bool is_market = cmd.type == OrdType::Market;
    const bool is_immediate = cmd.tif == TimeInForce::IOC || cmd.tif == TimeInForce::FOK;
    if (LOB_UNLIKELY(raw_value(cmd.side) > raw_value(Side::SELL) || raw_value(cmd.type) > raw_value(OrdType::Market) ||
                     raw_value(cmd.tif) > raw_value(TimeInForce::FOK) ||
                     raw_value(cmd.stp) > raw_value(StpMode::CancelBoth) ||
                     (cmd.post_only && (is_market || is_immediate)))) {
        return reject_new_order(cmd, RejectReason::InvalidField, out);
    }
    if (LOB_UNLIKELY(!book.is_valid_quantity(cmd.qty))) {
        return reject_new_order(cmd, RejectReason::InvalidQuantity, out);
    }
    std::int32_t limit_tick = 0;
    if (is_market) {
        limit_tick = cmd.side == Side::BUY ? static_cast<std::int32_t>(book.tick_count - 1) : 0;
    } else if (LOB_UNLIKELY(!book.tick_of(cmd.price, limit_tick))) {
        return reject_new_order(cmd, RejectReason::InvalidPrice, out);
    }
    switch (book.phase) {
        case Phase::Continuous:
            if (LOB_UNLIKELY(cmd.post_only && book.crosses_opposite_best(cmd.side, limit_tick))) {
                return reject_new_order(cmd, RejectReason::PostOnlyWouldCross, out);
            }
            break;
        case Phase::PreOpen:
        case Phase::PreClose:
            if (LOB_UNLIKELY(is_market || is_immediate)) {
                return reject_new_order(cmd, RejectReason::NotAllowedInPhase, out);
            }
            break;
        case Phase::Closed:
            return reject_new_order(cmd, RejectReason::MarketClosed, out);
        case Phase::Halted:
            return reject_new_order(cmd, RejectReason::InstrumentHalted, out);
    }
    if (LOB_UNLIKELY(live_count >= max_orders)) {
        return reject_new_order(cmd, RejectReason::CapacityExceeded, out);
    }

    IncomingOrder order;
    order.order_id = next_order_id++;
    order.cl_ord_id = cmd.cl_ord_id;
    order.price = cmd.price;
    order.limit_tick = limit_tick;
    order.qty = cmd.qty;
    order.leaves_qty = cmd.qty;
    order.cum_qty = 0;
    order.owner = cmd.owner;
    order.session = cmd.session;
    order.instrument = cmd.instrument;
    order.side = cmd.side;
    order.type = cmd.type;
    order.tif = cmd.tif;
    order.stp = cmd.stp;
    order.post_only = cmd.post_only;
    emit_accepted(order, cmd.ts, out);

    if (book.phase != Phase::Continuous) {
        rest(book, order, cmd.ts, out);  // call phase: no matching, no cross check
        return;
    }
    if (cmd.tif == TimeInForce::FOK && !fill_or_kill_can_fill(book, order)) {
        emit_incoming_canceled(order, CancelReason::FillOrKill, cmd.ts, out);
        return;
    }
    match_and_dispose(book, order, cmd.ts, out);
}

// ===========================================================================
// CancelOrder (SPEC 5)
// ===========================================================================

void MatchingEngine::Impl::on_cancel(const CancelOrder& cmd, EventBuffer& out) {
    const OrderIndex index = find_live_order(cmd.session, cmd.orig_cl_ord_id);
    if (LOB_UNLIKELY(index == kNilOrder)) {
        Event event = blank_event(EventType::Rejected, 0, cmd.ts);
        event.rejected.cmd = CommandType::Cancel;
        event.rejected.reason = RejectReason::UnknownOrder;
        event.rejected.session = cmd.session;
        event.rejected.cl_ord_id = cmd.cl_ord_id;
        event.rejected.orig_cl_ord_id = cmd.orig_cl_ord_id;
        append_event(out, event);
        return;
    }
    InstrumentBook& book = books[cold[index].instrument - 1];
    if (cmd.reduce_to_total_qty != 0) {
        OrderHot& order = hot[index];
        const Quantity target = cmd.reduce_to_total_qty > order.cum_qty
            ? cmd.reduce_to_total_qty - order.cum_qty : 0;
        if (target >= order.leaves_qty) return;
        if (target != 0) {
            Event event = blank_event(EventType::Canceled, book.id, cmd.ts);
            auto& c = event.canceled;
            c.order_id = order.order_id;
            c.cl_ord_id = cl_ord_id_of(order.session_token);
            c.request_cl_ord_id = cmd.cl_ord_id;
            c.session = cmd.session;
            c.side = cold[index].side;
            c.price = book.price_of(static_cast<std::int32_t>(cold[index].tick));
            c.canceled_qty = order.leaves_qty - target;
            c.cum_qty = order.cum_qty;
            c.reason = CancelReason::UserRequested;
            c.was_resting = true;
            c.leaves_qty = target;
            book.ladder(c.side).levels[cold[index].tick].total_qty -= c.canceled_qty;
            order.leaves_qty = target;
            append_event(out, event);
            return;
        }
    }
    emit_resting_canceled(book, index, CancelReason::UserRequested, cmd.cl_ord_id, cmd.ts, out);
    remove_resting(index);
}

// ===========================================================================
// ReplaceOrder (SPEC 6)
// ===========================================================================

void MatchingEngine::Impl::on_replace(const ReplaceOrder& cmd, EventBuffer& out) {
    const OrderIndex index = find_live_order(cmd.session, cmd.orig_cl_ord_id);
    if (LOB_UNLIKELY(index == kNilOrder)) {
        return reject_replace(cmd, RejectReason::UnknownOrder, 0, 0, out);
    }
    OrderHot& order_hot = hot[index];
    const OrderCold& order_cold = cold[index];
    InstrumentBook& book = books[order_cold.instrument - 1];
    const OrderId old_order_id = order_hot.order_id;
    const InstrumentId instrument = order_cold.instrument;

    if (LOB_UNLIKELY(cmd.cl_ord_id > kMaxClOrdId)) {
        return reject_replace(cmd, RejectReason::InvalidClOrdId, old_order_id, instrument, out);
    }
    const ClOrdId new_cl_ord_id = cmd.cl_ord_id != 0 ? cmd.cl_ord_id : cmd.orig_cl_ord_id;
    const bool token_changes = new_cl_ord_id != cmd.orig_cl_ord_id;
    if (LOB_UNLIKELY(token_changes &&
                     order_by_session_token.find(make_session_token(cmd.session, new_cl_ord_id)) != nullptr)) {
        return reject_replace(cmd, RejectReason::DuplicateClOrdId, old_order_id, instrument, out);
    }
    if (LOB_UNLIKELY(!book.is_valid_quantity(cmd.qty))) {
        return reject_replace(cmd, RejectReason::InvalidQuantity, old_order_id, instrument, out);
    }
    std::int32_t new_tick = 0;
    if (LOB_UNLIKELY(!book.tick_of(cmd.price, new_tick))) {
        return reject_replace(cmd, RejectReason::InvalidPrice, old_order_id, instrument, out);
    }
    if (LOB_UNLIKELY(book.phase == Phase::Closed)) {
        return reject_replace(cmd, RejectReason::MarketClosed, old_order_id, instrument, out);
    }
    if (LOB_UNLIKELY(book.phase == Phase::Halted)) {
        return reject_replace(cmd, RejectReason::InstrumentHalted, old_order_id, instrument, out);
    }
    const Side side = order_cold.side;
    if (LOB_UNLIKELY(order_cold.post_only && book.phase == Phase::Continuous &&
                     book.crosses_opposite_best(side, new_tick))) {
        return reject_replace(cmd, RejectReason::PostOnlyWouldCross, old_order_id, instrument, out);
    }

    // Quantity reduced to (or below) what already executed: the order is done.
    if (cmd.qty <= order_hot.cum_qty) {
        emit_resting_canceled(book, index, CancelReason::UserRequested, cmd.cl_ord_id, cmd.ts, out);
        remove_resting(index);
        return;
    }

    const std::int32_t old_tick = static_cast<std::int32_t>(order_cold.tick);
    const Price old_price = book.price_of(old_tick);
    const Quantity old_leaves_qty = order_hot.leaves_qty;
    const Quantity cum_qty = order_hot.cum_qty;
    const Quantity new_leaves_qty = cmd.qty - cum_qty;
    const SessionId session = cmd.session;

    Event event = blank_event(EventType::Replaced, instrument, cmd.ts);
    ReplacedEvent& replaced = event.replaced;
    replaced.old_order_id = old_order_id;
    replaced.orig_cl_ord_id = cmd.orig_cl_ord_id;
    replaced.cl_ord_id = new_cl_ord_id;
    replaced.session = session;
    replaced.side = side;
    replaced.old_price = old_price;
    replaced.price = cmd.price;
    replaced.old_leaves_qty = old_leaves_qty;
    replaced.qty = cmd.qty;
    replaced.leaves_qty = new_leaves_qty;
    replaced.cum_qty = cum_qty;

    if (!cmd.force_priority_loss && new_tick == old_tick && new_leaves_qty <= old_leaves_qty) {
        // Priority kept: update in place, FIFO position unchanged.
        Level& level = book.ladder(side).levels[order_cold.tick];
        level.total_qty -= old_leaves_qty - new_leaves_qty;
        order_hot.leaves_qty = new_leaves_qty;
        if (token_changes) {
            order_by_session_token.erase(order_hot.session_token);
            order_hot.session_token = make_session_token(session, new_cl_ord_id);
            const bool inserted = order_by_session_token.insert(order_hot.session_token, index);
            assert(inserted);
            (void)inserted;
        }
        replaced.new_order_id = old_order_id;
        replaced.priority_kept = true;
        append_event(out, event);
        return;
    }

    // Priority lost: leave the book, take a new OrderId, re-enter as an aggressor.
    IncomingOrder order;
    order.order_id = next_order_id++;
    order.cl_ord_id = new_cl_ord_id;
    order.price = cmd.price;
    order.limit_tick = new_tick;
    order.qty = cmd.qty;
    order.leaves_qty = new_leaves_qty;
    order.cum_qty = cum_qty;
    order.owner = order_hot.owner;
    order.session = session;
    order.instrument = instrument;
    order.side = side;
    order.type = OrdType::Limit;
    order.tif = order_cold.tif;
    order.stp = order_cold.stp;
    order.post_only = order_cold.post_only;
    remove_resting(index);

    replaced.new_order_id = order.order_id;
    replaced.priority_kept = false;
    append_event(out, event);

    if (book.phase == Phase::Continuous) {
        match_and_dispose(book, order, cmd.ts, out);
    } else {
        rest(book, order, cmd.ts, out);
    }
}

// ===========================================================================
// MassCancel (SPEC 7)
// ===========================================================================

void MatchingEngine::Impl::on_mass_cancel(const MassCancel& cmd, EventBuffer& out) {
    const CancelReason reason = cmd.cancel_on_disconnect ? CancelReason::CancelOnDisconnect : CancelReason::MassCancel;
    std::uint32_t canceled_count = 0;
    OrderIndex index = orders_by_session[cmd.session].head;
    while (index != kNilOrder) {
        const OrderIndex next = cold[index].next_in_session;
        const InstrumentId instrument = cold[index].instrument;
        if (cmd.instrument == 0 || instrument == cmd.instrument) {
            emit_resting_canceled(books[instrument - 1], index, reason, cmd.cl_ord_id, cmd.ts, out);
            remove_resting(index);
            ++canceled_count;
        }
        index = next;
    }
    Event event = blank_event(EventType::MassCancelDone, cmd.instrument, cmd.ts);
    event.mass_cancel_done.session = cmd.session;
    event.mass_cancel_done.request_cl_ord_id = cmd.cl_ord_id;
    event.mass_cancel_done.canceled_count = canceled_count;
    event.mass_cancel_done.reason = reason;
    append_event(out, event);
}

// ===========================================================================
// SetPhase, auction uncross, end-of-day expiry (SPEC 8, 9)
// ===========================================================================

UncrossPlan MatchingEngine::Impl::plan_uncross(const InstrumentBook& book) const noexcept {
    UncrossPlan plan;
    if (book.best_bid_tick == kNoBidTick || book.best_ask_tick == kNoAskTick || !book.is_crossed()) {
        return plan;
    }
    const std::uint32_t low_tick = static_cast<std::uint32_t>(book.best_ask_tick);
    const std::uint32_t high_tick = static_cast<std::uint32_t>(book.best_bid_tick);
    const PriceLadder& bids = book.ladder(Side::BUY);
    const PriceLadder& asks = book.ladder(Side::SELL);
    const std::size_t first_word = low_tick >> 6;
    const std::size_t last_word = high_tick >> 6;
    auto range_mask = [&](std::size_t word_index) {
        std::uint64_t mask = ~std::uint64_t{0};
        if (word_index == first_word) {
            mask &= ~std::uint64_t{0} << (low_tick & 63);
        }
        if (word_index == last_word) {
            mask &= ~std::uint64_t{0} >> (63 - (high_tick & 63));
        }
        return mask;
    };

    // B(low_tick): every bid at or above the best ask (none rest above the best bid).
    std::uint64_t buy_qty_at_or_above = 0;
    for (std::size_t word_index = first_word; word_index <= last_word; ++word_index) {
        std::uint64_t bits = bids.occupied.tick_word(word_index) & range_mask(word_index);
        while (bits != 0) {
            const std::size_t tick = (word_index << 6) | static_cast<std::size_t>(__builtin_ctzll(bits));
            buy_qty_at_or_above += bids.levels[tick].total_qty;
            bits &= bits - 1;
        }
    }

    const Price reference = book.has_last_trade_price ? book.last_trade_price : book.config.reference_price;
    std::uint64_t sell_qty_at_or_below = 0;
    std::uint64_t best_volume = 0;
    std::uint64_t best_imbalance = 0;
    std::uint64_t best_distance = 0;
    bool have_candidate = false;
    for (std::size_t word_index = first_word; word_index <= last_word; ++word_index) {
        std::uint64_t bits =
            (bids.occupied.tick_word(word_index) | asks.occupied.tick_word(word_index)) & range_mask(word_index);
        while (bits != 0) {
            const std::size_t tick = (word_index << 6) | static_cast<std::size_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            sell_qty_at_or_below += asks.levels[tick].total_qty;  // empty levels hold 0
            const std::uint64_t volume = std::min(buy_qty_at_or_above, sell_qty_at_or_below);
            const std::uint64_t imbalance = buy_qty_at_or_above >= sell_qty_at_or_below
                                                ? buy_qty_at_or_above - sell_qty_at_or_below
                                                : sell_qty_at_or_below - buy_qty_at_or_above;
            const Price price = book.price_of(static_cast<std::int32_t>(tick));
            const std::uint64_t distance = price >= reference
                                               ? static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(reference)
                                               : static_cast<std::uint64_t>(reference) - static_cast<std::uint64_t>(price);
            // Ascending scan: strict comparisons keep the lower price on a full tie.
            bool better = !have_candidate || volume > best_volume;
            if (!better && volume == best_volume) {
                better = imbalance < best_imbalance || (imbalance == best_imbalance && distance < best_distance);
            }
            if (better) {
                have_candidate = true;
                best_volume = volume;
                best_imbalance = imbalance;
                best_distance = distance;
                plan.tick = static_cast<std::int32_t>(tick);
                plan.buy_qty = buy_qty_at_or_above;
                plan.sell_qty = sell_qty_at_or_below;
            }
            buy_qty_at_or_above -= bids.levels[tick].total_qty;
        }
    }
    plan.crossed = have_candidate;
    plan.matched_qty = best_volume;
    return plan;
}

void MatchingEngine::Impl::uncross(InstrumentBook& book, Timestamp ts, EventBuffer& out) {
    const UncrossPlan plan = plan_uncross(book);
    if (!plan.crossed) {
        return;
    }
    const Price uncross_price = book.price_of(plan.tick);
    PriceLadder& bids = book.ladder(Side::BUY);
    PriceLadder& asks = book.ladder(Side::SELL);
    while (book.best_bid_tick >= plan.tick && book.best_ask_tick <= plan.tick) {
        const std::int32_t bid_tick = book.best_bid_tick;
        const std::int32_t ask_tick = book.best_ask_tick;
        Level& bid_level = bids.levels[static_cast<std::uint32_t>(bid_tick)];
        Level& ask_level = asks.levels[static_cast<std::uint32_t>(ask_tick)];
        const OrderIndex buy_index = bid_level.head;
        const OrderIndex sell_index = ask_level.head;
        OrderHot& buy = hot[buy_index];
        OrderHot& sell = hot[sell_index];
        const Quantity fill_qty = std::min(buy.leaves_qty, sell.leaves_qty);
        buy.leaves_qty -= fill_qty;
        buy.cum_qty += fill_qty;
        sell.leaves_qty -= fill_qty;
        sell.cum_qty += fill_qty;
        bid_level.total_qty -= fill_qty;
        ask_level.total_qty -= fill_qty;

        Event event = blank_event(EventType::Fill, book.id, ts);
        FillEvent& fill = event.fill;
        fill.match_id = next_match_id++;
        fill.price = uncross_price;
        fill.qty = fill_qty;
        fill.aggressor = Aggressor::None;
        set_fill_leg(fill.buy, buy.order_id, cl_ord_id_of(buy.session_token), session_of(buy.session_token),
                     buy.owner, buy.leaves_qty, buy.cum_qty);
        set_fill_leg(fill.sell, sell.order_id, cl_ord_id_of(sell.session_token), session_of(sell.session_token),
                     sell.owner, sell.leaves_qty, sell.cum_qty);
        append_event(out, event);

        if (buy.leaves_qty == 0) {
            pop_level_head(book, Side::BUY, bid_level, bid_tick, buy_index);
        }
        if (sell.leaves_qty == 0) {
            pop_level_head(book, Side::SELL, ask_level, ask_tick, sell_index);
        }
    }
    Event event = blank_event(EventType::AuctionSummary, book.id, ts);
    event.auction_summary.price = uncross_price;
    event.auction_summary.matched_qty = plan.matched_qty;
    event.auction_summary.match_id = next_match_id++;
    append_event(out, event);
    book.last_trade_price = uncross_price;
    book.has_last_trade_price = true;
}

void MatchingEngine::Impl::expire_day_orders(InstrumentBook& book, Timestamp ts, EventBuffer& out) {
    OrderIndex index = orders_by_instrument[book.id].head;
    while (index != kNilOrder) {
        const OrderIndex next = instrument_links[index].next;
        if (cold[index].tif == TimeInForce::Day) {
            emit_resting_canceled(book, index, CancelReason::EndOfDay, 0, ts, out);
            remove_resting(index);
        }
        index = next;
    }
}

void MatchingEngine::Impl::apply_phase(InstrumentBook& book, Phase new_phase, Timestamp ts, EventBuffer& out) {
    const Phase old_phase = book.phase;
    if (new_phase == old_phase) {
        return;
    }
    if ((new_phase == Phase::Continuous || new_phase == Phase::Closed) && book.is_crossed()) {
        uncross(book, ts, out);
    }
    if (new_phase == Phase::Closed) {
        expire_day_orders(book, ts, out);
    }
    book.phase = new_phase;
    Event event = blank_event(EventType::PhaseChanged, book.id, ts);
    event.phase_changed.old_phase = old_phase;
    event.phase_changed.new_phase = new_phase;
    append_event(out, event);
}

void MatchingEngine::Impl::on_set_phase(const SetPhase& cmd, EventBuffer& out) {
    InstrumentBook* const book = find_book(cmd.instrument);
    RejectReason reason = RejectReason::None;
    if (cmd.instrument != 0 && book == nullptr) {
        reason = RejectReason::UnknownInstrument;
    } else if (raw_value(cmd.phase) > raw_value(Phase::PreClose)) {
        reason = RejectReason::InvalidField;
    }
    if (LOB_UNLIKELY(reason != RejectReason::None)) {
        Event event = blank_event(EventType::Rejected, cmd.instrument, cmd.ts);
        event.rejected.cmd = CommandType::SetPhase;
        event.rejected.reason = reason;
        append_event(out, event);
        return;
    }
    if (book != nullptr) {
        apply_phase(*book, cmd.phase, cmd.ts, out);
        return;
    }
    for (InstrumentBook& each : books) {
        apply_phase(each, cmd.phase, cmd.ts, out);
    }
}

// ===========================================================================
// Queries
// ===========================================================================

OrderInfo MatchingEngine::Impl::order_info(OrderIndex index) const noexcept {
    const OrderHot& order_hot = hot[index];
    const OrderCold& order_cold = cold[index];
    const InstrumentBook& book = books[order_cold.instrument - 1];
    OrderInfo info;
    std::memset(static_cast<void*>(&info), 0, sizeof(info));
    info.order_id = order_hot.order_id;
    info.cl_ord_id = cl_ord_id_of(order_hot.session_token);
    info.instrument = order_cold.instrument;
    info.session = session_of(order_hot.session_token);
    info.owner = order_hot.owner;
    info.side = order_cold.side;
    info.tif = order_cold.tif;
    info.stp = order_cold.stp;
    info.post_only = order_cold.post_only;
    info.price = book.price_of(static_cast<std::int32_t>(order_cold.tick));
    info.qty = order_hot.leaves_qty + order_hot.cum_qty;
    info.leaves_qty = order_hot.leaves_qty;
    info.cum_qty = order_hot.cum_qty;
    return info;
}

// ===========================================================================
// MatchingEngine forwarding
// ===========================================================================

MatchingEngine::MatchingEngine(const EngineConfig& config) : impl_(std::make_unique<Impl>(config)) {}

MatchingEngine::~MatchingEngine() = default;

InstrumentId MatchingEngine::add_instrument(const InstrumentConfig& config) {
    Impl& impl = *impl_;
    if (impl.books.size() >= impl.max_instruments) {
        return 0;
    }
    if (!valid_instrument_config(config)) return 0;
    const std::uint64_t tick_count =
        static_cast<std::uint64_t>((config.max_price - config.min_price) / config.tick_size) + 1;
    if (tick_count > engine::kMaxLadderTicks) {
        return 0;
    }
    if (find_instrument(config.symbol) != 0) {
        return 0;
    }
    const InstrumentId id = static_cast<InstrumentId>(impl.books.size() + 1);
    impl.books.emplace_back(config, id, static_cast<std::uint32_t>(tick_count));
    return id;
}

// Proven bound from CURRENT live count: a sweep emits at most one event per
// removed resting order plus accepted/replaced and two terminal events. Phase
// batches emit at most N fills, N expirations and 2 events per instrument.
// This avoids reserving max_orders worth of output for an empty book.
HandleResult MatchingEngine::handle(const NewOrder& cmd, EventBuffer& out) {
    if (!out.can_append(std::size_t{impl_->live_count} + 3)) return HandleResult::InsufficientOutput;
    impl_->on_new_order(cmd, out); return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const CancelOrder& cmd, EventBuffer& out) {
    if (!out.can_append(1)) return HandleResult::InsufficientOutput;
    impl_->on_cancel(cmd, out); return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const ReplaceOrder& cmd, EventBuffer& out) {
    if (!out.can_append(std::size_t{impl_->live_count} + 3)) return HandleResult::InsufficientOutput;
    impl_->on_replace(cmd, out); return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const MassCancel& cmd, EventBuffer& out) {
    if (!out.can_append(std::size_t{impl_->live_count} + 1)) return HandleResult::InsufficientOutput;
    impl_->on_mass_cancel(cmd, out); return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const SetPhase& cmd, EventBuffer& out) {
    if (!out.can_append(2 * std::size_t{impl_->live_count} + 2 * impl_->books.size() + 1))
        return HandleResult::InsufficientOutput;
    impl_->on_set_phase(cmd, out); return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const Command& cmd, EventBuffer& out) {
    switch (cmd.type) {
        case CommandType::NewOrder: return handle(cmd.new_order, out);
        case CommandType::Cancel: return handle(cmd.cancel, out);
        case CommandType::Replace: return handle(cmd.replace, out);
        case CommandType::MassCancel: return handle(cmd.mass_cancel, out);
        case CommandType::SetPhase: return handle(cmd.set_phase, out);
    }
    return HandleResult::Applied;
}

std::size_t MatchingEngine::instrument_count() const noexcept { return impl_->books.size(); }

const InstrumentConfig* MatchingEngine::instrument(InstrumentId id) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    return book == nullptr ? nullptr : &book->config;
}

InstrumentId MatchingEngine::find_instrument(const Symbol& symbol) const noexcept {
    for (const InstrumentBook& book : impl_->books) {
        if (book.config.symbol == symbol) {
            return book.id;
        }
    }
    return 0;
}

Phase MatchingEngine::phase(InstrumentId id) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    return book == nullptr ? Phase::Closed : book->phase;
}

std::optional<Price> MatchingEngine::best_bid(InstrumentId id) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    if (book == nullptr || book->best_bid_tick == kNoBidTick) {
        return std::nullopt;
    }
    return book->price_of(book->best_bid_tick);
}

std::optional<Price> MatchingEngine::best_ask(InstrumentId id) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    if (book == nullptr || book->best_ask_tick == kNoAskTick) {
        return std::nullopt;
    }
    return book->price_of(book->best_ask_tick);
}

std::optional<Price> MatchingEngine::last_trade_price(InstrumentId id) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    if (book == nullptr || !book->has_last_trade_price) {
        return std::nullopt;
    }
    return book->last_trade_price;
}

std::size_t MatchingEngine::depth(InstrumentId id, Side side, LevelInfo* out, std::size_t max_levels) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    if (book == nullptr || out == nullptr || raw_value(side) > raw_value(Side::SELL)) {
        return 0;
    }
    const PriceLadder& ladder = book->ladder(side);
    std::int32_t tick = side == Side::BUY ? book->best_bid_tick : book->best_ask_tick;
    if (tick == kNoBidTick || tick == kNoAskTick) {
        return 0;
    }
    std::size_t written = 0;
    while (written < max_levels) {
        const Level& level = ladder.levels[static_cast<std::uint32_t>(tick)];
        out[written].price = book->price_of(tick);
        out[written].qty = level.total_qty;
        out[written].order_count = level.order_count;
        ++written;
        std::uint32_t next_tick;
        if (side == Side::BUY) {
            next_tick = tick == 0 ? OccupancyBitmap::kNotFound
                                  : ladder.occupied.find_at_or_below(static_cast<std::uint32_t>(tick) - 1);
        } else {
            next_tick = ladder.occupied.find_at_or_above(static_cast<std::uint32_t>(tick) + 1);
        }
        if (next_tick == OccupancyBitmap::kNotFound) {
            break;
        }
        tick = static_cast<std::int32_t>(next_tick);
    }
    return written;
}

std::size_t MatchingEngine::level_orders(InstrumentId id, Side side, Price price, OrderId* out,
                                         std::size_t max_orders) const noexcept {
    const InstrumentBook* book = impl_->find_book(id);
    std::int32_t tick = 0;
    if (book == nullptr || out == nullptr || raw_value(side) > raw_value(Side::SELL) || !book->tick_of(price, tick)) {
        return 0;
    }
    std::size_t written = 0;
    const Level& level = book->ladder(side).levels[static_cast<std::uint32_t>(tick)];
    for (OrderIndex index = level.head; index != kNilOrder && written < max_orders; index = impl_->hot[index].next) {
        out[written++] = impl_->hot[index].order_id;
    }
    return written;
}

std::optional<OrderInfo> MatchingEngine::find_order(SessionId session, ClOrdId cl_ord_id) const noexcept {
    const OrderIndex index = impl_->find_live_order(session, cl_ord_id);
    if (index == kNilOrder) {
        return std::nullopt;
    }
    return impl_->order_info(index);
}

std::optional<OrderInfo> MatchingEngine::find_order(OrderId order_id) const noexcept {
    const Impl& impl = *impl_;
    if (order_id == 0 || order_id >= impl.next_order_id) {
        return std::nullopt;
    }
    // Not latency critical: walk the ascending-OrderId list (no OrderId map).
    for (OrderIndex index = impl.all_orders.head; index != kNilOrder; index = impl.cold[index].next_by_order_id) {
        const OrderId current = impl.hot[index].order_id;
        if (current == order_id) {
            return impl.order_info(index);
        }
        if (current > order_id) {
            break;
        }
    }
    return std::nullopt;
}

std::size_t MatchingEngine::live_order_count() const noexcept { return impl_->live_count; }

AuctionIndicative MatchingEngine::auction_indicative(InstrumentId id) const noexcept {
    AuctionIndicative indicative;
    const InstrumentBook* book = impl_->find_book(id);
    if (book == nullptr) {
        return indicative;
    }
    const UncrossPlan plan = impl_->plan_uncross(*book);
    if (!plan.crossed) {
        std::uint64_t buy = 0, sell = 0;
        for (OrderIndex i = impl_->orders_by_instrument[id].head; i != kNilOrder; i = impl_->instrument_links[i].next) {
            (impl_->cold[i].side == Side::BUY ? buy : sell) += impl_->hot[i].leaves_qty;
        }
        indicative.imbalance_qty = buy >= sell ? buy - sell : sell - buy;
        indicative.imbalance_side = buy > sell ? Aggressor::Buy : sell > buy ? Aggressor::Sell : Aggressor::None;
        return indicative;
    }
    indicative.crossed = true;
    indicative.price = book->price_of(plan.tick);
    indicative.paired_qty = plan.matched_qty;
    if (plan.buy_qty > plan.sell_qty) {
        indicative.imbalance_qty = plan.buy_qty - plan.sell_qty;
        indicative.imbalance_side = Aggressor::Buy;
    } else if (plan.sell_qty > plan.buy_qty) {
        indicative.imbalance_qty = plan.sell_qty - plan.buy_qty;
        indicative.imbalance_side = Aggressor::Sell;
    }
    return indicative;
}

void MatchingEngine::visit_orders(void (*fn)(const OrderInfo&, void*), void* ctx) const {
    const Impl& impl = *impl_;
    for (OrderIndex index = impl.all_orders.head; index != kNilOrder; index = impl.cold[index].next_by_order_id) {
        fn(impl.order_info(index), ctx);
    }
}

void MatchingEngine::visit_level_orders(InstrumentId id, Side side, Price price,
                                        void (*fn)(const OrderInfo&, void*), void* ctx) const {
    const Impl& impl = *impl_;
    const InstrumentBook* book = impl.find_book(id);
    std::int32_t tick = 0;
    if (book == nullptr || raw_value(side) > raw_value(Side::SELL) || !book->tick_of(price, tick)) {
        return;
    }
    const Level& level = book->ladder(side).levels[static_cast<std::uint32_t>(tick)];
    for (OrderIndex index = level.head; index != kNilOrder; index = impl.hot[index].next) {
        fn(impl.order_info(index), ctx);
    }
}

}  // namespace exsim
