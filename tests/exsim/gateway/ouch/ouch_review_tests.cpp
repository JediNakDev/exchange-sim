// Independent QA review reproducers for the OUCH 4.2 / SoupBinTCP gateway
// (review of e2cdd11), updated to assert the fixed behaviour. See
// /tmp/exsim-review-proto/REPORT.md, findings F1 and F2.

#include "ouch_test_harness.hpp"

#include <string>
#include <vector>

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;

const ReceivedMessage* find_accepted(const RecordingHandler& handler, std::string_view token) {
    for (const ReceivedMessage& message : handler.messages) {
        if (message.type == 'A' && token_text(message.accepted.token) == token) return &message;
    }
    return nullptr;
}

std::size_t count_accepted(const RecordingHandler& handler, std::string_view token) {
    std::size_t count = 0;
    for (const ReceivedMessage& message : handler.messages) {
        count += message.type == 'A' && token_text(message.accepted.token) == token ? 1 : 0;
    }
    return count;
}

}  // namespace

// Finding F1 (fixed). SoupBinTCP Sequenced Data packets carry no sequence
// number: the client numbers them by counting, so a gap in a live stream can
// never be detected. `fault user X drop_out n` is therefore a transport cut:
// the n messages are stored but not written, nothing after them is written,
// and the connection closes after the n-th. A client reconnecting with its
// own count + 1 receives every dropped message once, correctly numbered.
TEST_CASE(gateway_ouch_review, drop_out_is_a_transport_cut_recovered_from_the_clients_own_count) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> bob(exchange, handler);  // no cancel-on-disconnect
    REQUIRE(bob.login("bob", "bobpw", 1));                // 1: S

    REQUIRE(bob.client().enter_order(enter_order("ASK1", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 2: A ASK1
    exchange.drop_next_outbound_messages(2, 2);
    REQUIRE(bob.client().enter_order(enter_order("ASK2", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 3: A ASK2, stored, not transmitted
    CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);  // one drop left
    REQUIRE(bob.client().enter_order(enter_order("ASK3", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 4: A ASK3, stored, not transmitted; the link is cut

    CHECK(bob.connection().close_reason() == OuchCloseReason::FaultDropOut);
    CHECK(find_accepted(handler, "ASK2") == nullptr);
    CHECK(find_accepted(handler, "ASK3") == nullptr);
    CHECK_EQ(bob.client().next_expected_sequence(), 3u);  // no message was mislabelled
    CHECK_EQ(exchange.user_state(2).statistics().outbound_messages_dropped_by_fault, 2u);

    // Documented recovery: reconnect asking for the client's next expected sequence.
    bob.drop_transport();
    REQUIRE(bob.login("bob", "bobpw", bob.client().next_expected_sequence()));
    const ReceivedMessage* ask2 = find_accepted(handler, "ASK2");
    const ReceivedMessage* ask3 = find_accepted(handler, "ASK3");
    REQUIRE(ask2 != nullptr);
    REQUIRE(ask3 != nullptr);
    CHECK_EQ(ask2->sequence, 3u);
    CHECK_EQ(ask3->sequence, 4u);
    CHECK_EQ(count_accepted(handler, "ASK1"), 1u);
    CHECK_EQ(count_accepted(handler, "ASK2"), 1u);
    CHECK_EQ(count_accepted(handler, "ASK3"), 1u);
    CHECK_EQ(bob.client().next_expected_sequence(), exchange.user_state(2).store().next_seq());
    CHECK(handler.sequence_mismatches.empty());
}

// Finding F2 (fixed). store_reserve_messages keeps room for reports on live
// orders. Admission is checked before any gateway J is stored; at the reserve
// an over-limit request stores nothing: the connection closes (not End of
// Session), the refusal is counted, and a normal reconnect with replay works.
// The execution of the resting order then still fits.
TEST_CASE(gateway_ouch_review, gateway_rejects_consume_the_store_reserve) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.store_messages = 40;
    config.limits.store_reserve_messages = 8;
    TestExchange exchange(config);

    RawSession bob(exchange);
    REQUIRE(bob.login("bob", "bobpw"));                                        // 1: S
    bob.send_message(enter_order("ASK", 'S', 100, "AAPL", kHundredDollars));  // 2: A, rests
    REQUIRE_EQ(bob.take_types(), "SA");

    // A misbehaving or buggy client keeps sending orders for an unknown symbol.
    for (int index = 0; index < 60; ++index) {
        bob.send_message(enter_order("SPAM" + std::to_string(index), 'B', 1, "ZZZZ", kHundredDollars));
    }
    // 30 J fit above the reserve (messages 3 to 32); the next request closes.
    CHECK_EQ(bob.take_types(), std::string(30, 'J'));
    CHECK(bob.connection().close_reason() == OuchCloseReason::StoreReserveReached);
    CHECK(!exchange.user_state(2).store_overflowed());
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 32u);
    CHECK_EQ(exchange.user_state(2).statistics().requests_refused_at_store_reserve, 1u);
    bob.disconnect();

    RawSession bob_again(exchange);
    REQUIRE(bob_again.login("bob", "bobpw", 1));
    CHECK_EQ(bob_again.take_types(), "SA" + std::string(30, 'J'));  // full replay

    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.send_message(enter_order("BID", 'B', 100, "AAPL", kHundredDollars));  // executes against ASK
    REQUIRE(exchange.find_live_order(2, padded_token("ASK")) == std::nullopt);

    // Bob receives the execution of his live order.
    CHECK_EQ(bob_again.take_types(), "E");
    CHECK(bob_again.connection().state() == OuchConnectionState::LoggedIn);
    CHECK(!exchange.user_state(2).store_overflowed());
    CHECK_EQ(exchange.user_state(2).statistics().messages_lost_to_store_overflow, 0u);
}
