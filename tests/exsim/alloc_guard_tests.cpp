#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
#include <tests/framework/test.hpp>
#include <tests/exsim/support/command_gen.hpp>
#include <atomic>
#include <cstdlib>
#include <new>
#include <unistd.h>
namespace {
thread_local bool counting=false;
thread_local std::size_t allocations=0;
void* allocate(std::size_t n,std::size_t alignment=0) {
    if(counting) ++allocations;
    void* p=nullptr;
    if(alignment) {if(posix_memalign(&p,alignment,n?n:1)) p=nullptr;}
    else p=std::malloc(n?n:1);
    if(!p) throw std::bad_alloc();
    return p;
}
}
void* operator new(std::size_t n) {return allocate(n);}
void* operator new[](std::size_t n) {return allocate(n);}
void* operator new(std::size_t n,std::align_val_t a) {return allocate(n,static_cast<std::size_t>(a));}
void* operator new[](std::size_t n,std::align_val_t a) {return allocate(n,static_cast<std::size_t>(a));}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void operator delete(void* p,std::align_val_t) noexcept {std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept {std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept {std::free(p);}
#ifdef EXSIM_TEST_PRODUCTION
TEST_CASE(ProductionEngine, no_allocations) {
    using namespace exsim;
    auto setup=testsupport::fuzz_setup();
    MatchingEngine e(setup.engine);
    for(const auto& i:setup.instruments) REQUIRE(e.add_instrument(i)!=0);
    EventBuffer out(required_event_capacity(setup.engine));testsupport::CommandGenerator gen(9123);
    allocations=0;
    for(unsigned j=0;j<200000;++j) {
        auto c=gen.next(e);out.clear();
        struct Guard {Guard(){counting=true;}~Guard(){counting=false;}};
        HandleResult result;
        {Guard guard;result=e.handle(c,out);}
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
    allocations=0;
    bool ok=true;
    counting=true;
    for(unsigned j=0;j<200000 && ok;++j) ok=writer.append(c);
    ok=writer.flush() && ok;
    counting=false;
    REQUIRE(ok);CHECK_EQ(allocations,0u);
    sim::JournalReader reader(path);
    std::size_t count=0;while(reader.next(c)) ++count;
    CHECK_EQ(count,200000u);
}
#endif
#endif
