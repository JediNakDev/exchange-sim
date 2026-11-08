#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || __has_feature(address_sanitizer) || \
    __has_feature(thread_sanitizer)
#define TESTFW_ALLOC_COUNTING 0
#else
#define TESTFW_ALLOC_COUNTING 1
#endif

namespace {
thread_local bool counting = false;
thread_local std::size_t allocations = 0;
}  // namespace

namespace testfw {

bool allocation_counting_available() noexcept { return TESTFW_ALLOC_COUNTING != 0; }

AllocationScope::AllocationScope() noexcept : start_(allocations), was_counting_(counting) { counting = true; }
AllocationScope::~AllocationScope() { counting = was_counting_; }
std::size_t AllocationScope::count() const noexcept { return allocations - start_; }

}  // namespace testfw

#if TESTFW_ALLOC_COUNTING
namespace {
void* allocate(std::size_t n, std::size_t alignment = 0) {
    if (counting) ++allocations;
    void* p = nullptr;
    if (alignment != 0) {
        if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, n ? n : 1) != 0) p = nullptr;
    } else {
        p = std::malloc(n ? n : 1);
    }
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocate(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocate(n); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif
