#include "../framework/test.hpp"
#include <lob/flat_hash_map.hpp>

#include <random>
#include <unordered_map>

using lob::FlatHashMap;

TEST_CASE(flat_hash_map, insert_find_erase) {
    FlatHashMap<int> map(8);
    CHECK(map.insert(1, 10));
    CHECK(map.insert(2, 20));
    CHECK(!map.insert(1, 99));  // duplicate
    CHECK(!map.insert(0, 1));   // 0 is the empty sentinel
    REQUIRE(map.find(1) != nullptr);
    CHECK_EQ(*map.find(1), 10);
    CHECK_EQ(map.size(), 2u);
    CHECK(map.erase(1));
    CHECK(!map.erase(1));
    CHECK(map.find(1) == nullptr);
    CHECK_EQ(*map.find(2), 20);
}

TEST_CASE(flat_hash_map, full_without_growth_rejects) {
    FlatHashMap<int> map(4);
    const std::size_t cap = map.max_entries();
    for (std::size_t i = 1; i <= cap; ++i) {
        CHECK(map.insert(i, static_cast<int>(i)));
    }
    CHECK(!map.insert(cap + 1, 0));
    CHECK_EQ(map.capacity(), cap * 2);
}

TEST_CASE(flat_hash_map, growth_preserves_entries) {
    FlatHashMap<std::uint64_t> map(4);
    map.set_allow_growth(true);
    for (std::uint64_t i = 1; i <= 10'000; ++i) {
        REQUIRE(map.insert(i * 7919, i));
    }
    for (std::uint64_t i = 1; i <= 10'000; ++i) {
        REQUIRE(map.find(i * 7919) != nullptr);
        CHECK_EQ(*map.find(i * 7919), i);
    }
}

// Randomised differential test against std::unordered_map, with heavy
// erase churn to exercise backward-shift deletion across wrap-around.
TEST_CASE(flat_hash_map, matches_unordered_map_under_churn) {
    FlatHashMap<std::uint64_t> map(512);
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint64_t> key_d(1, 2000);

    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t k = key_d(rng);
        switch (rng() % 3) {
            case 0: {
                const bool expect = ref.size() < map.max_entries() && !ref.count(k);
                const bool got = map.insert(k, k * 3);
                REQUIRE_EQ(got, expect);
                if (got) ref[k] = k * 3;
                break;
            }
            case 1:
                REQUIRE_EQ(map.erase(k), ref.erase(k) == 1);
                break;
            default: {
                const auto* v = map.find(k);
                const auto it = ref.find(k);
                REQUIRE_EQ(v != nullptr, it != ref.end());
                if (v) REQUIRE_EQ(*v, it->second);
                break;
            }
        }
        REQUIRE_EQ(map.size(), ref.size());
    }
}
