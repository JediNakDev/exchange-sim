// CancelOrder (including reduce_to_total_qty) and ReplaceOrder.

#include <test/exsim/engine/engine_fixture.hpp>

#include <limits>

using namespace engine_fixture;

namespace {

CancelOrder cancel_request(SessionId session, ClOrdId target_token, ClOrdId request_token = 0,
                           Quantity reduce_to_total_qty = 0) {
    return CancelOrder{900 + target_token, session, target_token, request_token, reduce_to_total_qty};
}

ReplaceOrder replace_request(SessionId session, ClOrdId target_token, ClOrdId new_token, Price price, Quantity qty,
                             bool force_priority_loss = false) {
    return ReplaceOrder{700 + target_token, session, target_token, new_token, price, qty, force_priority_loss};
}

void check_replace_rejection(const EngineFixture& fixture, const ReplaceOrder& command, RejectReason reason,
                             OrderId target_order_id, InstrumentId target_instrument) {
    const Event& event = fixture.only_event();
    REQUIRE_EQ(event.type, EventType::Rejected);
    CHECK_EQ(event.ts, command.ts);
    CHECK_EQ(event.instrument, target_instrument);
    CHECK_EQ(event.rejected.cmd, CommandType::Replace);
    CHECK_EQ(event.rejected.reason, reason);
    CHECK_EQ(event.rejected.session, command.session);
    CHECK_EQ(event.rejected.cl_ord_id, command.cl_ord_id);
    CHECK_EQ(event.rejected.orig_cl_ord_id, command.orig_cl_ord_id);
    CHECK_EQ(event.rejected.order_id, target_order_id);
}

}  // namespace

TEST_CASE(engine_cancel, unknown_order_rejection_fields) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 10));
    for (const CancelOrder& command : {cancel_request(kAliceSession, 2, 55), cancel_request(kBobSession, 1, 56),
                                       cancel_request(0, 1, 57), cancel_request(kAliceSession, 0, 58),
                                       cancel_request(kAliceSession, kMaxClOrdId + 1, 59)}) {
        fixture.apply(command);
        const Event& event = fixture.only_event();
        REQUIRE_EQ(event.type, EventType::Rejected);
        CHECK_EQ(event.instrument, InstrumentId{0});
        CHECK_EQ(event.ts, command.ts);
        CHECK_EQ(event.rejected.cmd, CommandType::Cancel);
        CHECK_EQ(event.rejected.reason, RejectReason::UnknownOrder);
        CHECK_EQ(event.rejected.session, command.session);
        CHECK_EQ(event.rejected.cl_ord_id, command.cl_ord_id);
        CHECK_EQ(event.rejected.orig_cl_ord_id, command.orig_cl_ord_id);
        CHECK_EQ(event.rejected.order_id, OrderId{0});
    }
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{1});
}

TEST_CASE(engine_cancel, full_cancel_in_every_phase_removes_from_the_fifo) {
    for (Phase phase : {Phase::Continuous, Phase::PreOpen, Phase::PreClose, Phase::Halted, Phase::Closed}) {
        EngineFixture fixture;
        NewOrder gtc = with_tif(limit_order(kAliceSession, 1, Side::SELL, 150, 10), TimeInForce::GTC);
        fixture.apply(gtc);
        fixture.apply(with_tif(limit_order(kAliceSession, 2, Side::SELL, 150, 10), TimeInForce::GTC));
        fixture.apply(with_tif(limit_order(kAliceSession, 3, Side::SELL, 150, 10), TimeInForce::GTC));
        fixture.apply(limit_order(kBobSession, 1, Side::BUY, 150, 4));  // partially fills order 1
        fixture.apply(SetPhase{5, 1, phase});

        const CancelOrder middle = cancel_request(kAliceSession, 2, 77);
        fixture.apply(middle);
        const Event& event = fixture.only_event();
        REQUIRE_EQ(event.type, EventType::Canceled);
        CHECK_EQ(event.instrument, InstrumentId{1});
        CHECK_EQ(event.ts, middle.ts);
        CHECK_EQ(event.canceled.order_id, OrderId{2});
        CHECK_EQ(event.canceled.cl_ord_id, ClOrdId{2});
        CHECK_EQ(event.canceled.request_cl_ord_id, ClOrdId{77});
        CHECK_EQ(event.canceled.session, kAliceSession);
        CHECK_EQ(event.canceled.side, Side::SELL);
        CHECK_EQ(event.canceled.price, Price{150});
        CHECK_EQ(event.canceled.canceled_qty, Quantity{10});
        CHECK_EQ(event.canceled.cum_qty, Quantity{0});
        CHECK_EQ(event.canceled.reason, CancelReason::UserRequested);
        CHECK_EQ(event.canceled.was_resting, true);
        CHECK_EQ(event.canceled.leaves_qty, Quantity{0});
        CHECK_EQ(level_fifo(fixture.engine, 1, Side::SELL, 150), (std::vector<OrderId>{1, 3}));

        fixture.apply(cancel_request(kAliceSession, 1));
        CHECK_EQ(fixture.only_event().canceled.canceled_qty, Quantity{6});
        CHECK_EQ(fixture.only_event().canceled.cum_qty, Quantity{4});
        fixture.apply(cancel_request(kAliceSession, 3));
        CHECK(!fixture.engine.best_ask(1).has_value());
        CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
        check_book_invariants(fixture.engine);
    }
}

TEST_CASE(engine_cancel, reduce_to_total_keeps_id_token_and_fifo_position) {
    for (Phase phase : {Phase::Continuous, Phase::PreOpen, Phase::PreClose, Phase::Halted, Phase::Closed}) {
        EngineFixture fixture;
        fixture.apply(with_tif(limit_order(kAliceSession, 1, Side::BUY, 100, 20), TimeInForce::GTC));
        fixture.apply(with_tif(limit_order(kAliceSession, 2, Side::BUY, 100, 5), TimeInForce::GTC));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 6));  // order 1: cum 6, leaves 14
        fixture.apply(SetPhase{5, 1, phase});

        // Total 13 with cum 6 leaves 7: 7 canceled, order stays at the head.
        const CancelOrder reduction = cancel_request(kAliceSession, 1, 31, 13);
        fixture.apply(reduction);
        const Event& event = fixture.only_event();
        REQUIRE_EQ(event.type, EventType::Canceled);
        CHECK_EQ(event.canceled.order_id, OrderId{1});
        CHECK_EQ(event.canceled.request_cl_ord_id, ClOrdId{31});
        CHECK_EQ(event.canceled.canceled_qty, Quantity{7});
        CHECK_EQ(event.canceled.cum_qty, Quantity{6});
        CHECK_EQ(event.canceled.leaves_qty, Quantity{7});
        CHECK_EQ(event.canceled.was_resting, true);
        CHECK_EQ(event.canceled.reason, CancelReason::UserRequested);
        CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 100), (std::vector<OrderId>{1, 2}));
        const auto reduced = fixture.engine.find_order(kAliceSession, 1);
        REQUIRE(reduced.has_value());
        CHECK_EQ(reduced->qty, Quantity{13});
        CHECK_EQ(reduced->leaves_qty, Quantity{7});
        CHECK_EQ(full_depth(fixture.engine, 1, Side::BUY).front().qty, std::uint64_t{12});

        // Target at or above the current leaves: no-op, no event.
        fixture.apply(cancel_request(kAliceSession, 1, 32, 13));
        CHECK(fixture.events.empty());
        fixture.apply(cancel_request(kAliceSession, 1, 33, 500));
        CHECK(fixture.events.empty());

        // Total below cum: target clamps to zero and the order is removed.
        fixture.apply(cancel_request(kAliceSession, 1, 34, 2));
        CHECK_EQ(fixture.only_event().canceled.canceled_qty, Quantity{7});
        CHECK_EQ(fixture.only_event().canceled.leaves_qty, Quantity{0});
        CHECK(!fixture.engine.find_order(OrderId{1}).has_value());
        CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 100), std::vector<OrderId>{2});
        check_book_invariants(fixture.engine);
    }
}

TEST_CASE(engine_cancel, reduction_ignores_lot_size_and_preserves_matching_priority) {
    EngineFixture fixture;
    InstrumentConfig lots = small_instrument_config("LOTS");
    lots.lot_size = 10;
    REQUIRE_EQ(fixture.engine.add_instrument(lots), InstrumentId{2});
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 30, 2));
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 100, 30, 2));
    fixture.apply(cancel_request(kAliceSession, 1, 0, 7));  // 7 is not a lot multiple: still applied
    CHECK_EQ(fixture.only_event().canceled.leaves_qty, Quantity{7});
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 10, 2));
    REQUIRE_EQ(fixture.events.size(), std::size_t{3});
    CHECK_EQ(fixture.events[1].fill.sell.order_id, OrderId{1});
    CHECK_EQ(fixture.events[1].fill.qty, Quantity{7});
    CHECK_EQ(fixture.events[2].fill.sell.order_id, OrderId{2});
    CHECK_EQ(fixture.events[2].fill.qty, Quantity{3});
}

TEST_CASE(engine_replace, rejections_in_spec_order_echo_the_target) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 10));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 90, 10));

    const ReplaceOrder unknown = replace_request(kAliceSession, 9, 0, 100, 10);
    fixture.apply(unknown);
    check_replace_rejection(fixture, unknown, RejectReason::UnknownOrder, 0, 0);

    const ReplaceOrder token_too_large = replace_request(kAliceSession, 1, kMaxClOrdId + 1, 7, 0);
    fixture.apply(token_too_large);
    check_replace_rejection(fixture, token_too_large, RejectReason::InvalidClOrdId, 1, 1);

    const ReplaceOrder duplicate = replace_request(kAliceSession, 1, 2, 7, 0);
    fixture.apply(duplicate);
    check_replace_rejection(fixture, duplicate, RejectReason::DuplicateClOrdId, 1, 1);

    const ReplaceOrder bad_qty = replace_request(kAliceSession, 1, 1, 7, 0);  // same token is fine
    fixture.apply(bad_qty);
    check_replace_rejection(fixture, bad_qty, RejectReason::InvalidQuantity, 1, 1);

    const ReplaceOrder too_much = replace_request(kAliceSession, 1, 0, 7, 1001);
    fixture.apply(too_much);
    check_replace_rejection(fixture, too_much, RejectReason::InvalidQuantity, 1, 1);

    const ReplaceOrder bad_price = replace_request(kAliceSession, 1, 0, 7, 10);
    fixture.apply(bad_price);
    check_replace_rejection(fixture, bad_price, RejectReason::InvalidPrice, 1, 1);

    fixture.apply(SetPhase{1, 1, Phase::Halted});
    const ReplaceOrder halted = replace_request(kAliceSession, 1, 0, 100, 10);
    fixture.apply(halted);
    check_replace_rejection(fixture, halted, RejectReason::InstrumentHalted, 1, 1);

    fixture.apply(SetPhase{2, 1, Phase::Closed});  // DAY orders expire: rest a GTC order to target
    fixture.apply(SetPhase{3, 1, Phase::PreOpen});
    fixture.apply(with_tif(limit_order(kAliceSession, 3, Side::BUY, 100, 10), TimeInForce::GTC));
    fixture.apply(SetPhase{4, 1, Phase::Closed});
    const ReplaceOrder closed = replace_request(kAliceSession, 3, 0, 100, 10);
    fixture.apply(closed);
    check_replace_rejection(fixture, closed, RejectReason::MarketClosed, 3, 1);
}

TEST_CASE(engine_replace, post_only_replace_cannot_cross_in_continuous) {
    EngineFixture fixture;
    NewOrder maker = limit_order(kAliceSession, 1, Side::BUY, 100, 10);
    maker.post_only = true;
    fixture.apply(maker);
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 120, 10));
    const ReplaceOrder crossing = replace_request(kAliceSession, 1, 0, 120, 10);
    fixture.apply(crossing);
    check_replace_rejection(fixture, crossing, RejectReason::PostOnlyWouldCross, 1, 1);
    fixture.apply(replace_request(kAliceSession, 1, 0, 110, 10));
    CHECK_EQ(fixture.events[0].type, EventType::Replaced);

    // In a call phase a post-only replace may cross: it rests without matching.
    fixture.apply(SetPhase{1, 1, Phase::PreOpen});
    fixture.apply(replace_request(kAliceSession, 1, 0, 130, 10));
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[1].type, EventType::Rested);
    CHECK_EQ(*fixture.engine.best_bid(1), Price{130});
}

TEST_CASE(engine_replace, quantity_at_or_below_cum_cancels_with_the_old_token) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 10));
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 4));
    const ReplaceOrder shrink = replace_request(kAliceSession, 1, 55, 110, 4);
    fixture.apply(shrink);
    const Event& event = fixture.only_event();
    REQUIRE_EQ(event.type, EventType::Canceled);
    CHECK_EQ(event.ts, shrink.ts);
    CHECK_EQ(event.canceled.order_id, OrderId{1});
    CHECK_EQ(event.canceled.cl_ord_id, ClOrdId{1});
    CHECK_EQ(event.canceled.request_cl_ord_id, ClOrdId{55});
    CHECK_EQ(event.canceled.price, Price{100});
    CHECK_EQ(event.canceled.canceled_qty, Quantity{6});
    CHECK_EQ(event.canceled.cum_qty, Quantity{4});
    CHECK_EQ(event.canceled.reason, CancelReason::UserRequested);
    CHECK_EQ(event.canceled.was_resting, true);
    CHECK(!fixture.engine.find_order(kAliceSession, 1).has_value());
    CHECK(!fixture.engine.find_order(kAliceSession, 55).has_value());
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
}

TEST_CASE(engine_replace, priority_kept_updates_in_place_and_rekeys_the_token) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 10));
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 10));
    fixture.apply(limit_order(kCarolSession, 1, Side::SELL, 100, 3));  // order 1: cum 3, leaves 7

    // New total 9: leaves 6 <= 7 at the same price keeps priority.
    const ReplaceOrder shrink = replace_request(kAliceSession, 1, 44, 100, 9);
    fixture.apply(shrink);
    const Event& event = fixture.only_event();
    REQUIRE_EQ(event.type, EventType::Replaced);
    CHECK_EQ(event.ts, shrink.ts);
    CHECK_EQ(event.instrument, InstrumentId{1});
    CHECK_EQ(event.replaced.old_order_id, OrderId{1});
    CHECK_EQ(event.replaced.new_order_id, OrderId{1});
    CHECK_EQ(event.replaced.orig_cl_ord_id, ClOrdId{1});
    CHECK_EQ(event.replaced.cl_ord_id, ClOrdId{44});
    CHECK_EQ(event.replaced.session, kAliceSession);
    CHECK_EQ(event.replaced.side, Side::BUY);
    CHECK_EQ(event.replaced.old_price, Price{100});
    CHECK_EQ(event.replaced.price, Price{100});
    CHECK_EQ(event.replaced.old_leaves_qty, Quantity{7});
    CHECK_EQ(event.replaced.qty, Quantity{9});
    CHECK_EQ(event.replaced.leaves_qty, Quantity{6});
    CHECK_EQ(event.replaced.cum_qty, Quantity{3});
    CHECK_EQ(event.replaced.priority_kept, true);

    CHECK(!fixture.engine.find_order(kAliceSession, 1).has_value());
    const auto moved = fixture.engine.find_order(kAliceSession, 44);
    REQUIRE(moved.has_value());
    CHECK_EQ(moved->order_id, OrderId{1});
    CHECK_EQ(moved->qty, Quantity{9});
    CHECK_EQ(level_fifo(fixture.engine, 1, Side::BUY, 100), (std::vector<OrderId>{1, 2}));
    CHECK_EQ(full_depth(fixture.engine, 1, Side::BUY).front().qty, std::uint64_t{16});

    // The old token is free; an equal-leaves replace also keeps priority.
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 50, 1));
    CHECK_EQ(fixture.events[1].type, EventType::Rested);
    fixture.apply(replace_request(kAliceSession, 44, 0, 100, 9));
    CHECK_EQ(fixture.only_event().replaced.priority_kept, true);
    CHECK_EQ(fixture.only_event().replaced.cl_ord_id, ClOrdId{44});
    check_book_invariants(fixture.engine);
}

TEST_CASE(engine_replace, priority_lost_on_price_change_quantity_increase_or_force) {
    struct LossCase {
        Price new_price;
        Quantity new_qty;
        bool force;
    };
    for (const LossCase& loss : {LossCase{90, 10, false}, LossCase{100, 11, false}, LossCase{100, 10, true},
                                 LossCase{100, 5, true}}) {
        EngineFixture fixture;
        fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 10));
        fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 10));
        fixture.apply(limit_order(kBobSession, 2, Side::BUY, 90, 10));
        const ReplaceOrder replace = replace_request(kAliceSession, 1, 0, loss.new_price, loss.new_qty, loss.force);
        fixture.apply(replace);
        REQUIRE_EQ(fixture.events.size(), std::size_t{2});
        const Event& replaced = fixture.events[0];
        REQUIRE_EQ(replaced.type, EventType::Replaced);
        CHECK_EQ(replaced.replaced.old_order_id, OrderId{1});
        CHECK_EQ(replaced.replaced.new_order_id, OrderId{4});
        CHECK_EQ(replaced.replaced.cl_ord_id, ClOrdId{1});
        CHECK_EQ(replaced.replaced.old_price, Price{100});
        CHECK_EQ(replaced.replaced.price, loss.new_price);
        CHECK_EQ(replaced.replaced.old_leaves_qty, Quantity{10});
        CHECK_EQ(replaced.replaced.qty, loss.new_qty);
        CHECK_EQ(replaced.replaced.leaves_qty, loss.new_qty);
        CHECK_EQ(replaced.replaced.priority_kept, false);
        const Event& rested = fixture.events[1];
        REQUIRE_EQ(rested.type, EventType::Rested);
        CHECK_EQ(rested.rested.order_id, OrderId{4});
        CHECK_EQ(rested.rested.price, loss.new_price);
        CHECK_EQ(rested.rested.leaves_qty, loss.new_qty);
        CHECK(!fixture.engine.find_order(OrderId{1}).has_value());
        const std::vector<OrderId> fifo = level_fifo(fixture.engine, 1, Side::BUY, loss.new_price);
        CHECK_EQ(fifo.back(), OrderId{4});
        check_book_invariants(fixture.engine);
    }
}

TEST_CASE(engine_replace, priority_lost_order_matches_as_aggressor_in_continuous_only) {
    {
        EngineFixture fixture;
        fixture.apply(with_owner(limit_order(kAliceSession, 1, Side::BUY, 100, 10), kAliceFirm,
                                 StpMode::CancelResting));
        fixture.apply(with_owner(limit_order(kAliceSession, 2, Side::SELL, 120, 3), kAliceFirm));
        fixture.apply(with_owner(limit_order(kBobSession, 1, Side::SELL, 120, 4), kBobFirm));
        fixture.apply(limit_order(kCarolSession, 1, Side::SELL, 100, 2));  // order 1: cum 2, leaves 8
        // Raise to 120 with new token: own resting order is STP-canceled (the
        // replaced order's own mode), then it trades with Bob and rests.
        fixture.apply(replace_request(kAliceSession, 1, 8, 120, 12));
        REQUIRE_EQ(fixture.events.size(), std::size_t{4});
        CHECK_EQ(fixture.events[0].replaced.new_order_id, OrderId{5});
        CHECK_EQ(fixture.events[0].replaced.cum_qty, Quantity{2});
        CHECK_EQ(fixture.events[0].replaced.leaves_qty, Quantity{10});
        CHECK_EQ(fixture.events[1].canceled.order_id, OrderId{2});
        CHECK_EQ(fixture.events[1].canceled.reason, CancelReason::SelfTradePrevention);
        CHECK_EQ(fixture.events[2].fill.buy.order_id, OrderId{5});
        CHECK_EQ(fixture.events[2].fill.buy.cl_ord_id, ClOrdId{8});
        CHECK_EQ(fixture.events[2].fill.buy.cum_qty, Quantity{6});
        CHECK_EQ(fixture.events[2].fill.buy.leaves_qty, Quantity{6});
        CHECK_EQ(fixture.events[2].fill.aggressor, Aggressor::Buy);
        CHECK_EQ(fixture.events[3].rested.order_id, OrderId{5});
        CHECK_EQ(fixture.events[3].rested.leaves_qty, Quantity{6});
        const auto stored = fixture.engine.find_order(kAliceSession, 8);
        REQUIRE(stored.has_value());
        CHECK_EQ(stored->qty, Quantity{12});
        CHECK_EQ(stored->cum_qty, Quantity{6});
        check_book_invariants(fixture.engine);
    }
    {
        // Fully filled by the replace: Replaced then Fill, nothing else.
        EngineFixture fixture;
        fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 5));
        fixture.apply(replace_request(kAliceSession, 1, 0, 110, 5));
        REQUIRE_EQ(fixture.events.size(), std::size_t{2});
        CHECK_EQ(fixture.events[1].fill.price, Price{110});
        CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
    }
    for (Phase call_phase : {Phase::PreOpen, Phase::PreClose}) {
        EngineFixture fixture(call_phase);
        fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5));
        fixture.apply(limit_order(kBobSession, 1, Side::SELL, 110, 5));
        fixture.apply(replace_request(kAliceSession, 1, 0, 120, 5));
        REQUIRE_EQ(fixture.events.size(), std::size_t{2});
        CHECK_EQ(fixture.events[1].type, EventType::Rested);
        CHECK_EQ(*fixture.engine.best_bid(1), Price{120});
        CHECK_EQ(*fixture.engine.best_ask(1), Price{110});
    }
}

TEST_CASE(engine_replace, amendment_chain_conserves_quantity) {
    // Quantity conservation: executed + current leaves + canceled = initial + replacement deltas.
    EngineFixture fixture;
    std::uint64_t executed = 0;
    std::uint64_t canceled = 0;
    std::int64_t replacement_deltas = 0;
    std::uint64_t previous_total = 20;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 20));
    auto account = [&](const EventBuffer& events) {
        for (const Event& event : events) {
            if (event.type == EventType::Fill && event.fill.buy.session == kAliceSession) executed += event.fill.qty;
            if (event.type == EventType::Canceled && event.canceled.session == kAliceSession)
                canceled += event.canceled.canceled_qty;
            if (event.type == EventType::Replaced) {
                replacement_deltas += static_cast<std::int64_t>(event.replaced.qty) -
                                      static_cast<std::int64_t>(previous_total);
                previous_total = event.replaced.qty;
            }
        }
    };
    account(fixture.apply(limit_order(kBobSession, 1, Side::SELL, 100, 6)));
    account(fixture.apply(replace_request(kAliceSession, 1, 2, 100, 15)));
    account(fixture.apply(replace_request(kAliceSession, 2, 3, 110, 30)));
    account(fixture.apply(limit_order(kBobSession, 2, Side::SELL, 110, 4)));
    account(fixture.apply(cancel_request(kAliceSession, 3, 0, 25)));
    previous_total = 25;  // a reduction is a cancellation, not an amendment
    account(fixture.apply(replace_request(kAliceSession, 3, 0, 110, 28, true)));
    const auto live = fixture.engine.find_order(kAliceSession, 3);
    REQUIRE(live.has_value());
    CHECK_EQ(live->qty, Quantity{28});
    CHECK_EQ(live->cum_qty, Quantity{10});
    CHECK_EQ(canceled, std::uint64_t{5});
    CHECK_EQ(executed + live->leaves_qty + canceled, static_cast<std::uint64_t>(20 + replacement_deltas));
}
