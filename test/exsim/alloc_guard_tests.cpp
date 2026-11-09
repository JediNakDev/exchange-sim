#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
#include <test/framework/test.hpp>
#include <test/exsim/support/command_gen.hpp>
#include <test/framework/alloc_counter.hpp>
#include <unistd.h>
#ifdef EXSIM_TEST_PRODUCTION
TEST_CASE(ProductionEngine, no_allocations) {
    using namespace exsim;
    auto setup=testsupport::fuzz_setup();
    MatchingEngine e(setup.engine);
    for(const auto& i:setup.instruments) REQUIRE(e.add_instrument(i)!=0);
    EventBuffer out(required_event_capacity(setup.engine));testsupport::CommandGenerator gen(9123);
    std::size_t allocations=0;
    for(unsigned j=0;j<200000;++j) {
        auto c=gen.next(e);out.clear();
        HandleResult result;
        {testfw::AllocationScope scope;result=e.handle(c,out);allocations+=scope.count();}
        REQUIRE_EQ(result,HandleResult::Applied);
    }
    CHECK_EQ(allocations,0u);
}
#endif
TEST_CASE(Journal, append_and_flush_allocate_nothing) {
    using namespace exsim;
    auto setup=testsupport::fuzz_setup();
    auto path="/tmp/exsim-"+std::to_string(::getpid())+"-allocation.journal";
    sim::JournalWriter writer(path,setup);
    Command c=CancelOrder{1,1,1,2,3};
    bool ok=true;
    std::size_t allocations=0;
    {
        testfw::AllocationScope scope;
        for(unsigned j=0;j<200000 && ok;++j) ok=writer.append(c);
        ok=writer.flush() && ok;
        allocations=scope.count();
    }
    REQUIRE(ok);CHECK_EQ(allocations,0u);
    sim::JournalReader reader(path);
    std::size_t count=0;while(reader.next(c)) ++count;
    CHECK_EQ(count,200000u);
}
#endif
#endif
