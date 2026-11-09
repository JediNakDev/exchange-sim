#ifndef BENCH_CYCLE_TIMER_HPP
#define BENCH_CYCLE_TIMER_HPP

#include <chrono>
#include <cstdint>

namespace bench {

// Low-overhead cycle counter for tight measurement loops.
// On x86_64: uses rdtsc (CPU timestamp counter).
// On ARM64: uses CNTVCT_EL0 (virtual timer count register).
// Results are in cycles/ticks, not nanoseconds — use calibrate_ticks_per_ns()
// to convert if needed.

inline uint64_t read_cycle_counter() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    #error "Unsupported architecture for cycle counter"
#endif
}

// Serialize + read: ensures all prior instructions complete before reading.
// Use for start timestamp to avoid out-of-order measurement.
inline uint64_t read_cycle_counter_serialized() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int lo, hi;
    __asm__ volatile("lfence\nrdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(val) :: "memory");
    return val;
#else
    #error "Unsupported architecture for cycle counter"
#endif
}

// Estimate ticks per nanosecond by measuring a short spin loop.
// Call once during setup, not in the hot path.
inline double calibrate_ticks_per_ns() {
    auto chrono_start = std::chrono::high_resolution_clock::now();
    uint64_t tsc_start = read_cycle_counter();

    // Spin for ~10ms
    volatile int sink = 0;
    for (int i = 0; i < 10'000'000; ++i) {
        sink += i;
    }

    uint64_t tsc_end = read_cycle_counter();
    auto chrono_end = std::chrono::high_resolution_clock::now();

    double ns = std::chrono::duration<double, std::nano>(chrono_end - chrono_start).count();
    double ticks = static_cast<double>(tsc_end - tsc_start);
    return ticks / ns;
}

} // namespace bench

#endif
