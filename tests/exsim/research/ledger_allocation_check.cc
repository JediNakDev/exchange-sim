// Standalone: replacement allocators must not affect the shared test runner.
#include "exsim/research/position_ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
bool counting = false;
std::size_t allocations = 0;
}

void* operator new(std::size_t size) {
    if (counting) ++allocations;
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (counting) ++allocations;
    void* pointer = nullptr;
    if (posix_memalign(&pointer, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
#endif

int main() {
    // Explicit allocator calls keep the guard self-test observable at -O2.
    counting = true;
    void* scalar = ::operator new(8);
    void* aligned = ::operator new(64, std::align_val_t{64});
    counting = false;
    ::operator delete(scalar);
    ::operator delete(aligned, std::align_val_t{64});
    if (allocations != 2) return 1;

    using namespace exsim;
    using namespace exsim::research;
    PositionLedger ledger(100'000'000, 2);
    allocations = 0;
    bool passed = true;
    counting = true;
    for (int cycle = 0; cycle < 10'000 && passed; ++cycle) {
        passed = ledger.record_fill(Side::BUY, 100, 10, 1) == LedgerStatus::Ok &&
                 ledger.record_fill(Side::BUY, 110, 5, -1) == LedgerStatus::Ok &&
                 ledger.record_fill(Side::BUY, 120, 1) == LedgerStatus::CapacityExceeded &&
                 ledger.record_fill(Side::SELL, 120, 20, 2) == LedgerStatus::Ok &&
                 ledger.record_fill(Side::BUY, 115, 5) == LedgerStatus::Ok;
        const auto value = ledger.valuation(110);
        passed = passed && value.has_value() && ledger.state().position == 0;
    }
    counting = false;
    std::printf("10000 ledger cycles: %s; C++ allocations after setup: %zu\n",
                passed ? "PASS" : "FAIL", allocations);
    return passed && allocations == 0 ? 0 : 1;
}
