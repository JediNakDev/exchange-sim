#include "test/framework/test.hpp"
#include <book/active_bitmap.hpp>

#include <cstddef>
#include <optional>
#include <random>
#include <vector>

using exsim::book::ActiveBitmap;

namespace {

std::optional<std::size_t> naive_at_or_above(const std::vector<bool>& bits, std::size_t from) {
    for (std::size_t i = from; i < bits.size(); ++i) {
        if (bits[i]) return i;
    }
    return std::nullopt;
}

std::optional<std::size_t> naive_at_or_below(const std::vector<bool>& bits, std::size_t from) {
    if (from >= bits.size()) return std::nullopt;
    for (std::size_t i = from + 1; i-- > 0;) {
        if (bits[i]) return i;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE(active_bitmap, searches_cross_every_summary_layer) {
    // 64^3 + 5 bits: four layers, the last word of each layer partly used.
    constexpr std::size_t kSize = 64 * 64 * 64 + 5;
    ActiveBitmap bitmap(kSize);
    CHECK(!bitmap.find_at_or_above(0).has_value());
    CHECK(!bitmap.find_at_or_below(kSize - 1).has_value());

    bitmap.set(3);
    bitmap.set(kSize - 1);
    CHECK_EQ(bitmap.find_at_or_above(4).value(), kSize - 1);
    CHECK_EQ(bitmap.find_at_or_below(kSize - 2).value(), 3u);
    CHECK_EQ(bitmap.find_at_or_above(kSize - 1).value(), kSize - 1);
    CHECK_EQ(bitmap.find_at_or_below(3).value(), 3u);
    CHECK(!bitmap.find_at_or_below(2).has_value());

    bitmap.set(64 * 64);
    CHECK_EQ(bitmap.find_at_or_above(4).value(), 64u * 64u);
    CHECK_EQ(bitmap.find_at_or_below(kSize - 2).value(), 64u * 64u);
    bitmap.clear(64 * 64);
    CHECK_EQ(bitmap.find_at_or_above(4).value(), kSize - 1);
    CHECK_EQ(bitmap.count(), 2u);
}

TEST_CASE(active_bitmap, out_of_range_starts_find_nothing) {
    ActiveBitmap bitmap(130);
    bitmap.set(0);
    bitmap.set(129);
    CHECK(!bitmap.find_at_or_above(130).has_value());
    CHECK(!bitmap.find_at_or_below(130).has_value());
    CHECK(!bitmap.find_at_or_above(static_cast<std::size_t>(-1)).has_value());
    CHECK_EQ(bitmap.find_at_or_below(129).value(), 129u);
    CHECK_EQ(bitmap.word_count(), 3u);
    CHECK_EQ(bitmap.word(2), std::uint64_t{2});
}

TEST_CASE(active_bitmap, count_ignores_repeated_set_and_clear) {
    ActiveBitmap bitmap(70);
    bitmap.set(5);
    bitmap.set(5);
    bitmap.set(69);
    CHECK_EQ(bitmap.count(), 2u);
    bitmap.clear(5);
    bitmap.clear(5);
    bitmap.clear(6);
    CHECK_EQ(bitmap.count(), 1u);
    CHECK(bitmap.test(69));
    CHECK(!bitmap.test(5));
}

TEST_CASE(active_bitmap, randomized_searches_match_a_linear_scan) {
    for (const std::size_t size : {std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65},
                                   std::size_t{4096}, std::size_t{4097}, std::size_t{20000}}) {
        ActiveBitmap bitmap(size);
        std::vector<bool> reference(size, false);
        std::size_t reference_count = 0;
        std::mt19937_64 random(size);
        for (int step = 0; step < 4000; ++step) {
            const std::size_t index = random() % size;
            if (random() % 3 == 0) {
                reference_count -= reference[index] ? 1 : 0;
                reference[index] = false;
                bitmap.clear(index);
            } else {
                reference_count += reference[index] ? 0 : 1;
                reference[index] = true;
                bitmap.set(index);
            }
            const std::size_t from = random() % size;
            REQUIRE(bitmap.find_at_or_above(from) == naive_at_or_above(reference, from));
            REQUIRE(bitmap.find_at_or_below(from) == naive_at_or_below(reference, from));
            REQUIRE_EQ(bitmap.count(), reference_count);
        }
    }
}
