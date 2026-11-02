#include "alloc_counter.hpp"
#include "test.hpp"

#include <memory>
#include <vector>

namespace {
// Makes a pointer escape so the compiler cannot elide the allocation.
template <typename T>
void escape(T* p) {
    asm volatile("" : : "g"(p) : "memory");
}
}  // namespace

TEST_CASE(test_framework, allocation_counter_counts_this_thread) {
    if (!testfw::allocation_counting_available()) return;  // sanitizer build
    testfw::AllocationScope outer;
    auto a = std::make_unique<int>(1);
    escape(a.get());
    {
        testfw::AllocationScope inner;
        std::vector<int> v;
        v.reserve(16);
        escape(v.data());
        auto b = std::make_unique<double[]>(4);
        escape(b.get());
        CHECK_EQ(inner.count(), 2u);
    }
    CHECK_EQ(outer.count(), 3u);
    testfw::AllocationScope idle;
    int on_stack[4] = {1, 2, 3, 4};
    escape(on_stack);
    CHECK_EQ(idle.count(), 0u);
}
