// SPEC 1 (identifiers), 2 (instruments), 3 (phase table for new orders),
// 4.1 (NewOrder validation order) and 12 (configuration bounds).

#include <test/exsim/engine/engine_fixture.hpp>

#include <limits>
#include <stdexcept>

using namespace engine_fixture;

namespace {

void check_new_order_rejection(const EngineFixture& fixture, const NewOrder& command, RejectReason reason) {
    const Event& event = fixture.only_event();
    REQUIRE_EQ(event.type, EventType::Rejected);
    CHECK_EQ(event.instrument, command.instrument);
    CHECK_EQ(event.ts, command.ts);
    CHECK_EQ(event.rejected.cmd, CommandType::NewOrder);
    CHECK_EQ(event.rejected.reason, reason);
    CHECK_EQ(event.rejected.session, command.session);
    CHECK_EQ(event.rejected.cl_ord_id, command.cl_ord_id);
    CHECK_EQ(event.rejected.orig_cl_ord_id, ClOrdId{0});
    CHECK_EQ(event.rejected.order_id, OrderId{0});
}

RejectReason reject_reason_of(EngineFixture& fixture, const NewOrder& command) {
    fixture.apply(command);
    REQUIRE_EQ(fixture.events.size(), std::size_t{1});
    REQUIRE_EQ(fixture.events[0].type, EventType::Rejected);
    return fixture.events[0].rejected.reason;
}

}  // namespace

TEST_CASE(engine_validation, unknown_instrument) {
    EngineFixture fixture;
    for (InstrumentId instrument : {InstrumentId{0}, InstrumentId{2}, InstrumentId{65535}}) {
        NewOrder command = limit_order(kAliceSession, 1, Side::BUY, 100, 10, instrument);
        fixture.apply(command);
        check_new_order_rejection(fixture, command, RejectReason::UnknownInstrument);
    }
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
}

TEST_CASE(engine_validation, invalid_session_and_token) {
    EngineFixture fixture;
    NewOrder zero_session = limit_order(0, 1, Side::BUY, 100, 10);
    fixture.apply(zero_session);
    check_new_order_rejection(fixture, zero_session, RejectReason::InvalidClOrdId);

    NewOrder zero_token = limit_order(kAliceSession, 0, Side::BUY, 100, 10);
    fixture.apply(zero_token);
    check_new_order_rejection(fixture, zero_token, RejectReason::InvalidClOrdId);

    NewOrder token_too_large = limit_order(kAliceSession, kMaxClOrdId + 1, Side::BUY, 100, 10);
    fixture.apply(token_too_large);
    check_new_order_rejection(fixture, token_too_large, RejectReason::InvalidClOrdId);

    NewOrder largest_token = limit_order(kAliceSession, kMaxClOrdId, Side::BUY, 100, 10);
    fixture.apply(largest_token);
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[0].accepted.cl_ord_id, kMaxClOrdId);
    CHECK_EQ(fixture.engine.find_order(kAliceSession, kMaxClOrdId)->order_id, OrderId{1});

    NewOrder largest_session = limit_order(65535, 7, Side::BUY, 100, 10);
    fixture.apply(largest_session);
    CHECK_EQ(fixture.events[0].type, EventType::Accepted);
    CHECK_EQ(fixture.engine.find_order(65535, 7)->session, SessionId{65535});
}

TEST_CASE(engine_validation, duplicate_token_only_among_live_orders_of_the_session) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 5, Side::BUY, 100, 10));
    NewOrder duplicate = limit_order(kAliceSession, 5, Side::SELL, 150, 3);
    fixture.apply(duplicate);
    check_new_order_rejection(fixture, duplicate, RejectReason::DuplicateClOrdId);

    // Same token in another session is a different order.
    fixture.apply(limit_order(kBobSession, 5, Side::SELL, 150, 3));
    CHECK_EQ(fixture.events[1].type, EventType::Rested);

    // Once the order is dead the token is free again.
    fixture.apply(CancelOrder{1, kAliceSession, 5, 0, 0});
    fixture.apply(limit_order(kAliceSession, 5, Side::BUY, 90, 1));
    CHECK_EQ(fixture.events[1].type, EventType::Rested);

    // A fully filled order's token is also reusable.
    fixture.apply(limit_order(kCarolSession, 9, Side::SELL, 90, 1));
    REQUIRE_EQ(fixture.events[1].type, EventType::Fill);
    CHECK(!fixture.engine.find_order(kAliceSession, 5).has_value());
    fixture.apply(limit_order(kAliceSession, 5, Side::BUY, 80, 1));
    CHECK_EQ(fixture.events[1].type, EventType::Rested);
}

TEST_CASE(engine_validation, invalid_enum_fields_and_post_only_combinations) {
    EngineFixture fixture;
    NewOrder bad_side = limit_order(kAliceSession, 1, static_cast<Side>(2), 100, 10);
    NewOrder bad_type = limit_order(kAliceSession, 2, Side::BUY, 100, 10);
    bad_type.type = static_cast<OrdType>(2);
    NewOrder bad_tif = with_tif(limit_order(kAliceSession, 3, Side::BUY, 100, 10), static_cast<TimeInForce>(4));
    NewOrder bad_stp = with_owner(limit_order(kAliceSession, 4, Side::BUY, 100, 10), 1, static_cast<StpMode>(4));
    NewOrder post_only_market = market_order(kAliceSession, 5, Side::BUY, 10);
    post_only_market.post_only = true;
    NewOrder post_only_ioc = with_tif(limit_order(kAliceSession, 6, Side::BUY, 100, 10), TimeInForce::IOC);
    post_only_ioc.post_only = true;
    NewOrder post_only_fok = with_tif(limit_order(kAliceSession, 7, Side::BUY, 100, 10), TimeInForce::FOK);
    post_only_fok.post_only = true;
    for (const NewOrder& command : {bad_side, bad_type, bad_tif, bad_stp, post_only_market, post_only_ioc,
                                    post_only_fok}) {
        fixture.apply(command);
        check_new_order_rejection(fixture, command, RejectReason::InvalidField);
    }
    NewOrder post_only_gtc = with_tif(limit_order(kAliceSession, 8, Side::BUY, 100, 10), TimeInForce::GTC);
    post_only_gtc.post_only = true;
    fixture.apply(post_only_gtc);
    CHECK_EQ(fixture.events[1].type, EventType::Rested);
    CHECK(fixture.engine.find_order(kAliceSession, 8)->post_only);
}

TEST_CASE(engine_validation, quantity_rules_with_lot_size) {
    EngineFixture fixture;
    InstrumentConfig lots = small_instrument_config("LOTS");
    lots.lot_size = 5;
    lots.max_order_qty = 50;
    const InstrumentId lots_id = fixture.engine.add_instrument(lots);
    REQUIRE_EQ(lots_id, InstrumentId{2});
    ClOrdId token = 1;
    for (Quantity invalid_qty : {Quantity{0}, Quantity{3}, Quantity{51}, Quantity{55},
                                 std::numeric_limits<Quantity>::max()}) {
        NewOrder command = limit_order(kAliceSession, token++, Side::BUY, 100, invalid_qty, lots_id);
        fixture.apply(command);
        check_new_order_rejection(fixture, command, RejectReason::InvalidQuantity);
    }
    for (Quantity valid_qty : {Quantity{5}, Quantity{50}}) {
        fixture.apply(limit_order(kAliceSession, token++, Side::BUY, 100, valid_qty, lots_id));
        CHECK_EQ(fixture.events[1].type, EventType::Rested);
    }
}

TEST_CASE(engine_validation, limit_price_grid_and_ignored_market_price) {
    EngineFixture fixture;
    ClOrdId token = 1;
    for (Price invalid_price : {Price{0}, Price{-10}, Price{5}, Price{15}, Price{210}, Price{101},
                                std::numeric_limits<Price>::min(), std::numeric_limits<Price>::max()}) {
        NewOrder command = limit_order(kAliceSession, token++, Side::BUY, invalid_price, 10);
        fixture.apply(command);
        check_new_order_rejection(fixture, command, RejectReason::InvalidPrice);
    }
    for (Price edge_price : {Price{10}, Price{200}}) {
        fixture.apply(limit_order(kAliceSession, token++, Side::SELL, edge_price, 1));
        CHECK_EQ(fixture.events[1].type, EventType::Rested);
    }
    // Market order prices are ignored, even when off the grid.
    NewOrder market = market_order(kBobSession, 1, Side::BUY, 2, std::numeric_limits<Price>::min());
    fixture.apply(market);
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[0].accepted.price, std::numeric_limits<Price>::min());
    CHECK_EQ(fixture.events[1].fill.price, Price{10});
    CHECK_EQ(fixture.events[2].fill.price, Price{200});
}

TEST_CASE(engine_validation, phase_table_for_new_orders) {
    struct PhaseCase {
        Phase phase;
        RejectReason limit_day;
        RejectReason market;
        RejectReason ioc;
        RejectReason fok;
    };
    const PhaseCase cases[] = {
        {Phase::Closed, RejectReason::MarketClosed, RejectReason::MarketClosed, RejectReason::MarketClosed,
         RejectReason::MarketClosed},
        {Phase::Halted, RejectReason::InstrumentHalted, RejectReason::InstrumentHalted,
         RejectReason::InstrumentHalted, RejectReason::InstrumentHalted},
        {Phase::PreOpen, RejectReason::None, RejectReason::NotAllowedInPhase, RejectReason::NotAllowedInPhase,
         RejectReason::NotAllowedInPhase},
        {Phase::PreClose, RejectReason::None, RejectReason::NotAllowedInPhase, RejectReason::NotAllowedInPhase,
         RejectReason::NotAllowedInPhase},
    };
    for (const PhaseCase& phase_case : cases) {
        EngineFixture fixture(phase_case.phase);
        const NewOrder limit_day = limit_order(kAliceSession, 1, Side::BUY, 100, 10);
        const NewOrder limit_gtc = with_tif(limit_order(kAliceSession, 2, Side::BUY, 100, 10), TimeInForce::GTC);
        const NewOrder market = market_order(kAliceSession, 3, Side::BUY, 10);
        const NewOrder ioc = with_tif(limit_order(kAliceSession, 4, Side::BUY, 100, 10), TimeInForce::IOC);
        const NewOrder fok = with_tif(limit_order(kAliceSession, 5, Side::BUY, 100, 10), TimeInForce::FOK);
        if (phase_case.limit_day == RejectReason::None) {
            fixture.apply(limit_day);
            CHECK_EQ(fixture.events[1].type, EventType::Rested);
            fixture.apply(limit_gtc);
            CHECK_EQ(fixture.events[1].type, EventType::Rested);
        } else {
            CHECK_EQ(reject_reason_of(fixture, limit_day), phase_case.limit_day);
            CHECK_EQ(reject_reason_of(fixture, limit_gtc), phase_case.limit_day);
        }
        CHECK_EQ(reject_reason_of(fixture, market), phase_case.market);
        CHECK_EQ(reject_reason_of(fixture, ioc), phase_case.ioc);
        CHECK_EQ(reject_reason_of(fixture, fok), phase_case.fok);
    }
}

TEST_CASE(engine_validation, post_only_would_cross_only_in_continuous) {
    EngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 120, 10));
    fixture.apply(limit_order(kBobSession, 2, Side::BUY, 80, 10));

    NewOrder buy_at_ask = limit_order(kAliceSession, 1, Side::BUY, 120, 5);
    buy_at_ask.post_only = true;
    fixture.apply(buy_at_ask);
    check_new_order_rejection(fixture, buy_at_ask, RejectReason::PostOnlyWouldCross);

    NewOrder sell_at_bid = limit_order(kAliceSession, 2, Side::SELL, 80, 5);
    sell_at_bid.post_only = true;
    fixture.apply(sell_at_bid);
    check_new_order_rejection(fixture, sell_at_bid, RejectReason::PostOnlyWouldCross);

    NewOrder buy_inside = limit_order(kAliceSession, 3, Side::BUY, 110, 5);
    buy_inside.post_only = true;
    fixture.apply(buy_inside);
    CHECK_EQ(fixture.events[1].type, EventType::Rested);

    // In a call phase post-only orders rest even though the book crosses.
    fixture.apply(SetPhase{50, 1, Phase::PreOpen});
    NewOrder crossing_in_call = limit_order(kAliceSession, 4, Side::BUY, 150, 5);
    crossing_in_call.post_only = true;
    fixture.apply(crossing_in_call);
    CHECK_EQ(fixture.events[1].type, EventType::Rested);
    CHECK_EQ(*fixture.engine.best_bid(1), Price{150});
    CHECK_EQ(*fixture.engine.best_ask(1), Price{120});
}

TEST_CASE(engine_validation, check_order_is_the_spec_order) {
    // Each step fixes the earliest failure, exposing the next one.
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 10));
    fixture.apply(SetPhase{1, 1, Phase::Halted});
    NewOrder command = limit_order(0, 1, static_cast<Side>(9), 7, 0, 3);
    command.post_only = true;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::UnknownInstrument);
    command.instrument = 1;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::InvalidClOrdId);
    command.session = kAliceSession;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::DuplicateClOrdId);
    command.cl_ord_id = 2;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::InvalidField);
    command.side = Side::BUY;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::InvalidQuantity);
    command.qty = 10;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::InvalidPrice);
    command.price = 100;
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::InstrumentHalted);
    fixture.apply(SetPhase{2, 1, Phase::Continuous});
    CHECK_EQ(reject_reason_of(fixture, command), RejectReason::PostOnlyWouldCross);
}

TEST_CASE(engine_validation, capacity_counts_live_orders_and_needs_a_slot_even_to_fill) {
    EngineFixture fixture(Phase::Continuous, 1, EngineConfig{1, 3});
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 10));
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 110, 10));
    fixture.apply(limit_order(kAliceSession, 3, Side::SELL, 120, 10));
    // A marketable IOC that would fully fill still needs a free slot.
    NewOrder taker = with_tif(limit_order(kBobSession, 1, Side::BUY, 100, 5), TimeInForce::IOC);
    fixture.apply(taker);
    check_new_order_rejection(fixture, taker, RejectReason::CapacityExceeded);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{3});

    fixture.apply(CancelOrder{1, kAliceSession, 3, 0, 0});
    fixture.apply(taker);
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[0].accepted.order_id, OrderId{4});
    CHECK_EQ(fixture.events[1].type, EventType::Fill);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{2});
}

TEST_CASE(engine_validation, rejections_consume_no_order_or_match_ids) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 10));
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 10));       // duplicate
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 105, 10));       // off grid
    fixture.apply(CancelOrder{1, kAliceSession, 77, 0, 0});                  // unknown
    fixture.apply(ReplaceOrder{1, kAliceSession, 1, 0, 105, 10, false});     // off grid
    fixture.apply(SetPhase{1, 9, Phase::Closed});                            // unknown instrument
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 4));
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[0].accepted.order_id, OrderId{2});
    CHECK_EQ(fixture.events[1].fill.match_id, MatchId{1});
}

TEST_CASE(engine_instruments, add_instrument_rejects_each_invalid_rule) {
    MatchingEngine engine(EngineConfig{8, 8});
    const InstrumentConfig valid = small_instrument_config("VALID");
    auto variant = [&](auto mutate) {
        InstrumentConfig config = valid;
        config.symbol = Symbol("OTHER");
        mutate(config);
        return config;
    };
    const InstrumentConfig invalid_configs[] = {
        variant([](InstrumentConfig& c) { c.tick_size = 0; }),
        variant([](InstrumentConfig& c) { c.tick_size = -10; }),
        variant([](InstrumentConfig& c) { c.lot_size = 0; }),
        variant([](InstrumentConfig& c) { c.max_order_qty = 0; }),
        variant([](InstrumentConfig& c) { c.lot_size = 10; c.max_order_qty = 9; }),
        variant([](InstrumentConfig& c) { c.min_price = 0; }),
        variant([](InstrumentConfig& c) { c.min_price = -10; }),
        variant([](InstrumentConfig& c) { c.max_price = 0; }),
        variant([](InstrumentConfig& c) { c.min_price = 200; c.max_price = 100; }),
        variant([](InstrumentConfig& c) { c.min_price = 15; }),
        variant([](InstrumentConfig& c) { c.max_price = 205; }),
        variant([](InstrumentConfig& c) { c.reference_price = 0; }),
        variant([](InstrumentConfig& c) { c.reference_price = 210; }),
        variant([](InstrumentConfig& c) { c.reference_price = 105; }),
        variant([](InstrumentConfig& c) { c.initial_phase = static_cast<Phase>(5); }),
        // Price ladders are capped at 2^24 ticks per side.
        variant([](InstrumentConfig& c) { c.tick_size = 1; c.min_price = 1; c.max_price = (1 << 24) + 1;
                                          c.reference_price = 1; }),
    };
    for (const InstrumentConfig& config : invalid_configs) {
        CHECK_EQ(engine.add_instrument(config), InstrumentId{0});
    }
    CHECK_EQ(engine.instrument_count(), std::size_t{0});

    REQUIRE_EQ(engine.add_instrument(valid), InstrumentId{1});
    CHECK_EQ(engine.add_instrument(valid), InstrumentId{0});  // duplicate symbol
    const InstrumentConfig second_instrument = variant([](InstrumentConfig& c) { c.reference_price = 200; });
    CHECK_EQ(engine.add_instrument(second_instrument), InstrumentId{2});
    CHECK_EQ(engine.instrument_count(), std::size_t{2});
    CHECK_EQ(engine.find_instrument(Symbol("VALID")), InstrumentId{1});
    CHECK_EQ(engine.find_instrument(Symbol("OTHER")), InstrumentId{2});
    CHECK_EQ(engine.find_instrument(Symbol("NONE")), InstrumentId{0});
    REQUIRE(engine.instrument(1) != nullptr);
    CHECK_EQ(engine.instrument(1)->reference_price, Price{100});
    CHECK(engine.instrument(0) == nullptr);
    CHECK(engine.instrument(3) == nullptr);
}

TEST_CASE(engine_instruments, max_instruments_limit_and_initial_phase) {
    MatchingEngine engine(EngineConfig{2, 8});
    CHECK_EQ(engine.add_instrument(small_instrument_config("A", Phase::PreOpen)), InstrumentId{1});
    CHECK_EQ(engine.add_instrument(small_instrument_config("B", Phase::Halted)), InstrumentId{2});
    CHECK_EQ(engine.add_instrument(small_instrument_config("C")), InstrumentId{0});
    CHECK_EQ(engine.phase(1), Phase::PreOpen);
    CHECK_EQ(engine.phase(2), Phase::Halted);
    CHECK_EQ(engine.phase(3), Phase::Closed);  // unknown instrument reads as Closed
    CHECK(!engine.last_trade_price(1).has_value());
}

TEST_CASE(engine_config, engine_config_bounds_throw) {
    auto constructor_throws = [](EngineConfig config) {
        try {
            MatchingEngine engine(config);
        } catch (const std::invalid_argument&) {
            return true;
        } catch (const std::length_error&) {
            return true;
        }
        return false;
    };
    CHECK(constructor_throws(EngineConfig{0, 8}));
    CHECK(constructor_throws(EngineConfig{1, 0}));
    CHECK(constructor_throws(EngineConfig{65536, 8}));
    CHECK(constructor_throws(EngineConfig{1, std::size_t{std::numeric_limits<std::uint32_t>::max()} + 1}));
    CHECK(!constructor_throws(EngineConfig{65535, 1}));
    CHECK_EQ(required_event_capacity(EngineConfig{3, 10}), std::size_t{2 * 10 + 2 * 3 + 4});
}
