#ifndef EXSIM_TESTS_CONTRACT_GATES_HPP
#define EXSIM_TESTS_CONTRACT_GATES_HPP
#include <tests/framework/test.hpp>
#include <tests/exsim/support/differential.hpp>
namespace exsim::testsupport {
template<class Engine> void check_auction_retry() {
    EngineConfig config{1,8};Engine engine(config),control(config);
    InstrumentConfig i;i.symbol=Symbol("GATE");i.initial_phase=Phase::PreOpen;
    REQUIRE_EQ(engine.add_instrument(i),1);REQUIRE_EQ(control.add_instrument(i),1);
    EventBuffer small(1),out(required_event_capacity(config)),expected(required_event_capacity(config));
    Event sentinel{};sentinel.type=EventType::PhaseChanged;sentinel.phase_changed={Phase::Closed,Phase::PreOpen};
    REQUIRE(small.push(sentinel));
    NewOrder buy;buy.instrument=1;buy.session=1;buy.cl_ord_id=1;buy.qty=10;buy.price=1000000;
    auto sell=buy;sell.session=2;sell.cl_ord_id=2;sell.side=Side::SELL;
    for(const Command c:{Command{buy},Command{sell},Command{SetPhase{0,1,Phase::Halted}},
                         Command{SetPhase{0,1,Phase::Continuous}}}) {
        auto result=engine.handle(c,small);
        REQUIRE_EQ(result,HandleResult::InsufficientOutput);
        REQUIRE_EQ(small.size(),1u);CHECK(events_equal(small[0],sentinel));
        CHECK(book_diff(engine,control).empty());
        out.clear();expected.clear();
        REQUIRE_EQ(engine.handle(c,out),HandleResult::Applied);
        REQUIRE_EQ(control.handle(c,expected),HandleResult::Applied);
        REQUIRE_EQ(out.size(),expected.size());
        for(std::size_t j=0;j<out.size();++j) CHECK(events_equal(out[j],expected[j]));
        CHECK(book_diff(engine,control).empty());
    }
    REQUIRE_EQ(out.size(),3u);
    CHECK_EQ(out[0].type,EventType::Fill);CHECK_EQ(out[0].fill.match_id,1u);
    CHECK_EQ(out[1].type,EventType::AuctionSummary);CHECK_EQ(out[1].auction_summary.match_id,2u);
}
template<class Engine> void check_reduction_fifo_all_phases() {
    for(auto phase:{Phase::PreOpen,Phase::Continuous,Phase::PreClose,Phase::Halted,Phase::Closed}) {
        EngineConfig config{1,8};Engine engine(config);
        InstrumentConfig i;i.symbol=Symbol("GATE");i.lot_size=2;
        REQUIRE_EQ(engine.add_instrument(i),1);
        EventBuffer out(required_event_capacity(config));
        NewOrder n;n.instrument=1;n.session=1;n.cl_ord_id=1;n.qty=10;n.price=1000000;n.tif=TimeInForce::GTC;
        REQUIRE_EQ(engine.handle(n,out),HandleResult::Applied);
        out.clear();n.cl_ord_id=2;REQUIRE_EQ(engine.handle(n,out),HandleResult::Applied);
        out.clear();REQUIRE_EQ(engine.handle(SetPhase{0,1,phase},out),HandleResult::Applied);
        out.clear();REQUIRE_EQ(engine.handle(CancelOrder{0,1,1,3,3},out),HandleResult::Applied);
        REQUIRE_EQ(out.size(),1u);CHECK_EQ(out[0].type,EventType::Canceled);
        CHECK_EQ(out[0].canceled.leaves_qty,3u);CHECK_EQ(out[0].canceled.canceled_qty,7u);
        OrderId ids[2]{};REQUIRE_EQ(engine.level_orders(1,Side::BUY,n.price,ids,2),2u);
        CHECK_EQ(ids[0],1u);CHECK_EQ(ids[1],2u);CHECK_EQ(engine.find_order(1)->qty,3u);
    }
}
}
#endif
