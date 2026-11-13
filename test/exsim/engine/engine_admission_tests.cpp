// SPEC 2 (instruments) and SPEC 4.1 (NewOrder validation, in order).

#include "engine_harness.hpp"

using namespace engine_harness;

// ---- SPEC 2: add_instrument ------------------------------------------------

TEST_CASE(engine_instrument, assigns_sequential_ids_and_finds_by_symbol) {
    MatchingEngine engine(small_engine_config());
    CHECK_EQ(engine.add_instrument(standard_instrument_config("AAA")), InstrumentId{1});
    CHECK_EQ(engine.add_instrument(standard_instrument_config("BBB")), InstrumentId{2});
    CHECK_EQ(engine.instrument_count(), std::size_t{2});
    CHECK_EQ(engine.find_instrument(Symbol("BBB")), InstrumentId{2});
    CHECK_EQ(engine.find_instrument(Symbol("ZZZ")), InstrumentId{0});
    REQUIRE(engine.instrument(1) != nullptr);
    CHECK(engine.instrument(1)->symbol == Symbol("AAA"));
    CHECK(engine.instrument(0) == nullptr);
    CHECK(engine.instrument(3) == nullptr);
    CHECK_EQ(engine.phase(1), Phase::Continuous);
}

TEST_CASE(engine_instrument, rejects_invalid_configs) {
    MatchingEngine engine(small_engine_config());
    const auto rejected = [&](auto mutate) {
        InstrumentConfig config = standard_instrument_config("BAD");
        mutate(config);
        return engine.add_instrument(config) == 0;
    };
    CHECK(rejected([](InstrumentConfig& c) { c.tick_size = 0; }));
    CHECK(rejected([](InstrumentConfig& c) { c.tick_size = -100; }));
    CHECK(rejected([](InstrumentConfig& c) { c.lot_size = 0; }));
    CHECK(rejected([](InstrumentConfig& c) { c.max_order_qty = 0; }));
    CHECK(rejected([](InstrumentConfig& c) { c.min_price = 0; }));
    CHECK(rejected([](InstrumentConfig& c) { c.min_price = -100; }));
    CHECK(rejected([](InstrumentConfig& c) {
        c.min_price = 2000;
        c.max_price = 1000;
    }));
    CHECK(rejected([](InstrumentConfig& c) { c.min_price = 150; }));
    CHECK(rejected([](InstrumentConfig& c) { c.max_price = 10'000'050; }));
    CHECK_EQ(engine.instrument_count(), std::size_t{0});

    // min == max is a valid one-tick band.
    InstrumentConfig one_tick = standard_instrument_config("ONE");
    one_tick.min_price = 5000;
    one_tick.max_price = 5000;
    one_tick.reference_price = 5000;
    CHECK_EQ(engine.add_instrument(one_tick), InstrumentId{1});
}

TEST_CASE(engine_instrument, rejects_duplicate_symbol_and_respects_max_instruments) {
    EngineConfig config = small_engine_config();
    config.max_instruments = 2;
    MatchingEngine engine(config);
    CHECK_EQ(engine.add_instrument(standard_instrument_config("AAA")), InstrumentId{1});
    CHECK_EQ(engine.add_instrument(standard_instrument_config("AAA")), InstrumentId{0});
    CHECK_EQ(engine.add_instrument(standard_instrument_config("BBB")), InstrumentId{2});
    CHECK_EQ(engine.add_instrument(standard_instrument_config("CCC")), InstrumentId{0});
    CHECK_EQ(engine.instrument_count(), std::size_t{2});
}

TEST_CASE(engine_instrument, initial_phase_is_applied) {
    MatchingEngine engine(small_engine_config());
    const InstrumentId id = engine.add_instrument(standard_instrument_config("PRE", Phase::PreOpen));
    CHECK_EQ(engine.phase(id), Phase::PreOpen);
    CHECK_EQ(engine.phase(42), Phase::Closed);  // unknown instrument reports Closed
}

// ---- SPEC 4.1: each rejection, field by field ---------------------------------

TEST_CASE(engine_admission, unknown_instrument) {
    Harness h;
    for (InstrumentId bad : {InstrumentId{0}, InstrumentId{2}, InstrumentId{999}}) {
        NewOrder cmd = h.limit(1, 1, Side::BUY, 1000, 10);
        cmd.instrument = bad;
        h.send(cmd);
        check_new_order_rejected(h, cmd, RejectReason::UnknownInstrument);
    }
}

TEST_CASE(engine_admission, invalid_cl_ord_id) {
    Harness h;
    NewOrder zero_session = h.limit(0, 1, Side::BUY, 1000, 10);
    h.send(zero_session);
    check_new_order_rejected(h, zero_session, RejectReason::InvalidClOrdId);

    NewOrder zero_token = h.limit(1, 0, Side::BUY, 1000, 10);
    h.send(zero_token);
    check_new_order_rejected(h, zero_token, RejectReason::InvalidClOrdId);

    NewOrder too_wide = h.limit(1, kMaxClOrdId + 1, Side::BUY, 1000, 10);
    h.send(too_wide);
    check_new_order_rejected(h, too_wide, RejectReason::InvalidClOrdId);

    NewOrder max_token = h.limit(1, kMaxClOrdId, Side::BUY, 1000, 10);
    h.send(max_token);
    CHECK_EQ(h.event(0).type, EventType::Accepted);
}

TEST_CASE(engine_admission, duplicate_cl_ord_id_only_among_live_orders) {
    Harness h;
    h.rest(1, 7, Side::BUY, 1000, 10);
    NewOrder duplicate = h.limit(1, 7, Side::SELL, 2000, 10);
    h.send(duplicate);
    check_new_order_rejected(h, duplicate, RejectReason::DuplicateClOrdId);

    // Same token in another session is fine.
    h.send(h.limit(2, 7, Side::SELL, 2000, 10));
    CHECK_EQ(h.event(0).type, EventType::Accepted);

    // Once the order is no longer live, the token can be reused.
    h.cancel(1, 7);
    h.send(h.limit(1, 7, Side::BUY, 1000, 10));
    CHECK_EQ(h.event(0).type, EventType::Accepted);

    // A fully filled IOC never becomes live, so its token is immediately reusable.
    NewOrder ioc = h.limit(3, 9, Side::SELL, 1000, 10);
    ioc.tif = TimeInForce::IOC;
    h.send(ioc);
    CHECK_EQ(h.event(0).type, EventType::Accepted);
    h.send(h.limit(3, 9, Side::SELL, 5000, 10));
    CHECK_EQ(h.event(0).type, EventType::Accepted);
}

TEST_CASE(engine_admission, invalid_enum_fields_and_post_only_combinations) {
    Harness h;
    const auto expect_invalid_field = [&](NewOrder cmd) {
        h.send(cmd);
        check_new_order_rejected(h, cmd, RejectReason::InvalidField);
    };
    NewOrder bad_side = h.limit(1, 1, Side::BUY, 1000, 10);
    bad_side.side = static_cast<Side>(2);
    expect_invalid_field(bad_side);

    NewOrder bad_type = h.limit(1, 1, Side::BUY, 1000, 10);
    bad_type.type = static_cast<OrdType>(2);
    expect_invalid_field(bad_type);

    NewOrder bad_tif = h.limit(1, 1, Side::BUY, 1000, 10);
    bad_tif.tif = static_cast<TimeInForce>(4);
    expect_invalid_field(bad_tif);

    NewOrder bad_stp = h.limit(1, 1, Side::BUY, 1000, 10);
    bad_stp.stp = static_cast<StpMode>(4);
    expect_invalid_field(bad_stp);

    NewOrder post_only_market = h.market(1, 1, Side::BUY, 10);
    post_only_market.post_only = true;
    expect_invalid_field(post_only_market);

    for (TimeInForce tif : {TimeInForce::IOC, TimeInForce::FOK}) {
        NewOrder post_only_immediate = h.limit(1, 1, Side::BUY, 1000, 10);
        post_only_immediate.post_only = true;
        post_only_immediate.tif = tif;
        expect_invalid_field(post_only_immediate);
    }

    NewOrder post_only_gtc = h.limit(1, 1, Side::BUY, 1000, 10);
    post_only_gtc.post_only = true;
    post_only_gtc.tif = TimeInForce::GTC;
    h.send(post_only_gtc);
    CHECK_EQ(h.event(0).type, EventType::Accepted);
}

TEST_CASE(engine_admission, invalid_quantity) {
    MatchingEngine engine(small_engine_config());
    InstrumentConfig config = standard_instrument_config("LOT");
    config.lot_size = 100;
    config.max_order_qty = 1000;
    const InstrumentId id = engine.add_instrument(config);
    EventBuffer out;
    for (Quantity bad : {Quantity{0}, Quantity{50}, Quantity{150}, Quantity{1100}}) {
        NewOrder cmd;
        cmd.instrument = id;
        cmd.session = 1;
        cmd.cl_ord_id = 1;
        cmd.price = 1000;
        cmd.qty = bad;
        out.clear();
        REQUIRE(engine.handle(cmd, out) == HandleResult::Applied);
        REQUIRE_EQ(out.size(), std::size_t{1});
        CHECK_EQ(out[0].rejected.reason, RejectReason::InvalidQuantity);
    }
    NewOrder at_max;
    at_max.instrument = id;
    at_max.session = 1;
    at_max.cl_ord_id = 1;
    at_max.price = 1000;
    at_max.qty = 1000;
    out.clear();
    REQUIRE(engine.handle(at_max, out) == HandleResult::Applied);
    CHECK_EQ(out[0].type, EventType::Accepted);
}

TEST_CASE(engine_admission, invalid_price_for_limit_but_ignored_for_market) {
    Harness h;
    for (Price bad : {Price{0}, Price{50}, Price{1050}, Price{10'000'100}, Price{-100}}) {
        NewOrder cmd = h.limit(1, 1, Side::BUY, bad, 10);
        h.send(cmd);
        check_new_order_rejected(h, cmd, RejectReason::InvalidPrice);
    }
    // Band edges are valid.
    h.rest(1, 1, Side::BUY, 100, 10);
    h.rest(1, 2, Side::SELL, 10'000'000, 10);

    // Market order with a nonsense price is accepted; its price is echoed verbatim.
    NewOrder market = h.market(2, 1, Side::BUY, 5);
    market.price = -12345;
    h.send(market);
    REQUIRE(h.event_count() >= 1);
    CHECK_EQ(h.event(0).type, EventType::Accepted);
    CHECK_EQ(h.event(0).accepted.price, Price{-12345});
    CHECK_EQ(h.event(0).accepted.ord_type, OrdType::Market);
}

TEST_CASE(engine_admission, phase_rejections) {
    Harness h(Phase::Closed);
    NewOrder closed = h.limit(1, 1, Side::BUY, 1000, 10);
    h.send(closed);
    check_new_order_rejected(h, closed, RejectReason::MarketClosed);

    h.set_phase(Phase::Halted);
    h.send(closed);
    check_new_order_rejected(h, closed, RejectReason::InstrumentHalted);

    for (Phase call : {Phase::PreOpen, Phase::PreClose}) {
        h.set_phase(call);
        NewOrder market = h.market(1, 1, Side::BUY, 10);
        h.send(market);
        check_new_order_rejected(h, market, RejectReason::NotAllowedInPhase);
        for (TimeInForce tif : {TimeInForce::IOC, TimeInForce::FOK}) {
            NewOrder immediate = h.limit(1, 1, Side::BUY, 1000, 10);
            immediate.tif = tif;
            h.send(immediate);
            check_new_order_rejected(h, immediate, RejectReason::NotAllowedInPhase);
        }
        // DAY and GTC limits are allowed.
        NewOrder gtc = h.limit(1, 50 + static_cast<ClOrdId>(call), Side::BUY, 1000, 10);
        gtc.tif = TimeInForce::GTC;
        h.send(gtc);
        CHECK_EQ(h.event(0).type, EventType::Accepted);
        CHECK_EQ(h.event(1).type, EventType::Rested);
    }
}

TEST_CASE(engine_admission, post_only_would_cross_only_in_continuous) {
    Harness h;
    h.rest(1, 1, Side::SELL, 1000, 10);
    h.rest(1, 2, Side::BUY, 900, 10);

    NewOrder buy_at_ask = h.limit(2, 1, Side::BUY, 1000, 10);
    buy_at_ask.post_only = true;
    h.send(buy_at_ask);
    check_new_order_rejected(h, buy_at_ask, RejectReason::PostOnlyWouldCross);

    NewOrder sell_at_bid = h.limit(2, 2, Side::SELL, 900, 10);
    sell_at_bid.post_only = true;
    h.send(sell_at_bid);
    check_new_order_rejected(h, sell_at_bid, RejectReason::PostOnlyWouldCross);

    NewOrder passive_buy = h.limit(2, 3, Side::BUY, 800 + kTick, 10);
    passive_buy.post_only = true;
    h.send(passive_buy);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Rested}));

    // In a call phase post-only orders may lock/cross the book.
    h.set_phase(Phase::PreOpen);
    NewOrder crossing_in_call = h.limit(2, 4, Side::BUY, 1100, 10);
    crossing_in_call.post_only = true;
    h.send(crossing_in_call);
    CHECK_EQ(event_types(h.events()), (std::vector<EventType>{EventType::Accepted, EventType::Rested}));
}

TEST_CASE(engine_admission, checks_run_in_spec_order) {
    Harness h(Phase::Closed);
    // Everything wrong at once: unknown instrument wins.
    NewOrder all_wrong = h.limit(0, 0, static_cast<Side>(9), 1, 0);
    all_wrong.instrument = 77;
    h.send(all_wrong);
    check_new_order_rejected(h, all_wrong, RejectReason::UnknownInstrument);

    // Then the token.
    all_wrong.instrument = h.instrument();
    h.send(all_wrong);
    check_new_order_rejected(h, all_wrong, RejectReason::InvalidClOrdId);

    // Duplicate beats bad enum: need a live order first (rest it in PreOpen).
    h.set_phase(Phase::PreOpen);
    NewOrder gtc = h.limit(1, 5, Side::BUY, 1000, 10);
    gtc.tif = TimeInForce::GTC;
    h.send(gtc);
    h.set_phase(Phase::Closed);
    REQUIRE(h.engine().find_order(1, 5).has_value());
    NewOrder duplicate_and_bad_enum = h.limit(1, 5, static_cast<Side>(9), 1, 0);
    h.send(duplicate_and_bad_enum);
    check_new_order_rejected(h, duplicate_and_bad_enum, RejectReason::DuplicateClOrdId);

    // Bad enum beats bad quantity.
    NewOrder bad_enum_and_qty = h.limit(1, 6, static_cast<Side>(9), 1, 0);
    h.send(bad_enum_and_qty);
    check_new_order_rejected(h, bad_enum_and_qty, RejectReason::InvalidField);

    // Bad quantity beats bad price.
    NewOrder bad_qty_and_price = h.limit(1, 6, Side::BUY, 1, 0);
    h.send(bad_qty_and_price);
    check_new_order_rejected(h, bad_qty_and_price, RejectReason::InvalidQuantity);

    // Bad price beats phase.
    NewOrder bad_price_closed = h.limit(1, 6, Side::BUY, 1, 10);
    h.send(bad_price_closed);
    check_new_order_rejected(h, bad_price_closed, RejectReason::InvalidPrice);

    // Phase beats post-only and capacity.
    NewOrder valid_closed = h.limit(1, 6, Side::BUY, 1000, 10);
    h.send(valid_closed);
    check_new_order_rejected(h, valid_closed, RejectReason::MarketClosed);
}

TEST_CASE(engine_admission, post_only_cross_beats_capacity) {
    Harness h(Phase::Continuous, 1);
    h.rest(1, 1, Side::SELL, 1000, 10);
    NewOrder crossing = h.limit(2, 1, Side::BUY, 1000, 10);
    crossing.post_only = true;
    h.send(crossing);
    check_new_order_rejected(h, crossing, RejectReason::PostOnlyWouldCross);
    NewOrder passive = h.limit(2, 1, Side::BUY, 900, 10);
    h.send(passive);
    check_new_order_rejected(h, passive, RejectReason::CapacityExceeded);
}

TEST_CASE(engine_admission, rejections_do_not_consume_order_ids) {
    Harness h;
    const OrderId first = h.rest(1, 1, Side::BUY, 1000, 10);
    NewOrder bad = h.limit(1, 2, Side::BUY, 1050, 10);
    h.send(bad);
    h.send(bad);
    const OrderId second = h.rest(1, 2, Side::BUY, 1000, 10);
    CHECK_EQ(first, OrderId{1});
    CHECK_EQ(second, OrderId{2});
}

// ---- SPEC 4.1 rule 9 / capacity ------------------------------------------------

TEST_CASE(engine_capacity, rejects_when_live_orders_reach_max_orders) {
    Harness h(Phase::Continuous, 2);
    h.rest(1, 1, Side::BUY, 1000, 10);
    h.rest(1, 2, Side::BUY, 900, 10);
    CHECK_EQ(h.engine().live_order_count(), std::size_t{2});

    // Even an order that would fully fill needs a slot.
    NewOrder marketable = h.limit(2, 1, Side::SELL, 1000, 10);
    marketable.tif = TimeInForce::IOC;
    h.send(marketable);
    check_new_order_rejected(h, marketable, RejectReason::CapacityExceeded);

    h.cancel(1, 2);
    h.send(marketable);
    CHECK_EQ(h.event(0).type, EventType::Accepted);
    CHECK_EQ(h.event(1).type, EventType::Fill);
    CHECK_EQ(h.engine().live_order_count(), std::size_t{0});

    // Fill the pool again and keep using it: slots are recycled.
    for (ClOrdId token = 10; token < 200; ++token) {
        h.rest(3, token, Side::BUY, 1000, 1);
        h.rest(4, token, Side::SELL, 2000, 1);
        NewOrder overflow = h.limit(5, token, Side::BUY, 500, 1);
        h.send(overflow);
        check_new_order_rejected(h, overflow, RejectReason::CapacityExceeded);
        h.cancel(3, token);
        h.cancel(4, token);
    }
}

TEST_CASE(engine_capacity, invalid_engine_configs_throw) {
    for (auto config : {EngineConfig{1,0}, EngineConfig{0,1}, EngineConfig{65536,1},
                        EngineConfig{1, std::size_t{UINT32_MAX}+1}}) {
        bool threw = false;
        try { MatchingEngine engine(config); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
}
