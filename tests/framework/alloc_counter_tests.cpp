#include "alloc_counter.hpp"
#include "test.hpp"

#include <memory>
#include <vector>

TEST_CASE(test_framework, allocation_counter_counts_this_thread) {
    if (!testfw::allocation_counting_available()) return;  // sanitizer build
    testfw::AllocationScope outer;
    auto a = std::make_unique<int>(1);
    {
        testfw::AllocationScope inner;
        std::vector<int> v;
        v.reserve(16);
        auto b = std::make_unique<double[]>(4);
        CHECK_EQ(inner.count(), 2u);
    }
    CHECK_EQ(outer.count(), 3u);
    testfw::AllocationScope idle;
    int on_stack[4] = {1, 2, 3, 4};
    CHECK_EQ(on_stack[3], 4);
    CHECK_EQ(idle.count(), 0u);
}
