// Outbound recovery: ResendRequest answered from the day store with
// PossDupFlag=Y, OrigSendingTime and the original MsgSeqNum; session messages
// replaced by SequenceReset-GapFill; recovery after orders complete and across
// reconnects.

#include "tests/exsim/gateway/fix/fix_test_support.hpp"

#include <string>
#include <vector>

using namespace fix_test;

namespace {

using Tokens = std::vector<std::string>;
constexpr exsim::Timestamp kSecond = fix::kNanosecondsPerSecond;

const std::string kAcceptedBody =
    "37=1|11=ORD1|17=1|150=0|39=0|55=AAPL|54=1|38=100|44=150.25|32=0|31=0|151=100|14=0|6=0|60=20261103-09:30:00.000|";

}  // namespace

TEST_CASE(gateway_fix_resend, possdup_resend_golden) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.send("D", new_order_fields("ORD1", "1", 100, "150.25"));
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("8", 2, kAcceptedBody)));

    client.tick(kSecond);
    client.send("2", "7=1|16=0|");
    messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(fix_frame("35=4|49=EXSIM|56=CLIENT1|34=1|43=Y|52=20261103-09:30:01.000|"
                                  "122=20261103-09:30:01.000|123=Y|36=2|")));
    CHECK_EQ(messages[1].pipes(),
             with_pipes(fix_frame("35=8|49=EXSIM|56=CLIENT1|34=2|43=Y|52=20261103-09:30:01.000|"
                                  "122=20261103-09:30:00.000|" +
                                  kAcceptedBody)));
    // Resends do not consume outbound sequence numbers.
    CHECK_EQ(exchange.user(1).session_day_state().next_outbound_seq(), 3u);
}

TEST_CASE(gateway_fix_resend, session_messages_in_range_become_gap_fills) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.send("D", new_order_fields("ORD1", "1", 100, "150.25"));  // ER 2
    client.tick(30 * kSecond);                                        // Heartbeat 3
    client.send("1", "112=X|");                                       // Heartbeat 4
    client.send("D", new_order_fields("ORD2", "1", 100, "150.25"));  // ER 5
    client.drain();

    client.send("2", "7=2|16=5|");
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{3});
    CHECK_EQ(messages[0].sequence(), 2u);
    CHECK_EQ(messages[0].get(43), std::string("Y"));
    CHECK_EQ(messages[0].get(11), std::string("ORD1"));
    CHECK_EQ(messages[1].type(), std::string("4"));
    CHECK_EQ(messages[1].sequence(), 3u);
    CHECK_EQ(messages[1].get(36), std::string("5"));
    CHECK_EQ(messages[2].sequence(), 5u);
    CHECK_EQ(messages[2].get(11), std::string("ORD2"));

    // Only session messages: one gap fill. End beyond the last sent is clamped.
    client.send("2", "7=3|16=4|");
    messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(36), std::string("5"));
    client.send("2", "7=4|16=99|");
    messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].type(), std::string("4"));
    CHECK_EQ(messages[0].sequence(), 4u);
    CHECK_EQ(messages[0].get(36), std::string("5"));
    CHECK_EQ(messages[1].sequence(), 5u);
    // Beginning past the last sent message: nothing to resend.
    client.send("2", "7=50|16=0|");
    CHECK(client.drain().empty());
}

TEST_CASE(gateway_fix_resend, resend_after_orders_complete_and_reconnect) {
    FixTestExchange exchange;
    FixTestConnection buyer(exchange, "CLIENT1");
    FixTestConnection seller(exchange, "CLIENT2");
    buyer.logon();
    seller.logon(30, false, {});
    buyer.send("D", new_order_fields("B1", "1", 100, "150"));
    seller.send("D", new_order_fields("S1", "2", 60, "150"));
    seller.send("D", new_order_fields("S2", "2", 40, "150"));
    buyer.send("D", new_order_fields("B2", "1", 10, "149"));
    buyer.send("F", "41=B2|11=C2|55=AAPL|54=1|60=20261103-09:30:00.000|");
    const auto live = buyer.drain();
    REQUIRE_EQ(live.size(), std::size_t{5});  // accepted, fill, fill (complete), accepted, canceled
    CHECK_EQ(live[2].get(39), std::string("2"));
    CHECK_EQ(live[4].get(150), std::string("4"));

    // Everything is complete; the client reconnects and recovers the day.
    buyer.reconnect();
    buyer.logon();
    buyer.tick(kSecond);
    buyer.send("2", "7=1|16=0|");
    const auto recovered = buyer.drain();
    // GapFill(1) + 5 application messages + GapFill(7, the second Logon).
    REQUIRE_EQ(recovered.size(), std::size_t{7});
    CHECK_EQ(recovered[0].type(), std::string("4"));
    for (std::size_t index = 0; index < live.size(); ++index) {
        const ParsedFixMessage& original = live[index];
        const ParsedFixMessage& again = recovered[index + 1];
        CHECK_EQ(again.sequence(), original.sequence());
        CHECK_EQ(again.get(43), std::string("Y"));
        CHECK_EQ(again.get(122), original.get(52));
        CHECK_EQ(again.get(52), std::string("20261103-09:30:01.000"));
        CHECK_EQ(again.get(17), original.get(17));
        CHECK_EQ(again.get(11), original.get(11));
        CHECK_EQ(again.get(14), original.get(14));
        CHECK_EQ(again.get(10).size(), std::size_t{3});
    }
    CHECK_EQ(recovered[6].type(), std::string("4"));
    CHECK_EQ(recovered[6].sequence(), 7u);
    CHECK_EQ(recovered[6].get(36), std::string("8"));
}

TEST_CASE(gateway_fix_resend, store_capacity_exhaustion_fails_closed) {
    fix::FixUserLimits limits = small_limits();
    // Session messages take no slot: two ExecutionReports fill the store.
    limits.store_max_messages = 2;
    FixTestExchange exchange(limits);
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.send("D", new_order_fields("ORD1", "1", 100, "150"));
    client.send("D", new_order_fields("ORD2", "1", 100, "150"));
    client.send("D", new_order_fields("ORD3", "1", 100, "150"));
    CHECK_EQ(client.drain().size(), std::size_t{2});
    CHECK(client.connection().wants_disconnect());
    CHECK(exchange.user(1).session_day_state().store_exhausted());
    CHECK_EQ(exchange.user(1).session_day_state().next_outbound_seq(), 4u);
    CHECK_EQ(client.feed(client_message("D", 5, new_order_fields("ORD4", "1", 100, "150"))), 0u);
    CHECK_EQ(exchange.commands_of(1).size(), std::size_t{3});
    client.reconnect();
    client.send("A", "98=0|108=30|554=secret1|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].type(), std::string("5"));
    CHECK(client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_resend, output_overflow_requests_disconnect) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.sink().set_refuse_prepare(true);
    client.send("D", new_order_fields("ORD1", "1", 100, "150"));
    CHECK(client.connection().wants_disconnect());
    // The message was sequenced and stored; it is recoverable after reconnect.
    client.sink().set_refuse_prepare(false);
    CHECK_EQ(exchange.user(1).session_day_state().store().last_seq(), 1u);
    CHECK(!exchange.user(1).session_day_state().stored_message(2).bytes.empty());
    client.reconnect();
    client.logon();
    client.send("2", "7=2|16=2|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(11), std::string("ORD1"));
    CHECK_EQ(messages[0].get(43), std::string("Y"));
}

TEST_CASE(gateway_fix_resend, byte_capacity_exhaustion_is_explicit) {
    auto limits = small_limits();
    limits.store_max_bytes = 1;
    FixTestExchange exchange(limits);
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.send("D", new_order_fields("ORDER", "1", 100, "150"));
    CHECK(client.drain().empty());
    CHECK(client.connection().wants_disconnect());
    CHECK(exchange.user(1).session_day_state().store_exhausted());
    CHECK_EQ(exchange.user(1).session_day_state().messages_not_stored(), 1u);
    CHECK_EQ(exchange.user(1).session_day_state().store().last_seq(), 0u);
}

TEST_CASE(gateway_fix_resend, token_capacity_and_reset_preserve_day_identity) {
    auto limits = small_limits();
    limits.max_tokens = 2;
    FixTestExchange exchange(limits);
    FixTestConnection client(exchange, "CLIENT1");
    client.logon();
    client.send("D", new_order_fields("ORDER", "1", 100, "150"));
    client.send("F", "41=ORDER|11=CANCEL|55=AAPL|54=1|60=20261103-09:30:00.000|");
    client.send("D", new_order_fields("OVERFLOW", "1", 100, "150"));
    auto frames = client.drain();
    REQUIRE_EQ(frames.size(), 3u);
    CHECK_EQ(frames[2].get(39), std::string("8"));
    CHECK_EQ(frames[2].get(58), std::string("ClOrdID capacity exhausted"));
    CHECK_EQ(exchange.commands_of(1).size(), 2u);
    const auto issued = exchange.user(1).execution_ids_issued();
    client.reconnect();
    client.logon(30, true);
    client.send("D", new_order_fields("ORDER", "1", 100, "150"));
    frames = client.drain();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].get(103), std::string("6"));
    CHECK_EQ(frames[0].get(17), std::to_string(issued + 1));
    CHECK_EQ(exchange.commands_of(1).size(), 2u);
}
