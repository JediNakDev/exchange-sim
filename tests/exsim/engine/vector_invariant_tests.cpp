#include "vector_test_support.hpp"
#include <random>
using namespace vector_engine_test;

// Explicit active-field encoding: no union bytes, padding, or object hashing.
static std::vector<std::uint64_t> fields(const EventBuffer& out) {
    std::vector<std::uint64_t> v;
    const auto put=[&](auto... x) { (v.push_back(static_cast<std::uint64_t>(x)),...); };
    const auto leg=[&](const FillLeg& l) { put(l.order_id,l.cl_ord_id,l.session,l.owner,l.leaves_qty,l.cum_qty); };
    for(const auto& e:out) {
        put(e.type,e.instrument,e.ts);
        switch(e.type) {
        case EventType::Accepted: {const auto& a=e.accepted; put(a.order_id,a.cl_ord_id,a.session,a.owner,a.side,a.ord_type,a.tif,a.stp,a.post_only,a.price,a.qty); break;}
        case EventType::Rejected: {const auto& a=e.rejected; put(a.cmd,a.reason,a.session,a.cl_ord_id,a.orig_cl_ord_id,a.order_id); break;}
        case EventType::Rested: {const auto& a=e.rested; put(a.order_id,a.cl_ord_id,a.session,a.side,a.price,a.leaves_qty); break;}
        case EventType::Fill: {const auto& a=e.fill; put(a.match_id,a.price,a.qty,a.aggressor); leg(a.buy); leg(a.sell); break;}
        case EventType::Canceled: {const auto& a=e.canceled; put(a.order_id,a.cl_ord_id,a.request_cl_ord_id,a.session,a.side,a.price,a.canceled_qty,a.cum_qty,a.reason,a.was_resting,a.leaves_qty); break;}
        case EventType::Replaced: {const auto& a=e.replaced; put(a.old_order_id,a.new_order_id,a.orig_cl_ord_id,a.cl_ord_id,a.session,a.side,a.old_price,a.price,a.old_leaves_qty,a.qty,a.leaves_qty,a.cum_qty,a.priority_kept); break;}
        case EventType::MassCancelDone: {const auto& a=e.mass_cancel_done; put(a.session,a.request_cl_ord_id,a.canceled_count,a.reason); break;}
        case EventType::PhaseChanged: put(e.phase_changed.old_phase,e.phase_changed.new_phase); break;
        case EventType::AuctionSummary: put(e.auction_summary.price,e.auction_summary.matched_qty,e.auction_summary.match_id); break;
        }
    }
    return v;
}

static void invariants(const MatchingEngine& e) {
    std::size_t count=0; OrderId previous=0;
    e.for_each_order([&](const OrderInfo& o) {
        CHECK(o.order_id>previous); previous=o.order_id; ++count;
        CHECK(o.leaves_qty>0); CHECK_EQ(std::uint64_t{o.leaves_qty}+o.cum_qty,o.qty);
        CHECK_EQ(e.find_order(o.session,o.cl_ord_id)->order_id,o.order_id);
    });
    CHECK_EQ(count,e.live_order_count());
    std::size_t depth_count=0;
    for(InstrumentId id=1;id<=e.instrument_count();++id) {
        auto bid=e.best_bid(id), ask=e.best_ask(id);
        if(e.phase(id)==Phase::Continuous || e.phase(id)==Phase::Closed) CHECK(!bid || !ask || *bid<*ask);
        for(Side side:{Side::BUY,Side::SELL}) {
            LevelInfo levels[64]; auto n=e.depth(id,side,levels,64);
            for(std::size_t i=0;i<n;++i) {
                OrderId ids[64]; auto m=e.level_orders(id,side,levels[i].price,ids,64);
                CHECK(m>0); CHECK_EQ(m,levels[i].order_count); depth_count+=m;
                std::uint64_t qty=0; OrderId last=0;
                for(std::size_t j=0;j<m;++j) { auto o=e.find_order(ids[j]); REQUIRE(o); CHECK(ids[j]>last); last=ids[j]; qty+=o->leaves_qty; CHECK_EQ(o->price,levels[i].price); CHECK_EQ(o->side,side); }
                CHECK_EQ(qty,levels[i].qty);
                if(i>0) CHECK(side==Side::BUY?levels[i-1].price>levels[i].price:levels[i-1].price<levels[i].price);
            }
        }
    }
    CHECK_EQ(depth_count,count);
}

TEST_CASE(vector_output, nonempty_insufficient_output_retry_all_commands) {
    // Each operation is retried on the same engine, compared against an untouched twin.
    for(unsigned kind=0;kind<6;++kind) {
        MatchingEngine a({2,16}),b({2,16});
        auto cfg=standard_instrument_config("A",Phase::PreOpen);
        REQUIRE_EQ(a.add_instrument(cfg),1u); REQUIRE_EQ(b.add_instrument(cfg),1u);
        EventBuffer x(64),y(64);
        for(ClOrdId token=1;token<=4;++token) {
            NewOrder c{token,1,1,1,token,token<3?Side::BUY:Side::SELL,OrdType::Limit,TimeInForce::Day,StpMode::None,false,token<3?1200:1000,10};
            REQUIRE(a.handle(c,x)==HandleResult::Applied); REQUIRE(b.handle(c,y)==HandleResult::Applied); x.clear();y.clear();
        }
        Command c;
        switch(kind) {
        case 0: c=NewOrder{9,1,2,0,1,Side::BUY,OrdType::Limit,TimeInForce::Day,StpMode::None,false,900,10}; break;
        case 1: c=CancelOrder{9,1,1,99,5}; break;
        case 2: c=ReplaceOrder{9,1,1,8,1300,20,true}; break;
        case 3: c=MassCancel{9,1,0,88,true}; break;
        case 4: c=SetPhase{9,0,Phase::Closed}; break;
        default: c=NewOrder{}; break;
        }
        EventBuffer small(1); Event sentinel{}; sentinel.type=EventType::PhaseChanged; sentinel.ts=777;
        REQUIRE(small.push(sentinel)); const auto before=fields(small);
        REQUIRE(a.handle(c,small)==HandleResult::InsufficientOutput);
        CHECK_EQ(fields(small),before); CHECK_EQ(a.live_order_count(),4u); CHECK_EQ(a.phase(1),Phase::PreOpen);
        invariants(a);
        REQUIRE(a.handle(c,x)==HandleResult::Applied); REQUIRE(b.handle(c,y)==HandleResult::Applied);
        CHECK_EQ(fields(x),fields(y));
        // A subsequent ID assignment/trade also detects counter changes during failure.
        NewOrder next{10,1,3,0,20,Side::SELL,OrdType::Market,TimeInForce::IOC,StpMode::None,false,777,1};
        x.clear();y.clear(); REQUIRE(a.handle(next,x)==HandleResult::Applied); REQUIRE(b.handle(next,y)==HandleResult::Applied);
        CHECK_EQ(fields(x),fields(y));
    }
}

TEST_CASE(vector_invariants, randomized_commands_and_deterministic_active_fields) {
    MatchingEngine a({2,32}),b({2,32});
    for(const char* symbol:{"A","B"}) { auto c=standard_instrument_config(symbol); REQUIRE(a.add_instrument(c)!=0); REQUIRE(b.add_instrument(c)!=0); }
    EventBuffer x(128),y(128); std::mt19937 rng(7319);
    // Conservation tracks executed + leaves + cancellations against initial + amendment delta.
    std::int64_t expected=0,executed=0,canceled=0;
    for(unsigned i=0;i<20000;++i) {
        Command c; const auto kind=rng()%10;
        const SessionId session=static_cast<SessionId>(1+rng()%3); const ClOrdId token=1+rng()%40;
        if(kind<5) {
            NewOrder n; n.ts=i; n.instrument=static_cast<InstrumentId>(1+rng()%2); n.session=session; n.cl_ord_id=token; n.owner=rng()%4;
            n.side=static_cast<Side>(rng()%2); n.price=100*(1+rng()%20); n.qty=1+rng()%30;
            n.tif=static_cast<TimeInForce>(rng()%4); n.stp=static_cast<StpMode>(rng()%4); n.type=rng()%8==0?OrdType::Market:OrdType::Limit;
            c=n;
        } else if(kind==5) c=CancelOrder{i,session,token,99,static_cast<Quantity>(rng()%40)};
        else if(kind==6) c=ReplaceOrder{i,session,token,static_cast<ClOrdId>(rng()%40),static_cast<Price>(100*(1+rng()%20)),static_cast<Quantity>(1+rng()%30),rng()%2==0};
        else if(kind==7) c=MassCancel{i,session,static_cast<InstrumentId>(rng()%3),88,rng()%2==0};
        else c=SetPhase{i,static_cast<InstrumentId>(rng()%3),static_cast<Phase>(rng()%5)};
        auto old=c.type==CommandType::Replace?a.find_order(c.replace.session,c.replace.orig_cl_ord_id):std::optional<OrderInfo>{};
        x.clear();y.clear(); REQUIRE(a.handle(c,x)==HandleResult::Applied); REQUIRE(b.handle(c,y)==HandleResult::Applied);
        CHECK_EQ(fields(x),fields(y));
        for(const auto& e:x) {
            if(e.type==EventType::Accepted) expected+=e.accepted.qty;
            if(e.type==EventType::Replaced) { REQUIRE(old); expected+=std::int64_t{e.replaced.qty}-old->qty; }
            if(e.type==EventType::Fill) { CHECK(e.fill.qty>0); executed+=2ll*e.fill.qty; }
            if(e.type==EventType::Canceled) canceled+=e.canceled.canceled_qty;
        }
        // Cancel reductions change current qty but are already counted by the cancellation.
        std::int64_t leaves=0; a.for_each_order([&](const OrderInfo& o) { leaves+=o.leaves_qty; });
        CHECK_EQ(expected,executed+canceled+leaves);
        invariants(a);
    }
}
