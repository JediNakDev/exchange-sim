#include <test/framework/test.hpp>
#include <engine/matching_engine.hpp>
#include <exchange/flat_hash_map.hpp>
#include <limits>
#include <memory>

TEST_CASE(foundation, zero_and_moved_map) {
    exsim::exchange::FlatHashMap<int> map(8);
    CHECK(!map.contains(0));
    CHECK(map.insert(1, 7));
    CHECK(!map.contains(0));
    map.erase(1);
    CHECK(!map.contains(0));
    CHECK(map.insert(2, 9));
    exsim::exchange::FlatHashMap<int> moved(std::move(map));
    CHECK_EQ(moved.size(), 1u);
    CHECK_EQ(map.capacity(), 0u);
    CHECK(!map.find(2));
    map.clear();
    map.reserve(8);
    CHECK(map.insert(3, 4));
    moved = std::move(map);
    CHECK(!map.find(3));
    map.clear();
    CHECK_EQ(map.size(), 0u);
    moved.clear();
    CHECK(!moved.contains(0));
}
TEST_CASE(foundation, reserve_overflow_and_constness) {
    exsim::exchange::FlatHashMap<int> map(8);
    CHECK(map.insert(1, 2));
    for (auto n : {std::numeric_limits<std::size_t>::max(), std::size_t{1} << 63}) {
        bool rejected = false;
        try { map.reserve(n); } catch (const std::length_error&) { rejected = true; }
        CHECK(rejected);
        CHECK_EQ(*map.find(1), 2);
    }
    const auto& const_map = map;
    const_map.for_each([](auto& key, auto& value) {
        static_assert(std::is_const_v<std::remove_reference_t<decltype(key)>>);
        static_assert(std::is_const_v<std::remove_reference_t<decltype(value)>>);
    });
}
TEST_CASE(foundation, bounded_events) {
    exsim::EventBuffer out(2);
    exsim::Event event{};
    CHECK(out.push(event));
    CHECK(out.push(event));
    CHECK(!out.push(event));
    CHECK_EQ(out.size(), 2u);
    CHECK(!out.can_append(1));
    out.clear();
    CHECK_EQ(out.remaining(), out.capacity());
    CHECK(out.can_append(2));
    exsim::EventBuffer empty(0);
    CHECK(!empty.push(event));
    CHECK_EQ(exsim::required_event_capacity({2, 10}), 28u);
    bool rejected = false;
    try { (void)exsim::required_event_capacity({65536, 10}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
TEST_CASE(foundation, comparison_temporary_and_noncopyable) {
    struct Item { int value; };
    CHECK_EQ(std::optional<Item>({7})->value, 7);
    auto ptr = std::make_unique<int>(4);
    CHECK_EQ(ptr, ptr);
    CHECK_EQ(std::make_unique<int>(1) == nullptr, false);
}
// Explicit instantiations compile the erased wrapper without linking an engine.
// The helper is never called; no production implementation is supplied here.
static void free_callback(const exsim::OrderInfo&) {}
[[maybe_unused]] static void compile_callbacks(const exsim::MatchingEngine& engine) {
    const auto callback = [](const exsim::OrderInfo&) {};
    engine.for_each_order(free_callback);
    engine.for_each_order(callback);
    engine.for_each_order(+callback);
}

TEST_CASE(foundation, instrument_validation) {
    exsim::InstrumentConfig c;
    CHECK(exsim::valid_instrument_config(c));
    c.initial_phase = static_cast<exsim::Phase>(255);
    CHECK(!exsim::valid_instrument_config(c));
    c.initial_phase = exsim::Phase::Continuous;
    c.reference_price = std::numeric_limits<exsim::Price>::min();
    CHECK(!exsim::valid_instrument_config(c));
    c.reference_price = c.min_price + 1;
    CHECK(!exsim::valid_instrument_config(c));
}
