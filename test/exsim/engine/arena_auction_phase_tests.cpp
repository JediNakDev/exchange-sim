// SPEC 7 (MassCancel), 8 (SetPhase and end-of-day expiry), 9 (auction
// uncross and indicative) and the phase rules of SPEC 12.

#include <test/exsim/engine/arena_test_support.hpp>

#include <limits>

using namespace arena_test;

namespace {

struct UncrossOutcome {
    Price price;
    std::uint64_t matched_qty;
};

// Rests the orders in PreOpen, checks the indicative, then opens.
UncrossOutcome open_auction(ArenaEngineFixture& fixture) {
    const AuctionIndicative indicative = fixture.engine.auction_indicative(1);
    fixture.apply(SetPhase{500, 1, Phase::Continuous});
    REQUIRE(fixture.events.size() >= 2);
    const Event& summary = fixture.events[fixture.events.size() - 2];
    REQUIRE_EQ(summary.type, EventType::AuctionSummary);
    CHECK(indicative.crossed);
    CHECK_EQ(indicative.price, summary.auction_summary.price);
    CHECK_EQ(indicative.paired_qty, summary.auction_summary.matched_qty);
    return UncrossOutcome{summary.auction_summary.price, summary.auction_summary.matched_qty};
}

}  // namespace

TEST_CASE(arena_auction, uncross_price_maximises_volume) {
    ArenaEngineFixture fixture(Phase::PreOpen);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 110, 10));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 4));
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 110, 4));
    const AuctionIndicative indicative = fixture.engine.auction_indicative(1);
    CHECK_EQ(indicative.imbalance_qty, std::uint64_t{2});
    CHECK_EQ(indicative.imbalance_side, Aggressor::Buy);
    const UncrossOutcome outcome = open_auction(fixture);
    CHECK_EQ(outcome.price, Price{110});
    CHECK_EQ(outcome.matched_qty, std::uint64_t{8});
}

TEST_CASE(arena_auction, volume_tie_broken_by_smallest_imbalance_before_distance) {
    ArenaEngineFixture fixture(Phase::PreOpen);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 110, 10));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 100, 5));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 10));
    // V(100) = V(110) = 10; |I(100)| = 5, |I(110)| = 0. Reference 100 loses.
    const AuctionIndicative indicative = fixture.engine.auction_indicative(1);
    CHECK_EQ(indicative.imbalance_qty, std::uint64_t{0});
    CHECK_EQ(indicative.imbalance_side, Aggressor::None);
    const UncrossOutcome outcome = open_auction(fixture);
    CHECK_EQ(outcome.price, Price{110});
    CHECK_EQ(outcome.matched_qty, std::uint64_t{10});
}

TEST_CASE(arena_auction, imbalance_tie_broken_by_distance_to_reference_or_last_trade) {
    {
        ArenaEngineFixture fixture(Phase::PreOpen);
        fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 120, 10));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 10));
        CHECK_EQ(open_auction(fixture).price, Price{100});  // reference price 100
    }
    {
        // A continuous trade at 120 becomes the auction reference.
        ArenaEngineFixture fixture;
        fixture.apply(limit_order(kCarolSession, 1, Side::BUY, 120, 1));
        fixture.apply(limit_order(kCarolSession, 2, Side::SELL, 120, 1));
        REQUIRE_EQ(*fixture.engine.last_trade_price(1), Price{120});
        fixture.apply(SetPhase{1, 1, Phase::PreOpen});
        fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 120, 10));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 10));
        CHECK_EQ(open_auction(fixture).price, Price{120});
    }
}

TEST_CASE(arena_auction, equal_distance_picks_the_lower_price) {
    ArenaEngineFixture fixture(Phase::PreOpen);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 110, 10));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 90, 10));
    CHECK_EQ(open_auction(fixture).price, Price{90});
}

TEST_CASE(arena_auction, execution_pairs_by_priority_at_one_price_without_stp) {
    ArenaEngineFixture fixture(Phase::PreClose);
    fixture.apply(with_owner(limit_order(kAliceSession, 1, Side::BUY, 120, 5), kAliceFirm, StpMode::CancelBoth));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 110, 5));
    fixture.apply(limit_order(kAliceSession, 3, Side::BUY, 110, 5));
    fixture.apply(with_owner(limit_order(kAliceSession, 4, Side::SELL, 100, 8), kAliceFirm, StpMode::CancelBoth));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 4));
    const AuctionIndicative indicative = fixture.engine.auction_indicative(1);
    CHECK(indicative.crossed);
    CHECK_EQ(indicative.price, Price{110});
    CHECK_EQ(indicative.paired_qty, std::uint64_t{12});
    CHECK_EQ(indicative.imbalance_qty, std::uint64_t{3});
    CHECK_EQ(indicative.imbalance_side, Aggressor::Buy);

    const SetPhase open{4242, 1, Phase::Continuous};
    fixture.apply(open);
    REQUIRE_EQ(fixture.events.size(), std::size_t{6});
    struct ExpectedFill {
        OrderId buy;
        OrderId sell;
        Quantity qty;
        Quantity buy_leaves;
        Quantity sell_leaves;
    };
    const ExpectedFill expected[] = {{1, 4, 5, 0, 3}, {2, 4, 3, 2, 0}, {2, 5, 2, 0, 2}, {3, 5, 2, 3, 0}};
    for (std::size_t i = 0; i < 4; ++i) {
        const Event& fill = fixture.events[i];
        REQUIRE_EQ(fill.type, EventType::Fill);
        CHECK_EQ(fill.ts, open.ts);
        CHECK_EQ(fill.instrument, InstrumentId{1});
        CHECK_EQ(fill.fill.match_id, static_cast<MatchId>(i + 1));
        CHECK_EQ(fill.fill.price, Price{110});
        CHECK_EQ(fill.fill.aggressor, Aggressor::None);
        CHECK_EQ(fill.fill.buy.order_id, expected[i].buy);
        CHECK_EQ(fill.fill.sell.order_id, expected[i].sell);
        CHECK_EQ(fill.fill.qty, expected[i].qty);
        CHECK_EQ(fill.fill.buy.leaves_qty, expected[i].buy_leaves);
        CHECK_EQ(fill.fill.sell.leaves_qty, expected[i].sell_leaves);
    }
    CHECK_EQ(fixture.events[0].fill.buy.owner, kAliceFirm);
    CHECK_EQ(fixture.events[0].fill.sell.owner, kAliceFirm);
    CHECK_EQ(fixture.events[1].fill.sell.cum_qty, Quantity{8});
    const Event& summary = fixture.events[4];
    REQUIRE_EQ(summary.type, EventType::AuctionSummary);
    CHECK_EQ(summary.instrument, InstrumentId{1});
    CHECK_EQ(summary.ts, open.ts);
    CHECK_EQ(summary.auction_summary.price, Price{110});
    CHECK_EQ(summary.auction_summary.matched_qty, std::uint64_t{12});
    CHECK_EQ(summary.auction_summary.match_id, MatchId{5});
    const Event& changed = fixture.events[5];
    REQUIRE_EQ(changed.type, EventType::PhaseChanged);
    CHECK_EQ(changed.instrument, InstrumentId{1});
    CHECK_EQ(changed.phase_changed.old_phase, Phase::PreClose);
    CHECK_EQ(changed.phase_changed.new_phase, Phase::Continuous);

    CHECK_EQ(*fixture.engine.last_trade_price(1), Price{110});
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 110), std::vector<OrderId>{3});
    CHECK(!fixture.engine.best_ask(1).has_value());
    CHECK(!fixture.engine.auction_indicative(1).crossed);

    // The next continuous fill continues the global match id sequence.
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 110, 1));
    CHECK_EQ(fixture.events[1].fill.match_id, MatchId{6});
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_auction, non_crossed_indicative_reports_total_side_imbalance) {
    ArenaEngineFixture fixture(Phase::PreOpen);
    AuctionIndicative empty = fixture.engine.auction_indicative(1);
    CHECK(!empty.crossed);
    CHECK_EQ(empty.imbalance_qty, std::uint64_t{0});
    CHECK_EQ(empty.imbalance_side, Aggressor::None);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 90, 7));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 80, 2));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 3));
    AuctionIndicative indicative = fixture.engine.auction_indicative(1);
    CHECK(!indicative.crossed);
    CHECK_EQ(indicative.price, Price{0});
    CHECK_EQ(indicative.paired_qty, std::uint64_t{0});
    CHECK_EQ(indicative.imbalance_qty, std::uint64_t{6});
    CHECK_EQ(indicative.imbalance_side, Aggressor::Buy);
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 120, 10));
    indicative = fixture.engine.auction_indicative(1);
    CHECK_EQ(indicative.imbalance_qty, std::uint64_t{4});
    CHECK_EQ(indicative.imbalance_side, Aggressor::Sell);
    CHECK(!fixture.engine.auction_indicative(9).crossed);

    // Opening a non-crossed book emits only PhaseChanged and consumes no match id.
    fixture.apply(SetPhase{1, 1, Phase::Continuous});
    REQUIRE_EQ(fixture.events.size(), std::size_t{1});
    CHECK_EQ(fixture.events[0].type, EventType::PhaseChanged);
    fixture.apply(limit_order(kCarolSession, 1, Side::BUY, 110, 1));
    CHECK_EQ(fixture.events[1].fill.match_id, MatchId{1});
}

TEST_CASE(arena_auction, wide_aggregates_use_64_bit_sums) {
    ArenaEngineFixture fixture(Phase::PreOpen);
    InstrumentConfig wide = small_instrument_config("WIDE", Phase::PreOpen);
    wide.max_order_qty = std::numeric_limits<Quantity>::max();
    REQUIRE_EQ(fixture.engine.add_instrument(wide), InstrumentId{2});
    constexpr Quantity kMax = std::numeric_limits<Quantity>::max();
    for (ClOrdId token = 1; token <= 3; ++token) {
        fixture.apply(limit_order(kAliceSession, token, Side::BUY, 110, kMax, 2));
        fixture.apply(limit_order(kBobSession, token, Side::SELL, 100, kMax, 2));
    }
    const std::uint64_t three_max = 3 * std::uint64_t{kMax};
    CHECK_EQ(full_depth(fixture.engine, 2, Side::BUY).front().qty, three_max);
    const AuctionIndicative indicative = fixture.engine.auction_indicative(2);
    CHECK(indicative.crossed);
    CHECK_EQ(indicative.paired_qty, three_max);
    // A crossed book reports I(p*): a fourth bid at 110 leaves one max surplus.
    fixture.apply(limit_order(kAliceSession, 4, Side::BUY, 110, kMax, 2));
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_qty, std::uint64_t{kMax});
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_side, Aggressor::Buy);
    fixture.apply(SetPhase{1, 2, Phase::Continuous});
    REQUIRE_EQ(fixture.events.size(), std::size_t{5});
    CHECK_EQ(fixture.events[3].auction_summary.matched_qty, three_max);
    CHECK_EQ(fixture.events[3].auction_summary.price, Price{100});
}

TEST_CASE(arena_phase, halted_keeps_a_crossed_book_and_leaving_it_uncrosses) {
    for (Phase exit_phase : {Phase::Continuous, Phase::Closed}) {
        ArenaEngineFixture fixture(Phase::PreOpen);
        fixture.apply(with_tif(limit_order(kAliceSession, 1, Side::BUY, 120, 10), TimeInForce::GTC));
        fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 50, 3));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 4));
        fixture.apply(SetPhase{1, 1, Phase::Halted});
        REQUIRE_EQ(fixture.events.size(), std::size_t{1});
        CHECK_EQ(fixture.events[0].phase_changed.new_phase, Phase::Halted);
        CHECK_EQ(*fixture.engine.best_bid(1), Price{120});
        CHECK_EQ(*fixture.engine.best_ask(1), Price{100});
        // Call phase to call phase also keeps the cross.
        fixture.apply(SetPhase{2, 1, Phase::PreClose});
        fixture.apply(SetPhase{3, 1, Phase::Halted});
        CHECK_EQ(fixture.engine.live_order_count(), std::size_t{3});

        fixture.apply(SetPhase{4, 1, exit_phase});
        if (exit_phase == Phase::Continuous) {
            REQUIRE_EQ(fixture.events.size(), std::size_t{3});
        } else {
            // Uncross, then expire the remaining DAY order, then change phase.
            REQUIRE_EQ(fixture.events.size(), std::size_t{4});
            CHECK_EQ(fixture.events[2].canceled.order_id, OrderId{2});
            CHECK_EQ(fixture.events[2].canceled.reason, CancelReason::EndOfDay);
        }
        CHECK_EQ(fixture.events[0].fill.qty, Quantity{4});
        CHECK_EQ(fixture.events[0].fill.price, Price{100});  // reference 100 is closest
        CHECK_EQ(fixture.events[1].auction_summary.match_id, MatchId{2});
        CHECK_EQ(fixture.events[fixture.events.size() - 1].phase_changed.old_phase, Phase::Halted);
        CHECK_EQ(fixture.engine.find_order(OrderId{1})->leaves_qty, Quantity{6});
        check_book_invariants(fixture.engine);
    }
}

TEST_CASE(arena_phase, set_phase_rejections_and_same_phase_no_op) {
    ArenaEngineFixture fixture;
    const SetPhase unknown{17, 9, Phase::Halted};
    fixture.apply(unknown);
    const Event& unknown_event = fixture.only_event();
    REQUIRE_EQ(unknown_event.type, EventType::Rejected);
    CHECK_EQ(unknown_event.instrument, InstrumentId{9});
    CHECK_EQ(unknown_event.ts, Timestamp{17});
    CHECK_EQ(unknown_event.rejected.cmd, CommandType::SetPhase);
    CHECK_EQ(unknown_event.rejected.reason, RejectReason::UnknownInstrument);
    CHECK_EQ(unknown_event.rejected.session, SessionId{0});
    CHECK_EQ(unknown_event.rejected.cl_ord_id, ClOrdId{0});
    CHECK_EQ(unknown_event.rejected.orig_cl_ord_id, ClOrdId{0});
    CHECK_EQ(unknown_event.rejected.order_id, OrderId{0});

    fixture.apply(SetPhase{18, 9, static_cast<Phase>(7)});
    CHECK_EQ(fixture.only_event().rejected.reason, RejectReason::UnknownInstrument);
    for (InstrumentId instrument : {InstrumentId{0}, InstrumentId{1}}) {
        fixture.apply(SetPhase{19, instrument, static_cast<Phase>(5)});
        CHECK_EQ(fixture.only_event().rejected.reason, RejectReason::InvalidField);
        CHECK_EQ(fixture.only_event().instrument, instrument);
    }
    CHECK_EQ(fixture.engine.phase(1), Phase::Continuous);

    fixture.apply(SetPhase{20, 1, Phase::Continuous});
    CHECK(fixture.events.empty());
    fixture.apply(SetPhase{21, 0, Phase::Continuous});
    CHECK(fixture.events.empty());
}

TEST_CASE(arena_phase, all_instruments_in_ascending_id_with_per_instrument_batches) {
    ArenaEngineFixture fixture(Phase::PreOpen, 3);
    fixture.apply(SetPhase{1, 2, Phase::Closed});
    // Instrument 3 is crossed; instrument 1 holds a DAY and a GTC order.
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5, 1));
    fixture.apply(with_tif(limit_order(kAliceSession, 2, Side::SELL, 150, 5, 1), TimeInForce::GTC));
    fixture.apply(limit_order(kAliceSession, 3, Side::BUY, 120, 5, 3));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 2, 3));
    const SetPhase close_all{77, 0, Phase::Closed};
    fixture.apply(close_all);
    // inst 1: Canceled(EOD order 1), PhaseChanged; inst 2 already Closed;
    // inst 3: Fill, AuctionSummary, Canceled(EOD order 3), PhaseChanged.
    REQUIRE_EQ(fixture.events.size(), std::size_t{6});
    CHECK_EQ(fixture.events[0].instrument, InstrumentId{1});
    CHECK_EQ(fixture.events[0].canceled.order_id, OrderId{1});
    CHECK_EQ(fixture.events[0].canceled.reason, CancelReason::EndOfDay);
    CHECK_EQ(fixture.events[0].canceled.request_cl_ord_id, ClOrdId{0});
    CHECK_EQ(fixture.events[0].canceled.was_resting, true);
    CHECK_EQ(fixture.events[1].type, EventType::PhaseChanged);
    CHECK_EQ(fixture.events[1].instrument, InstrumentId{1});
    CHECK_EQ(fixture.events[2].type, EventType::Fill);
    CHECK_EQ(fixture.events[2].instrument, InstrumentId{3});
    CHECK_EQ(fixture.events[3].type, EventType::AuctionSummary);
    CHECK_EQ(fixture.events[3].instrument, InstrumentId{3});
    CHECK_EQ(fixture.events[4].canceled.order_id, OrderId{3});
    CHECK_EQ(fixture.events[4].canceled.canceled_qty, Quantity{3});
    CHECK_EQ(fixture.events[4].canceled.cum_qty, Quantity{2});
    CHECK_EQ(fixture.events[5].instrument, InstrumentId{3});
    for (const Event& event : fixture.events) CHECK_EQ(event.ts, close_all.ts);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{1});
    CHECK(fixture.engine.find_order(kAliceSession, 2).has_value());
}

TEST_CASE(arena_phase, end_of_day_expires_day_orders_in_ascending_order_id) {
    ArenaEngineFixture fixture;
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 150, 5));                                   // 1
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 90, 5));                                   // 2
    fixture.apply(with_tif(limit_order(kAliceSession, 2, Side::BUY, 90, 5), TimeInForce::GTC));       // 3
    fixture.apply(limit_order(kCarolSession, 1, Side::BUY, 100, 5));                                  // 4
    fixture.apply(ReplaceOrder{5, kBobSession, 1, 0, 160, 5, false});                                 // 1 becomes 5
    fixture.apply(SetPhase{9, 1, Phase::Closed});
    CHECK_EQ(order_ids_of_events(fixture.events, EventType::Canceled), (std::vector<OrderId>{2, 4, 5}));
    CHECK_EQ(fixture.events[fixture.events.size() - 1].type, EventType::PhaseChanged);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{1});
    CHECK(fixture.engine.find_order(OrderId{3}).has_value());
    // Reopening keeps the GTC order and needs no expiry.
    fixture.apply(SetPhase{10, 1, Phase::PreOpen});
    CHECK_EQ(fixture.only_event().type, EventType::PhaseChanged);
}

// Expiry and the indicative imbalance walk one instrument's orders only, so
// orders of other instruments interleaved by OrderId must neither expire nor
// count, including after a priority-losing Replace moves an order to the end.
TEST_CASE(arena_phase, end_of_day_and_imbalance_see_only_their_instrument) {
    ArenaEngineFixture fixture(Phase::PreOpen, 3);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 90, 5, 2));                                // 1
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 90, 7, 1));                                // 2
    fixture.apply(with_tif(limit_order(kBobSession, 1, Side::SELL, 150, 4, 2), TimeInForce::GTC));    // 3
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 150, 3, 3));                                // 4
    fixture.apply(limit_order(kCarolSession, 1, Side::BUY, 80, 2, 2));                                // 5
    fixture.apply(limit_order(kCarolSession, 2, Side::SELL, 160, 9, 1));                              // 6
    fixture.apply(ReplaceOrder{7, kAliceSession, 1, 3, 100, 5, false});                               // 1 becomes 7

    CHECK_EQ(fixture.engine.auction_indicative(1).imbalance_qty, std::uint64_t{2});
    CHECK_EQ(fixture.engine.auction_indicative(1).imbalance_side, Aggressor::Sell);
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_qty, std::uint64_t{3});
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_side, Aggressor::Buy);
    CHECK_EQ(fixture.engine.auction_indicative(3).imbalance_qty, std::uint64_t{3});
    CHECK_EQ(fixture.engine.auction_indicative(3).imbalance_side, Aggressor::Sell);

    fixture.apply(SetPhase{8, 2, Phase::Closed});
    CHECK_EQ(order_ids_of_events(fixture.events, EventType::Canceled), (std::vector<OrderId>{5, 7}));
    for (const Event& event : fixture.events) CHECK_EQ(event.instrument, InstrumentId{2});
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_qty, std::uint64_t{4});
    CHECK_EQ(fixture.engine.auction_indicative(2).imbalance_side, Aggressor::Sell);

    fixture.apply(SetPhase{9, 0, Phase::Closed});
    CHECK_EQ(order_ids_of_events(fixture.events, EventType::Canceled), (std::vector<OrderId>{2, 6, 4}));
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{1});
    CHECK(fixture.engine.find_order(OrderId{3}).has_value());
    for (InstrumentId instrument : {InstrumentId{1}, InstrumentId{3}}) {
        CHECK_EQ(fixture.engine.auction_indicative(instrument).imbalance_qty, std::uint64_t{0});
    }
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_mass_cancel, cancels_one_session_in_ascending_order_id) {
    ArenaEngineFixture fixture(Phase::Continuous, 2);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5, 1));   // 1
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 150, 5, 2));  // 2
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 5, 1));     // 3
    fixture.apply(limit_order(kAliceSession, 3, Side::SELL, 160, 5, 1));  // 4
    fixture.apply(ReplaceOrder{5, kAliceSession, 1, 0, 90, 5, false});    // order 1 becomes 5
    const MassCancel mass{6, kAliceSession, 0, 444, false};
    fixture.apply(mass);
    REQUIRE_EQ(fixture.events.size(), std::size_t{4});
    CHECK_EQ(order_ids_of_events(fixture.events, EventType::Canceled), (std::vector<OrderId>{2, 4, 5}));
    CHECK_EQ(fixture.events[0].instrument, InstrumentId{2});
    CHECK_EQ(fixture.events[1].instrument, InstrumentId{1});
    for (std::size_t i = 0; i < 3; ++i) {
        const CanceledEvent& canceled = fixture.events[i].canceled;
        CHECK_EQ(canceled.reason, CancelReason::MassCancel);
        CHECK_EQ(canceled.request_cl_ord_id, ClOrdId{444});
        CHECK_EQ(canceled.session, kAliceSession);
        CHECK_EQ(canceled.was_resting, true);
        CHECK_EQ(canceled.canceled_qty, Quantity{5});
        CHECK_EQ(fixture.events[i].ts, mass.ts);
    }
    const Event& done = fixture.events[3];
    REQUIRE_EQ(done.type, EventType::MassCancelDone);
    CHECK_EQ(done.instrument, InstrumentId{0});
    CHECK_EQ(done.ts, mass.ts);
    CHECK_EQ(done.mass_cancel_done.session, kAliceSession);
    CHECK_EQ(done.mass_cancel_done.request_cl_ord_id, ClOrdId{444});
    CHECK_EQ(done.mass_cancel_done.canceled_count, 3u);
    CHECK_EQ(done.mass_cancel_done.reason, CancelReason::MassCancel);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{1});
    CHECK(fixture.engine.find_order(kBobSession, 1).has_value());
    check_book_invariants(fixture.engine);
}

TEST_CASE(arena_mass_cancel, instrument_filter_disconnect_reason_and_every_phase) {
    for (Phase phase : {Phase::Continuous, Phase::PreOpen, Phase::Halted, Phase::PreClose, Phase::Closed}) {
        ArenaEngineFixture fixture(Phase::PreOpen, 2);
        fixture.apply(with_tif(limit_order(kAliceSession, 1, Side::BUY, 100, 5, 1), TimeInForce::GTC));
        fixture.apply(with_tif(limit_order(kAliceSession, 2, Side::BUY, 100, 5, 2), TimeInForce::GTC));
        fixture.apply(with_tif(limit_order(kAliceSession, 3, Side::SELL, 110, 5, 2), TimeInForce::GTC));
        fixture.apply(SetPhase{1, 0, phase});
        fixture.apply(MassCancel{2, kAliceSession, 2, 9, true});
        REQUIRE_EQ(fixture.events.size(), std::size_t{3});
        CHECK_EQ(fixture.events[0].canceled.order_id, OrderId{2});
        CHECK_EQ(fixture.events[0].canceled.reason, CancelReason::CancelOnDisconnect);
        CHECK_EQ(fixture.events[1].canceled.order_id, OrderId{3});
        CHECK_EQ(fixture.events[2].instrument, InstrumentId{2});
        CHECK_EQ(fixture.events[2].mass_cancel_done.canceled_count, 2u);
        CHECK_EQ(fixture.events[2].mass_cancel_done.reason, CancelReason::CancelOnDisconnect);
        CHECK(fixture.engine.find_order(OrderId{1}).has_value());

        // Unknown instrument and empty session: only MassCancelDone.
        fixture.apply(MassCancel{3, kAliceSession, 9, 10, false});
        CHECK_EQ(fixture.only_event().instrument, InstrumentId{9});
        CHECK_EQ(fixture.only_event().mass_cancel_done.canceled_count, 0u);
        fixture.apply(MassCancel{4, kCarolSession, 0, 11, false});
        CHECK_EQ(fixture.only_event().mass_cancel_done.canceled_count, 0u);
        CHECK_EQ(fixture.only_event().mass_cancel_done.session, kCarolSession);
    }
}
