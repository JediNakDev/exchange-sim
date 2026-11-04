// Independent QA review reproducers for the OUCH 4.2 / SoupBinTCP gateway
// (review of e2cdd11). Each test documents a defect and is expected to FAIL
// until the gateway is fixed. See /tmp/exsim-review-proto/REPORT.md.

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

// Finding F1. SoupBinTCP Sequenced Data packets carry no sequence number: the
// client numbers them by counting. `fault user X drop_out n` stores a message
// but skips it on the wire (ouch_server.hpp deliver_stored_messages), so every
// later message is numbered one too low by any spec-conformant client, and a
// reconnect with the client's next expected sequence replays a message the
// client already has instead of the dropped one. SERVER.md says the fault
// "exercises client recovery on reconnect"; for OUCH that recovery is
// impossible. The existing fault_hooks_reject_and_drop test hides this by
// re-logging in with the true sequence, which a real client cannot know.
TEST_CASE(gateway_ouch_review, drop_out_desynchronises_implicit_soup_sequence) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> bob(exchange, handler);  // no cancel-on-disconnect
    REQUIRE(bob.login("bob", "bobpw", 1));                // 1: S

    REQUIRE(bob.client().enter_order(enter_order("ASK1", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 2: A ASK1
    exchange.drop_next_outbound_messages(2, 1);
    REQUIRE(bob.client().enter_order(enter_order("ASK2", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 3: A ASK2, stored, not transmitted
    REQUIRE(bob.client().enter_order(enter_order("ASK3", 'S', 10, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();  // 4: A ASK3

    const ReceivedMessage* ask3 = find_accepted(handler, "ASK3");
    REQUIRE(ask3 != nullptr);
    // The server stored ASK3 as message 4; the client numbers it 3.
    CHECK_EQ(ask3->sequence, 4u);
    CHECK_EQ(bob.client().next_expected_sequence(), exchange.user_state(2).store().next_seq());

    // Documented recovery: reconnect asking for the client's next expected sequence.
    bob.drop_transport();
    REQUIRE(bob.login("bob", "bobpw", bob.client().next_expected_sequence()));
    // The dropped Accepted for ASK2 is never recovered...
    CHECK(find_accepted(handler, "ASK2") != nullptr);
    // ...and ASK3's Accepted is delivered a second time.
    CHECK_EQ(count_accepted(handler, "ASK3"), 1u);
}

// Finding F2. store_reserve_messages exists to keep "room for reports on
// orders that are already live" (OuchUserStateLimits comment), but every
// gateway J reject, including the reserve reject itself and the unknown-stock
// reject, is appended to the same store. A session that keeps sending orders
// after the reserve is reached fills the store with J messages; the next
// report (here an execution of a resting order) overflows it, the session is
// closed with End of Session (StoreFull) and cannot log in again until the
// next trading day.
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
    bob.take_frames();

    // The reserve must still hold room for the report on ASK.
    CHECK(!exchange.user_state(2).store_overflowed());
    CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);

    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.send_message(enter_order("BID", 'B', 100, "AAPL", kHundredDollars));  // executes against ASK
    REQUIRE(!exchange.find_live_order(2, "ASK").has_value());

    // Bob should receive the execution of his live order.
    CHECK(sequenced_types(bob.take_frames()).find('E') != std::string::npos);
    CHECK(!exchange.user_state(2).store_overflowed());
    CHECK_EQ(exchange.user_state(2).statistics().messages_lost_to_store_overflow, 0u);
}
