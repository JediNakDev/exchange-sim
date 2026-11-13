// exsim::util::ObjectPool: exact reservation, bounded exhaustion and slot reuse.

#include "test/framework/test.hpp"
#include <util/object_pool.hpp>
#include <test/framework/alloc_counter.hpp>

#include <cstdint>

using exsim::util::ObjectPool;

TEST_CASE(object_pool, bounded_pool_holds_exactly_its_reservation_and_reuses_slots) {
    ObjectPool<std::uint64_t, 16> pool;
    pool.set_allow_growth(false);
    pool.reserve(5);  // not a multiple of the 16-node growth block

    std::uint64_t* objects[5] = {};
    std::size_t allocations = 0;
    std::uint64_t* overflow = nullptr;
    std::uint64_t* reused = nullptr;
    {
        testfw::AllocationScope scope;
        for (std::uint64_t i = 0; i < 5; ++i) {
            objects[i] = pool.create(i);
        }
        overflow = pool.create(99);
        pool.destroy(objects[2]);
        reused = pool.create(7);
        allocations = scope.count();
    }
    for (std::uint64_t i = 0; i < 5; ++i) {
        REQUIRE(objects[i] != nullptr);
    }
    CHECK(overflow == nullptr);
    CHECK(reused == objects[2]);
    CHECK_EQ(*reused, 7u);
    CHECK_EQ(*objects[4], 4u);
    CHECK_EQ(allocations, 0u);
}

TEST_CASE(object_pool, growth_adds_blocks_only_when_the_free_list_is_empty) {
    ObjectPool<std::uint64_t, 4> pool;
    pool.reserve(2);
    std::size_t first_two_allocations = 0;
    std::size_t growth_allocations = 0;
    {
        testfw::AllocationScope scope;
        (void)pool.create(1);
        (void)pool.create(2);
        first_two_allocations = scope.count();
    }
    {
        testfw::AllocationScope scope;
        pool.ensure_free_node();      // grows by one 4-node block
        for (int i = 0; i < 4; ++i) {
            REQUIRE(pool.create(3) != nullptr);
        }
        growth_allocations = scope.count();
    }
    CHECK_EQ(first_two_allocations, 0u);
    if (testfw::allocation_counting_available()) {
        CHECK_GE(growth_allocations, 1u);
        CHECK_LE(growth_allocations, 2u);  // the block, plus possibly the block list
    }
}

TEST_CASE(object_pool, throwing_constructor_restores_the_free_slot) {
    struct SometimesThrows {
        int value;
        explicit SometimesThrows(bool fail) : value(42) { if (fail) throw 7; }
    };
    exsim::util::ObjectPool<SometimesThrows> pool;
    pool.reserve(1);
    pool.set_allow_growth(false);
    bool threw = false;
    try { (void)pool.create(true); } catch (int) { threw = true; }
    REQUIRE(threw);
    auto* value = pool.create(false);
    REQUIRE(value != nullptr);
    CHECK_EQ(value->value, 42);
    CHECK(pool.create(false) == nullptr);
    pool.destroy(value);
}
