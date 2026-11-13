#ifndef TESTS_EXSIM_ENGINE_ENGINE_HARNESS_HPP
#define TESTS_EXSIM_ENGINE_ENGINE_HARNESS_HPP

// Shared helpers for the engine tests built on Harness. Public API only.

#include "../../framework/test.hpp"
#include <engine/matching_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine_harness {

using namespace exsim;

constexpr Price kTick = 100;
constexpr Price kReferencePrice = 1'000'000;

inline InstrumentConfig standard_instrument_config(const char* symbol = "TEST", Phase initial_phase = Phase::Continuous) {
    InstrumentConfig config;
    config.symbol = Symbol(symbol);
    config.tick_size = kTick;
    config.lot_size = 1;
    config.max_order_qty = 1'000'000;
    config.min_price = 100;
    config.max_price = 10'000'000;
    config.reference_price = kReferencePrice;
    config.initial_phase = initial_phase;
    return config;
}

inline EngineConfig small_engine_config(std::size_t max_orders = 1024) {
    EngineConfig config;
    config.max_instruments = 8;
    config.max_orders = max_orders;
    return config;
}

// One engine with one registered instrument, plus a reusable event buffer.
// Every command gets a fresh, increasing timestamp so tests can assert that
// events copy ts from their command.
class Harness {
public:
    explicit Harness(Phase initial_phase = Phase::Continuous, std::size_t max_orders = 1024)
        : engine_(small_engine_config(max_orders)) {
        instrument_ = engine_.add_instrument(standard_instrument_config("TEST", initial_phase));
    }

    MatchingEngine& engine() { return engine_; }
    InstrumentId instrument() const { return instrument_; }
    const EventBuffer& events() const { return out_; }
    const Event& event(std::size_t index) const { return out_[index]; }
    std::size_t event_count() const { return out_.size(); }
    Timestamp last_ts() const { return ts_; }

    NewOrder limit(SessionId session, ClOrdId token, Side side, Price price, Quantity qty) const {
        NewOrder order;
        order.instrument = instrument_;
        order.session = session;
        order.owner = 0;
        order.cl_ord_id = token;
        order.side = side;
        order.type = OrdType::Limit;
        order.tif = TimeInForce::Day;
        order.stp = StpMode::None;
        order.post_only = false;
        order.price = price;
        order.qty = qty;
        return order;
    }

    NewOrder market(SessionId session, ClOrdId token, Side side, Quantity qty) const {
        NewOrder order = limit(session, token, side, 0, qty);
        order.type = OrdType::Market;
        return order;
    }

    const EventBuffer& send(NewOrder cmd) {
        cmd.ts = ++ts_;
        return run(Command(cmd));
    }
    const EventBuffer& cancel(SessionId session, ClOrdId orig, ClOrdId request = 0) {
        CancelOrder cmd;
        cmd.ts = ++ts_;
        cmd.session = session;
        cmd.orig_cl_ord_id = orig;
        cmd.cl_ord_id = request;
        return run(Command(cmd));
    }
    const EventBuffer& replace(SessionId session, ClOrdId orig, ClOrdId new_token, Price price, Quantity qty) {
        ReplaceOrder cmd;
        cmd.ts = ++ts_;
        cmd.session = session;
        cmd.orig_cl_ord_id = orig;
        cmd.cl_ord_id = new_token;
        cmd.price = price;
        cmd.qty = qty;
        return run(Command(cmd));
    }
    const EventBuffer& mass_cancel(SessionId session, InstrumentId instrument, ClOrdId request,
                                   bool cancel_on_disconnect = false) {
        MassCancel cmd;
        cmd.ts = ++ts_;
        cmd.session = session;
        cmd.instrument = instrument;
        cmd.cl_ord_id = request;
        cmd.cancel_on_disconnect = cancel_on_disconnect;
        return run(Command(cmd));
    }
    const EventBuffer& set_phase(Phase phase, InstrumentId instrument) {
        SetPhase cmd;
        cmd.ts = ++ts_;
        cmd.instrument = instrument;
        cmd.phase = phase;
        return run(Command(cmd));
    }
    const EventBuffer& set_phase(Phase phase) { return set_phase(phase, instrument_); }

    // Rests a plain DAY limit order and requires it to be accepted and rested.
    OrderId rest(SessionId session, ClOrdId token, Side side, Price price, Quantity qty, OwnerId owner = 0) {
        NewOrder order = limit(session, token, side, price, qty);
        order.owner = owner;
        send(order);
        REQUIRE(out_.size() == 2);
        REQUIRE(out_[0].type == EventType::Accepted);
        REQUIRE(out_[1].type == EventType::Rested);
        return out_[0].accepted.order_id;
    }

    std::vector<OrderId> fifo(Side side, Price price) const {
        std::vector<OrderId> ids(4096);
        ids.resize(engine_.level_orders(instrument_, side, price, ids.data(), ids.size()));
        return ids;
    }

    std::vector<LevelInfo> levels(Side side) const {
        std::vector<LevelInfo> result(4096);
        result.resize(engine_.depth(instrument_, side, result.data(), result.size()));
        return result;
    }

    std::vector<OrderInfo> all_orders() const {
        std::vector<OrderInfo> result;
        engine_.for_each_order([&](const OrderInfo& info) { result.push_back(info); });
        return result;
    }

private:
    const EventBuffer& run(const Command& cmd) {
        out_.clear();
        REQUIRE(engine_.handle(cmd, out_) == HandleResult::Applied);
        return out_;
    }

    MatchingEngine engine_;
    EventBuffer out_;
    InstrumentId instrument_ = 0;
    Timestamp ts_ = 0;
};

// ---- field-by-field expectations ---------------------------------------

inline void check_rejected(const Event& event, CommandType cmd, RejectReason reason, SessionId session,
                           ClOrdId cl_ord_id, ClOrdId orig_cl_ord_id, OrderId order_id, InstrumentId instrument) {
    REQUIRE(event.type == EventType::Rejected);
    CHECK_EQ(event.rejected.cmd, cmd);
    CHECK_EQ(event.rejected.reason, reason);
    CHECK_EQ(event.rejected.session, session);
    CHECK_EQ(event.rejected.cl_ord_id, cl_ord_id);
    CHECK_EQ(event.rejected.orig_cl_ord_id, orig_cl_ord_id);
    CHECK_EQ(event.rejected.order_id, order_id);
    CHECK_EQ(event.instrument, instrument);
}

// Asserts the command produced exactly one Rejected event for a NewOrder.
inline void check_new_order_rejected(const Harness& h, const NewOrder& cmd, RejectReason reason) {
    REQUIRE_EQ(h.event_count(), std::size_t{1});
    check_rejected(h.event(0), CommandType::NewOrder, reason, cmd.session, cmd.cl_ord_id, 0, 0, cmd.instrument);
    CHECK_EQ(h.event(0).ts, h.last_ts());
}

inline void check_canceled(const Event& event, OrderId order_id, ClOrdId cl_ord_id, ClOrdId request_cl_ord_id,
                           Side side, Price price, Quantity canceled_qty, Quantity cum_qty, CancelReason reason,
                           bool was_resting) {
    REQUIRE(event.type == EventType::Canceled);
    CHECK_EQ(event.canceled.order_id, order_id);
    CHECK_EQ(event.canceled.cl_ord_id, cl_ord_id);
    CHECK_EQ(event.canceled.request_cl_ord_id, request_cl_ord_id);
    CHECK_EQ(event.canceled.side, side);
    CHECK_EQ(event.canceled.price, price);
    CHECK_EQ(event.canceled.canceled_qty, canceled_qty);
    CHECK_EQ(event.canceled.cum_qty, cum_qty);
    CHECK_EQ(event.canceled.reason, reason);
    CHECK_EQ(event.canceled.was_resting, was_resting);
}

inline void check_fill(const Event& event, Price price, Quantity qty, Aggressor aggressor, OrderId buy_order_id,
                       Quantity buy_leaves, Quantity buy_cum, OrderId sell_order_id, Quantity sell_leaves,
                       Quantity sell_cum) {
    REQUIRE(event.type == EventType::Fill);
    CHECK_EQ(event.fill.price, price);
    CHECK_EQ(event.fill.qty, qty);
    CHECK_EQ(event.fill.aggressor, aggressor);
    CHECK_EQ(event.fill.buy.order_id, buy_order_id);
    CHECK_EQ(event.fill.buy.leaves_qty, buy_leaves);
    CHECK_EQ(event.fill.buy.cum_qty, buy_cum);
    CHECK_EQ(event.fill.sell.order_id, sell_order_id);
    CHECK_EQ(event.fill.sell.leaves_qty, sell_leaves);
    CHECK_EQ(event.fill.sell.cum_qty, sell_cum);
}

inline std::vector<EventType> event_types(const EventBuffer& events) {
    std::vector<EventType> types;
    for (const Event& event : events) {
        types.push_back(event.type);
    }
    return types;
}

}  // namespace engine_harness

#endif
