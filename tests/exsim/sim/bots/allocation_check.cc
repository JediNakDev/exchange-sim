#include "fake_exchange.hpp"
#include "tests/framework/test.hpp"
#include <cstdlib>
#include <new>

namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocation_count = 0;
void* allocate_memory(std::size_t size) {
    if (count_allocations) ++allocation_count;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t size, std::size_t alignment) {
    if (count_allocations) ++allocation_count;
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size ? size : 1) != 0) throw std::bad_alloc{};
    return memory;
}
}
// All replacements forward to malloc/free, including sanitizer builds.
void* operator new(std::size_t size) { return allocate_memory(size); }
void* operator new[](std::size_t size) { return allocate_memory(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment) { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { try { return allocate_memory(size); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { try { return allocate_memory(size); } catch (...) { return nullptr; } }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { std::free(memory); }

TEST_CASE(sim_bots_allocations, active_hot_paths_allocate_zero) {
    using namespace bot_tests;
    BotSet<2> bots;
    auto config = maker_config(); config.fair_value_step_ms = 1;
    auto* maker = bots.add(config); REQUIRE(maker);
    NoiseTakerConfig noise; noise.session_id = 2; noise.instrument = 1; noise.mean_interval_ms = 1; noise.limit_ppm = 100000;
    REQUIRE(bots.add(noise));
    FakeExchange exchange; exchange.bind(bots);
    allocation_count = 0; count_allocations = true;
    for (Timestamp i = 0; i < 1000; ++i) tick(bots, exchange, i * 1000000);
    exchange.change_phase(Phase::Halted); tick(bots, exchange, 1000000000);
    exchange.change_phase(Phase::Continuous); tick(bots, exchange, 1001000000);
    bots.set_enabled(false); tick(bots, exchange, 1002000000);
    count_allocations = false;
    CHECK_EQ(allocation_count, 0u);
    CHECK_GT(maker->stats().fills_received, 0u); CHECK_EQ(maker->orders().size(), 0u);
}
