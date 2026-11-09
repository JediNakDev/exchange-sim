#include "gateway/sequenced_store.hpp"
#include "../../framework/test.hpp"

#include <string>
#include <string_view>

using exsim::gateway::SequencedStore;

namespace {
std::span<const char> bytes(std::string_view s) { return {s.data(), s.size()}; }
std::string_view str(std::span<const char> s) { return {s.data(), s.size()}; }
}  // namespace

TEST_CASE(gateway_sequenced_store, append_and_get) {
    SequencedStore st(4, 64);
    CHECK_EQ(st.next_seq(), 1u);
    CHECK(st.append(bytes("hello"), 7));
    CHECK(st.append(bytes(""), 8));
    CHECK(st.append(bytes("world!"), 9));
    CHECK_EQ(st.last_seq(), 3u);
    CHECK(str(st.get(1).bytes) == "hello");
    CHECK_EQ(st.get(1).meta, 7u);
    CHECK(st.get(2).bytes.empty());
    CHECK_EQ(st.get(2).meta, 8u);
    CHECK(str(st.get(3).bytes) == "world!");
    CHECK(st.get(0).bytes.empty());
    CHECK(st.get(4).bytes.empty());
    CHECK_EQ(st.bytes_used(), 11u);
}

TEST_CASE(gateway_sequenced_store, message_limit) {
    SequencedStore st(2, 1024);
    CHECK(st.append(bytes("a")));
    CHECK(st.append(bytes("b")));
    CHECK(!st.append(bytes("c")));
    CHECK_EQ(st.last_seq(), 2u);
    CHECK(str(st.get(2).bytes) == "b");
}

TEST_CASE(gateway_sequenced_store, byte_limit_is_exact) {
    SequencedStore st(16, 10);
    CHECK(st.append(bytes("12345")));
    CHECK(!st.append(bytes("123456")));  // would exceed by one
    CHECK(st.append(bytes("12345")));    // fills exactly
    CHECK(!st.append(bytes("x")));
    CHECK_EQ(st.last_seq(), 2u);
    CHECK_EQ(st.bytes_used(), 10u);
}

TEST_CASE(gateway_sequenced_store, replay_from_sequence_after_clear) {
    SequencedStore st(1000, 100'000);
    for (int i = 1; i <= 1000; ++i) {
        const std::string m = "msg" + std::to_string(i);
        REQUIRE(st.append(bytes(m), static_cast<std::uint64_t>(i)));
    }
    for (std::uint64_t s = 990; s <= st.last_seq(); ++s) {
        CHECK(str(st.get(s).bytes) == "msg" + std::to_string(s));
        CHECK_EQ(st.get(s).meta, s);
    }
    st.clear();
    CHECK_EQ(st.last_seq(), 0u);
    CHECK(st.append(bytes("new")));
    CHECK(str(st.get(1).bytes) == "new");
}
