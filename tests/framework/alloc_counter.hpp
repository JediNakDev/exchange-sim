#ifndef TESTS_FRAMEWORK_ALLOC_COUNTER_HPP
#define TESTS_FRAMEWORK_ALLOC_COUNTER_HPP

// Counts global C++ heap allocations (every operator new / new[] form,
// including aligned) made by the current thread while a scope is active.
// The replacement operators live in tests/framework/alloc_counter.cpp, the
// only translation unit in run_tests allowed to replace them.
//
//   if (!testfw::allocation_counting_available()) return;  // ASan/TSan builds
//   testfw::AllocationScope scope;
//   hot_path();
//   CHECK_EQ(scope.count(), 0u);
//
// Sanitizer builds keep their own allocator, so counting is unavailable
// there and scopes always report 0. Direct malloc/mmap calls are not seen.

#include <cstddef>

namespace testfw {

bool allocation_counting_available() noexcept;

class AllocationScope {
public:
    AllocationScope() noexcept;
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    // Allocations on this thread since the scope opened (nested scopes each
    // count from their own start).
    [[nodiscard]] std::size_t count() const noexcept;

private:
    std::size_t start_;
    bool was_counting_;
};

}  // namespace testfw

#endif
