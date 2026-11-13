#ifndef TESTS_EXSIM_ENGINE_ENGINE_FIXTURE_HPP
#define TESTS_EXSIM_ENGINE_ENGINE_FIXTURE_HPP

// Shared helpers for the engine tests built on EngineFixture. Everything here goes through
// the public MatchingEngine API only.

#include <test/framework/test.hpp>

#include <engine/matching_engine.hpp>

#include <cstdint>
#include <vector>

namespace engine_fixture {

using namespace exsim;

inline constexpr SessionId kAliceSession = 1;
inline constexpr SessionId kBobSession = 2;
inline constexpr SessionId kCarolSession = 3;

inline constexpr OwnerId kAliceFirm = 11;
inline constexpr OwnerId kBobFirm = 22;

// Price band 10..200 in ticks of 10 (20 ticks), reference price 100.
inline InstrumentConfig small_instrument_config(const char* symbol, Phase initial_phase = Phase::Continuous) {
    InstrumentConfig config;
    config.symbol = Symbol(symbol);
    config.tick_size = 10;
    config.lot_size = 1;
    config.max_order_qty = 1000;
    config.min_price = 10;
    config.max_price = 200;
    config.reference_price = 100;
    config.initial_phase = initial_phase;
    return config;
}

// One engine plus an output buffer sized by required_event_capacity. Every
// apply() must be Applied; the events of the last command stay in `events`.
struct EngineFixture {
    EngineConfig engine_config;
    MatchingEngine engine;
    EventBuffer events;

    explicit EngineFixture(Phase initial_phase = Phase::Continuous, std::size_t instrument_count = 1,
                                EngineConfig config = EngineConfig{4, 64})
        : engine_config(config), engine(config), events(required_event_capacity(config)) {
        static const char* const kSymbols[] = {"AAA", "BBB", "CCC", "DDD"};
        for (std::size_t i = 0; i < instrument_count; ++i) {
            REQUIRE_EQ(engine.add_instrument(small_instrument_config(kSymbols[i], initial_phase)),
                       static_cast<InstrumentId>(i + 1));
        }
    }

    template <typename CommandT>
    const EventBuffer& apply(const CommandT& command) {
        events.clear();
        REQUIRE_EQ(engine.handle(command, events), HandleResult::Applied);
        return events;
    }

    const Event& only_event() const {
        REQUIRE_EQ(events.size(), std::size_t{1});
        return events[0];
    }
};

inline NewOrder limit_order(SessionId session, ClOrdId token, Side side, Price price, Quantity qty,
                            InstrumentId instrument = 1) {
    NewOrder order;
    order.ts = token * 10;
    order.instrument = instrument;
    order.session = session;
    order.cl_ord_id = token;
    order.side = side;
    order.type = OrdType::Limit;
    order.tif = TimeInForce::Day;
    order.price = price;
    order.qty = qty;
    return order;
}

inline NewOrder with_owner(NewOrder order, OwnerId owner, StpMode stp = StpMode::None) {
    order.owner = owner;
    order.stp = stp;
    return order;
}

inline NewOrder with_tif(NewOrder order, TimeInForce tif) {
    order.tif = tif;
    return order;
}

inline NewOrder market_order(SessionId session, ClOrdId token, Side side, Quantity qty, Price ignored_price = 0,
                             InstrumentId instrument = 1) {
    NewOrder order = limit_order(session, token, side, ignored_price, qty, instrument);
    order.type = OrdType::Market;
    return order;
}

inline std::vector<OrderId> level_fifo(const MatchingEngine& engine, InstrumentId instrument, Side side,
                                       Price price) {
    std::vector<OrderId> ids(engine.live_order_count() + 1);
    ids.resize(engine.level_orders(instrument, side, price, ids.data(), ids.size()));
    return ids;
}

inline std::vector<LevelInfo> full_depth(const MatchingEngine& engine, InstrumentId instrument, Side side) {
    std::vector<LevelInfo> levels(engine.live_order_count() + 1);
    levels.resize(engine.depth(instrument, side, levels.data(), levels.size()));
    return levels;
}

inline std::vector<OrderInfo> all_live_orders(const MatchingEngine& engine) {
    std::vector<OrderInfo> orders;
    engine.for_each_order([&](const OrderInfo& order) { orders.push_back(order); });
    return orders;
}

inline std::vector<OrderId> order_ids_of_events(const EventBuffer& events, EventType type) {
    std::vector<OrderId> ids;
    for (const Event& event : events) {
        if (event.type != type) continue;
        if (type == EventType::Canceled) ids.push_back(event.canceled.order_id);
        if (type == EventType::Rested) ids.push_back(event.rested.order_id);
        if (type == EventType::Accepted) ids.push_back(event.accepted.order_id);
    }
    return ids;
}

// Ordered active-field digest of an event stream. Never reads padding or
// inactive union members.
inline std::uint64_t event_stream_digest(const EventBuffer& events) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](auto field) {
        auto bits = static_cast<std::uint64_t>(field);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= bits & 0xff;
            hash *= 1099511628211ULL;
            bits >>= 8;
        }
    };
    auto mix_leg = [&](const FillLeg& leg) {
        mix(leg.order_id); mix(leg.cl_ord_id); mix(leg.session); mix(leg.owner); mix(leg.leaves_qty); mix(leg.cum_qty);
    };
    mix(events.size());
    for (const Event& event : events) {
        mix(event.type); mix(event.instrument); mix(event.ts);
        switch (event.type) {
            case EventType::Accepted: {
                const auto& x = event.accepted;
                mix(x.order_id); mix(x.cl_ord_id); mix(x.session); mix(x.owner); mix(x.side); mix(x.ord_type);
                mix(x.tif); mix(x.stp); mix(x.post_only); mix(x.price); mix(x.qty);
                break;
            }
            case EventType::Rejected: {
                const auto& x = event.rejected;
                mix(x.cmd); mix(x.reason); mix(x.session); mix(x.cl_ord_id); mix(x.orig_cl_ord_id); mix(x.order_id);
                break;
            }
            case EventType::Rested: {
                const auto& x = event.rested;
                mix(x.order_id); mix(x.cl_ord_id); mix(x.session); mix(x.side); mix(x.price); mix(x.leaves_qty);
                break;
            }
            case EventType::Fill: {
                const auto& x = event.fill;
                mix(x.match_id); mix(x.price); mix(x.qty); mix(x.aggressor); mix_leg(x.buy); mix_leg(x.sell);
                break;
            }
            case EventType::Canceled: {
                const auto& x = event.canceled;
                mix(x.order_id); mix(x.cl_ord_id); mix(x.request_cl_ord_id); mix(x.session); mix(x.side);
                mix(x.price); mix(x.canceled_qty); mix(x.cum_qty); mix(x.reason); mix(x.was_resting);
                mix(x.leaves_qty);
                break;
            }
            case EventType::Replaced: {
                const auto& x = event.replaced;
                mix(x.old_order_id); mix(x.new_order_id); mix(x.orig_cl_ord_id); mix(x.cl_ord_id); mix(x.session);
                mix(x.side); mix(x.old_price); mix(x.price); mix(x.old_leaves_qty); mix(x.qty); mix(x.leaves_qty);
                mix(x.cum_qty); mix(x.priority_kept);
                break;
            }
            case EventType::MassCancelDone: {
                const auto& x = event.mass_cancel_done;
                mix(x.session); mix(x.request_cl_ord_id); mix(x.canceled_count); mix(x.reason);
                break;
            }
            case EventType::PhaseChanged:
                mix(event.phase_changed.old_phase); mix(event.phase_changed.new_phase);
                break;
            case EventType::AuctionSummary:
                mix(event.auction_summary.price); mix(event.auction_summary.matched_qty);
                mix(event.auction_summary.match_id);
                break;
        }
    }
    return hash;
}

// Digest of the whole observable book state through queries.
inline std::uint64_t book_state_digest(const MatchingEngine& engine) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](auto field) {
        auto bits = static_cast<std::uint64_t>(field);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= bits & 0xff;
            hash *= 1099511628211ULL;
            bits >>= 8;
        }
    };
    mix(engine.live_order_count());
    for (const OrderInfo& order : all_live_orders(engine)) {
        mix(order.order_id); mix(order.cl_ord_id); mix(order.instrument); mix(order.session); mix(order.owner);
        mix(order.side); mix(order.tif); mix(order.stp); mix(order.post_only); mix(order.price); mix(order.qty);
        mix(order.leaves_qty); mix(order.cum_qty);
    }
    for (InstrumentId id = 1; id <= engine.instrument_count(); ++id) {
        mix(engine.phase(id));
        mix(engine.last_trade_price(id).value_or(-1));
        for (Side side : {Side::BUY, Side::SELL}) {
            for (const LevelInfo& level : full_depth(engine, id, side)) {
                mix(level.price); mix(level.qty); mix(level.order_count);
                for (OrderId order_id : level_fifo(engine, id, side, level.price)) mix(order_id);
            }
        }
        const AuctionIndicative indicative = engine.auction_indicative(id);
        mix(indicative.crossed); mix(indicative.price); mix(indicative.paired_qty); mix(indicative.imbalance_qty);
        mix(indicative.imbalance_side);
    }
    return hash;
}

// Book invariants plus query consistency, through the public API only.
inline void check_book_invariants(const MatchingEngine& engine) {
    const std::vector<OrderInfo> orders = all_live_orders(engine);
    CHECK_EQ(orders.size(), engine.live_order_count());
    for (std::size_t i = 0; i < orders.size(); ++i) {
        const OrderInfo& order = orders[i];
        if (i > 0) CHECK_GT(order.order_id, orders[i - 1].order_id);
        CHECK_GT(order.leaves_qty, 0u);
        CHECK_EQ(order.qty, order.leaves_qty + order.cum_qty);
        const auto by_id = engine.find_order(order.order_id);
        const auto by_token = engine.find_order(order.session, order.cl_ord_id);
        REQUIRE(by_id.has_value());
        REQUIRE(by_token.has_value());
        CHECK_EQ(by_id->order_id, order.order_id);
        CHECK_EQ(by_token->order_id, order.order_id);
        CHECK_EQ(by_token->leaves_qty, order.leaves_qty);
    }
    std::size_t orders_on_levels = 0;
    for (InstrumentId id = 1; id <= engine.instrument_count(); ++id) {
        const Phase phase = engine.phase(id);
        const auto best_bid = engine.best_bid(id);
        const auto best_ask = engine.best_ask(id);
        if ((phase == Phase::Continuous || phase == Phase::Closed) && best_bid && best_ask) {
            CHECK_LT(*best_bid, *best_ask);
        }
        for (Side side : {Side::BUY, Side::SELL}) {
            const std::vector<LevelInfo> levels = full_depth(engine, id, side);
            const auto best = side == Side::BUY ? best_bid : best_ask;
            CHECK_EQ(levels.empty(), !best.has_value());
            if (!levels.empty()) CHECK_EQ(levels.front().price, *best);
            for (std::size_t j = 0; j < levels.size(); ++j) {
                if (j > 0) {
                    if (side == Side::BUY) CHECK_LT(levels[j].price, levels[j - 1].price);
                    else CHECK_GT(levels[j].price, levels[j - 1].price);
                }
                CHECK_GE(levels[j].order_count, 1u);
                const std::vector<OrderId> fifo = level_fifo(engine, id, side, levels[j].price);
                CHECK_EQ(fifo.size(), std::size_t{levels[j].order_count});
                std::uint64_t leaves_sum = 0;
                for (OrderId order_id : fifo) {
                    const auto order = engine.find_order(order_id);
                    REQUIRE(order.has_value());
                    CHECK_EQ(order->instrument, id);
                    CHECK_EQ(order->side, side);
                    CHECK_EQ(order->price, levels[j].price);
                    leaves_sum += order->leaves_qty;
                }
                CHECK_EQ(leaves_sum, levels[j].qty);
                orders_on_levels += fifo.size();
            }
        }
    }
    CHECK_EQ(orders_on_levels, engine.live_order_count());

    // An uncrossed auction indication reports the imbalance of every live
    // order's leaves, whatever phase the instrument is in.
    for (InstrumentId id = 1; id <= engine.instrument_count(); ++id) {
        const AuctionIndicative indicative = engine.auction_indicative(id);
        if (indicative.crossed) continue;
        std::uint64_t buy_leaves = 0;
        std::uint64_t sell_leaves = 0;
        for (const OrderInfo& order : orders) {
            if (order.instrument == id) (order.side == Side::BUY ? buy_leaves : sell_leaves) += order.leaves_qty;
        }
        CHECK_EQ(indicative.imbalance_qty,
                 buy_leaves >= sell_leaves ? buy_leaves - sell_leaves : sell_leaves - buy_leaves);
        CHECK_EQ(indicative.imbalance_side, buy_leaves > sell_leaves   ? Aggressor::Buy
                                            : sell_leaves > buy_leaves ? Aggressor::Sell
                                                                       : Aggressor::None);
    }
}

}  // namespace engine_fixture

#endif
