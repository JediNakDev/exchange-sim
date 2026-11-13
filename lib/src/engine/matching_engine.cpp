// Exchange validation, STP, auctions and event generation over
// exsim::exchange::OrderBook. Each instrument has its own book (tick ladders,
// occupancy bitmaps, FIFO links); all books share one bounded OrderBookStorage
// (order pool, level pool, OrderId map) sized by EngineConfig::max_orders. The
// engine walks and mutates book orders through Order pointers and keeps only
// engine-only metadata (session, STP, cumulative fills, query lists) in
// preallocated arrays.

#include <engine/matching_engine.hpp>

#include "fixed_array.hpp"
#include "order_metadata.hpp"
#include "price_ladder.hpp"

#include <exchange/compiler.hpp>
#include <exchange/flat_hash_map.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
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
using engine::make_session_token;
using engine::OrderCold;
using engine::OrderHot;
using engine::OrderIndex;
using engine::OrderListEnds;
using engine::session_of;
using exsim::exchange::Order;
using Level = exsim::exchange::PriceLevel;

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

// The 8 symbol bytes as one integer, the key of the symbol -> instrument map.
[[nodiscard]] std::uint64_t symbol_key(const Symbol& symbol) noexcept {
    std::uint64_t key = 0;
    static_assert(sizeof(key) == sizeof(symbol.chars));
    std::memcpy(&key, symbol.chars.data(), sizeof(key));
    return key;
}

inline void append_event(EventBuffer& out, const Event& event) noexcept {
    if (!out.push(event)) std::abort(); // preflight failure is an implementation defect
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

// Result of the uncross price search.
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
    // Order pool, level pool and OrderId map shared by every instrument book.
    std::shared_ptr<exsim::exchange::OrderBookStorage> shared_order_storage;
    std::vector<InstrumentBook> books;  // books[id - 1]; reserved up front so pointers stay stable
    std::unordered_map<std::uint64_t, InstrumentId> instrument_by_symbol;  // key: symbol_key(symbol)
    FixedArray<OrderHot> hot;
    FixedArray<OrderCold> cold;
    OrderIndex free_head = kNilOrder;
    std::uint32_t live_count = 0;
    exsim::exchange::FlatHashMap<std::uint32_t> order_by_session_token;
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

    void remove_resting(InstrumentBook& book, OrderIndex index);
    void refresh_best_tick(InstrumentBook& book, Side side) noexcept;
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
    : max_instruments(((void)required_event_capacity(config), config.max_instruments)),
      max_orders(static_cast<std::uint32_t>(config.max_orders)),
      // Every live level holds at least one live order, so max_orders bounds both pools.
      shared_order_storage(std::make_shared<exsim::exchange::OrderBookStorage>(max_orders, max_orders, false)),
      hot(std::size_t{max_orders} + 1),
      cold(std::size_t{max_orders} + 1),
      order_by_session_token(max_orders),
      orders_by_session(kSessionSlots),
      instrument_links(std::size_t{max_orders} + 1),
      orders_by_instrument(max_instruments + 1) {
    books.reserve(max_instruments);
    instrument_by_symbol.reserve(max_instruments);
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
    const Order& resting = *order_hot.order;
    Event event = blank_event(EventType::Canceled, book.id, ts);
    CanceledEvent& canceled = event.canceled;
    canceled.order_id = resting.id;
    canceled.cl_ord_id = cl_ord_id_of(order_hot.session_token);
    canceled.request_cl_ord_id = request_cl_ord_id;
    canceled.session = session_of(order_hot.session_token);
    canceled.side = resting.side;
    canceled.price = book.price_of(static_cast<std::int32_t>(resting.price));
    canceled.canceled_qty = static_cast<Quantity>(resting.remaining_quantity);
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

// Copies the book's best price of `side` into the cached tick after an order
// of that side rested or left the book.
void MatchingEngine::Impl::refresh_best_tick(InstrumentBook& book, Side side) noexcept {
    if (side == Side::BUY) {
        const auto bid = book.order_book->get_best_bid();
        book.best_bid_tick = bid ? static_cast<std::int32_t>(*bid) : kNoBidTick;
    } else {
        const auto ask = book.order_book->get_best_ask();
        book.best_ask_tick = ask ? static_cast<std::int32_t>(*ask) : kNoAskTick;
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

// Unlinks a resting order from its FIFO (head or anywhere) and retires it.
void MatchingEngine::Impl::remove_resting(InstrumentBook& book, OrderIndex index) {
    Order& resting = *hot[index].order;
    const Side side = resting.side;
    book.order_book->remove_order(resting);
    refresh_best_tick(book, side);
    retire(index);
}

void MatchingEngine::Impl::rest(InstrumentBook& book, const IncomingOrder& order, Timestamp ts, EventBuffer& out) {
    assert(free_head != kNilOrder);
    const OrderIndex index = free_head;
    OrderHot& order_hot = hot[index];
    free_head = order_hot.next;

    order_hot.next = kNilOrder;
    order_hot.cum_qty = order.cum_qty;
    order_hot.owner = order.owner;
    order_hot.session_token = make_session_token(order.session, order.cl_ord_id);

    OrderCold& order_cold = cold[index];
    order_cold.instrument = order.instrument;
    order_cold.tif = order.tif;
    order_cold.stp = order.stp;
    order_cold.post_only = order.post_only;

    // The order book owns the resting order (ID, tick, leaves), its level and FIFO.
    order_hot.order = book.order_book->rest_order(order.order_id, order.limit_tick, order.qty, order.leaves_qty,
                                                  order.side, index);
    if (order_hot.order == nullptr) std::abort();  // shared storage holds max_orders; admission checked live_count
    refresh_best_tick(book, order.side);

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
// Matching
// ===========================================================================

bool MatchingEngine::Impl::fill_or_kill_can_fill(const InstrumentBook& book, const IncomingOrder& order) const noexcept {
    const Side resting_side = opposite(order.side);
    const bool stp_active = order.stp != StpMode::None && order.owner != 0;
    const std::uint64_t needed = order.leaves_qty;
    std::uint64_t available = 0;
    std::int32_t tick = order.side == Side::BUY ? book.best_ask_tick : book.best_bid_tick;
    while (book.crosses_opposite_best(order.side, order.limit_tick) &&
           (order.side == Side::BUY ? order.limit_tick >= tick : order.limit_tick <= tick)) {
        const Level& level = *book.order_book->price_level(resting_side, tick);
        if (!stp_active) {
            available += level.total_volume;
            if (available >= needed) {
                return true;
            }
        } else {
            for (const Order* resting = level.head_order; resting != nullptr; resting = resting->next_order) {
                if (hot[resting->context_index].owner == order.owner) {
                    if (order.stp == StpMode::CancelResting) {
                        continue;
                    }
                    return false;  // walk stops at the self-match with available < needed
                }
                available += resting->remaining_quantity;
                if (available >= needed) {
                    return true;
                }
            }
        }
        const std::optional<exsim::exchange::Price> next_tick = order.side == Side::BUY
            ? book.order_book->next_price(resting_side, tick + 1, true)
            : book.order_book->next_price(resting_side, tick - 1, false);
        if (!next_tick) {
            return false;
        }
        tick = static_cast<std::int32_t>(*next_tick);
    }
    return false;
}

// Matches `order` against the opposite side. Returns true if the
// order was cancelled by self-trade prevention.
template <Side kAggressorSide>
bool MatchingEngine::Impl::match(InstrumentBook& book, IncomingOrder& order, Timestamp ts, EventBuffer& out) {
    constexpr Side kRestingSide = opposite(kAggressorSide);
    const bool stp_active = order.stp != StpMode::None && order.owner != 0;

    while (order.leaves_qty != 0) {
        const std::int32_t best_tick = kAggressorSide == Side::BUY ? book.best_ask_tick : book.best_bid_tick;
        if (kAggressorSide == Side::BUY ? order.limit_tick < best_tick : order.limit_tick > best_tick) {
            return false;  // opposite side empty or no longer crossing
        }
        const Price level_price = book.price_of(best_tick);
        // Walk the best level's FIFO by pointer. Removing the last order frees
        // the level, so the walk ends there and the outer loop rereads the best.
        Order* resting = book.order_book->price_level(kRestingSide, best_tick)->head_order;
        do {
            Order* const next_resting = resting->next_order;
            const OrderIndex index = resting->context_index;
            OrderHot& resting_hot = hot[index];

            if (EXSIM_EXCHANGE_UNLIKELY(stp_active && resting_hot.owner == order.owner)) {
                const StpMode mode = order.stp;
                if (mode != StpMode::CancelAggressor) {
                    emit_resting_canceled(book, index, CancelReason::SelfTradePrevention, 0, ts, out);
                    remove_resting(book, index);
                    resting = next_resting;
                }
                if (mode != StpMode::CancelResting) {
                    emit_incoming_canceled(order, CancelReason::SelfTradePrevention, ts, out);
                    return true;
                }
                continue;
            }

            const Quantity fill_qty = std::min(order.leaves_qty, static_cast<Quantity>(resting->remaining_quantity));
            book.order_book->execute_order(*resting, fill_qty);
            resting_hot.cum_qty += fill_qty;
            order.leaves_qty -= fill_qty;
            order.cum_qty += fill_qty;
            const Quantity resting_leaves = static_cast<Quantity>(resting->remaining_quantity);

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
            set_fill_leg(resting_leg, resting->id, cl_ord_id_of(resting_hot.session_token),
                         session_of(resting_hot.session_token), resting_hot.owner, resting_leaves, resting_hot.cum_qty);
            append_event(out, event);

            book.last_trade_price = level_price;
            book.has_last_trade_price = true;

            if (resting_leaves == 0) {
                remove_resting(book, index);
                resting = next_resting;
            }
        } while (order.leaves_qty != 0 && resting != nullptr);
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
// NewOrder
// ===========================================================================

void MatchingEngine::Impl::on_new_order(const NewOrder& cmd, EventBuffer& out) {
    InstrumentBook* const book_ptr = find_book(cmd.instrument);
    if (EXSIM_EXCHANGE_UNLIKELY(book_ptr == nullptr)) {
        return reject_new_order(cmd, RejectReason::UnknownInstrument, out);
    }
    InstrumentBook& book = *book_ptr;
    if (EXSIM_EXCHANGE_UNLIKELY(cmd.session == 0 || cmd.cl_ord_id == 0 || cmd.cl_ord_id > kMaxClOrdId)) {
        return reject_new_order(cmd, RejectReason::InvalidClOrdId, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(order_by_session_token.find(make_session_token(cmd.session, cmd.cl_ord_id)) != nullptr)) {
        return reject_new_order(cmd, RejectReason::DuplicateClOrdId, out);
    }
    const bool is_market = cmd.type == OrdType::Market;
    const bool is_immediate = cmd.tif == TimeInForce::IOC || cmd.tif == TimeInForce::FOK;
    if (EXSIM_EXCHANGE_UNLIKELY(raw_value(cmd.side) > raw_value(Side::SELL) || raw_value(cmd.type) > raw_value(OrdType::Market) ||
                     raw_value(cmd.tif) > raw_value(TimeInForce::FOK) ||
                     raw_value(cmd.stp) > raw_value(StpMode::CancelBoth) ||
                     (cmd.post_only && (is_market || is_immediate)))) {
        return reject_new_order(cmd, RejectReason::InvalidField, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(!book.is_valid_quantity(cmd.qty))) {
        return reject_new_order(cmd, RejectReason::InvalidQuantity, out);
    }
    std::int32_t limit_tick = 0;
    if (is_market) {
        limit_tick = cmd.side == Side::BUY ? static_cast<std::int32_t>(book.tick_count - 1) : 0;
    } else if (EXSIM_EXCHANGE_UNLIKELY(!book.tick_of(cmd.price, limit_tick))) {
        return reject_new_order(cmd, RejectReason::InvalidPrice, out);
    }
    switch (book.phase) {
        case Phase::Continuous:
            if (EXSIM_EXCHANGE_UNLIKELY(cmd.post_only && book.crosses_opposite_best(cmd.side, limit_tick))) {
                return reject_new_order(cmd, RejectReason::PostOnlyWouldCross, out);
            }
            break;
        case Phase::PreOpen:
        case Phase::PreClose:
            if (EXSIM_EXCHANGE_UNLIKELY(is_market || is_immediate)) {
                return reject_new_order(cmd, RejectReason::NotAllowedInPhase, out);
            }
            break;
        case Phase::Closed:
            return reject_new_order(cmd, RejectReason::MarketClosed, out);
        case Phase::Halted:
            return reject_new_order(cmd, RejectReason::InstrumentHalted, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(live_count >= max_orders)) {
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
// CancelOrder
// ===========================================================================

void MatchingEngine::Impl::on_cancel(const CancelOrder& cmd, EventBuffer& out) {
    const OrderIndex index = find_live_order(cmd.session, cmd.orig_cl_ord_id);
    if (EXSIM_EXCHANGE_UNLIKELY(index == kNilOrder)) {
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
        OrderHot& order_hot = hot[index];
        Order& resting = *order_hot.order;
        const Quantity leaves_qty = static_cast<Quantity>(resting.remaining_quantity);
        const Quantity target = cmd.reduce_to_total_qty > order_hot.cum_qty
            ? cmd.reduce_to_total_qty - order_hot.cum_qty : 0;
        if (target >= leaves_qty) return;
        if (target != 0) {
            Event event = blank_event(EventType::Canceled, book.id, cmd.ts);
            auto& c = event.canceled;
            c.order_id = resting.id;
            c.cl_ord_id = cl_ord_id_of(order_hot.session_token);
            c.request_cl_ord_id = cmd.cl_ord_id;
            c.session = cmd.session;
            c.side = resting.side;
            c.price = book.price_of(static_cast<std::int32_t>(resting.price));
            c.canceled_qty = leaves_qty - target;
            c.cum_qty = order_hot.cum_qty;
            c.reason = CancelReason::UserRequested;
            c.was_resting = true;
            c.leaves_qty = target;
            if (!book.order_book->amend_order(resting, order_hot.cum_qty + target, target)) std::abort();
            append_event(out, event);
            return;
        }
    }
    emit_resting_canceled(book, index, CancelReason::UserRequested, cmd.cl_ord_id, cmd.ts, out);
    remove_resting(book, index);
}

// ===========================================================================
// ReplaceOrder
// ===========================================================================

void MatchingEngine::Impl::on_replace(const ReplaceOrder& cmd, EventBuffer& out) {
    const OrderIndex index = find_live_order(cmd.session, cmd.orig_cl_ord_id);
    if (EXSIM_EXCHANGE_UNLIKELY(index == kNilOrder)) {
        return reject_replace(cmd, RejectReason::UnknownOrder, 0, 0, out);
    }
    OrderHot& order_hot = hot[index];
    const OrderCold& order_cold = cold[index];
    Order& resting = *order_hot.order;
    InstrumentBook& book = books[order_cold.instrument - 1];
    const OrderId old_order_id = resting.id;
    const InstrumentId instrument = order_cold.instrument;

    if (EXSIM_EXCHANGE_UNLIKELY(cmd.cl_ord_id > kMaxClOrdId)) {
        return reject_replace(cmd, RejectReason::InvalidClOrdId, old_order_id, instrument, out);
    }
    const ClOrdId new_cl_ord_id = cmd.cl_ord_id != 0 ? cmd.cl_ord_id : cmd.orig_cl_ord_id;
    const bool token_changes = new_cl_ord_id != cmd.orig_cl_ord_id;
    if (EXSIM_EXCHANGE_UNLIKELY(token_changes &&
                     order_by_session_token.find(make_session_token(cmd.session, new_cl_ord_id)) != nullptr)) {
        return reject_replace(cmd, RejectReason::DuplicateClOrdId, old_order_id, instrument, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(!book.is_valid_quantity(cmd.qty))) {
        return reject_replace(cmd, RejectReason::InvalidQuantity, old_order_id, instrument, out);
    }
    std::int32_t new_tick = 0;
    if (EXSIM_EXCHANGE_UNLIKELY(!book.tick_of(cmd.price, new_tick))) {
        return reject_replace(cmd, RejectReason::InvalidPrice, old_order_id, instrument, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(book.phase == Phase::Closed)) {
        return reject_replace(cmd, RejectReason::MarketClosed, old_order_id, instrument, out);
    }
    if (EXSIM_EXCHANGE_UNLIKELY(book.phase == Phase::Halted)) {
        return reject_replace(cmd, RejectReason::InstrumentHalted, old_order_id, instrument, out);
    }
    const Side side = resting.side;
    if (EXSIM_EXCHANGE_UNLIKELY(order_cold.post_only && book.phase == Phase::Continuous &&
                     book.crosses_opposite_best(side, new_tick))) {
        return reject_replace(cmd, RejectReason::PostOnlyWouldCross, old_order_id, instrument, out);
    }

    // Quantity reduced to (or below) what already executed: the order is done.
    if (cmd.qty <= order_hot.cum_qty) {
        emit_resting_canceled(book, index, CancelReason::UserRequested, cmd.cl_ord_id, cmd.ts, out);
        remove_resting(book, index);
        return;
    }

    const std::int32_t old_tick = static_cast<std::int32_t>(resting.price);
    const Price old_price = book.price_of(old_tick);
    const Quantity old_leaves_qty = static_cast<Quantity>(resting.remaining_quantity);
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
        if (!book.order_book->amend_order(resting, cmd.qty, new_leaves_qty)) std::abort();
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
    remove_resting(book, index);

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
// MassCancel
// ===========================================================================

void MatchingEngine::Impl::on_mass_cancel(const MassCancel& cmd, EventBuffer& out) {
    const CancelReason reason = cmd.cancel_on_disconnect ? CancelReason::CancelOnDisconnect : CancelReason::MassCancel;
    std::uint32_t canceled_count = 0;
    OrderIndex index = orders_by_session[cmd.session].head;
    while (index != kNilOrder) {
        const OrderIndex next = cold[index].next_in_session;
        const InstrumentId instrument = cold[index].instrument;
        if (cmd.instrument == 0 || instrument == cmd.instrument) {
            InstrumentBook& book = books[instrument - 1];
            emit_resting_canceled(book, index, reason, cmd.cl_ord_id, cmd.ts, out);
            remove_resting(book, index);
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
// SetPhase, auction uncross, end-of-day expiry
// ===========================================================================

UncrossPlan MatchingEngine::Impl::plan_uncross(const InstrumentBook& book) const noexcept {
    UncrossPlan plan;
    if (book.best_bid_tick == kNoBidTick || book.best_ask_tick == kNoAskTick || !book.is_crossed()) {
        return plan;
    }
    const std::uint32_t low_tick = static_cast<std::uint32_t>(book.best_ask_tick);
    const std::uint32_t high_tick = static_cast<std::uint32_t>(book.best_bid_tick);
    const exsim::exchange::OrderBook& order_book = *book.order_book;
    auto volume_at = [&](Side side, std::size_t tick) -> std::uint64_t {
        const Level* level = order_book.price_level(side, static_cast<exsim::exchange::Price>(tick));
        return level == nullptr ? 0 : level->total_volume;  // empty levels hold 0
    };
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
        std::uint64_t bits = order_book.active_word(Side::BUY, word_index) & range_mask(word_index);
        while (bits != 0) {
            const std::size_t tick = (word_index << 6) | static_cast<std::size_t>(__builtin_ctzll(bits));
            buy_qty_at_or_above += volume_at(Side::BUY, tick);
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
            (order_book.active_word(Side::BUY, word_index) | order_book.active_word(Side::SELL, word_index)) &
            range_mask(word_index);
        while (bits != 0) {
            const std::size_t tick = (word_index << 6) | static_cast<std::size_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            sell_qty_at_or_below += volume_at(Side::SELL, tick);
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
            buy_qty_at_or_above -= volume_at(Side::BUY, tick);
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
    while (book.best_bid_tick >= plan.tick && book.best_ask_tick <= plan.tick) {
        Order& buy = *book.order_book->price_level(Side::BUY, book.best_bid_tick)->head_order;
        Order& sell = *book.order_book->price_level(Side::SELL, book.best_ask_tick)->head_order;
        const OrderIndex buy_index = buy.context_index;
        const OrderIndex sell_index = sell.context_index;
        OrderHot& buy_hot = hot[buy_index];
        OrderHot& sell_hot = hot[sell_index];
        const Quantity fill_qty = static_cast<Quantity>(std::min(buy.remaining_quantity, sell.remaining_quantity));
        book.order_book->execute_order(buy, fill_qty);
        book.order_book->execute_order(sell, fill_qty);
        buy_hot.cum_qty += fill_qty;
        sell_hot.cum_qty += fill_qty;
        const Quantity buy_leaves = static_cast<Quantity>(buy.remaining_quantity);
        const Quantity sell_leaves = static_cast<Quantity>(sell.remaining_quantity);

        Event event = blank_event(EventType::Fill, book.id, ts);
        FillEvent& fill = event.fill;
        fill.match_id = next_match_id++;
        fill.price = uncross_price;
        fill.qty = fill_qty;
        fill.aggressor = Aggressor::None;
        set_fill_leg(fill.buy, buy.id, cl_ord_id_of(buy_hot.session_token), session_of(buy_hot.session_token),
                     buy_hot.owner, buy_leaves, buy_hot.cum_qty);
        set_fill_leg(fill.sell, sell.id, cl_ord_id_of(sell_hot.session_token), session_of(sell_hot.session_token),
                     sell_hot.owner, sell_leaves, sell_hot.cum_qty);
        append_event(out, event);

        if (buy_leaves == 0) {
            remove_resting(book, buy_index);
        }
        if (sell_leaves == 0) {
            remove_resting(book, sell_index);
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
            remove_resting(book, index);
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
    if (EXSIM_EXCHANGE_UNLIKELY(reason != RejectReason::None)) {
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
    const Order& resting = *order_hot.order;
    const InstrumentBook& book = books[order_cold.instrument - 1];
    OrderInfo info;
    std::memset(static_cast<void*>(&info), 0, sizeof(info));
    info.order_id = resting.id;
    info.cl_ord_id = cl_ord_id_of(order_hot.session_token);
    info.instrument = order_cold.instrument;
    info.session = session_of(order_hot.session_token);
    info.owner = order_hot.owner;
    info.side = resting.side;
    info.tif = order_cold.tif;
    info.stp = order_cold.stp;
    info.post_only = order_cold.post_only;
    info.price = book.price_of(static_cast<std::int32_t>(resting.price));
    info.leaves_qty = static_cast<Quantity>(resting.remaining_quantity);
    info.qty = info.leaves_qty + order_hot.cum_qty;
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
    const InstrumentId id = static_cast<InstrumentId>(impl.books.size() + 1);
    if (!impl.instrument_by_symbol.try_emplace(symbol_key(config.symbol), id).second) {
        return 0;  // symbol taken
    }
    try {
        impl.books.emplace_back(config, id, static_cast<std::uint32_t>(tick_count), impl.shared_order_storage);
    } catch (...) {
        impl.instrument_by_symbol.erase(symbol_key(config.symbol));
        throw;
    }
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
    const auto found = impl_->instrument_by_symbol.find(symbol_key(symbol));
    return found == impl_->instrument_by_symbol.end() ? InstrumentId{0} : found->second;
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
    std::int32_t tick = side == Side::BUY ? book->best_bid_tick : book->best_ask_tick;
    if (tick == kNoBidTick || tick == kNoAskTick) {
        return 0;
    }
    std::size_t written = 0;
    while (written < max_levels) {
        const Level& level = *book->order_book->price_level(side, tick);
        out[written].price = book->price_of(tick);
        out[written].qty = level.total_volume;
        out[written].order_count = static_cast<std::uint32_t>(level.order_count());
        ++written;
        const std::optional<exsim::exchange::Price> next_tick = side == Side::BUY
            ? book->order_book->next_price(side, tick - 1, false)
            : book->order_book->next_price(side, tick + 1, true);
        if (!next_tick) {
            break;
        }
        tick = static_cast<std::int32_t>(*next_tick);
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
    const Level* level = book->order_book->price_level(side, tick);
    std::size_t written = 0;
    for (const Order* resting = level == nullptr ? nullptr : level->head_order;
         resting != nullptr && written < max_orders; resting = resting->next_order) {
        out[written++] = resting->id;
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
    // One OrderId map serves every instrument book.
    const Order* const* resting = impl.shared_order_storage->orders.find(order_id);
    if (resting == nullptr) {
        return std::nullopt;
    }
    return impl.order_info((*resting)->context_index);
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
        const std::uint64_t buy = book->order_book->get_total_leaves(Side::BUY);
        const std::uint64_t sell = book->order_book->get_total_leaves(Side::SELL);
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
    const Level* level = book->order_book->price_level(side, tick);
    for (const Order* resting = level == nullptr ? nullptr : level->head_order; resting != nullptr;
         resting = resting->next_order) {
        fn(impl.order_info(resting->context_index), ctx);
    }
}

}  // namespace exsim
