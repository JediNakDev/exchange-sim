// SPEC 4.2 - 4.6: acceptance, resting, continuous matching, market/IOC/FOK,
// self-trade prevention and disposition.

#include "engine_harness.hpp"

using namespace engine_harness;

TEST_CASE(engine_order_types, accepted_and_rested_fields) {
    Harness h;
    NewOrder cmd = h.limit(3, 42, Side::SELL, 123'400, 77);
    cmd.owner = 9;
    cmd.tif = TimeInForce::GTC;
    cmd.stp = StpMode::CancelBoth;
    cmd.post_only = true;
    h.send(cmd);
    REQUIRE_EQ(h.event_count(), std::size_t{2});

    const Event& accepted = h.event(0);
    REQUIRE(accepted.type == EventType::Accepted);
    CHECK_EQ(accepted.instrument, h.instrument());
    CHECK_EQ(accepted.ts, h.last_ts());
    CHECK_EQ(accepted.accepted.order_id, OrderId{1});
    CHECK_EQ(accepted.accepted.cl_ord_id, ClOrdId{42});
    CHECK_EQ(accepted.accepted.session, SessionId{3});
    CHECK_EQ(accepted.accepted.owner, OwnerId{9});
    CHECK_EQ(accepted.accepted.side, Side::SELL);
    CHECK_EQ(accepted.accepted.ord_type, OrdType::Limit);
    CHECK_EQ(accepted.accepted.tif, TimeInForce::GTC);
    CHECK_EQ(accepted.accepted.stp, StpMode::CancelBoth);
    CHECK_EQ(accepted.accepted.post_only, true);
    CHECK_EQ(accepted.accepted.price, Price{123'400});
    CHECK_EQ(accepted.accepted.qty, Quantity{77});

    const Event& rested = h.event(1);
    REQUIRE(rested.type == EventType::Rested);
    CHECK_EQ(rested.instrument, h.instrument());
    CHECK_EQ(rested.ts, h.last_ts());
    CHECK_EQ(rested.rested.order_id, OrderId{1});
    CHECK_EQ(rested.rested.cl_ord_id, ClOrdId{42});
    CHECK_EQ(rested.rested.session, SessionId{3});
    CHECK_EQ(rested.rested.side, Side::SELL);
    CHECK_EQ(rested.rested.price, Price{123'400});
    CHECK_EQ(rested.rested.leaves_qty, Quantity{77});

    CHECK_EQ(h.engine().best_ask(h.instrument()), std::optional<Price>(123'400));
    CHECK(!h.engine().best_bid(h.instrument()).has_value());
    CHECK_EQ(h.engine().live_order_count(), std::size_t{1});
}

TEST_CASE(engine_order_types, full_fill_event_fields) {
    Harness h;
    NewOrder resting = h.limit(1, 10, Side::SELL, 1000, 50);
    resting.owner = 11;
    h.send(resting);
    NewOrder aggressor = h.limit(2, 20, Side::BUY, 1100, 50);
    aggressor.owner = 22;
    h.send(aggressor);

    REQUIRE_EQ(h.event_count(), std::size_t{2});
    CHECK_EQ(h.event(0).type, EventType::Accepted);
    const Event& fill = h.event(1);
    check_fill(fill, 1000, 50, Aggressor::Buy, 2, 0, 50, 1, 0, 50);
    CHECK_EQ(fill.instrument, h.instrument());
    CHECK_EQ(fill.ts, h.last_ts());
    CHECK_EQ(fill.fill.match_id, MatchId{1});
    CHECK_EQ(fill.fill.buy.cl_ord_id, ClOrdId{20});
    CHECK_EQ(fill.fill.buy.session, SessionId{2});
    CHECK_EQ(fill.fill.buy.owner, OwnerId{22});
    CHECK_EQ(fill.fill.sell.cl_ord_id, ClOrdId{10});
    CHECK_EQ(fill.fill.sell.session, SessionId{1});
    CHECK_EQ(fill.fill.sell.owner, OwnerId{11});

    CHECK_EQ(h.engine().live_order_count(), std::size_t{0});
    CHECK(!h.engine().best_ask(h.instrument()).has_value());
    CHECK_EQ(h.engine().last_trade_price(h.instrument()), std::optional<Price>(1000));
    CHECK(!h.engine().find_order(1, 10).has_value());
    CHECK(!h.engine().find_order(OrderId{1}).has_value());
}

TEST_CASE(engine_order_types, sell_aggressor_trades_at_resting_price) {
    Harness h;
    const OrderId bid = h.rest(1, 1, Side::BUY, 1500, 30);
    h.send(h.limit(2, 1, Side::SELL, 1000, 10));
    REQUIRE_EQ(h.event_count(), std::size_t{2});
    check_fill(h.event(1), 1500, 10, Aggressor::Sell, bid, 20, 10, 2, 0, 10);
    const auto info = h.engine().find_order(bid);
    REQUIRE(info.has_value());
    CHECK_EQ(info->leaves_qty, Quantity{20});
    CHECK_EQ(info->cum_qty, Quantity{10});
    CHECK_EQ(h.levels(Side::BUY)[0].qty, std::uint64_t{20});
}

TEST_CASE(engine_order_types, price_time_priority) {
    Harness h;
    const OrderId late_better = h.rest(1, 1, Side::SELL, 1100, 10);
    const OrderId first_at_1000 = h.rest(1, 2, Side::SELL, 1000, 10);
    const OrderId second_at_1000 = h.rest(1, 3, Side::SELL, 1000, 10);
    const OrderId worse = h.rest(1, 4, Side::SELL, 1200, 10);
    (void)late_better;
    (void)worse;

    h.send(h.limit(2, 1, Side::BUY, 1000, 15));
    REQUIRE_EQ(h.event_count(), std::size_t{3});
    check_fill(h.event(1), 1000, 10, Aggressor::Buy, 5, 5, 10, first_at_1000, 0, 10);
    check_fill(h.event(2), 1000, 5, Aggressor::Buy, 5, 0, 15, second_at_1000, 5, 5);
    CHECK_EQ(h.event(1).fill.match_id, MatchId{1});
    CHECK_EQ(h.event(2).fill.match_id, MatchId{2});
    CHECK_EQ(h.fifo(Side::SELL, 1000), (std::vector<OrderId>{second_at_1000}));
    CHECK_EQ(h.engine().best_ask(h.instrument()), std::optional<Price>(1000));
}

TEST_CASE(engine_order_types, partial_fill_then_rest_remainder) {
    Harness h;
    const OrderId ask = h.rest(1, 1, Side::SELL, 1000, 30);
    h.send(h.limit(2, 1, Side::BUY, 1000, 100));
    REQUIRE_EQ(event_types(h.events()),
               (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Rested}));
    check_fill(h.event(1), 1000, 30, Aggressor::Buy, 2, 70, 30, ask, 0, 30);
    CHECK_EQ(h.event(2).rested.order_id, OrderId{2});
    CHECK_EQ(h.event(2).rested.price, Price{1000});
    CHECK_EQ(h.event(2).rested.leaves_qty, Quantity{70});
    CHECK_EQ(h.engine().best_bid(h.instrument()), std::optional<Price>(1000));
    CHECK(!h.engine().best_ask(h.instrument()).has_value());
    const auto info = h.engine().find_order(2, 1);
    REQUIRE(info.has_value());
    CHECK_EQ(info->qty, Quantity{100});
    CHECK_EQ(info->leaves_qty, Quantity{70});
    CHECK_EQ(info->cum_qty, Quantity{30});
}

TEST_CASE(engine_order_types, multi_level_sweep_stops_at_limit) {
    Harness h;
    const OrderId a1 = h.rest(1, 1, Side::SELL, 1000, 10);
    const OrderId a2 = h.rest(1, 2, Side::SELL, 1100, 10);
    const OrderId a3 = h.rest(1, 3, Side::SELL, 1200, 10);
    const OrderId a4 = h.rest(1, 4, Side::SELL, 1300, 10);
    h.send(h.limit(2, 1, Side::BUY, 1200, 35));
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill,
                                                                EventType::Fill, EventType::Fill, EventType::Rested}));
    check_fill(h.event(1), 1000, 10, Aggressor::Buy, 5, 25, 10, a1, 0, 10);
    check_fill(h.event(2), 1100, 10, Aggressor::Buy, 5, 15, 20, a2, 0, 10);
    check_fill(h.event(3), 1200, 10, Aggressor::Buy, 5, 5, 30, a3, 0, 10);
    CHECK_EQ(h.event(4).rested.leaves_qty, Quantity{5});
    CHECK_EQ(h.event(4).rested.price, Price{1200});
    CHECK_EQ(h.engine().best_ask(h.instrument()), std::optional<Price>(1300));
    CHECK_EQ(h.engine().best_bid(h.instrument()), std::optional<Price>(1200));
    CHECK_EQ(h.engine().last_trade_price(h.instrument()), std::optional<Price>(1200));
    CHECK_EQ(h.fifo(Side::SELL, 1300), (std::vector<OrderId>{a4}));
}

TEST_CASE(engine_order_types, market_order_sweeps_and_cancels_remainder) {
    Harness h;
    h.rest(1, 1, Side::BUY, 1000, 10);
    h.rest(1, 2, Side::BUY, 100, 10);  // far away; market orders ignore price
    NewOrder market = h.market(2, 9, Side::SELL, 25);
    market.price = 777;
    market.tif = TimeInForce::GTC;  // TIF is ignored: a market order never rests
    h.send(market);
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill,
                                                                EventType::Fill, EventType::Canceled}));
    check_fill(h.event(1), 1000, 10, Aggressor::Sell, 1, 0, 10, 3, 15, 10);
    check_fill(h.event(2), 100, 10, Aggressor::Sell, 2, 0, 10, 3, 5, 20);
    check_canceled(h.event(3), 3, 9, 0, Side::SELL, 777, 5, 20, CancelReason::ImmediateOrCancel, false);
    CHECK_EQ(h.event(3).canceled.session, SessionId{2});
    CHECK_EQ(h.engine().live_order_count(), std::size_t{0});
    CHECK(!h.engine().find_order(2, 9).has_value());
}

TEST_CASE(engine_order_types, market_order_on_empty_book) {
    Harness h;
    h.send(h.market(1, 1, Side::BUY, 10));
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    check_canceled(h.event(1), 1, 1, 0, Side::BUY, 0, 10, 0, CancelReason::ImmediateOrCancel, false);
}

TEST_CASE(engine_order_types, ioc_partial_and_no_cross) {
    Harness h;
    const OrderId ask = h.rest(1, 1, Side::SELL, 1000, 10);
    NewOrder ioc = h.limit(2, 1, Side::BUY, 1000, 25);
    ioc.tif = TimeInForce::IOC;
    h.send(ioc);
    REQUIRE_EQ(event_types(h.events()),
               (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Canceled}));
    check_fill(h.event(1), 1000, 10, Aggressor::Buy, 2, 15, 10, ask, 0, 10);
    check_canceled(h.event(2), 2, 1, 0, Side::BUY, 1000, 15, 10, CancelReason::ImmediateOrCancel, false);

    h.rest(1, 2, Side::SELL, 2000, 10);
    NewOrder passive_ioc = h.limit(2, 2, Side::BUY, 1500, 25);
    passive_ioc.tif = TimeInForce::IOC;
    h.send(passive_ioc);
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    check_canceled(h.event(1), 4, 2, 0, Side::BUY, 1500, 25, 0, CancelReason::ImmediateOrCancel, false);
    CHECK(!h.engine().best_bid(h.instrument()).has_value());
}

TEST_CASE(engine_order_types, ioc_fully_filled_emits_no_cancel) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10);
    NewOrder ioc = h.limit(2, 1, Side::BUY, 1000, 10);
    ioc.tif = TimeInForce::IOC;
    h.send(ioc);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));
}

TEST_CASE(engine_order_types, fok_insufficient_liquidity_kills_without_fills) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10);
    h.rest(1, 2, Side::SELL, 1100, 10);
    h.rest(1, 3, Side::SELL, 1300, 100);  // beyond the limit: not counted
    NewOrder fok = h.limit(2, 1, Side::BUY, 1200, 21);
    fok.tif = TimeInForce::FOK;
    h.send(fok);
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    check_canceled(h.event(1), 4, 1, 0, Side::BUY, 1200, 21, 0, CancelReason::FillOrKill, false);
    CHECK_EQ(h.engine().live_order_count(), std::size_t{3});
    CHECK(!h.engine().last_trade_price(h.instrument()).has_value());
}

TEST_CASE(engine_order_types, fok_exact_liquidity_fills_completely) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10);
    h.rest(1, 2, Side::SELL, 1100, 10);
    NewOrder fok = h.limit(2, 1, Side::BUY, 1100, 20);
    fok.tif = TimeInForce::FOK;
    h.send(fok);
    CHECK_EQ(event_types(h.events()),
             (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Fill}));
    CHECK_EQ(h.engine().live_order_count(), std::size_t{0});
}

TEST_CASE(engine_order_types, fok_market_order_uses_the_whole_opposite_side) {
    Harness h;
    h.rest(1, 1, Side::BUY, 1000, 10);
    h.rest(1, 2, Side::BUY, 100, 10);
    NewOrder fok_market = h.market(2, 1, Side::SELL, 21);
    fok_market.tif = TimeInForce::FOK;
    h.send(fok_market);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    CHECK_EQ(h.event(1).canceled.reason, CancelReason::FillOrKill);

    fok_market.cl_ord_id = 2;
    fok_market.qty = 20;
    h.send(fok_market);
    CHECK_EQ(event_types(h.events()),
             (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Fill}));
}

// SPEC 4.3: CancelResting skips self-matches while counting.
TEST_CASE(engine_order_types, fok_cancel_resting_skips_own_liquidity) {
    Harness h;
    const OrderId own = h.rest(1, 1, Side::SELL, 1000, 50, /*owner=*/7);
    const OrderId other = h.rest(2, 1, Side::SELL, 1000, 10, /*owner=*/8);
    NewOrder fok = h.limit(3, 1, Side::BUY, 1000, 11);
    fok.owner = 7;
    fok.stp = StpMode::CancelResting;
    fok.tif = TimeInForce::FOK;
    h.send(fok);  // only 10 non-self shares available
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    CHECK_EQ(h.event(1).canceled.reason, CancelReason::FillOrKill);
    CHECK_EQ(h.engine().live_order_count(), std::size_t{2});

    fok.cl_ord_id = 2;
    fok.qty = 10;
    h.send(fok);  // passes; matching cancels the own resting order, then fills
    REQUIRE_EQ(event_types(h.events()),
               (std::vector<EventType>{EventType::Accepted, EventType::Canceled, EventType::Fill}));
    check_canceled(h.event(1), own, 1, 0, Side::SELL, 1000, 50, 0, CancelReason::SelfTradePrevention, true);
    check_fill(h.event(2), 1000, 10, Aggressor::Buy, 4, 0, 10, other, 0, 10);
    CHECK_EQ(h.engine().live_order_count(), std::size_t{0});
}

// SPEC 4.3: any other STP mode stops the walk at the first self-match.
TEST_CASE(engine_order_types, fok_cancel_aggressor_stops_at_self_match) {
    for (StpMode mode : {StpMode::CancelAggressor, StpMode::CancelBoth}) {
        Harness h;
        h.rest(2, 1, Side::SELL, 1000, 10, /*owner=*/8);
        h.rest(1, 1, Side::SELL, 1000, 10, /*owner=*/7);
        h.rest(2, 2, Side::SELL, 1000, 100, /*owner=*/8);  // behind the self-match: not counted
        NewOrder fok = h.limit(3, 1, Side::BUY, 1000, 11);
        fok.owner = 7;
        fok.stp = mode;
        fok.tif = TimeInForce::FOK;
        h.send(fok);
        CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
        CHECK_EQ(h.event(1).canceled.reason, CancelReason::FillOrKill);
        CHECK_EQ(h.event(1).canceled.canceled_qty, Quantity{11});

        // Enough liquidity before the self-match: fills without touching it.
        fok.cl_ord_id = 2;
        fok.qty = 10;
        h.send(fok);
        CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));
        CHECK_EQ(h.engine().live_order_count(), std::size_t{2});
    }
}

TEST_CASE(engine_order_types, fok_without_self_match_conditions_counts_everything) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10, /*owner=*/7);
    h.rest(2, 1, Side::SELL, 1000, 10, /*owner=*/8);
    // stp None: same owner is not a self-match.
    NewOrder fok = h.limit(3, 1, Side::BUY, 1000, 20);
    fok.owner = 7;
    fok.tif = TimeInForce::FOK;
    h.send(fok);
    CHECK_EQ(event_types(h.events()),
             (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Fill}));

    // owner 0 never self-matches even with an STP mode.
    h.rest(1, 2, Side::SELL, 1000, 10, /*owner=*/0);
    NewOrder owner_zero = h.limit(3, 2, Side::BUY, 1000, 10);
    owner_zero.stp = StpMode::CancelAggressor;
    owner_zero.tif = TimeInForce::FOK;
    h.send(owner_zero);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));
}

TEST_CASE(engine_order_types, stp_cancel_resting_continues_matching) {
    Harness h;
    const OrderId other_first = h.rest(2, 1, Side::SELL, 1000, 5, 8);
    const OrderId own = h.rest(1, 1, Side::SELL, 1000, 10, 7);
    const OrderId other_second = h.rest(2, 2, Side::SELL, 1100, 10, 8);
    NewOrder aggressor = h.limit(1, 2, Side::BUY, 1100, 12);
    aggressor.owner = 7;
    aggressor.stp = StpMode::CancelResting;
    h.send(aggressor);
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill,
                                                                EventType::Canceled, EventType::Fill}));
    check_fill(h.event(1), 1000, 5, Aggressor::Buy, 4, 7, 5, other_first, 0, 5);
    check_canceled(h.event(2), own, 1, 0, Side::SELL, 1000, 10, 0, CancelReason::SelfTradePrevention, true);
    CHECK_EQ(h.event(2).canceled.session, SessionId{1});
    check_fill(h.event(3), 1100, 7, Aggressor::Buy, 4, 0, 12, other_second, 3, 7);
    CHECK(!h.engine().find_order(1, 1).has_value());
    CHECK_EQ(h.engine().live_order_count(), std::size_t{1});
}

TEST_CASE(engine_order_types, stp_cancel_aggressor_keeps_resting) {
    Harness h;
    const OrderId other = h.rest(2, 1, Side::SELL, 1000, 5, 8);
    const OrderId own = h.rest(1, 1, Side::SELL, 1000, 10, 7);
    NewOrder aggressor = h.limit(1, 2, Side::BUY, 1100, 12);
    aggressor.owner = 7;
    aggressor.stp = StpMode::CancelAggressor;
    aggressor.tif = TimeInForce::GTC;  // would rest, but STP cancels it
    h.send(aggressor);
    REQUIRE_EQ(event_types(h.events()),
               (std::vector<EventType>{EventType::Accepted, EventType::Fill, EventType::Canceled}));
    check_fill(h.event(1), 1000, 5, Aggressor::Buy, 3, 7, 5, other, 0, 5);
    check_canceled(h.event(2), 3, 2, 0, Side::BUY, 1100, 7, 5, CancelReason::SelfTradePrevention, false);
    CHECK_EQ(h.fifo(Side::SELL, 1000), (std::vector<OrderId>{own}));
    CHECK(!h.engine().best_bid(h.instrument()).has_value());
}

TEST_CASE(engine_order_types, stp_cancel_both_cancels_resting_then_aggressor) {
    Harness h;
    const OrderId own = h.rest(1, 1, Side::BUY, 1000, 10, 7);
    const OrderId behind = h.rest(2, 1, Side::BUY, 1000, 10, 8);
    NewOrder aggressor = h.limit(1, 2, Side::SELL, 1000, 4);
    aggressor.owner = 7;
    aggressor.stp = StpMode::CancelBoth;
    h.send(aggressor);
    REQUIRE_EQ(event_types(h.events()),
               (std::vector<EventType>{EventType::Accepted, EventType::Canceled, EventType::Canceled}));
    check_canceled(h.event(1), own, 1, 0, Side::BUY, 1000, 10, 0, CancelReason::SelfTradePrevention, true);
    check_canceled(h.event(2), 3, 2, 0, Side::SELL, 1000, 4, 0, CancelReason::SelfTradePrevention, false);
    CHECK_EQ(h.fifo(Side::BUY, 1000), (std::vector<OrderId>{behind}));
    CHECK_EQ(h.levels(Side::BUY)[0].qty, std::uint64_t{10});
}

TEST_CASE(engine_order_types, stp_uses_aggressor_mode_and_owner_only) {
    Harness h;
    // Resting order has an STP mode; the aggressor has none: they trade.
    NewOrder resting = h.limit(1, 1, Side::SELL, 1000, 10);
    resting.owner = 7;
    resting.stp = StpMode::CancelBoth;
    h.send(resting);
    NewOrder aggressor = h.limit(1, 2, Side::BUY, 1000, 10);
    aggressor.owner = 7;
    h.send(aggressor);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));

    // Different owners never self-match; same session does not matter.
    h.rest(1, 3, Side::SELL, 1000, 10, 8);
    NewOrder different_owner = h.limit(1, 4, Side::BUY, 1000, 10);
    different_owner.owner = 7;
    different_owner.stp = StpMode::CancelAggressor;
    h.send(different_owner);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));

    // Owner 0 never self-matches.
    h.rest(2, 1, Side::SELL, 1000, 10, 0);
    NewOrder owner_zero = h.limit(3, 1, Side::BUY, 1000, 10);
    owner_zero.stp = StpMode::CancelAggressor;
    h.send(owner_zero);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Fill}));
}

TEST_CASE(engine_order_types, stp_cancel_aggressor_on_market_order) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10, 7);
    NewOrder market = h.market(2, 1, Side::BUY, 10);
    market.owner = 7;
    market.stp = StpMode::CancelAggressor;
    h.send(market);
    REQUIRE_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled}));
    CHECK_EQ(h.event(1).canceled.reason, CancelReason::SelfTradePrevention);
    CHECK_EQ(h.event(1).canceled.was_resting, false);
}

TEST_CASE(engine_order_types, stp_cancel_resting_can_empty_the_book_and_rest) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10, 7);
    h.rest(1, 2, Side::SELL, 1100, 10, 7);
    NewOrder aggressor = h.limit(2, 1, Side::BUY, 1200, 5);
    aggressor.owner = 7;
    aggressor.stp = StpMode::CancelResting;
    h.send(aggressor);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Canceled,
                                                              EventType::Canceled, EventType::Rested}));
    CHECK(!h.engine().best_ask(h.instrument()).has_value());
    CHECK_EQ(h.engine().best_bid(h.instrument()), std::optional<Price>(1200));
}

TEST_CASE(engine_order_types, match_ids_are_engine_global_across_instruments) {
    MatchingEngine engine(small_engine_config());
    const InstrumentId a = engine.add_instrument(standard_instrument_config("AAA"));
    const InstrumentId b = engine.add_instrument(standard_instrument_config("BBB"));
    EventBuffer out;
    const auto send = [&](InstrumentId instrument, ClOrdId token, Side side) {
        NewOrder cmd;
        cmd.instrument = instrument;
        cmd.session = 1;
        cmd.cl_ord_id = token;
        cmd.side = side;
        cmd.price = 1000;
        cmd.qty = 1;
        out.clear();
        REQUIRE(engine.handle(cmd, out) == HandleResult::Applied);
    };
    send(a, 1, Side::BUY);
    send(b, 2, Side::BUY);
    send(b, 3, Side::SELL);
    REQUIRE_EQ(out.size(), std::size_t{2});
    CHECK_EQ(out[1].fill.match_id, MatchId{1});
    CHECK_EQ(out[1].instrument, b);
    CHECK_EQ(out[0].accepted.order_id, OrderId{3});
    send(a, 4, Side::SELL);
    CHECK_EQ(out[1].fill.match_id, MatchId{2});
    CHECK_EQ(out[1].instrument, a);
}
