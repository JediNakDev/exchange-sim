#include "engine_harness.hpp"
#include <random>
using namespace engine_harness;

static void apply(MatchingEngine& e, const Command& c, EventBuffer& out) {
    out.clear();
    REQUIRE(e.handle(c, out) == HandleResult::Applied);
}

TEST_CASE(engine_cancel, reductions_preserve_fifo_every_phase) {
    for (Phase phase : {Phase::Continuous, Phase::PreOpen, Phase::PreClose, Phase::Halted, Phase::Closed}) {
        Harness h;
        auto order = h.limit(1,1,Side::BUY,1000,100); order.tif = TimeInForce::GTC;
        h.send(order); order.cl_ord_id = 2; h.send(order);
        h.send(h.limit(2,1,Side::SELL,1000,30));
        h.set_phase(phase);
        EventBuffer out(16);
        CancelOrder c{99,1,1,88,50};
        apply(h.engine(), c, out);
        REQUIRE_EQ(out.size(), 1u);
        check_canceled(out[0],1,1,88,Side::BUY,1000,50,30,CancelReason::UserRequested,true);
        CHECK_EQ(out[0].canceled.leaves_qty,20u);
        CHECK_EQ(out[0].ts,99u);
        CHECK_EQ(out[0].instrument,1u);
        CHECK_EQ(out[0].canceled.session,1u);
        CHECK_EQ(h.fifo(Side::BUY,1000),(std::vector<OrderId>{1,2}));
        CHECK_EQ(h.engine().find_order(1)->qty,50u);
        c.reduce_to_total_qty = 500; apply(h.engine(),c,out); CHECK(out.empty());
        c.reduce_to_total_qty = 30; apply(h.engine(),c,out);
        CHECK_EQ(out[0].canceled.canceled_qty,20u);
        CHECK_EQ(out[0].canceled.leaves_qty,0u);
        CHECK(!h.engine().find_order(OrderId{1}));
    }
}

TEST_CASE(engine_replace, retention_loss_tokens_and_cumulative_quantity) {
    Harness h;
    h.rest(1,1,Side::BUY,1000,100); h.rest(1,2,Side::BUY,1000,100);
    h.send(h.limit(2,1,Side::SELL,1000,30));
    h.replace(1,1,3,1000,80);
    REQUIRE_EQ(h.event_count(),1u);
    const auto r = h.event(0).replaced;
    CHECK(r.priority_kept); CHECK_EQ(r.new_order_id,1u); CHECK_EQ(r.orig_cl_ord_id,1u);
    CHECK_EQ(r.cl_ord_id,3u); CHECK_EQ(r.old_leaves_qty,70u); CHECK_EQ(r.leaves_qty,50u);
    CHECK_EQ(r.cum_qty,30u); CHECK_EQ(r.qty,80u); CHECK_EQ(r.old_price,1000);
    CHECK(!h.engine().find_order(1,1));
    CHECK_EQ(h.fifo(Side::BUY,1000),(std::vector<OrderId>{1,2}));
    EventBuffer out(16);
    ReplaceOrder c{8,1,3,4,1000,80,true}; apply(h.engine(),c,out);
    REQUIRE_EQ(out.size(),2u); CHECK(!out[0].replaced.priority_kept);
    CHECK_EQ(out[0].replaced.new_order_id,4u);
    CHECK_EQ(h.fifo(Side::BUY,1000),(std::vector<OrderId>{2,4}));
    h.replace(1,4,0,1000,30);
    REQUIRE_EQ(h.event_count(),1u);
    check_canceled(h.event(0),4,4,0,Side::BUY,1000,50,30,CancelReason::UserRequested,true);
}

TEST_CASE(engine_replace, validation_order_and_crossing) {
    Harness h;
    h.replace(1,1,kMaxClOrdId+1,3,0);
    check_rejected(h.event(0),CommandType::Replace,RejectReason::UnknownOrder,1,kMaxClOrdId+1,1,0,0);
    h.rest(1,1,Side::BUY,1000,10); h.rest(1,2,Side::SELL,2000,10);
    h.replace(1,1,kMaxClOrdId+1,3,0); CHECK_EQ(h.event(0).rejected.reason,RejectReason::InvalidClOrdId);
    h.replace(1,1,2,3,0); CHECK_EQ(h.event(0).rejected.reason,RejectReason::DuplicateClOrdId);
    h.replace(1,1,3,3,0); CHECK_EQ(h.event(0).rejected.reason,RejectReason::InvalidQuantity);
    h.replace(1,1,3,3,10); CHECK_EQ(h.event(0).rejected.reason,RejectReason::InvalidPrice);
    h.set_phase(Phase::Halted); h.replace(1,1,3,1000,10);
    CHECK_EQ(h.event(0).rejected.reason,RejectReason::InstrumentHalted);
    h.set_phase(Phase::Continuous); h.replace(1,1,3,2000,15);
    REQUIRE_EQ(h.event_count(),3u);
    CHECK_EQ(h.event(0).type,EventType::Replaced); CHECK_EQ(h.event(1).type,EventType::Fill);
    CHECK_EQ(h.event(2).rested.leaves_qty,5u);
    h.cancel(1,99,77);
    check_rejected(h.event(0),CommandType::Cancel,RejectReason::UnknownOrder,1,77,99,0,0);
}

TEST_CASE(engine_mass_cancel, ordered_after_replacements_and_filtered) {
    Harness h;
    h.rest(1,1,Side::BUY,1000,1); h.rest(2,1,Side::BUY,1000,1); h.rest(1,2,Side::BUY,900,1);
    h.replace(1,1,3,800,1);
    h.mass_cancel(1,999,77); CHECK_EQ(h.event(0).mass_cancel_done.canceled_count,0u);
    h.mass_cancel(1,0,77,true);
    REQUIRE_EQ(h.event_count(),3u);
    CHECK_EQ(h.event(0).canceled.order_id,3u); CHECK_EQ(h.event(1).canceled.order_id,4u);
    for (std::size_t i=0;i<2;++i) { CHECK_EQ(h.event(i).canceled.reason,CancelReason::CancelOnDisconnect); CHECK_EQ(h.event(i).canceled.request_cl_ord_id,77u); }
    CHECK_EQ(h.event(2).mass_cancel_done.canceled_count,2u); CHECK_EQ(h.event(2).instrument,0u);
    CHECK_EQ(h.engine().live_order_count(),1u);
}

TEST_CASE(engine_auction, halted_cross_summary_ids_and_expiration) {
    for (Phase target : {Phase::Continuous,Phase::Closed}) {
        Harness h(Phase::PreOpen);
        h.rest(1,1,Side::BUY,1200,15); h.rest(1,2,Side::SELL,1000,10);
        auto gtc=h.limit(1,3,Side::BUY,900,7); gtc.tif=TimeInForce::GTC; h.send(gtc);
        h.set_phase(Phase::Halted); CHECK(!h.engine().last_trade_price(1));
        auto ind=h.engine().auction_indicative(1);
        CHECK(ind.crossed); CHECK_EQ(ind.price,1200); CHECK_EQ(ind.paired_qty,10u);
        h.set_phase(target);
        REQUIRE(h.event_count()>=3);
        check_fill(h.event(0),1200,10,Aggressor::None,1,5,10,2,0,10);
        CHECK_EQ(h.event(1).type,EventType::AuctionSummary);
        CHECK_EQ(h.event(1).auction_summary.match_id,2u);
        CHECK_EQ(h.event(1).auction_summary.matched_qty,10u);
        if(target==Phase::Closed) {
            CHECK_EQ(h.event(2).canceled.reason,CancelReason::EndOfDay);
            CHECK_EQ(h.event(2).canceled.canceled_qty,5u);
            CHECK_EQ(h.engine().live_order_count(),1u);
        }
        CHECK_EQ(h.event(h.event_count()-1).type,EventType::PhaseChanged);
        CHECK_EQ(h.engine().last_trade_price(1).value(),1200);
        CHECK(!h.engine().best_ask(1));
    }
}

TEST_CASE(engine_auction, tie_breaks_and_large_volume) {
    // Symmetric volume and imbalance: nearest reference, then lower price.
    for (Price ref : {Price{1000},Price{1100},Price{1200}}) {
        MatchingEngine e({1,8}); auto cfg=standard_instrument_config("A",Phase::PreClose);
        cfg.reference_price=ref; cfg.max_order_qty=UINT32_MAX;
        REQUIRE_EQ(e.add_instrument(cfg),1u); EventBuffer out(32);
        NewOrder c{1,1,1,9,1,Side::BUY,OrdType::Limit,TimeInForce::GTC,StpMode::CancelBoth,false,1200,UINT32_MAX};
        apply(e,c,out); c.cl_ord_id=2; apply(e,c,out);
        c.side=Side::SELL; c.price=1000; c.cl_ord_id=3; apply(e,c,out); c.cl_ord_id=4; apply(e,c,out);
        auto ind=e.auction_indicative(1);
        CHECK_EQ(ind.price,ref==1200?1200:1000); CHECK_EQ(ind.paired_qty,2ull*UINT32_MAX);
        apply(e,SetPhase{9,1,Phase::Continuous},out);
        REQUIRE_EQ(out.size(),4u); CHECK_EQ(out[0].fill.qty,UINT32_MAX);
        CHECK_EQ(out[2].auction_summary.match_id,3u); CHECK_EQ(e.live_order_count(),0u);
    }
}

TEST_CASE(engine_auction, volume_then_imbalance_precede_reference) {
    Harness h(Phase::PreOpen);
    h.rest(1,1,Side::BUY,1300,10); h.rest(1,2,Side::BUY,1100,20);
    h.rest(1,3,Side::SELL,1000,15); h.rest(1,4,Side::SELL,1200,20);
    auto i=h.engine().auction_indicative(1);
    CHECK_EQ(i.price,1100); CHECK_EQ(i.paired_qty,15u); CHECK_EQ(i.imbalance_qty,15u);
    h.set_phase(Phase::Continuous); CHECK(!h.engine().auction_indicative(1).crossed);
    CHECK(h.engine().best_bid(1).value()<h.engine().best_ask(1).value());
}

TEST_CASE(engine_queries, one_sided_indicative_and_empty_phase_batches) {
    Harness h;
    h.rest(1,1,Side::BUY,1000,42);
    auto i=h.engine().auction_indicative(1);
    CHECK(!i.crossed); CHECK_EQ(i.price,0); CHECK_EQ(i.paired_qty,0u);
    CHECK_EQ(i.imbalance_qty,42u); CHECK_EQ(i.imbalance_side,Aggressor::Buy);
    h.set_phase(Phase::Continuous); CHECK_EQ(h.event_count(),0u);
    h.set_phase(static_cast<Phase>(255)); CHECK_EQ(h.event(0).rejected.reason,RejectReason::InvalidField);
    h.set_phase(Phase::Closed,999); CHECK_EQ(h.event(0).rejected.reason,RejectReason::UnknownInstrument);
    MatchingEngine e({2,4}); REQUIRE_EQ(e.add_instrument(standard_instrument_config("A")),1u);
    REQUIRE_EQ(e.add_instrument(standard_instrument_config("B")),2u);
    EventBuffer out(16); apply(e,SetPhase{5,0,Phase::Closed},out);
    REQUIRE_EQ(out.size(),2u); CHECK_EQ(out[0].instrument,1u); CHECK_EQ(out[1].instrument,2u);
}

TEST_CASE(engine_ladder, deep_search_shifts_and_fifo_deletion) {
    Harness h(Phase::Continuous,256);
    for(ClOrdId t=1;t<=100;++t) h.rest(1,t,Side::BUY,static_cast<Price>(t*100),1);
    h.rest(1,101,Side::BUY,100,2);
    h.replace(1,101,102,100,1);
    CHECK_EQ(h.fifo(Side::BUY,100),(std::vector<OrderId>{1,101}));
    h.cancel(1,1); CHECK_EQ(h.fifo(Side::BUY,100),(std::vector<OrderId>{101}));
    h.cancel(1,50); h.cancel(1,100); h.cancel(1,102);
    auto levels=h.levels(Side::BUY); REQUIRE_EQ(levels.size(),97u);
    CHECK_EQ(levels.front().price,9900); CHECK_EQ(levels.back().price,200);
    h.rest(1,103,Side::BUY,100,1); CHECK_EQ(h.levels(Side::BUY).back().price,100);
    h.send(h.market(2,1,Side::SELL,98)); CHECK_EQ(h.engine().live_order_count(),0u);
}

TEST_CASE(engine_output, bounds_cover_worst_case_sweep_mass_cancel_and_phase) {
    for(unsigned kind=0;kind<3;++kind) {
        MatchingEngine e({2,8}); auto cfg=standard_instrument_config("A",Phase::PreOpen);
        REQUIRE_EQ(e.add_instrument(cfg),1u); cfg.symbol=Symbol("B"); REQUIRE_EQ(e.add_instrument(cfg),2u);
        EventBuffer setup(64);
        for(ClOrdId i=1;i<=7;++i) {
            NewOrder c{1,static_cast<InstrumentId>(kind==2?1+i%2:1),1,0,i,
                kind==2&&i%2?Side::BUY:Side::SELL,OrdType::Limit,TimeInForce::Day,StpMode::None,false,
                kind==2&&i%2?1200:1000,1}; apply(e,c,setup);
        }
        if(kind==0) apply(e,SetPhase{2,0,Phase::Continuous},setup);
        // The contract guarantees Applied only when free output reaches
        // required_event_capacity(config). Tighter preflight bounds are optional,
        // so the worst-case event counts are checked against the output instead.
        const std::size_t bound=kind==0?9:kind==1?8:19;
        EventBuffer out(required_event_capacity(EngineConfig{2,8}));
        Command cmd;
        if(kind==0) cmd=NewOrder{3,1,2,0,1,Side::BUY,OrdType::Market,TimeInForce::IOC,StpMode::None,false,999,8};
        else if(kind==1) cmd=MassCancel{3,1,0,99,false};
        else cmd=SetPhase{3,0,Phase::Closed};
        REQUIRE(e.handle(cmd,out)==HandleResult::Applied);
        CHECK(out.size()<=bound); CHECK_EQ(e.live_order_count(),0u);
        if(kind==0) CHECK_EQ(out.size(),9u);
        if(kind==1) CHECK_EQ(out.size(),8u);
    }
}

TEST_CASE(engine_replace, post_only_and_closed_reject_before_cancel) {
    Harness h;
    auto n=h.limit(1,1,Side::BUY,1000,10); n.post_only=true; n.tif=TimeInForce::GTC; h.send(n);
    h.rest(2,1,Side::SELL,2000,1);
    h.replace(1,1,2,2000,10); CHECK_EQ(h.event(0).rejected.reason,RejectReason::PostOnlyWouldCross);
    h.set_phase(Phase::Closed);
    h.replace(1,1,2,1000,10); CHECK_EQ(h.event(0).rejected.reason,RejectReason::MarketClosed);
    CHECK_EQ(h.event(0).rejected.order_id,1u); CHECK_EQ(h.event(0).instrument,1u);
    h.cancel(1,1,99); CHECK_EQ(h.event(0).canceled.request_cl_ord_id,99u);
}

TEST_CASE(engine_auction, minimum_imbalance_wins_before_reference_distance) {
    MatchingEngine e({1,8}); auto cfg=standard_instrument_config("A",Phase::PreOpen); cfg.reference_price=1000;
    REQUIRE_EQ(e.add_instrument(cfg),1u); EventBuffer out(32);
    NewOrder c{1,1,1,0,1,Side::BUY,OrdType::Limit,TimeInForce::Day,StpMode::None,false,1000,10};
    apply(e,c,out); c.cl_ord_id=2; c.price=1100; apply(e,c,out); c.cl_ord_id=3; c.price=1200; apply(e,c,out);
    c.cl_ord_id=4; c.side=Side::SELL; c.price=1000; c.qty=20; apply(e,c,out);
    c.cl_ord_id=5; c.price=1200; apply(e,c,out);
    auto ind=e.auction_indicative(1); CHECK_EQ(ind.price,1100); CHECK_EQ(ind.paired_qty,20u); CHECK_EQ(ind.imbalance_qty,0u);
    apply(e,SetPhase{9,1,Phase::Continuous},out);
    CHECK_EQ(out[0].fill.buy.order_id,3u); CHECK_EQ(out[1].fill.buy.order_id,2u);
    CHECK_EQ(out[2].auction_summary.price,1100);
}

TEST_CASE(engine_cancel, reductions_ignore_lot_grid_and_do_not_increase) {
    MatchingEngine e({1,8}); auto cfg=standard_instrument_config(); cfg.lot_size=10;
    REQUIRE_EQ(e.add_instrument(cfg),1u); EventBuffer out(32);
    NewOrder c{1,1,1,0,1,Side::BUY,OrdType::Limit,TimeInForce::Day,StpMode::None,false,1000,100}; apply(e,c,out);
    apply(e,CancelOrder{2,1,1,77,53},out);
    REQUIRE_EQ(out.size(),1u); CHECK_EQ(out[0].canceled.leaves_qty,53u); CHECK_EQ(out[0].canceled.canceled_qty,47u);
    apply(e,CancelOrder{3,1,1,78,53},out); CHECK(out.empty());
    CHECK_EQ(e.find_order(OrderId{1})->qty,53u);
}

TEST_CASE(engine_instrument, canonical_config_validation) {
    MatchingEngine e({1,4});
    for(unsigned k=0;k<5;++k) {
        auto c=standard_instrument_config();
        if(k==0) c.reference_price=0;
        if(k==1) c.reference_price=c.max_price+100;
        if(k==2) c.reference_price=105;
        if(k==3) c.initial_phase=static_cast<Phase>(255);
        if(k==4) { c.lot_size=100; c.max_order_qty=50; }
        CHECK_EQ(e.add_instrument(c),0u);
    }
}
