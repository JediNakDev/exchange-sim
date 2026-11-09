#ifdef EXSIM_TEST_PRODUCTION
#include <test/framework/test.hpp>
#include <test/exsim/support/differential.hpp>
#include <sim/scenario.hpp>
#include <filesystem>
TEST_CASE(ProductionEngine, differential) {
    for(std::uint64_t seed=1;seed<=40;++seed) {
        auto error=exsim::testsupport::fuzz<exsim::MatchingEngine>(seed,3000,seed%2==0);
        if(!error.empty()) FAIL(error);
    }
}
TEST_CASE(ProductionEngine, scenarios) {
    // Runs every file and names each failing one with the scenario's own message.
    std::size_t count=0;
    std::string failures;
    for(const auto& entry:std::filesystem::directory_iterator("test/scenarios")) if(entry.path().extension()==".scn") {
        std::ifstream input(entry.path());
        try {exsim::sim::run_scenario<exsim::MatchingEngine>(input);}
        catch(const std::exception& error){failures+=entry.path().string()+": "+error.what()+"\n";}
        ++count;
    }
    if(!failures.empty()) FAIL(failures);
    CHECK_GE(count,15u);
}
#endif
#ifdef EXSIM_TEST_PRODUCTION
TEST_CASE(ProductionEngine, preflight_preserves_nonempty_output_and_counters) {
    using namespace exsim;
    auto setup=testsupport::fuzz_setup(true);
    MatchingEngine a(setup.engine),control(setup.engine);
    testsupport::setup_engine(a,setup);testsupport::setup_engine(control,setup);
    EventBuffer small(1),out(required_event_capacity(setup.engine)),expected(required_event_capacity(setup.engine));
    Event sentinel{};sentinel.type=EventType::PhaseChanged;sentinel.phase_changed={Phase::Closed,Phase::PreOpen};
    REQUIRE(small.push(sentinel));
    NewOrder n;n.instrument=1;n.session=1;n.cl_ord_id=1;n.price=1000000;n.qty=20;
    for(const Command c:{Command{n},Command{ReplaceOrder{0,1,1,2,1000000,10,true}},
                        Command{CancelOrder{0,1,2,9,5}},Command{SetPhase{0,1,Phase::Closed}},
                        Command{MassCancel{0,1}}}) {
        auto result=a.handle(c,small);
        if(result==HandleResult::InsufficientOutput) {
            REQUIRE_EQ(small.size(),1u);CHECK(testsupport::events_equal(small[0],sentinel));
            CHECK(testsupport::book_diff(a,control).empty());
            out.clear();expected.clear();REQUIRE_EQ(a.handle(c,out),HandleResult::Applied);
            REQUIRE_EQ(control.handle(c,expected),HandleResult::Applied);
            REQUIRE_EQ(out.size(),expected.size());
            for(std::size_t j=0;j<out.size();++j) CHECK(testsupport::events_equal(out[j],expected[j]));
        } else {
            // A proven tighter preflight may apply a zero-event command.
            CHECK_EQ(small.size(),1u);expected.clear();REQUIRE_EQ(control.handle(c,expected),HandleResult::Applied);
            CHECK(expected.empty());
        }
        CHECK(testsupport::book_diff(a,control).empty());
    }
}
#endif
#ifdef EXSIM_TEST_PRODUCTION
#include <test/exsim/support/contract_gates.hpp>
TEST_CASE(ProductionEngine, auction_retry_counters) {
    exsim::testsupport::check_auction_retry<exsim::MatchingEngine>();
}
TEST_CASE(ProductionEngine, reduction_fifo_all_phases) {
    exsim::testsupport::check_reduction_fifo_all_phases<exsim::MatchingEngine>();
}
#endif
