#include <test/framework/test.hpp>
#include <engine/matching_engine.hpp>
#include <array>
#include <limits>
#include <random>
#include <vector>
using namespace exsim;
namespace {
constexpr EngineConfig cfg{2, 32};
struct Fixture {
    MatchingEngine e{cfg};
    EventBuffer out{required_event_capacity(cfg)};
    Fixture(Phase phase = Phase::Continuous) {
        InstrumentConfig c; c.symbol = Symbol("TEST"); c.min_price = 10; c.max_price = 200;
        c.tick_size = 10; c.reference_price = 100; c.initial_phase = phase;
        REQUIRE(e.add_instrument(c) == 1);
    }
    template<class T> void run(const T& c) { out.clear(); REQUIRE(e.handle(c, out) == HandleResult::Applied); }
};
NewOrder order(ClOrdId token, Side side = Side::BUY, Price price = 100, Quantity qty = 10) {
    NewOrder n; n.ts = token; n.instrument = 1; n.session = 1; n.cl_ord_id = token;
    n.side = side; n.price = price; n.qty = qty; return n;
}
// Field-based digest, including every active payload field. Never reads padding.
std::uint64_t digest(const EventBuffer& out) {
    std::uint64_t h = 1469598103934665603ULL;
    auto add = [&](auto value) { auto v = static_cast<std::uint64_t>(value); for (int i=0;i<8;++i) { h ^= v & 255; h *= 1099511628211ULL; v >>= 8; } };
    auto leg = [&](const FillLeg& l) { add(l.order_id); add(l.cl_ord_id); add(l.session); add(l.owner); add(l.leaves_qty); add(l.cum_qty); };
    add(out.size());
    for (const auto& e : out) {
        add(e.type); add(e.instrument); add(e.ts);
        switch(e.type) {
        case EventType::Accepted: { const auto& x=e.accepted; add(x.order_id);add(x.cl_ord_id);add(x.session);add(x.owner);add(x.side);add(x.ord_type);add(x.tif);add(x.stp);add(x.post_only);add(x.price);add(x.qty); break; }
        case EventType::Rejected: { const auto& x=e.rejected;add(x.cmd);add(x.reason);add(x.session);add(x.cl_ord_id);add(x.orig_cl_ord_id);add(x.order_id);break; }
        case EventType::Rested: { const auto& x=e.rested;add(x.order_id);add(x.cl_ord_id);add(x.session);add(x.side);add(x.price);add(x.leaves_qty);break; }
        case EventType::Fill: { const auto& x=e.fill;add(x.match_id);add(x.price);add(x.qty);add(x.aggressor);leg(x.buy);leg(x.sell);break; }
        case EventType::Canceled: { const auto& x=e.canceled;add(x.order_id);add(x.cl_ord_id);add(x.request_cl_ord_id);add(x.session);add(x.side);add(x.price);add(x.canceled_qty);add(x.cum_qty);add(x.reason);add(x.was_resting);add(x.leaves_qty);break; }
        case EventType::Replaced: { const auto& x=e.replaced;add(x.old_order_id);add(x.new_order_id);add(x.orig_cl_ord_id);add(x.cl_ord_id);add(x.session);add(x.side);add(x.old_price);add(x.price);add(x.old_leaves_qty);add(x.qty);add(x.leaves_qty);add(x.cum_qty);add(x.priority_kept);break; }
        case EventType::MassCancelDone: {const auto& x=e.mass_cancel_done;add(x.session);add(x.request_cl_ord_id);add(x.canceled_count);add(x.reason);break;}
        case EventType::PhaseChanged: add(e.phase_changed.old_phase);add(e.phase_changed.new_phase);break;
        case EventType::AuctionSummary: add(e.auction_summary.price);add(e.auction_summary.matched_qty);add(e.auction_summary.match_id);break;
        }
    } return h;
}
void invariants(const MatchingEngine& e) {
    std::size_t count = 0; OrderId last = 0;
    e.for_each_order([&](const OrderInfo& o) {
        CHECK(o.order_id > last); last=o.order_id; ++count;
        CHECK(o.leaves_qty > 0); CHECK_EQ(o.qty, o.leaves_qty + o.cum_qty);
        CHECK_EQ(e.find_order(o.order_id)->cl_ord_id, o.cl_ord_id);
    });
    CHECK_EQ(count,e.live_order_count());
    for (InstrumentId id=1;id<=e.instrument_count();++id) {
        if (e.phase(id)==Phase::Continuous || e.phase(id)==Phase::Closed) {
            auto b=e.best_bid(id),a=e.best_ask(id); CHECK(!a || !b || *b<*a);
        }
        for(auto side : {Side::BUY,Side::SELL}) {
            LevelInfo levels[20]; const auto n=e.depth(id,side,levels,20);
            for(std::size_t j=0;j<n;++j) {
                OrderId ids[32];auto m=e.level_orders(id,side,levels[j].price,ids,32);
                CHECK_EQ(m,levels[j].order_count); std::uint64_t sum=0;
                for(std::size_t k=0;k<m;++k) { auto o=e.find_order(ids[k]); REQUIRE(o);sum+=o->leaves_qty;if(k)CHECK(ids[k]>ids[k-1]); }
                CHECK_EQ(sum,levels[j].qty);
            }
        }
    }
}
}
TEST_CASE(arena_contract, retry_randomized_all_commands) {
    Fixture a(Phase::PreOpen),b(Phase::PreOpen); EventBuffer short_out(1);
    const Event marker{EventType::PhaseChanged,0,999,{.phase_changed={Phase::Closed,Phase::Halted}}};
    std::mt19937 rng(9304);
    for(unsigned i=0;i<12000;++i) {
        Command cmd;const auto token=1+rng()%48; const auto price=10*(1+rng()%20); const auto qty=1+rng()%30;
        switch(rng()%8) {
        case 0: cmd=CancelOrder{i,1,token,77,static_cast<Quantity>(rng()%20)};break;
        case 1: cmd=ReplaceOrder{i,1,token,static_cast<ClOrdId>(rng()%49),static_cast<Price>(price),static_cast<Quantity>(qty),bool(rng()%2)};break;
        case 2: cmd=MassCancel{i,static_cast<SessionId>(1+rng()%2),static_cast<InstrumentId>(rng()%3),66,bool(rng()%2)};break;
        case 3: cmd=SetPhase{i,static_cast<InstrumentId>(rng()%3),static_cast<Phase>(rng()%6)};break;
        default: {auto n=order(token,static_cast<Side>(rng()%3),price,qty);n.ts=i;n.owner=rng()%3;n.stp=static_cast<StpMode>(rng()%5);n.tif=static_cast<TimeInForce>(rng()%5);n.type=static_cast<OrdType>(rng()%3);n.post_only=rng()%2;cmd=n;break;}
        }
        short_out.clear(); REQUIRE(short_out.push(marker)); const auto before=digest(short_out);
        REQUIRE(a.e.handle(cmd,short_out)==HandleResult::InsufficientOutput); CHECK_EQ(digest(short_out),before);
        a.run(cmd);b.run(cmd);CHECK_EQ(digest(a.out),digest(b.out));invariants(a.e);
    }
}
TEST_CASE(arena_validation, new_validation_order_and_capacity) {
    Fixture f;
    std::array<RejectReason,9> expected{RejectReason::UnknownInstrument,RejectReason::InvalidClOrdId,RejectReason::DuplicateClOrdId,RejectReason::InvalidField,RejectReason::InvalidQuantity,RejectReason::InvalidPrice,RejectReason::MarketClosed,RejectReason::PostOnlyWouldCross,RejectReason::CapacityExceeded};
    for(unsigned i=0;i<9;++i) {
        Fixture x; x.run(order(1));auto n=order(2);
        switch(i) {
        case 0:n.instrument=2;n.session=0;break;
        case 1:n.session=0;n.qty=0;break;
        case 2:n.cl_ord_id=1;n.side=static_cast<Side>(3);break;
        case 3:n.side=static_cast<Side>(3);n.qty=0;break;
        case 4:n.qty=0;n.price=0;break;
        case 5:n.price=11;break;
        case 6:x.run(SetPhase{0,1,Phase::Closed});break;
        case 7:n.side=Side::SELL;n.post_only=true;break;
        case 8:for(unsigned k=2;k<=32;++k)x.run(order(k));n.cl_ord_id=33;break;
        }
        x.run(n); REQUIRE(x.out.size()==1);CHECK_EQ(x.out[0].rejected.reason,expected[i]);
        CHECK_EQ(x.out[0].rejected.cl_ord_id,n.cl_ord_id);CHECK_EQ(x.out[0].rejected.order_id,0u);
    }
}
TEST_CASE(arena_matching, fifo_sweep_market_and_fields) {
    Fixture f;f.run(order(1,Side::SELL,100,4));f.run(order(2,Side::SELL,100,6));f.run(order(3,Side::SELL,110,9));
    auto n=order(4,Side::BUY,-123,12);n.type=OrdType::Market;n.owner=7;n.tif=TimeInForce::GTC;f.run(n);
    REQUIRE(f.out.size()==4);CHECK_EQ(f.out[0].accepted.price,-123);
    for(unsigned j=1;j<=3;++j) {auto& e=f.out[j];CHECK_EQ(e.type,EventType::Fill);CHECK_EQ(e.fill.match_id,j);CHECK_EQ(e.fill.sell.cl_ord_id,j);CHECK_EQ(e.fill.buy.owner,7u);CHECK_EQ(e.ts,4u);}
    CHECK_EQ(f.out[3].fill.buy.cum_qty,12u);CHECK_EQ(f.out[3].fill.sell.leaves_qty,7u);CHECK_EQ(*f.e.last_trade_price(1),110);invariants(f.e);
    n=order(5,Side::BUY,999,20);n.type=OrdType::Market;f.run(n);CHECK_EQ(f.out[2].canceled.reason,CancelReason::ImmediateOrCancel);CHECK_EQ(f.out[2].canceled.price,999);CHECK_EQ(f.out[2].canceled.canceled_qty,13u);
}
TEST_CASE(arena_matching, stp_and_fok) {
    for(auto mode:{StpMode::None,StpMode::CancelResting,StpMode::CancelAggressor,StpMode::CancelBoth}) for(bool fok:{false,true}) {
        Fixture f;auto n=order(1,Side::SELL);n.owner=7;f.run(n);f.run(order(2,Side::SELL,100,10));
        n=order(3);n.owner=7;n.stp=mode;n.tif=fok?TimeInForce::FOK:TimeInForce::IOC;f.run(n);
        if(fok && (mode==StpMode::CancelAggressor || mode==StpMode::CancelBoth)) {REQUIRE(f.out.size()==2);CHECK_EQ(f.out[1].canceled.reason,CancelReason::FillOrKill);CHECK_EQ(f.e.live_order_count(),2u);}
        else if(mode==StpMode::CancelResting) {REQUIRE(f.out.size()==3);CHECK_EQ(f.out[1].canceled.cl_ord_id,1u);CHECK_EQ(f.out[2].fill.sell.cl_ord_id,2u);}
        else if(mode==StpMode::None) {REQUIRE(f.out.size()==2);CHECK_EQ(f.out[1].type,EventType::Fill);}
        else if(mode==StpMode::CancelBoth) {REQUIRE(f.out.size()==3);CHECK(f.out[1].canceled.was_resting);CHECK(!f.out[2].canceled.was_resting);}
        else {REQUIRE(f.out.size()==2);CHECK(!f.out[1].canceled.was_resting);}
        invariants(f.e);
    }
    Fixture f;f.run(order(1,Side::SELL,100,4));auto n=order(2);n.tif=TimeInForce::FOK;f.run(n);CHECK_EQ(f.out[1].canceled.reason,CancelReason::FillOrKill);CHECK_EQ(f.e.find_order(1)->leaves_qty,4u);
}
TEST_CASE(arena_amend, reductions_all_phases_and_force_loss) {
    for(auto phase:{Phase::Continuous,Phase::PreOpen,Phase::PreClose,Phase::Halted,Phase::Closed}) {
        Fixture f;f.run(order(1,Side::BUY,100,20));f.run(order(2));f.run(order(3,Side::SELL,100,5));
        f.run(SetPhase{0,1,phase});if(phase==Phase::Closed) continue; // DAY expiry tested below
        f.run(CancelOrder{8,1,1,88,12});REQUIRE(f.out.size()==1);CHECK_EQ(f.out[0].canceled.leaves_qty,7u);CHECK_EQ(f.out[0].canceled.canceled_qty,8u);CHECK_EQ(f.out[0].canceled.cum_qty,5u);
        OrderId ids[3];CHECK_EQ(f.e.level_orders(1,Side::BUY,100,ids,3),2u);CHECK_EQ(ids[0],1u);
        f.run(CancelOrder{9,1,1,89,99});CHECK(f.out.empty());
        f.run(CancelOrder{10,1,1,90,4});CHECK(!f.e.find_order(1));CHECK_EQ(f.out[0].canceled.leaves_qty,0u);
    }
    Fixture f;f.run(order(1));f.run(order(2));f.run(ReplaceOrder{8,1,1,3,100,9});CHECK(f.out[0].replaced.priority_kept);CHECK_EQ(f.e.find_order(1)->cl_ord_id,3u);
    f.run(ReplaceOrder{9,1,3,0,100,9,true});CHECK(!f.out[0].replaced.priority_kept);CHECK_EQ(f.out[0].replaced.new_order_id,3u);CHECK_EQ(f.out[1].type,EventType::Rested);
    f.run(MassCancel{10,1,0,99,true});REQUIRE(f.out.size()==3);CHECK_EQ(f.out[0].canceled.order_id,2u);CHECK_EQ(f.out[1].canceled.order_id,3u);CHECK_EQ(f.out[2].mass_cancel_done.reason,CancelReason::CancelOnDisconnect);
}
TEST_CASE(arena_auction, halted_cross_summary_and_expiry) {
    Fixture f(Phase::PreOpen);auto n=order(1,Side::BUY,110,12);n.tif=TimeInForce::GTC;f.run(n);f.run(order(2,Side::SELL,90,7));
    auto ind=f.e.auction_indicative(1);CHECK(ind.crossed);CHECK_EQ(ind.price,90);CHECK_EQ(ind.paired_qty,7u);CHECK_EQ(ind.imbalance_qty,5u);
    f.run(SetPhase{9,1,Phase::Halted});CHECK_EQ(f.out.size(),1u);CHECK_EQ(f.e.live_order_count(),2u);
    f.run(SetPhase{10,1,Phase::Closed});REQUIRE(f.out.size()==3);CHECK_EQ(f.out[0].fill.aggressor,Aggressor::None);CHECK_EQ(f.out[0].fill.match_id,1u);CHECK_EQ(f.out[1].auction_summary.match_id,2u);CHECK_EQ(f.out[1].auction_summary.matched_qty,7u);CHECK_EQ(f.out[2].type,EventType::PhaseChanged);
    CHECK_EQ(f.e.live_order_count(),1u);ind=f.e.auction_indicative(1);CHECK(!ind.crossed);CHECK_EQ(ind.imbalance_qty,5u);CHECK_EQ(ind.imbalance_side,Aggressor::Buy);
    f.run(SetPhase{11,1,Phase::Continuous});f.run(order(3,Side::SELL,90,1));CHECK_EQ(f.out[1].fill.match_id,3u);
    f.run(order(4,Side::BUY,80));f.run(SetPhase{12,1,Phase::Closed});CHECK_EQ(f.out[0].canceled.reason,CancelReason::EndOfDay);invariants(f.e);
}
TEST_CASE(arena_config, invalid_configs_and_wide_aggregates) {
    bool threw=false;try{MatchingEngine e(EngineConfig{0,1});}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
    Fixture f;auto c=*f.e.instrument(1);c.symbol=Symbol("OTHER");c.reference_price=11;CHECK_EQ(f.e.add_instrument(c),0u);c.reference_price=100;c.max_order_qty=std::numeric_limits<Quantity>::max();CHECK_EQ(f.e.add_instrument(c),2u);
    auto n=order(1);n.instrument=2;n.qty=std::numeric_limits<Quantity>::max();f.run(n);n.cl_ord_id=2;f.run(n);LevelInfo l;REQUIRE(f.e.depth(2,Side::BUY,&l,1)==1);CHECK_EQ(l.qty,2ULL*std::numeric_limits<Quantity>::max());
}
