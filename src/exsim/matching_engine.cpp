// Matching engine, variant B: per side, one sorted vector of price levels
// with the best level at the back (see engine/price_ladder.hpp). Semantics
// follow docs/SPEC.md; section numbers below refer to it.

#include <exsim/matching_engine.hpp>
#include <lob/compiler.hpp>
#include <lob/flat_hash_map.hpp>

#include "engine/order_pool.hpp"
#include "engine/price_ladder.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>

namespace exsim {

namespace {

using engine::kNullSlot;
using engine::ladder_key;
using engine::Order;
using engine::OrderPool;
using engine::OrderSlot;
using engine::price_from_ladder_key;
using engine::PriceLadder;
using engine::PriceLevel;

constexpr Price kMarketCrossingKey = std::numeric_limits<Price>::min();

[[nodiscard]] constexpr std::uint64_t client_order_key(SessionId session, ClOrdId cl_ord_id) noexcept {
    return (std::uint64_t{session} << 48) | cl_ord_id;
}

// Only tokens a live order can hold produce a key; anything else (0, or a
// token wider than 48 bits that would alias another session) cannot match.
[[nodiscard]] constexpr bool is_storable_client_token(ClOrdId cl_ord_id) noexcept {
    return cl_ord_id != 0 && cl_ord_id <= kMaxClOrdId;
}

[[nodiscard]] constexpr Side opposite_side(Side side) noexcept {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

[[nodiscard]] constexpr bool is_call_phase(Phase phase) noexcept {
    return phase == Phase::PreOpen || phase == Phase::PreClose;
}

[[nodiscard]] constexpr Aggressor aggressor_of(Side side) noexcept {
    return side == Side::BUY ? Aggressor::Buy : Aggressor::Sell;
}

[[nodiscard]] constexpr std::uint64_t absolute_distance(Price a, Price b) noexcept {
    return a >= b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                  : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
}

// Only active fields define event equality.
[[nodiscard]] inline Event blank_event(EventType type, InstrumentId instrument, Timestamp ts) noexcept {
    Event event{};
    event.type = type;
    event.instrument = instrument;
    event.ts = ts;
    return event;
}

[[nodiscard]] inline FillLeg fill_leg_of(const Order& order) noexcept {
    FillLeg leg{};
    leg.order_id = order.order_id;
    leg.cl_ord_id = order.cl_ord_id;
    leg.session = order.session;
    leg.owner = order.owner;
    leg.leaves_qty = order.leaves_qty;
    leg.cum_qty = order.cum_qty;
    return leg;
}

struct Instrument {
    InstrumentConfig config;
    InstrumentId id = 0;
    Phase phase = Phase::Closed;
    bool has_last_trade_price = false;
    Price last_trade_price = 0;
    PriceLadder bids;
    PriceLadder asks;

    [[nodiscard]] PriceLadder& ladder(Side side) noexcept { return side == Side::BUY ? bids : asks; }
    [[nodiscard]] const PriceLadder& ladder(Side side) const noexcept { return side == Side::BUY ? bids : asks; }

    [[nodiscard]] bool is_valid_price(Price price) const noexcept {
        return price >= config.min_price && price <= config.max_price && price % config.tick_size == 0;
    }
    [[nodiscard]] bool is_valid_quantity(Quantity qty) const noexcept {
        return qty > 0 && qty % config.lot_size == 0 && qty <= config.max_order_qty;
    }
    [[nodiscard]] bool is_crossed() const noexcept {
        return !bids.empty() && !asks.empty() && bids.best_key() >= -asks.best_key();
    }
    // A limit order at `price` on `side` would trade against the opposite best.
    [[nodiscard]] bool would_cross_opposite_best(Side side, Price price) const noexcept {
        const PriceLadder& opposite = ladder(opposite_side(side));
        return !opposite.empty() && opposite.best_key() >= -ladder_key(side, price);
    }
};

// Result of the uncross price search (SPEC 9).
struct UncrossPrice {
    bool crossed = false;
    Price price = 0;
    std::uint64_t paired_qty = 0;               // V(p*)
    std::uint64_t imbalance_qty = 0;            // |B(p*) - S(p*)|
    Aggressor imbalance_side = Aggressor::None;  // side with the surplus
};

enum class AggressorOutcome : std::uint8_t {
    FullyFilled,
    CanceledBySelfTradePrevention,
    HasRemainder,
};

}  // namespace

struct MatchingEngine::Impl {
    explicit Impl(const EngineConfig& engine_config)
        : config(engine_config),
          max_instruments((static_cast<void>(required_event_capacity(engine_config)), engine_config.max_instruments)),
          max_live_orders(engine_config.max_orders),
          orders(max_live_orders) {
        instruments.reserve(max_instruments);
        slot_by_client_key.reserve(max_live_orders);
        slot_by_order_id.reserve(max_live_orders);
        std::memset(static_cast<void*>(&incoming_order), 0, sizeof(incoming_order));
    }

    // ---- configuration ------------------------------------------------------
    EngineConfig config;
    std::size_t max_instruments;
    std::size_t max_live_orders;
    std::vector<Instrument> instruments;  // index = InstrumentId - 1; never reallocates

    // ---- order state --------------------------------------------------------
    OrderPool orders;
    lob::FlatHashMap<OrderSlot> slot_by_client_key;  // client_order_key(session, cl_ord_id)
    lob::FlatHashMap<OrderSlot> slot_by_order_id;
    OrderSlot oldest_live_slot = kNullSlot;  // head of the ascending-OrderId list
    OrderSlot newest_live_slot = kNullSlot;
    std::size_t live_order_count = 0;
    OrderId next_order_id = 1;
    MatchId next_match_id = 1;
    Order incoming_order;  // a new order while it aggresses; copied into the pool only if it rests

    // ---- lookups --------------------------------------------------------------
    [[nodiscard]] Instrument* find_instrument(InstrumentId id) noexcept {
        return (id != 0 && id <= instruments.size()) ? &instruments[id - 1] : nullptr;
    }
    [[nodiscard]] const Instrument* find_instrument(InstrumentId id) const noexcept {
        return (id != 0 && id <= instruments.size()) ? &instruments[id - 1] : nullptr;
    }
    [[nodiscard]] OrderSlot find_live_slot(SessionId session, ClOrdId cl_ord_id) const noexcept {
        if (LOB_UNLIKELY(!is_storable_client_token(cl_ord_id))) {
            return kNullSlot;
        }
        const OrderSlot* slot = slot_by_client_key.find(client_order_key(session, cl_ord_id));
        return slot ? *slot : kNullSlot;
    }

    // ---- book maintenance ---------------------------------------------------
    void append_to_level(Instrument& instrument, OrderSlot slot) noexcept {
        Order& order = orders[slot];
        PriceLadder& ladder = instrument.ladder(order.side);
        const Price key = ladder_key(order.side, order.price);
        const std::uint32_t after = ladder.count_not_better_than(key);
        order.next_in_level = kNullSlot;
        if (after != 0 && ladder.key_at(after - 1) == key) {
            PriceLevel& level = ladder.level_at(after - 1);
            order.prev_in_level = level.tail;
            orders[level.tail].next_in_level = slot;
            level.tail = slot;
            level.total_qty += order.leaves_qty;
        } else {
            PriceLevel& level = ladder.insert_level(after, key);
            order.prev_in_level = kNullSlot;
            level.head = slot;
            level.tail = slot;
            level.total_qty = order.leaves_qty;
        }
    }

    void unlink_from_level(Instrument& instrument, OrderSlot slot) noexcept {
        Order& order = orders[slot];
        PriceLadder& ladder = instrument.ladder(order.side);
        const std::uint32_t index = ladder.find(ladder_key(order.side, order.price));
        PriceLevel& level = ladder.level_at(index);
        if (order.prev_in_level != kNullSlot) {
            orders[order.prev_in_level].next_in_level = order.next_in_level;
        } else {
            level.head = order.next_in_level;
        }
        if (order.next_in_level != kNullSlot) {
            orders[order.next_in_level].prev_in_level = order.prev_in_level;
        } else {
            level.tail = order.prev_in_level;
        }
        level.total_qty -= order.leaves_qty;
        if (level.head == kNullSlot) {
            ladder.erase_level(index);
        }
    }

    // Removes the head order of the best level of `ladder` (it has just been
    // fully executed, so its leaves are already subtracted from the level).
    void pop_best_head(PriceLadder& ladder) noexcept {
        PriceLevel& level = ladder.best_level();
        const OrderSlot slot = level.head;
        const OrderSlot next = orders[slot].next_in_level;
        level.head = next;
        if (next != kNullSlot) {
            orders[next].prev_in_level = kNullSlot;
        } else {
            ladder.pop_best();
        }
        unregister_live_order(slot);
    }

    // Makes the order findable and counts it as live (it is on the book).
    void register_live_order(OrderSlot slot) noexcept {
        Order& order = orders[slot];
        if (!slot_by_client_key.insert(client_order_key(order.session, order.cl_ord_id), slot)) std::abort();
        if (!slot_by_order_id.insert(order.order_id, slot)) std::abort();
        order.prev_by_order_id = newest_live_slot;
        order.next_by_order_id = kNullSlot;
        if (newest_live_slot != kNullSlot) {
            orders[newest_live_slot].next_by_order_id = slot;
        } else {
            oldest_live_slot = slot;
        }
        newest_live_slot = slot;
        ++live_order_count;
    }

    // Drops every index entry of an order that has left the book for good and
    // returns its slot to the pool.
    void unregister_live_order(OrderSlot slot) noexcept {
        forget_live_order(slot);
        orders.release(slot);
    }

    // Removes the order from the id maps and the age list but keeps its slot.
    void forget_live_order(OrderSlot slot) noexcept {
        Order& order = orders[slot];
        slot_by_client_key.erase(client_order_key(order.session, order.cl_ord_id));
        slot_by_order_id.erase(order.order_id);
        if (order.prev_by_order_id != kNullSlot) {
            orders[order.prev_by_order_id].next_by_order_id = order.next_by_order_id;
        } else {
            oldest_live_slot = order.next_by_order_id;
        }
        if (order.next_by_order_id != kNullSlot) {
            orders[order.next_by_order_id].prev_by_order_id = order.prev_by_order_id;
        } else {
            newest_live_slot = order.prev_by_order_id;
        }
        --live_order_count;
    }

    void remove_resting_order(Instrument& instrument, OrderSlot slot) noexcept {
        unlink_from_level(instrument, slot);
        unregister_live_order(slot);
    }

    // ---- events -------------------------------------------------------------
    static void emit_canceled(const Order& order, CancelReason reason, ClOrdId request_cl_ord_id,
                              Quantity canceled_qty, bool was_resting, Timestamp ts, EventBuffer& out, Quantity leaves = 0) {
        Event event = blank_event(EventType::Canceled, order.instrument, ts);
        CanceledEvent& canceled = event.canceled;
        canceled.order_id = order.order_id;
        canceled.cl_ord_id = order.cl_ord_id;
        canceled.request_cl_ord_id = request_cl_ord_id;
        canceled.session = order.session;
        canceled.side = order.side;
        canceled.price = order.price;
        canceled.canceled_qty = canceled_qty;
        canceled.leaves_qty = leaves;
        canceled.cum_qty = order.cum_qty;
        canceled.reason = reason;
        canceled.was_resting = was_resting;
        if (!out.push(event)) std::abort();
    }

    void emit_fill(InstrumentId instrument, Timestamp ts, Price price, Quantity qty, Aggressor aggressor,
                   const Order& buy, const Order& sell, EventBuffer& out) {
        Event event = blank_event(EventType::Fill, instrument, ts);
        FillEvent& fill = event.fill;
        fill.match_id = next_match_id++;
        fill.price = price;
        fill.qty = qty;
        fill.aggressor = aggressor;
        fill.buy = fill_leg_of(buy);
        fill.sell = fill_leg_of(sell);
        if (!out.push(event)) std::abort();
    }

    static void emit_rested(const Order& order, Timestamp ts, EventBuffer& out) {
        Event event = blank_event(EventType::Rested, order.instrument, ts);
        RestedEvent& rested = event.rested;
        rested.order_id = order.order_id;
        rested.cl_ord_id = order.cl_ord_id;
        rested.session = order.session;
        rested.side = order.side;
        rested.price = order.price;
        rested.leaves_qty = order.leaves_qty;
        if (!out.push(event)) std::abort();
    }

    static void emit_rejected(CommandType command, RejectReason reason, InstrumentId instrument, SessionId session,
                              ClOrdId cl_ord_id, ClOrdId orig_cl_ord_id, OrderId order_id, Timestamp ts,
                              EventBuffer& out) {
        Event event = blank_event(EventType::Rejected, instrument, ts);
        RejectedEvent& rejected = event.rejected;
        rejected.cmd = command;
        rejected.reason = reason;
        rejected.session = session;
        rejected.cl_ord_id = cl_ord_id;
        rejected.orig_cl_ord_id = orig_cl_ord_id;
        rejected.order_id = order_id;
        if (!out.push(event)) std::abort();
    }

    // ---- continuous matching (SPEC 4.3, 4.5) --------------------------------
    [[nodiscard]] static Price crossing_key_for(const Order& aggressor, bool is_market) noexcept {
        // A resting level crosses iff its ladder key is >= this value.
        return is_market ? kMarketCrossingKey : -ladder_key(aggressor.side, aggressor.price);
    }

    [[nodiscard]] bool fill_or_kill_can_fill(const Instrument& instrument, const Order& aggressor,
                                             bool is_market) const noexcept {
        const PriceLadder& opposite = instrument.ladder(opposite_side(aggressor.side));
        const Price crossing_key = crossing_key_for(aggressor, is_market);
        const bool may_self_match = aggressor.stp != StpMode::None && aggressor.owner != 0;
        const std::uint64_t needed = aggressor.leaves_qty;
        std::uint64_t available = 0;
        for (std::uint32_t index = opposite.size(); index > 0 && opposite.key_at(index - 1) >= crossing_key;
             --index) {
            const PriceLevel& level = opposite.level_at(index - 1);
            if (!may_self_match) {
                available += level.total_qty;
                if (available >= needed) {
                    return true;
                }
                continue;
            }
            for (OrderSlot slot = level.head; slot != kNullSlot; slot = orders[slot].next_in_level) {
                const Order& resting = orders[slot];
                if (resting.owner == aggressor.owner) {
                    if (aggressor.stp == StpMode::CancelResting) {
                        continue;
                    }
                    return false;
                }
                available += resting.leaves_qty;
                if (available >= needed) {
                    return true;
                }
            }
        }
        return false;
    }

    AggressorOutcome match_aggressor(Instrument& instrument, Order& aggressor, bool is_market, Timestamp ts,
                                     EventBuffer& out) {
        PriceLadder& opposite = instrument.ladder(opposite_side(aggressor.side));
        const Price crossing_key = crossing_key_for(aggressor, is_market);
        const bool may_self_match = aggressor.stp != StpMode::None && aggressor.owner != 0;
        const Aggressor aggressor_side = aggressor_of(aggressor.side);
        const bool aggressor_is_buy = aggressor.side == Side::BUY;

        while (!opposite.empty() && opposite.best_key() >= crossing_key) {
            PriceLevel& level = opposite.best_level();
            Order& resting = orders[level.head];

            if (LOB_UNLIKELY(may_self_match && resting.owner == aggressor.owner)) {
                switch (aggressor.stp) {
                    case StpMode::CancelResting:
                        emit_canceled(resting, CancelReason::SelfTradePrevention, 0, resting.leaves_qty, true, ts, out);
                        level.total_qty -= resting.leaves_qty;
                        pop_best_head(opposite);
                        continue;
                    case StpMode::CancelBoth:
                        emit_canceled(resting, CancelReason::SelfTradePrevention, 0, resting.leaves_qty, true, ts, out);
                        level.total_qty -= resting.leaves_qty;
                        pop_best_head(opposite);
                        [[fallthrough]];
                    case StpMode::CancelAggressor:
                    case StpMode::None:
                        emit_canceled(aggressor, CancelReason::SelfTradePrevention, 0, aggressor.leaves_qty, false, ts,
                                      out);
                        return AggressorOutcome::CanceledBySelfTradePrevention;
                }
            }

            const Quantity traded = std::min(aggressor.leaves_qty, resting.leaves_qty);
            aggressor.leaves_qty -= traded;
            aggressor.cum_qty += traded;
            resting.leaves_qty -= traded;
            resting.cum_qty += traded;
            level.total_qty -= traded;
            const Price trade_price = resting.price;
            if (aggressor_is_buy) {
                emit_fill(instrument.id, ts, trade_price, traded, aggressor_side, aggressor, resting, out);
            } else {
                emit_fill(instrument.id, ts, trade_price, traded, aggressor_side, resting, aggressor, out);
            }
            instrument.has_last_trade_price = true;
            instrument.last_trade_price = trade_price;
            if (resting.leaves_qty == 0) {
                pop_best_head(opposite);
            }
            if (aggressor.leaves_qty == 0) {
                return AggressorOutcome::FullyFilled;
            }
        }
        return AggressorOutcome::HasRemainder;
    }

    void rest_incoming_order(Instrument& instrument, Timestamp ts, EventBuffer& out) {
        const OrderSlot slot = orders.allocate();
        orders[slot] = incoming_order;
        append_to_level(instrument, slot);
        register_live_order(slot);
        emit_rested(orders[slot], ts, out);
    }

    // ---- NewOrder (SPEC 4) --------------------------------------------------
    void handle_new_order(const NewOrder& cmd, EventBuffer& out) {
        const auto reject = [&](RejectReason reason) {
            emit_rejected(CommandType::NewOrder, reason, cmd.instrument, cmd.session, cmd.cl_ord_id, 0, 0, cmd.ts, out);
        };

        Instrument* const instrument = find_instrument(cmd.instrument);
        if (LOB_UNLIKELY(instrument == nullptr)) {
            return reject(RejectReason::UnknownInstrument);
        }
        if (LOB_UNLIKELY(cmd.session == 0 || !is_storable_client_token(cmd.cl_ord_id))) {
            return reject(RejectReason::InvalidClOrdId);
        }
        if (LOB_UNLIKELY(slot_by_client_key.find(client_order_key(cmd.session, cmd.cl_ord_id)) != nullptr)) {
            return reject(RejectReason::DuplicateClOrdId);
        }
        const bool is_market = cmd.type == OrdType::Market;
        const bool is_immediate = cmd.tif == TimeInForce::IOC || cmd.tif == TimeInForce::FOK;
        if (LOB_UNLIKELY(static_cast<std::uint8_t>(cmd.side) > static_cast<std::uint8_t>(Side::SELL) ||
                         static_cast<std::uint8_t>(cmd.type) > static_cast<std::uint8_t>(OrdType::Market) ||
                         static_cast<std::uint8_t>(cmd.tif) > static_cast<std::uint8_t>(TimeInForce::FOK) ||
                         static_cast<std::uint8_t>(cmd.stp) > static_cast<std::uint8_t>(StpMode::CancelBoth) ||
                         (cmd.post_only && (is_market || is_immediate)))) {
            return reject(RejectReason::InvalidField);
        }
        if (LOB_UNLIKELY(!instrument->is_valid_quantity(cmd.qty))) {
            return reject(RejectReason::InvalidQuantity);
        }
        if (LOB_UNLIKELY(!is_market && !instrument->is_valid_price(cmd.price))) {
            return reject(RejectReason::InvalidPrice);
        }
        const Phase phase = instrument->phase;
        if (LOB_UNLIKELY(phase != Phase::Continuous)) {
            if (phase == Phase::Closed) {
                return reject(RejectReason::MarketClosed);
            }
            if (phase == Phase::Halted) {
                return reject(RejectReason::InstrumentHalted);
            }
            if (is_call_phase(phase) && (is_market || is_immediate)) {
                return reject(RejectReason::NotAllowedInPhase);
            }
        }
        if (LOB_UNLIKELY(cmd.post_only && phase == Phase::Continuous &&
                         instrument->would_cross_opposite_best(cmd.side, cmd.price))) {
            return reject(RejectReason::PostOnlyWouldCross);
        }
        if (LOB_UNLIKELY(live_order_count >= max_live_orders)) {
            return reject(RejectReason::CapacityExceeded);
        }

        Order& order = incoming_order;
        order.next_in_level = kNullSlot;
        order.prev_in_level = kNullSlot;
        order.leaves_qty = cmd.qty;
        order.cum_qty = 0;
        order.price = cmd.price;
        order.order_id = next_order_id++;
        order.cl_ord_id = cmd.cl_ord_id;
        order.owner = cmd.owner;
        order.qty = cmd.qty;
        order.session = cmd.session;
        order.instrument = cmd.instrument;
        order.side = cmd.side;
        order.tif = cmd.tif;
        order.stp = cmd.stp;
        order.post_only = cmd.post_only;
        emit_accepted(cmd, order.order_id, out);

        if (LOB_UNLIKELY(phase != Phase::Continuous)) {  // call phase: rest without matching
            rest_incoming_order(*instrument, cmd.ts, out);
            return;
        }
        if (LOB_UNLIKELY(cmd.tif == TimeInForce::FOK) && !fill_or_kill_can_fill(*instrument, order, is_market)) {
            emit_canceled(order, CancelReason::FillOrKill, 0, order.qty, false, cmd.ts, out);
            return;
        }
        if (match_aggressor(*instrument, order, is_market, cmd.ts, out) != AggressorOutcome::HasRemainder) {
            return;
        }
        if (is_market || is_immediate) {
            emit_canceled(order, CancelReason::ImmediateOrCancel, 0, order.leaves_qty, false, cmd.ts, out);
            return;
        }
        rest_incoming_order(*instrument, cmd.ts, out);
    }

    static void emit_accepted(const NewOrder& cmd, OrderId order_id, EventBuffer& out) {
        Event event = blank_event(EventType::Accepted, cmd.instrument, cmd.ts);
        AcceptedEvent& accepted = event.accepted;
        accepted.order_id = order_id;
        accepted.cl_ord_id = cmd.cl_ord_id;
        accepted.session = cmd.session;
        accepted.owner = cmd.owner;
        accepted.side = cmd.side;
        accepted.ord_type = cmd.type;
        accepted.tif = cmd.tif;
        accepted.stp = cmd.stp;
        accepted.post_only = cmd.post_only;
        accepted.price = cmd.price;
        accepted.qty = cmd.qty;
        if (!out.push(event)) std::abort();
    }

    // ---- CancelOrder (SPEC 5) -----------------------------------------------
    void handle_cancel(const CancelOrder& cmd, EventBuffer& out) {
        const OrderSlot slot = find_live_slot(cmd.session, cmd.orig_cl_ord_id);
        if (LOB_UNLIKELY(slot == kNullSlot)) {
            emit_rejected(CommandType::Cancel, RejectReason::UnknownOrder, 0, cmd.session, cmd.cl_ord_id,
                          cmd.orig_cl_ord_id, 0, cmd.ts, out);
            return;
        }
        Order& order = orders[slot];
        const Quantity target = cmd.reduce_to_total_qty == 0 ? 0 :
            (cmd.reduce_to_total_qty > order.cum_qty ? cmd.reduce_to_total_qty - order.cum_qty : 0);
        if (target >= order.leaves_qty) return;
        const Quantity removed = order.leaves_qty - target;
        if (target == 0) {
            emit_canceled(order, CancelReason::UserRequested, cmd.cl_ord_id, removed, true, cmd.ts, out);
            remove_resting_order(instruments[order.instrument - 1], slot);
        } else {
            auto& ladder = instruments[order.instrument - 1].ladder(order.side);
            ladder.level_at(ladder.find(ladder_key(order.side, order.price))).total_qty -= removed;
            order.leaves_qty = target;
            order.qty = order.cum_qty + target;
            emit_canceled(order, CancelReason::UserRequested, cmd.cl_ord_id, removed, true, cmd.ts, out, target);
        }
    }

    // ---- ReplaceOrder (SPEC 6) ----------------------------------------------
    void handle_replace(const ReplaceOrder& cmd, EventBuffer& out) {
        const OrderSlot slot = find_live_slot(cmd.session, cmd.orig_cl_ord_id);
        if (LOB_UNLIKELY(slot == kNullSlot)) {
            emit_rejected(CommandType::Replace, RejectReason::UnknownOrder, 0, cmd.session, cmd.cl_ord_id,
                          cmd.orig_cl_ord_id, 0, cmd.ts, out);
            return;
        }
        Order& order = orders[slot];
        Instrument& instrument = instruments[order.instrument - 1];
        const auto reject = [&](RejectReason reason) {
            emit_rejected(CommandType::Replace, reason, order.instrument, cmd.session, cmd.cl_ord_id,
                          cmd.orig_cl_ord_id, order.order_id, cmd.ts, out);
        };

        if (LOB_UNLIKELY(cmd.cl_ord_id > kMaxClOrdId)) {
            return reject(RejectReason::InvalidClOrdId);
        }
        const ClOrdId new_token = cmd.cl_ord_id != 0 ? cmd.cl_ord_id : cmd.orig_cl_ord_id;
        if (LOB_UNLIKELY(new_token != cmd.orig_cl_ord_id && find_live_slot(cmd.session, new_token) != kNullSlot)) {
            return reject(RejectReason::DuplicateClOrdId);
        }
        if (LOB_UNLIKELY(!instrument.is_valid_quantity(cmd.qty))) {
            return reject(RejectReason::InvalidQuantity);
        }
        if (LOB_UNLIKELY(!instrument.is_valid_price(cmd.price))) {
            return reject(RejectReason::InvalidPrice);
        }
        if (LOB_UNLIKELY(instrument.phase == Phase::Closed)) {
            return reject(RejectReason::MarketClosed);
        }
        if (LOB_UNLIKELY(instrument.phase == Phase::Halted)) {
            return reject(RejectReason::InstrumentHalted);
        }
        if (LOB_UNLIKELY(order.post_only && instrument.phase == Phase::Continuous &&
                         instrument.would_cross_opposite_best(order.side, cmd.price))) {
            return reject(RejectReason::PostOnlyWouldCross);
        }

        if (LOB_UNLIKELY(cmd.qty <= order.cum_qty)) {  // nothing left to work: the order is done
            emit_canceled(order, CancelReason::UserRequested, cmd.cl_ord_id, order.leaves_qty, true, cmd.ts, out);
            remove_resting_order(instrument, slot);
            return;
        }

        const OrderId old_order_id = order.order_id;
        const Price old_price = order.price;
        const Quantity old_leaves_qty = order.leaves_qty;
        const Quantity new_leaves_qty = cmd.qty - order.cum_qty;

        if (!cmd.force_priority_loss && cmd.price == order.price && new_leaves_qty <= old_leaves_qty) {  // priority kept: update in place
            if (new_leaves_qty != old_leaves_qty) {
                PriceLadder& ladder = instrument.ladder(order.side);
                ladder.level_at(ladder.find(ladder_key(order.side, order.price))).total_qty -=
                    old_leaves_qty - new_leaves_qty;
            }
            if (new_token != order.cl_ord_id) {
                slot_by_client_key.erase(client_order_key(order.session, order.cl_ord_id));
                if (!slot_by_client_key.insert(client_order_key(order.session, new_token), slot)) std::abort();
            }
            order.cl_ord_id = new_token;
            order.qty = cmd.qty;
            order.leaves_qty = new_leaves_qty;
            emit_replaced(order, old_order_id, cmd.orig_cl_ord_id, old_price, old_leaves_qty, true, cmd.ts, out);
            return;
        }

        // Priority lost: leave the book, take a new OrderId, re-enter as an aggressor.
        unlink_from_level(instrument, slot);
        forget_live_order(slot);
        order.order_id = next_order_id++;
        order.cl_ord_id = new_token;
        order.price = cmd.price;
        order.qty = cmd.qty;
        order.leaves_qty = new_leaves_qty;
        emit_replaced(order, old_order_id, cmd.orig_cl_ord_id, old_price, old_leaves_qty, false, cmd.ts, out);
        if (instrument.phase == Phase::Continuous &&
            match_aggressor(instrument, order, false, cmd.ts, out) != AggressorOutcome::HasRemainder) {
            orders.release(slot);
            return;
        }
        append_to_level(instrument, slot);
        register_live_order(slot);
        emit_rested(order, cmd.ts, out);
    }

    static void emit_replaced(const Order& order, OrderId old_order_id, ClOrdId orig_cl_ord_id, Price old_price,
                              Quantity old_leaves_qty, bool priority_kept, Timestamp ts, EventBuffer& out) {
        Event event = blank_event(EventType::Replaced, order.instrument, ts);
        ReplacedEvent& replaced = event.replaced;
        replaced.old_order_id = old_order_id;
        replaced.new_order_id = order.order_id;
        replaced.orig_cl_ord_id = orig_cl_ord_id;
        replaced.cl_ord_id = order.cl_ord_id;
        replaced.session = order.session;
        replaced.side = order.side;
        replaced.old_price = old_price;
        replaced.price = order.price;
        replaced.old_leaves_qty = old_leaves_qty;
        replaced.qty = order.qty;
        replaced.leaves_qty = order.leaves_qty;
        replaced.cum_qty = order.cum_qty;
        replaced.priority_kept = priority_kept;
        if (!out.push(event)) std::abort();
    }

    // ---- MassCancel (SPEC 7) ------------------------------------------------
    void handle_mass_cancel(const MassCancel& cmd, EventBuffer& out) {
        const CancelReason reason = cmd.cancel_on_disconnect ? CancelReason::CancelOnDisconnect : CancelReason::MassCancel;
        std::uint32_t canceled_count = 0;
        for (OrderSlot slot = oldest_live_slot; slot != kNullSlot;) {
            const Order& order = orders[slot];
            const OrderSlot next_slot = order.next_by_order_id;
            if (order.session == cmd.session && (cmd.instrument == 0 || order.instrument == cmd.instrument)) {
                emit_canceled(order, reason, cmd.cl_ord_id, order.leaves_qty, true, cmd.ts, out);
                remove_resting_order(instruments[order.instrument - 1], slot);
                ++canceled_count;
            }
            slot = next_slot;
        }
        Event event = blank_event(EventType::MassCancelDone, cmd.instrument, cmd.ts);
        MassCancelDoneEvent& done = event.mass_cancel_done;
        done.session = cmd.session;
        done.request_cl_ord_id = cmd.cl_ord_id;
        done.canceled_count = canceled_count;
        done.reason = reason;
        if (!out.push(event)) std::abort();
    }

    // ---- SetPhase (SPEC 8) --------------------------------------------------
    void handle_set_phase(const SetPhase& cmd, EventBuffer& out) {
        if (LOB_UNLIKELY(cmd.instrument != 0 && find_instrument(cmd.instrument) == nullptr)) {
            emit_rejected(CommandType::SetPhase, RejectReason::UnknownInstrument, cmd.instrument, 0, 0, 0, 0, cmd.ts,
                          out);
            return;
        }
        if (LOB_UNLIKELY(static_cast<std::uint8_t>(cmd.phase) > static_cast<std::uint8_t>(Phase::PreClose))) {
            emit_rejected(CommandType::SetPhase, RejectReason::InvalidField, cmd.instrument, 0, 0, 0, 0, cmd.ts, out);
            return;
        }
        if (cmd.instrument != 0) {
            apply_phase(instruments[cmd.instrument - 1], cmd.phase, cmd.ts, out);
            return;
        }
        for (Instrument& instrument : instruments) {
            apply_phase(instrument, cmd.phase, cmd.ts, out);
        }
    }

    void apply_phase(Instrument& instrument, Phase new_phase, Timestamp ts, EventBuffer& out) {
        const Phase old_phase = instrument.phase;
        if (new_phase == old_phase) {
            return;
        }
        if ((new_phase == Phase::Continuous || new_phase == Phase::Closed) && instrument.is_crossed()) {
            uncross(instrument, ts, out);
        }
        if (new_phase == Phase::Closed) {
            expire_day_orders(instrument, ts, out);
        }
        instrument.phase = new_phase;
        Event event = blank_event(EventType::PhaseChanged, instrument.id, ts);
        event.phase_changed.old_phase = old_phase;
        event.phase_changed.new_phase = new_phase;
        if (!out.push(event)) std::abort();
    }

    void expire_day_orders(Instrument& instrument, Timestamp ts, EventBuffer& out) {
        for (OrderSlot slot = oldest_live_slot; slot != kNullSlot;) {
            const Order& order = orders[slot];
            const OrderSlot next_slot = order.next_by_order_id;
            if (order.instrument == instrument.id && order.tif == TimeInForce::Day) {
                emit_canceled(order, CancelReason::EndOfDay, 0, order.leaves_qty, true, ts, out);
                remove_resting_order(instrument, slot);
            }
            slot = next_slot;
        }
    }

    // ---- auction uncross (SPEC 9) -------------------------------------------
    [[nodiscard]] UncrossPrice compute_uncross_price(const Instrument& instrument) const noexcept {
        UncrossPrice result;
        if (!instrument.is_crossed()) {
            std::uint64_t buy = 0, sell = 0;
            for (std::uint32_t i = 0; i < instrument.bids.size(); ++i) buy += instrument.bids.level_at(i).total_qty;
            for (std::uint32_t i = 0; i < instrument.asks.size(); ++i) sell += instrument.asks.level_at(i).total_qty;
            result.imbalance_qty = buy >= sell ? buy - sell : sell - buy;
            result.imbalance_side = buy > sell ? Aggressor::Buy : buy < sell ? Aggressor::Sell : Aggressor::None;
            return result;
        }
        const PriceLadder& bids = instrument.bids;
        const PriceLadder& asks = instrument.asks;
        const Price best_bid = bids.best_key();
        const Price best_ask = -asks.best_key();
        const Price reference_price =
            instrument.has_last_trade_price ? instrument.last_trade_price : instrument.config.reference_price;

        // Levels inside [best_ask, best_bid]: bids [lowest_bid_index, bids.size()) ascending in price,
        // asks [highest_ask_index, asks.size()) descending in price.
        const std::uint32_t lowest_bid_index = bids.count_worse_than(best_ask);
        const std::uint32_t highest_ask_index = asks.count_worse_than(-best_bid);

        std::uint64_t buy_volume = 0;  // B(p): bids priced >= candidate
        for (std::uint32_t index = lowest_bid_index; index < bids.size(); ++index) {
            buy_volume += bids.level_at(index).total_qty;
        }
        std::uint64_t sell_volume = 0;  // S(p): asks priced <= candidate

        std::uint32_t next_bid_index = lowest_bid_index;  // next bid level by ascending price
        std::uint32_t asks_not_added = asks.size();       // asks [highest_ask_index, asks_not_added) not yet in S
        bool have_candidate = false;
        std::uint64_t best_abs_imbalance = 0;
        std::uint64_t best_reference_distance = 0;
        while (next_bid_index < bids.size() || asks_not_added > highest_ask_index) {
            Price candidate = std::numeric_limits<Price>::max();
            if (next_bid_index < bids.size()) {
                candidate = bids.key_at(next_bid_index);
            }
            if (asks_not_added > highest_ask_index) {
                candidate = std::min(candidate, -asks.key_at(asks_not_added - 1));
            }
            while (asks_not_added > highest_ask_index && -asks.key_at(asks_not_added - 1) <= candidate) {
                sell_volume += asks.level_at(asks_not_added - 1).total_qty;
                --asks_not_added;
            }

            const std::uint64_t paired = std::min(buy_volume, sell_volume);
            const std::uint64_t abs_imbalance =
                buy_volume >= sell_volume ? buy_volume - sell_volume : sell_volume - buy_volume;
            const std::uint64_t reference_distance = absolute_distance(candidate, reference_price);
            // Candidates arrive in ascending price, so keeping the first of equals picks the lower price.
            const bool better =
                !have_candidate || paired > result.paired_qty ||
                (paired == result.paired_qty &&
                 (abs_imbalance < best_abs_imbalance ||
                  (abs_imbalance == best_abs_imbalance && reference_distance < best_reference_distance)));
            if (better) {
                have_candidate = true;
                result.price = candidate;
                result.paired_qty = paired;
                result.imbalance_qty = abs_imbalance;
                result.imbalance_side = buy_volume > sell_volume   ? Aggressor::Buy
                                        : buy_volume < sell_volume ? Aggressor::Sell
                                                                   : Aggressor::None;
                best_abs_imbalance = abs_imbalance;
                best_reference_distance = reference_distance;
            }

            while (next_bid_index < bids.size() && bids.key_at(next_bid_index) <= candidate) {
                buy_volume -= bids.level_at(next_bid_index).total_qty;
                ++next_bid_index;
            }
        }
        result.crossed = true;
        return result;
    }

    void uncross(Instrument& instrument, Timestamp ts, EventBuffer& out) {
        const UncrossPrice auction = compute_uncross_price(instrument);
        const Price uncross_price = auction.price;
        PriceLadder& bids = instrument.bids;
        PriceLadder& asks = instrument.asks;
        while (!bids.empty() && !asks.empty() && bids.best_key() >= uncross_price &&
               -asks.best_key() <= uncross_price) {
            PriceLevel& bid_level = bids.best_level();
            PriceLevel& ask_level = asks.best_level();
            Order& buy = orders[bid_level.head];
            Order& sell = orders[ask_level.head];
            const Quantity traded = std::min(buy.leaves_qty, sell.leaves_qty);
            buy.leaves_qty -= traded;
            buy.cum_qty += traded;
            sell.leaves_qty -= traded;
            sell.cum_qty += traded;
            bid_level.total_qty -= traded;
            ask_level.total_qty -= traded;
            emit_fill(instrument.id, ts, uncross_price, traded, Aggressor::None, buy, sell, out);
            if (buy.leaves_qty == 0) {
                pop_best_head(bids);
            }
            if (sell.leaves_qty == 0) {
                pop_best_head(asks);
            }
        }
        instrument.has_last_trade_price = true;
        instrument.last_trade_price = uncross_price;
        Event event = blank_event(EventType::AuctionSummary, instrument.id, ts);
        event.auction_summary.price = uncross_price;
        event.auction_summary.matched_qty = auction.paired_qty;
        event.auction_summary.match_id = next_match_id++;
        if (!out.push(event)) std::abort();
    }

    // ---- queries ------------------------------------------------------------
    [[nodiscard]] OrderInfo order_info_of(const Order& order) const noexcept {
        OrderInfo info;
        std::memset(static_cast<void*>(&info), 0, sizeof(info));
        info.order_id = order.order_id;
        info.cl_ord_id = order.cl_ord_id;
        info.instrument = order.instrument;
        info.session = order.session;
        info.owner = order.owner;
        info.side = order.side;
        info.tif = order.tif;
        info.stp = order.stp;
        info.post_only = order.post_only;
        info.price = order.price;
        info.qty = order.qty;
        info.leaves_qty = order.leaves_qty;
        info.cum_qty = order.cum_qty;
        return info;
    }

    [[nodiscard]] std::uint32_t count_orders_in_level(const PriceLevel& level) const noexcept {
        std::uint32_t count = 0;
        for (OrderSlot slot = level.head; slot != kNullSlot; slot = orders[slot].next_in_level) {
            ++count;
        }
        return count;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

MatchingEngine::MatchingEngine(const EngineConfig& config) : impl_(std::make_unique<Impl>(config)) {}

MatchingEngine::~MatchingEngine() = default;

InstrumentId MatchingEngine::add_instrument(const InstrumentConfig& config) {
    Impl& engine = *impl_;
    if (!valid_instrument_config(config) || engine.instruments.size() >= engine.max_instruments ||
        find_instrument(config.symbol) != 0) {
        return 0;
    }
    // A side can never hold more distinct prices than the band has ticks, nor
    // more than there can be live orders.
    const std::uint64_t band_levels =
        static_cast<std::uint64_t>((config.max_price - config.min_price) / config.tick_size) + 1;
    const auto max_levels = static_cast<std::uint32_t>(
        std::min<std::uint64_t>({band_levels, engine.max_live_orders, std::numeric_limits<std::uint32_t>::max()}));

    if (std::numeric_limits<std::size_t>::max() / max_levels < sizeof(PriceLevel) ||
        std::numeric_limits<std::size_t>::max() / max_levels < sizeof(Price)) return 0;
    Instrument& instrument = engine.instruments.emplace_back();
    instrument.config = config;
    instrument.id = static_cast<InstrumentId>(engine.instruments.size());
    instrument.phase = config.initial_phase;
    instrument.bids.reserve(max_levels);
    instrument.asks.reserve(max_levels);
    return instrument.id;
}

// A continuous step removes a resting order or terminates the aggressor.
// Thus new/replace need at most live+2 events (ack, steps, disposition).
// Phase batches need at most 2*live + 2*instruments (fills, expiry, summaries, phases).
HandleResult MatchingEngine::handle(const NewOrder& cmd, EventBuffer& out) {
    if (!out.can_append(impl_->live_order_count + 2)) return HandleResult::InsufficientOutput;
    impl_->handle_new_order(cmd, out);
    return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const CancelOrder& cmd, EventBuffer& out) {
    if (!out.can_append(1)) return HandleResult::InsufficientOutput;
    impl_->handle_cancel(cmd, out);
    return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const ReplaceOrder& cmd, EventBuffer& out) {
    if (!out.can_append(impl_->live_order_count + 2)) return HandleResult::InsufficientOutput;
    impl_->handle_replace(cmd, out);
    return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const MassCancel& cmd, EventBuffer& out) {
    if (!out.can_append(impl_->live_order_count + 1)) return HandleResult::InsufficientOutput;
    impl_->handle_mass_cancel(cmd, out);
    return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const SetPhase& cmd, EventBuffer& out) {
    if (!out.can_append(2 * impl_->live_order_count + 2 * impl_->instruments.size() + 1)) return HandleResult::InsufficientOutput;
    impl_->handle_set_phase(cmd, out);
    return HandleResult::Applied;
}
HandleResult MatchingEngine::handle(const Command& cmd, EventBuffer& out) {
    switch (cmd.type) {
        case CommandType::NewOrder: return handle(cmd.new_order, out);
        case CommandType::Cancel: return handle(cmd.cancel, out);
        case CommandType::Replace: return handle(cmd.replace, out);
        case CommandType::MassCancel: return handle(cmd.mass_cancel, out);
        case CommandType::SetPhase: return handle(cmd.set_phase, out);
    }
    std::abort();
}

std::size_t MatchingEngine::instrument_count() const noexcept { return impl_->instruments.size(); }

const InstrumentConfig* MatchingEngine::instrument(InstrumentId id) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    return instrument ? &instrument->config : nullptr;
}

InstrumentId MatchingEngine::find_instrument(const Symbol& symbol) const noexcept {
    for (const Instrument& instrument : impl_->instruments) {
        if (instrument.config.symbol == symbol) {
            return instrument.id;
        }
    }
    return 0;
}

Phase MatchingEngine::phase(InstrumentId id) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    return instrument ? instrument->phase : Phase::Closed;
}

std::optional<Price> MatchingEngine::best_bid(InstrumentId id) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr || instrument->bids.empty()) {
        return std::nullopt;
    }
    return instrument->bids.best_key();
}

std::optional<Price> MatchingEngine::best_ask(InstrumentId id) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr || instrument->asks.empty()) {
        return std::nullopt;
    }
    return -instrument->asks.best_key();
}

std::optional<Price> MatchingEngine::last_trade_price(InstrumentId id) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr || !instrument->has_last_trade_price) {
        return std::nullopt;
    }
    return instrument->last_trade_price;
}

std::size_t MatchingEngine::depth(InstrumentId id, Side side, LevelInfo* out, std::size_t max_levels) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr) {
        return 0;
    }
    const PriceLadder& ladder = instrument->ladder(side);
    const std::size_t written = std::min<std::size_t>(max_levels, ladder.size());
    for (std::size_t rank = 0; rank < written; ++rank) {
        const auto index = static_cast<std::uint32_t>(ladder.size() - 1 - rank);
        const PriceLevel& level = ladder.level_at(index);
        out[rank].price = price_from_ladder_key(side, ladder.key_at(index));
        out[rank].qty = level.total_qty;
        out[rank].order_count = impl_->count_orders_in_level(level);
    }
    return written;
}

std::size_t MatchingEngine::level_orders(InstrumentId id, Side side, Price price, OrderId* out,
                                         std::size_t max_orders) const noexcept {
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr) {
        return 0;
    }
    const PriceLadder& ladder = instrument->ladder(side);
    const std::uint32_t index = ladder.find(ladder_key(side, price));
    if (index == PriceLadder::kNotFound) {
        return 0;
    }
    std::size_t written = 0;
    for (OrderSlot slot = ladder.level_at(index).head; slot != kNullSlot && written < max_orders;
         slot = impl_->orders[slot].next_in_level) {
        out[written++] = impl_->orders[slot].order_id;
    }
    return written;
}

std::optional<OrderInfo> MatchingEngine::find_order(SessionId session, ClOrdId cl_ord_id) const noexcept {
    const OrderSlot slot = impl_->find_live_slot(session, cl_ord_id);
    if (slot == kNullSlot) {
        return std::nullopt;
    }
    return impl_->order_info_of(impl_->orders[slot]);
}

std::optional<OrderInfo> MatchingEngine::find_order(OrderId order_id) const noexcept {
    if (order_id == 0) {
        return std::nullopt;
    }
    const OrderSlot* slot = impl_->slot_by_order_id.find(order_id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return impl_->order_info_of(impl_->orders[*slot]);
}

std::size_t MatchingEngine::live_order_count() const noexcept { return impl_->live_order_count; }

AuctionIndicative MatchingEngine::auction_indicative(InstrumentId id) const noexcept {
    AuctionIndicative indicative;
    const Instrument* instrument = impl_->find_instrument(id);
    if (instrument == nullptr) {
        return indicative;
    }
    const UncrossPrice auction = impl_->compute_uncross_price(*instrument);
    indicative.crossed = auction.crossed;
    indicative.price = auction.price;
    indicative.paired_qty = auction.paired_qty;
    indicative.imbalance_qty = auction.imbalance_qty;
    indicative.imbalance_side = auction.imbalance_side;
    return indicative;
}

void MatchingEngine::visit_orders(void (*fn)(const OrderInfo&, void*), void* ctx) const {
    for (OrderSlot slot = impl_->oldest_live_slot; slot != kNullSlot; slot = impl_->orders[slot].next_by_order_id) {
        fn(impl_->order_info_of(impl_->orders[slot]), ctx);
    }
}

}  // namespace exsim
