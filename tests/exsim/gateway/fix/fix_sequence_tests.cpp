// Inbound MsgSeqNum handling (docs/GATEWAYS.md 3, session layer): gaps with
// trading messages, gap fills (in order, duplicate, overlapping, high),
// SequenceReset-Reset, too-low sequences, PossDup suppression, bounded hold
// queue overflow, early service of ResendRequest, CompID and structural errors.

#include "tests/exsim/gateway/fix/fix_test_support.hpp"

#include <string>
#include <vector>

using namespace fix_test;

namespace {

using Tokens = std::vector<std::string>;

std::string order(std::string_view client_order_id) {
    return new_order_fields(client_order_id, "1", 100, "150", "AAPL");
}

std::uint64_t expected_inbound(FixTestExchange& exchange) {
    return exchange.user(2).session_day_state().expected_inbound_seq();
}

}  // namespace

TEST_CASE(gateway_fix_sequence, gap_with_trading_messages_is_held_until_filled) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 3, order("B3"));
    client.send_at("D", 4, order("B4"));
    // Nothing reaches the engine before 2 arrives.
    CHECK(engine_command_tokens(exchange, 2).empty());
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("2", 2, "7=2|16=0|", "CLIENT2")));
    CHECK_EQ(client.connection().held_message_count(), std::size_t{2});

    client.send_at("D", 2, order("B2"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2", "D:B3", "D:B4"}));
    CHECK_EQ(expected_inbound(exchange), 5u);
    CHECK(!client.connection().resend_request_outstanding());
    CHECK_EQ(client.connection().held_message_count(), std::size_t{0});
    CHECK_EQ(message_types(client.drain()), (Tokens{"8", "8", "8"}));

    // The client's PossDup resends of 3 and 4 are suppressed.
    client.send_at("D", 3, order("B3"), true);
    client.send_at("D", 4, order("B4"), true);
    CHECK_EQ(engine_command_tokens(exchange, 2).size(), std::size_t{3});
    CHECK(client.drain().empty());
    CHECK_EQ(client.connection().counters().duplicates_suppressed, 2u);
}

TEST_CASE(gateway_fix_sequence, gap_fill_releases_held_message) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 4, order("B4"));
    client.drain();
    client.send_at("4", 2, "123=Y|36=4|", true);
    CHECK_EQ(expected_inbound(exchange), 5u);
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B4"}));

    // Duplicate gap fill (NewSeqNo not above expected): suppressed.
    client.send_at("4", 2, "123=Y|36=4|", true);
    CHECK_EQ(expected_inbound(exchange), 5u);
    // Overlapping duplicate gap fill starting below expected advances to NewSeqNo.
    client.send_at("4", 3, "123=Y|36=7|", true);
    CHECK_EQ(expected_inbound(exchange), 7u);
    // An in-order gap fill advances too.
    client.send_at("4", 7, "123=Y|36=9|");
    CHECK_EQ(expected_inbound(exchange), 9u);
    client.drain();
    client.send_at("D", 9, order("B9"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B4", "D:B9"}));
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, high_gap_fill_waits_for_its_gap) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("4", 3, "123=Y|36=6|", true);
    CHECK_EQ(expected_inbound(exchange), 2u);
    client.send_at("D", 2, order("B2"));
    CHECK_EQ(expected_inbound(exchange), 6u);
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2"}));
}

TEST_CASE(gateway_fix_sequence, gap_fill_not_above_its_sequence_is_rejected) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("4", 2, "123=Y|36=2|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("3", 2, "45=2|371=36|372=4|373=5|58=NewSeqNo must exceed MsgSeqNum|",
                                         "CLIENT2")));
    CHECK_EQ(expected_inbound(exchange), 3u);
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, low_gap_fill_without_possdup_logs_out) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("0", 2, "");
    client.send_at("4", 2, "123=Y|36=9|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("5", 2, "58=MsgSeqNum too low, expecting 3 but received 2|", "CLIENT2")));
    CHECK(client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, sequence_reset_reset_mode) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("0", 2, "");
    client.send_at("0", 3, "");
    // Decreasing NewSeqNo: rejected, expected unchanged. MsgSeqNum is ignored.
    client.send_at("4", 99, "123=N|36=2|");
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("3", 2, "45=99|371=36|372=4|373=5|58=attempt to lower sequence number|",
                                         "CLIENT2")));
    CHECK_EQ(expected_inbound(exchange), 4u);
    // Equal: no effect. Increasing: advances without application delivery.
    client.send_at("4", 1, "123=N|36=4|");
    CHECK_EQ(expected_inbound(exchange), 4u);
    client.send_at("4", 1, "123=N|36=20|");
    CHECK_EQ(expected_inbound(exchange), 20u);
    CHECK(client.drain().empty());
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, sequence_reset_reset_releases_held_messages) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 5, order("B5"));
    client.drain();
    client.send_at("4", 1, "123=N|36=5|");
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B5"}));
    CHECK_EQ(expected_inbound(exchange), 6u);
    CHECK(!client.connection().resend_request_outstanding());
}

TEST_CASE(gateway_fix_sequence, too_low_without_possdup_logs_out) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 2, order("B2"));
    client.drain();
    client.send_at("D", 2, order("B2X"));
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("5", 3, "58=MsgSeqNum too low, expecting 3 but received 2|", "CLIENT2")));
    CHECK(client.connection().wants_disconnect());
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2"}));
    // Further input after the decision to disconnect is ignored.
    client.send_at("D", 3, order("B3"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2"}));
}

TEST_CASE(gateway_fix_sequence, duplicate_high_message_is_held_once) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 3, order("B3"));
    client.send_at("D", 3, order("B3"), true);
    CHECK_EQ(client.connection().held_message_count(), std::size_t{1});
    CHECK_EQ(client.connection().counters().resend_requests_sent, 1u);  // one outstanding request
    client.send_at("D", 2, order("B2"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2", "D:B3"}));
}

TEST_CASE(gateway_fix_sequence, bounded_hold_overflow_discards_and_recovers_in_order) {
    FixTestExchange exchange;
    fix::FixSessionSettings settings;
    settings.held_inbound_message_slots = 2;
    FixTestConnection client(exchange, "CLIENT2", settings);
    client.logon(30, false, {});
    for (std::uint64_t sequence = 3; sequence <= 6; ++sequence) {
        client.send_at("D", sequence, order("B" + std::to_string(sequence)));
    }
    CHECK_EQ(client.connection().held_message_count(), std::size_t{2});
    CHECK_EQ(client.connection().counters().held_messages_discarded, 2u);
    CHECK(engine_command_tokens(exchange, 2).empty());

    client.send_at("D", 2, order("B2"));
    // 5 and 6 were discarded: never applied early, recovery still pending.
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2", "D:B3", "D:B4"}));
    CHECK_EQ(expected_inbound(exchange), 5u);
    CHECK(client.connection().resend_request_outstanding());
    client.drain();

    // Progress was made since the request: the retry timer waits another period.
    client.tick(2 * fix::kNanosecondsPerSecond);
    CHECK(client.drain().empty());
    client.tick(2 * fix::kNanosecondsPerSecond);
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].type(), std::string("2"));
    CHECK_EQ(messages[0].get(7), std::string("5"));
    CHECK_EQ(messages[0].get(16), std::string("0"));

    client.send_at("D", 5, order("B5"), true);
    client.send_at("D", 6, order("B6"), true);
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:B2", "D:B3", "D:B4", "D:B5", "D:B6"}));
    CHECK(!client.connection().resend_request_outstanding());
}

TEST_CASE(gateway_fix_sequence, early_resend_request_is_serviced_and_gap_requested) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("D", 2, order("B2"));
    client.drain();  // Accepted ER at exchange seq 2
    client.send_at("2", 4, "7=1|16=0|");
    const auto messages = client.drain();
    // Gap fill for the Logon, the ER again (PossDup), then our own ResendRequest.
    REQUIRE_EQ(messages.size(), std::size_t{3});
    CHECK_EQ(messages[0].type(), std::string("4"));
    CHECK_EQ(messages[0].sequence(), 1u);
    CHECK_EQ(messages[0].get(36), std::string("2"));
    CHECK_EQ(messages[1].type(), std::string("8"));
    CHECK_EQ(messages[1].sequence(), 2u);
    CHECK_EQ(messages[1].get(43), std::string("Y"));
    CHECK_EQ(messages[2].pipes(), with_pipes(exchange_message("2", 3, "7=3|16=0|", "CLIENT2")));
    // When the gap is filled the held ResendRequest is not serviced twice.
    client.send_at("0", 3, "");
    CHECK_EQ(expected_inbound(exchange), 5u);
    CHECK(client.drain().empty());
    CHECK_EQ(client.connection().counters().resend_requests_serviced, 1u);
}

TEST_CASE(gateway_fix_sequence, high_logout_is_answered) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("5", 7, "");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].type(), std::string("5"));
    CHECK(client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, comp_id_problem_rejects_and_logs_out) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.feed(client_message("0", 2, "", "CLIENT9"));
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("3", 2, "45=2|371=49|372=0|373=9|58=CompID problem|", "CLIENT2")));
    CHECK_EQ(messages[1].pipes(), with_pipes(exchange_message("5", 3, "58=CompID problem|", "CLIENT2")));
    CHECK(client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, parseable_semantic_errors_consume_sequence_and_reject) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    // Duplicate tag.
    client.send("0", "112=A|112=B|");
    // Unsupported MsgType.
    client.send("Z", "");
    // Missing required application field (Symbol).
    client.send("D", "11=X1|54=1|38=100|40=2|44=150|59=0|60=20261103-09:30:00.000|");
    // Disallowed value (Side 3).
    client.send("D", "11=X2|55=AAPL|54=3|38=100|40=2|44=150|59=0|60=20261103-09:30:00.000|");
    // Limit order without Price.
    client.send("D", "11=X3|55=AAPL|54=1|38=100|40=2|59=0|60=20261103-09:30:00.000|");
    // ResendRequest with BeginSeqNo 0 and with EndSeqNo below BeginSeqNo.
    client.send("2", "7=0|16=0|");
    client.send("2", "7=5|16=4|");
    // Bad SendingTime.
    client.feed(fix_frame("35=0|49=CLIENT2|56=EXSIM|34=" + std::to_string(client.next_sequence++) +
                          "|52=yesterday|"));
    // Empty value.
    client.send("D", "11=|55=AAPL|54=1|38=100|40=2|44=150|59=0|60=20261103-09:30:00.000|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{9});
    const char* expected[] = {
        "45=2|371=112|372=0|373=13|58=tag appears more than once|",
        "45=3|371=35|372=Z|373=11|58=unsupported MsgType|",
        "45=4|371=55|372=D|373=1|58=required tag missing|",
        "45=5|371=54|372=D|373=5|58=value is incorrect for this tag|",
        "45=6|371=44|372=D|373=1|58=Price(44) required for OrdType 2|",
        "45=7|371=7|372=2|373=5|58=BeginSeqNo must be positive|",
        "45=8|371=16|372=2|373=5|58=EndSeqNo below BeginSeqNo|",
        "45=9|371=52|372=0|373=6|58=incorrect data format|",
        "45=10|371=11|372=D|373=4|58=tag specified without a value|",
    };
    for (std::size_t index = 0; index < messages.size(); ++index) {
        CHECK_EQ(messages[index].pipes(),
                 with_pipes(exchange_message("3", 2 + index, expected[index], "CLIENT2")));
    }
    CHECK_EQ(expected_inbound(exchange), 11u);
    CHECK(engine_command_tokens(exchange, 2).empty());
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_sequence, unexpected_logon_while_active_logs_out) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send("A", "98=0|108=30|141=Y|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("5", 2, "58=unexpected Logon|", "CLIENT2")));
    CHECK(client.connection().wants_disconnect());
    CHECK_EQ(exchange.user(2).session_day_state().next_outbound_seq(), 3u);  // no reset happened
}
