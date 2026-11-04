// SPEC 4.2 acceptance, 4.3 FOK pre-check, 4.4 resting, 4.5 continuous
// matching with STP, 4.6 disposition and the NewOrder rows of SPEC 10.

#include <tests/exsim/engine/arena_test_support.hpp>

using namespace arena_test;

namespace {

void check_fill_leg(const FillLeg& leg, OrderId order_id, ClOrdId token, SessionId session, OwnerId owner,
                    Quantity leaves_qty, Quantity cum_qty) {
    CHECK_EQ(leg.order_id, order_id);
    CHECK_EQ(leg.cl_ord_id, token);
    CHECK_EQ(leg.session, session);
    CHECK_EQ(leg.owner, owner);
    CHECK_EQ(leg.leaves_qty, leaves_qty);
    CHECK_EQ(leg.cum_qty, cum_qty);
}

}  // namespace

TEST_CASE(arena_matching, accepted_and_rested_echo_every_field) {
    ArenaEngineFixture fixture;
    NewOrder command = with_owner(limit_order(kAliceSession, 42, Side::SELL, 130, 25), kAliceFirm,
                                  StpMode::CancelBoth);
    command.tif = TimeInForce::GTC;
    command.ts = 987654321;
    fixture.apply(command);
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    const Event& accepted = fixture.events[0];
    REQUIRE_EQ(accepted.type, EventType::Accepted);
    CHECK_EQ(accepted.instrument, InstrumentId{1});
    CHECK_EQ(accepted.ts, Timestamp{987654321});
    CHECK_EQ(accepted.accepted.order_id, OrderId{1});
    CHECK_EQ(accepted.accepted.cl_ord_id, ClOrdId{42});
    CHECK_EQ(accepted.accepted.session, kAliceSession);
    CHECK_EQ(accepted.accepted.owner, kAliceFirm);
    CHECK_EQ(accepted.accepted.side, Side::SELL);
    CHECK_EQ(accepted.accepted.ord_type, OrdType::Limit);
    CHECK_EQ(accepted.accepted.tif, TimeInForce::GTC);
    CHECK_EQ(accepted.accepted.stp, StpMode::CancelBoth);
    CHECK_EQ(accepted.accepted.post_only, false);
    CHECK_EQ(accepted.accepted.price, Price{130});
    CHECK_EQ(accepted.accepted.qty, Quantity{25});

    const Event& rested = fixture.events[1];
    REQUIRE_EQ(rested.type, EventType::Rested);
    CHECK_EQ(rested.ts, Timestamp{987654321});
    CHECK_EQ(rested.rested.order_id, OrderId{1});
    CHECK_EQ(rested.rested.cl_ord_id, ClOrdId{42});
    CHECK_EQ(rested.rested.session, kAliceSession);
    CHECK_EQ(rested.rested.side, Side::SELL);
    CHECK_EQ(rested.rested.price, Price{130});
    CHECK_EQ(rested.rested.leaves_qty, Quantity{25});

    const auto stored = fixture.engine.find_order(kAliceSession, 42);
    REQUIRE(stored.has_value());
    CHECK_EQ(stored->order_id, OrderId{1});
    CHECK_EQ(stored->instrument, InstrumentId{1});
    CHECK_EQ(stored->owner, kAliceFirm);
    CHECK_EQ(stored->tif, TimeInForce::GTC);
    CHECK_EQ(stored->stp, StpMode::CancelBoth);
    CHECK_EQ(stored->qty, Quantity{25});
    CHECK_EQ(stored->leaves_qty, Quantity{25});
    CHECK_EQ(stored->cum_qty, Quantity{0});
}

TEST_CASE(arena_matching, buy_sweep_uses_price_then_time_priority) {
    ArenaEngineFixture fixture;
    fixture.apply(with_owner(limit_order(kBobSession, 1, Side::SELL, 120, 10), kBobFirm));   // id 1
    fixture.apply(with_owner(limit_order(kBobSession, 2, Side::SELL, 110, 4), kBobFirm));    // id 2
    fixture.apply(with_owner(limit_order(kCarolSession, 3, Side::SELL, 110, 6), 0));         // id 3
    fixture.apply(with_owner(limit_order(kBobSession, 4, Side::SELL, 140, 5), kBobFirm));    // id 4

    NewOrder taker = with_owner(limit_order(kAliceSession, 9, Side::BUY, 130, 25), kAliceFirm);
    taker.ts = 5000;
    fixture.apply(taker);
    REQUIRE_EQ(fixture.events.size(), std::size_t{5});
    CHECK_EQ(fixture.events[0].accepted.order_id, OrderId{5});

    const Event& first = fixture.events[1];
    REQUIRE_EQ(first.type, EventType::Fill);
    CHECK_EQ(first.ts, Timestamp{5000});
    CHECK_EQ(first.fill.match_id, MatchId{1});
    CHECK_EQ(first.fill.price, Price{110});
    CHECK_EQ(first.fill.qty, Quantity{4});
    CHECK_EQ(first.fill.aggressor, Aggressor::Buy);
    check_fill_leg(first.fill.buy, 5, 9, kAliceSession, kAliceFirm, 21, 4);
    check_fill_leg(first.fill.sell, 2, 2, kBobSession, kBobFirm, 0, 4);

    const Event& second = fixture.events[2];
    CHECK_EQ(second.fill.match_id, MatchId{2});
    CHECK_EQ(second.fill.price, Price{110});
    check_fill_leg(second.fill.buy, 5, 9, kAliceSession, kAliceFirm, 15, 10);
    check_fill_leg(second.fill.sell, 3, 3, kCarolSession, 0, 0, 6);

    const Event& third = fixture.events[3];
    CHECK_EQ(third.fill.match_id, MatchId{3});
    CHECK_EQ(third.fill.price, Price{120});
    CHECK_EQ(third.fill.qty, Quantity{10});
    check_fill_leg(third.fill.buy, 5, 9, kAliceSession, kAliceFirm, 5, 20);
    check_fill_leg(third.fill.sell, 1, 1, kBobSession, kBobFirm, 0, 10);

    // 140 does not cross a 130 limit: the remainder rests.
    const Event& rested = fixture.events[4];
    REQUIRE_EQ(rested.type, EventType::Rested);
    CHECK_EQ(rested.rested.order_id, OrderId{5});
    CHECK_EQ(rested.rested.price, Price{130});
    CHECK_EQ(rested.rested.leaves_qty, Quantity{5});

    CHECK_EQ(*fixture.engine.last_trade_price(1), Price{120});
    CHECK_EQ(*fixture.engine.best_bid(1), Price{130});
    CHECK_EQ(*fixture.engine.best_ask(1), Price{140});
    const auto stored = fixture.engine.find_order(OrderId{5});
    REQUIRE(stored.has_value());
    CHECK_EQ(stored->qty, Quantity{25});
    CHECK_EQ(stored->cum_qty, Quantity{20});
    CHECK_EQ(stored->leaves_qty, Quantity{5});
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_matching, sell_aggressor_hits_highest_bid_first) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 90, 5));
    fixture.apply(limit_order(kBobSession, 2, Side::BUY, 100, 5));
    fixture.apply(limit_order(kBobSession, 3, Side::BUY, 100, 5));
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 90, 12));
    REQUIRE_EQ(fixture.events.size(), std::size_t{4});
    CHECK_EQ(fixture.events[1].fill.sell.order_id, OrderId{4});
    CHECK_EQ(fixture.events[1].fill.buy.order_id, OrderId{2});
    CHECK_EQ(fixture.events[1].fill.aggressor, Aggressor::Sell);
    CHECK_EQ(fixture.events[2].fill.buy.order_id, OrderId{3});
    CHECK_EQ(fixture.events[3].fill.buy.order_id, OrderId{1});
    CHECK_EQ(fixture.events[3].fill.price, Price{90});
    CHECK_EQ(fixture.events[3].fill.qty, Quantity{2});
    CHECK_EQ(fixture.events[3].fill.buy.leaves_qty, Quantity{3});
    CHECK_EQ(fixture.events[3].fill.sell.leaves_qty, Quantity{0});
    CHECK_EQ(fixture.events[3].fill.sell.cum_qty, Quantity{12});
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 90), std::vector<OrderId>{1});
    CHECK_EQ(full_depth(fixture.engine, 1, Side::BUY).front().qty, std::uint64_t{3});
}

TEST_CASE(arena_matching, partially_filled_resting_order_keeps_the_head) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 10));
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 100, 10));
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 4));
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::SELL, 100), (std::vector<OrderId>{1, 2}));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 100, 7));
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[1].fill.sell.order_id, OrderId{1});
    CHECK_EQ(fixture.events[1].fill.qty, Quantity{6});
    CHECK_EQ(fixture.events[2].fill.sell.order_id, OrderId{2});
    CHECK_EQ(fixture.events[2].fill.qty, Quantity{1});
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::SELL, 100), std::vector<OrderId>{2});
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_matching, ioc_remainder_is_canceled) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 3));
    NewOrder ioc = with_tif(limit_order(kAliceSession, 7, Side::BUY, 120, 10), TimeInForce::IOC);
    fixture.apply(ioc);
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[1].type, EventType::Fill);
    const Event& canceled = fixture.events[2];
    REQUIRE_EQ(canceled.type, EventType::Canceled);
    CHECK_EQ(canceled.ts, ioc.ts);
    CHECK_EQ(canceled.canceled.order_id, OrderId{2});
    CHECK_EQ(canceled.canceled.cl_ord_id, ClOrdId{7});
    CHECK_EQ(canceled.canceled.request_cl_ord_id, ClOrdId{0});
    CHECK_EQ(canceled.canceled.session, kAliceSession);
    CHECK_EQ(canceled.canceled.side, Side::BUY);
    CHECK_EQ(canceled.canceled.price, Price{120});
    CHECK_EQ(canceled.canceled.canceled_qty, Quantity{7});
    CHECK_EQ(canceled.canceled.cum_qty, Quantity{3});
    CHECK_EQ(canceled.canceled.reason, CancelReason::ImmediateOrCancel);
    CHECK_EQ(canceled.canceled.was_resting, false);
    CHECK_EQ(canceled.canceled.leaves_qty, Quantity{0});
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});

    // An IOC with nothing to match: Accepted then Canceled for the full qty.
    fixture.apply(with_tif(limit_order(kAliceSession, 8, Side::SELL, 200, 4), TimeInForce::IOC));
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[1].canceled.canceled_qty, Quantity{4});
    CHECK_EQ(fixture.events[1].canceled.cum_qty, Quantity{0});
}

TEST_CASE(arena_matching, market_orders_never_rest_and_echo_their_price) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 50, 3));
    fixture.apply(limit_order(kBobSession, 2, Side::BUY, 200, 2));
    for (TimeInForce tif : {TimeInForce::Day, TimeInForce::GTC}) {
        NewOrder market = with_tif(market_order(kAliceSession, 1, Side::SELL, 4, -777), tif);
        fixture.apply(market);
        if (tif == TimeInForce::Day) {
            REQUIRE_EQ(fixture.events.size(), std::size_t{3});
            CHECK_EQ(fixture.events[1].fill.price, Price{200});
            CHECK_EQ(fixture.events[2].fill.price, Price{50});
            CHECK_EQ(fixture.events[2].fill.qty, Quantity{2});
        } else {
            // Book now has one share at 50; the remaining 3 are canceled.
            REQUIRE_EQ(fixture.events.size(), std::size_t{3});
            CHECK_EQ(fixture.events[1].fill.qty, Quantity{1});
            CHECK_EQ(fixture.events[2].canceled.reason, CancelReason::ImmediateOrCancel);
            CHECK_EQ(fixture.events[2].canceled.price, Price{-777});
            CHECK_EQ(fixture.events[2].canceled.canceled_qty, Quantity{3});
            CHECK_EQ(fixture.events[2].canceled.cum_qty, Quantity{1});
        }
        CHECK(!fixture.engine.find_order(kAliceSession, 1).has_value());
    }
    // Market into an empty side.
    fixture.apply(market_order(kAliceSession, 2, Side::BUY, 9, 12345));
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[1].canceled.canceled_qty, Quantity{9});
    CHECK_EQ(fixture.events[1].canceled.price, Price{12345});
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
}

TEST_CASE(arena_matching, fok_fills_completely_or_is_killed_without_touching_the_book) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 4));
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 110, 4));
    fixture.apply(limit_order(kBobSession, 3, Side::SELL, 130, 100));
    const std::uint64_t book_before = book_state_digest(fixture.engine);

    // Only 8 shares cross a 120 limit.
    NewOrder too_big = with_tif(limit_order(kAliceSession, 1, Side::BUY, 120, 9), TimeInForce::FOK);
    fixture.apply(too_big);
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[0].accepted.order_id, OrderId{4});
    const Event& killed = fixture.events[1];
    CHECK_EQ(killed.canceled.reason, CancelReason::FillOrKill);
    CHECK_EQ(killed.canceled.canceled_qty, Quantity{9});
    CHECK_EQ(killed.canceled.cum_qty, Quantity{0});
    CHECK_EQ(killed.canceled.was_resting, false);
    CHECK_EQ(killed.canceled.price, Price{120});
    CHECK_EQ(book_state_digest(fixture.engine), book_before);

    NewOrder exact = with_tif(limit_order(kAliceSession, 2, Side::BUY, 120, 8), TimeInForce::FOK);
    fixture.apply(exact);
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[0].accepted.order_id, OrderId{5});  // the killed FOK consumed id 4
    CHECK_EQ(fixture.events[1].fill.match_id, MatchId{1});      // but no match id
    CHECK_EQ(fixture.events[2].fill.buy.leaves_qty, Quantity{0});
    CHECK_EQ(*fixture.engine.best_ask(1), Price{130});

    // A market FOK also runs the pre-check (SPEC 4.2 applies to tif == FOK).
    NewOrder market_fok = with_tif(market_order(kAliceSession, 3, Side::BUY, 101), TimeInForce::FOK);
    fixture.apply(market_fok);
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[1].canceled.reason, CancelReason::FillOrKill);
    CHECK_EQ(fixture.engine.find_order(OrderId{3})->leaves_qty, Quantity{100});
}

TEST_CASE(arena_matching, fok_precheck_skips_or_stops_at_self_matches) {
    // Book: own 5 @100 (head), other 5 @100, other 5 @110.
    auto build_book = [](ArenaEngineFixture& fixture) {
        fixture.apply(with_owner(limit_order(kAliceSession, 1, Side::SELL, 100, 5), kAliceFirm));
        fixture.apply(with_owner(limit_order(kBobSession, 1, Side::SELL, 100, 5), kBobFirm));
        fixture.apply(with_owner(limit_order(kBobSession, 2, Side::SELL, 110, 5), kBobFirm));
    };
    {
        // CancelResting skips the own order: 10 other shares are available.
        ArenaEngineFixture fixture;
        build_book(fixture);
        NewOrder fok = with_tif(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 10), kAliceFirm,
                                           StpMode::CancelResting), TimeInForce::FOK);
        fixture.apply(fok);
        REQUIRE_EQ(fixture.events.size(), std::size_t{4});
        CHECK_EQ(fixture.events[1].canceled.reason, CancelReason::SelfTradePrevention);
        CHECK_EQ(fixture.events[1].canceled.order_id, OrderId{1});
        CHECK_EQ(fixture.events[1].canceled.was_resting, true);
        CHECK_EQ(fixture.events[2].fill.sell.order_id, OrderId{2});
        CHECK_EQ(fixture.events[3].fill.sell.order_id, OrderId{3});
        CHECK_EQ(fixture.events[3].fill.buy.leaves_qty, Quantity{0});
        CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
    }
    for (StpMode stopping_mode : {StpMode::CancelAggressor, StpMode::CancelBoth}) {
        // The walk stops at the own head order: nothing is available.
        ArenaEngineFixture fixture;
        build_book(fixture);
        NewOrder fok = with_tif(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 1), kAliceFirm,
                                           stopping_mode), TimeInForce::FOK);
        fixture.apply(fok);
        REQUIRE_EQ(fixture.events.size(), std::size_t{2});
        CHECK_EQ(fixture.events[1].canceled.reason, CancelReason::FillOrKill);
        CHECK_EQ(fixture.engine.live_order_count(), std::size_t{3});
    }
    {
        // Liquidity ahead of the self-match is enough: the FOK executes.
        ArenaEngineFixture fixture;
        fixture.apply(with_owner(limit_order(kBobSession, 1, Side::SELL, 100, 5), kBobFirm));
        fixture.apply(with_owner(limit_order(kAliceSession, 1, Side::SELL, 100, 5), kAliceFirm));
        NewOrder fok = with_tif(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 5), kAliceFirm,
                                           StpMode::CancelAggressor), TimeInForce::FOK);
        fixture.apply(fok);
        REQUIRE_EQ(fixture.events.size(), std::size_t{2});
        CHECK_EQ(fixture.events[1].fill.qty, Quantity{5});
    }
    {
        // Without an owner there is no self-match, whatever the STP mode.
        ArenaEngineFixture fixture;
        build_book(fixture);
        NewOrder fok = with_tif(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 15), 0,
                                           StpMode::CancelBoth), TimeInForce::FOK);
        fixture.apply(fok);
        REQUIRE_EQ(fixture.events.size(), std::size_t{4});
        CHECK_EQ(fixture.events[1].fill.sell.order_id, OrderId{1});
    }
}

TEST_CASE(arena_matching, stp_modes_follow_the_aggressor) {
    auto book_with_own_head = [](ArenaEngineFixture& fixture) {
        // Resting own order carries a different STP mode: it must be ignored.
        fixture.apply(with_owner(limit_order(kBobSession, 1, Side::SELL, 100, 3), kBobFirm));
        fixture.apply(with_owner(limit_order(kAliceSession, 1, Side::SELL, 100, 4), kAliceFirm, StpMode::None));
        fixture.apply(with_owner(limit_order(kBobSession, 2, Side::SELL, 110, 5), kBobFirm));
    };
    {
        ArenaEngineFixture fixture;
        book_with_own_head(fixture);
        fixture.apply(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 6), kAliceFirm,
                                 StpMode::CancelResting));
        REQUIRE_EQ(fixture.events.size(), std::size_t{4});
        CHECK_EQ(fixture.events[1].fill.qty, Quantity{3});
        const Event& stp = fixture.events[2];
        CHECK_EQ(stp.canceled.order_id, OrderId{2});
        CHECK_EQ(stp.canceled.reason, CancelReason::SelfTradePrevention);
        CHECK_EQ(stp.canceled.canceled_qty, Quantity{4});
        CHECK_EQ(stp.canceled.was_resting, true);
        CHECK_EQ(stp.canceled.request_cl_ord_id, ClOrdId{0});
        CHECK_EQ(stp.canceled.price, Price{100});
        CHECK_EQ(fixture.events[3].fill.price, Price{110});
        CHECK_EQ(fixture.events[3].fill.qty, Quantity{3});
        CHECK_EQ(fixture.engine.find_order(OrderId{3})->leaves_qty, Quantity{2});
        check_book_invariants(fixture.engine);
    }
    {
        ArenaEngineFixture fixture;
        book_with_own_head(fixture);
        fixture.apply(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 6), kAliceFirm,
                                 StpMode::CancelAggressor));
        REQUIRE_EQ(fixture.events.size(), std::size_t{3});
        const Event& stp = fixture.events[2];
        CHECK_EQ(stp.canceled.order_id, OrderId{4});
        CHECK_EQ(stp.canceled.was_resting, false);
        CHECK_EQ(stp.canceled.canceled_qty, Quantity{3});
        CHECK_EQ(stp.canceled.cum_qty, Quantity{3});
        CHECK_EQ(stp.canceled.price, Price{110});
        CHECK_EQ(level_fifo(fixture.engine, 1, Side::SELL, 100), std::vector<OrderId>{2});
        CHECK(!fixture.engine.best_bid(1).has_value());
    }
    {
        ArenaEngineFixture fixture;
        book_with_own_head(fixture);
        fixture.apply(with_owner(limit_order(kAliceSession, 9, Side::BUY, 110, 6), kAliceFirm,
                                 StpMode::CancelBoth));
        REQUIRE_EQ(fixture.events.size(), std::size_t{4});
        CHECK_EQ(fixture.events[2].canceled.order_id, OrderId{2});
        CHECK_EQ(fixture.events[2].canceled.was_resting, true);
        CHECK_EQ(fixture.events[3].canceled.order_id, OrderId{4});
        CHECK_EQ(fixture.events[3].canceled.was_resting, false);
        CHECK_EQ(fixture.events[3].canceled.canceled_qty, Quantity{3});
        CHECK(!fixture.engine.find_order(OrderId{2}).has_value());
        CHECK_EQ(*fixture.engine.best_ask(1), Price{110});
    }
    {
        // STP None: same owner trades.
        ArenaEngineFixture fixture;
        book_with_own_head(fixture);
        fixture.apply(with_owner(limit_order(kAliceSession, 9, Side::BUY, 100, 7), kAliceFirm, StpMode::None));
        REQUIRE_EQ(fixture.events.size(), std::size_t{3});
        CHECK_EQ(fixture.events[2].fill.sell.order_id, OrderId{2});
        CHECK_EQ(fixture.events[2].fill.buy.owner, kAliceFirm);
        CHECK_EQ(fixture.events[2].fill.sell.owner, kAliceFirm);
    }
}

TEST_CASE(arena_matching, limit_remainder_rests_behind_existing_orders) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 5));
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 110, 5));
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 110, 8));
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[2].rested.leaves_qty, Quantity{3});
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 100, 1));
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 100), (std::vector<OrderId>{1, 4}));
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 110), std::vector<OrderId>{3});
    const std::vector<LevelInfo> bids = full_depth(fixture.engine, 1, Side::BUY);
    REQUIRE_EQ(bids.size(), std::size_t{2});
    CHECK_EQ(bids[0].price, Price{110});
    CHECK_EQ(bids[0].qty, std::uint64_t{3});
    CHECK_EQ(bids[1].qty, std::uint64_t{6});
    CHECK_EQ(bids[1].order_count, 2u);
    CHECK(!fixture.engine.best_ask(1).has_value());
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_matching, aggressive_order_can_empty_many_levels) {
    ArenaEngineFixture fixture;
    for (ClOrdId token = 1; token <= 20; ++token) {
        fixture.apply(limit_order(kBobSession, token, Side::SELL, static_cast<Price>(10 * token), 2));
    }
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 200, 50));
    REQUIRE_EQ(fixture.events.size(), std::size_t{22});
    for (std::size_t i = 1; i <= 20; ++i) {
        CHECK_EQ(fixture.events[i].fill.price, static_cast<Price>(10 * i));
        CHECK_EQ(fixture.events[i].fill.match_id, static_cast<MatchId>(i));
    }
    CHECK_EQ(fixture.events[21].rested.leaves_qty, Quantity{10});
    CHECK(!fixture.engine.best_ask(1).has_value());
    CHECK_EQ(*fixture.engine.best_bid(1), Price{200});
    CHECK_EQ(*fixture.engine.last_trade_price(1), Price{200});
    check_book_invariants(fixture.engine);
}
