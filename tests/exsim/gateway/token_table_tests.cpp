#include "exsim/gateway/token_table.hpp"
#include "../../framework/test.hpp"

#include <random>
#include <string>
#include <unordered_map>

using exsim::gateway::TokenTable;
using Status = TokenTable::InternStatus;

TEST_CASE(gateway_token_table, interns_in_first_seen_order) {
    TokenTable t(8);
    auto a = t.intern("ORDER0000001  ");
    auto b = t.intern("ORDER0000002  ");
    CHECK(a.status == Status::Inserted);
    CHECK(b.status == Status::Inserted);
    CHECK_EQ(a.id, 1u);
    CHECK_EQ(b.id, 2u);
    auto again = t.intern("ORDER0000001  ");
    CHECK(again.status == Status::Existing);
    CHECK_EQ(again.id, 1u);
    CHECK_EQ(t.size(), 2u);
    CHECK(t.token(1) == "ORDER0000001  ");
    CHECK(t.token(2) == "ORDER0000002  ");
    CHECK(t.token(3).empty());
    CHECK(t.token(0).empty());
}

TEST_CASE(gateway_token_table, byte_exact_comparison) {
    TokenTable t(8);
    CHECK_EQ(t.intern("abc").id, 1u);
    CHECK_EQ(t.intern("abc ").id, 2u);  // padding is significant
    CHECK_EQ(t.intern("ABC").id, 3u);   // case is significant
    CHECK_EQ(t.find("abc"), 1u);
    CHECK_EQ(t.find("abc "), 2u);
    CHECK_EQ(t.find("ab"), 0u);
}

TEST_CASE(gateway_token_table, rejects_invalid_and_full) {
    TokenTable t(2);
    CHECK(t.intern("").status == Status::Invalid);
    CHECK(t.intern(std::string(21, 'x')).status == Status::Invalid);
    CHECK(t.intern(std::string(20, 'x')).status == Status::Inserted);
    CHECK(t.intern("b").status == Status::Inserted);
    auto full = t.intern("c");
    CHECK(full.status == Status::Full);
    CHECK_EQ(full.id, 0u);
    CHECK(t.intern("b").status == Status::Existing);  // lookups still work when full
    CHECK_EQ(t.find(""), 0u);
}

TEST_CASE(gateway_token_table, clear_resets_ids) {
    TokenTable t(4);
    (void)t.intern("x");
    (void)t.intern("y");
    t.clear();
    CHECK_EQ(t.size(), 0u);
    CHECK_EQ(t.find("x"), 0u);
    CHECK_EQ(t.intern("y").id, 1u);
}

TEST_CASE(gateway_token_table, differential_against_unordered_map) {
    constexpr std::uint32_t kMax = 50'000;
    TokenTable t(kMax);
    std::unordered_map<std::string, std::uint64_t> ref;
    std::vector<std::string> by_id;
    std::mt19937_64 rng(42);
    for (int step = 0; step < 200'000; ++step) {
        // Small alphabet + short lengths -> plenty of repeats and near-collisions.
        std::string tok;
        const std::size_t len = 1 + rng() % 20;
        for (std::size_t i = 0; i < len; ++i) {
            tok.push_back(static_cast<char>('0' + rng() % 4));
        }
        if (rng() % 3 == 0) {
            const auto got = t.find(tok);
            const auto it = ref.find(tok);
            CHECK_EQ(got, it == ref.end() ? 0u : it->second);
            continue;
        }
        const auto r = t.intern(tok);
        const auto it = ref.find(tok);
        if (it != ref.end()) {
            CHECK(r.status == Status::Existing);
            CHECK_EQ(r.id, it->second);
        } else if (ref.size() == kMax) {
            CHECK(r.status == Status::Full);
        } else {
            REQUIRE(r.status == Status::Inserted);
            ref.emplace(tok, r.id);
            by_id.push_back(tok);
            CHECK_EQ(r.id, by_id.size());
        }
    }
    for (std::size_t i = 0; i < by_id.size(); ++i) {
        CHECK(t.token(i + 1) == by_id[i]);
    }
}
