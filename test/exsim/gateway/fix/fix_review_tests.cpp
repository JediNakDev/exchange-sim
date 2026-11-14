// Independent QA review reproducers for the FIX 4.4 order-entry gateway.
//
// Every test here asserts behaviour required by the FIX 4.4 session
// specification or by the gateway rules in docs/fix-gateway.md. Each test
// reproduces a review finding, and its name starts with the finding id.

#include "test/exsim/gateway/fix/fix_test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace fix_test;

namespace {

constexpr std::uint64_t kMaxSequence = UINT64_MAX;

std::uint64_t expected_inbound(FixTestExchange& exchange, exsim::SessionId session = 2) {
    return exchange.user(session).session_day_state().expected_inbound_seq();
}

std::size_t count_type(const std::vector<ParsedFixMessage>& messages, std::string_view type) {
    std::size_t count = 0;
    for (const auto& message : messages) {
        if (message.type() == type) ++count;
    }
    return count;
}

const ParsedFixMessage* first_of_type(const std::vector<ParsedFixMessage>& messages, std::string_view type) {
    for (const auto& message : messages) {
        if (message.type() == type) return &message;
    }
    return nullptr;
}

std::size_t cancel_on_disconnect_commands(const FixTestExchange& exchange, exsim::SessionId session) {
    std::size_t count = 0;
    for (const auto& command : exchange.submitted_commands) {
        if (command.type == exsim::CommandType::MassCancel && command.mass_cancel.session == session &&
            command.mass_cancel.cancel_on_disconnect) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// R1 (medium). FixConnection::on_disconnected unconditionally unbinds the day
// state's sink and calls ctx.unbind for its session, even when the session
// was already released by the server (ExchangeCore::take_disconnect_requests
// says "Close each one and call unbind") and re-bound by a newer connection.
// The stale connection then silences the live connection's output and fires
// cancel-on-disconnect against the live session.
TEST_CASE(gateway_fix_review, r1_stale_connection_disconnect_must_not_break_newer_login) {
    FixTestExchange exchange;
    FixTestConnection stale(exchange, "CLIENT2");
    stale.logon(30, false, {});

    // Admin disconnect path: the server releases the session first.
    exchange.unbind(2, stale.connection().connection_id());
    REQUIRE(!exchange.is_bound(2));

    FixTestConnection live(exchange, "CLIENT2");
    live.next_sequence = stale.next_sequence;
    live.logon(30, false, {});
    REQUIRE(exchange.is_bound(2));
    live.send("D", new_order_fields("LIVE1", "1", 100, "150"));
    REQUIRE_EQ(count_type(live.drain(), "8"), std::size_t{1});
    const std::size_t cancels_before = cancel_on_disconnect_commands(exchange, 2);

    // The transport of the stale connection is finally torn down.
    stale.disconnect();

    CHECK(exchange.is_bound(2));
    CHECK_EQ(cancel_on_disconnect_commands(exchange, 2), cancels_before);
    live.send("D", new_order_fields("LIVE2", "1", 100, "150"));
    CHECK_EQ(count_type(live.drain(), "8"), std::size_t{1});
    CHECK(exchange.user(2).session_day_state().has_sink());
}

// R2 (low). MsgSeqNum arithmetic wraps: SequenceReset-Reset may set expected
// to UINT64_MAX and the in-order message at that number sets expected to
// frame.sequence + 1 == 0 (fix_session_layer.hpp:573). Every later message is
// then "high" and the ResendRequest carries the invalid BeginSeqNo(7)=0.
TEST_CASE(gateway_fix_review, r2_expected_sequence_must_not_wrap_to_zero) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("4", 2, "36=" + std::to_string(kMaxSequence) + "|");
    REQUIRE_EQ(expected_inbound(exchange), kMaxSequence);
    client.send_at("0", kMaxSequence, "");
    CHECK(expected_inbound(exchange) != 0u);

    client.drain();
    client.send_at("0", 5, "");
    for (const auto& message : client.drain()) {
        if (message.type() == "2") {
            CHECK(message.get(7) != std::string("0"));
        }
    }
}

// R3 (low). FIX 4.4: a message with PossDupFlag(43)=Y must carry
// OrigSendingTime(122); when it is missing the receiver sends Reject(3)
// 373=1 371=122 and ignores the message. The gateway applies it.
TEST_CASE(gateway_fix_review, r3_possdup_without_orig_sending_time_is_rejected) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.feed(client_message("D", 2, new_order_fields("PD1", "1", 100, "150"), "CLIENT2", "43=Y|"));
    const auto messages = client.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "3");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(371), std::string("122"));
    CHECK_EQ(reject->get(373), std::string("1"));
    CHECK(engine_command_tokens(exchange, 2).empty());
}

// R4 (low). FIX 4.4: OrigSendingTime(122) later than SendingTime(52) is a
// SendingTime accuracy problem: Reject(3) 373=10, then Logout.
TEST_CASE(gateway_fix_review, r4_orig_sending_time_after_sending_time_is_rejected) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.feed(client_message("D", 2, new_order_fields("PD2", "1", 100, "150"), "CLIENT2",
                               "43=Y|122=20261103-09:31:00.000|"));
    const auto messages = client.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "3");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(373), std::string("10"));
    CHECK(engine_command_tokens(exchange, 2).empty());
}

// R5 (low). A repeated early ResendRequest is serviced each time it arrives
// (service_early runs before the hold queue reports AlreadyHeld,
// fix_session_layer.hpp:495-505), so a duplicate retransmits the whole range
// again while the duplicate itself is counted as suppressed.
TEST_CASE(gateway_fix_review, r5_duplicate_early_resend_request_serviced_once) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("2", 5, "7=1|16=0|");
    client.send_at("2", 5, "7=1|16=0|");
    CHECK_EQ(client.connection().counters().resend_requests_serviced, 1u);
    CHECK_EQ(count_type(client.drain(), "4"), std::size_t{1});
}

// R6 (low). FIX 4.4: BeginString(8), BodyLength(9) and MsgType(35) must be
// the first three fields; otherwise the message is garbled and must not
// touch MsgSeqNum. The codec does not check field order, so the gateway
// consumes the sequence number.
TEST_CASE(gateway_fix_review, r6_msg_type_not_third_field_is_garbled) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    const std::uint64_t garbled_before = client.connection().counters().garbled_frames;
    client.feed(fix_frame("49=CLIENT2|35=0|56=EXSIM|34=2|52=" + std::string(kStartTimestamp) + "|"));
    CHECK_EQ(expected_inbound(exchange), 2u);
    CHECK_EQ(client.connection().counters().garbled_frames, garbled_before + 1);
}

// R7 (low). GapFillFlag(123) is a Boolean: any value other than Y or N is a
// value error (Reject 373=5). The gateway treats every non-"Y" value as
// SequenceReset-Reset and moves expected (fix_session_layer.hpp:444).
TEST_CASE(gateway_fix_review, r7_invalid_gap_fill_flag_is_rejected) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send_at("4", 2, "123=X|36=10|");
    CHECK(expected_inbound(exchange) != 10u);
    const auto messages = client.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "3");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(371), std::string("123"));
    CHECK_EQ(reject->get(373), std::string("5"));
}

// R8 (low). FIX 4.4 OrderCancelRequest(F) and OrderCancelReplaceRequest(G)
// require TransactTime(60). NewOrderSingle checks it
// (fix_connection.hpp:240); F and G do not (fix_connection.hpp:248-278).
TEST_CASE(gateway_fix_review, r8_cancel_and_replace_require_transact_time) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send("D", new_order_fields("B1", "1", 100, "150"));
    client.drain();

    client.send("F", "41=B1|11=C1|55=AAPL|54=1|");
    auto messages = client.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "3");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(371), std::string("60"));
    CHECK_EQ(reject->get(373), std::string("1"));

    client.send("G", "41=B1|11=R1|55=AAPL|54=1|38=50|40=2|44=150|");
    messages = client.drain();
    reject = first_of_type(messages, "3");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(371), std::string("60"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (std::vector<std::string>{"D:B1"}));
}

// R9 (low). OrderMassCancelReport(r) with MassCancelResponse(531)=0 must say
// why in MassCancelRejectReason(532). Neither the codec nor the gateway can
// emit 532, and a valid but unsupported MassCancelRequestType(530) (2..6) is
// answered with a session Reject(3) instead of 531=0 532=0.
TEST_CASE(gateway_fix_review, r9_mass_cancel_rejections_carry_reject_reason) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send("q", "11=MC1|530=1|55=NOPE|");
    auto messages = client.drain();
    const ParsedFixMessage* report = first_of_type(messages, "r");
    REQUIRE(report != nullptr);
    CHECK_EQ(report->get(531), std::string("0"));
    CHECK(report->has(532));

    client.send("q", "11=MC2|530=5|167=CS|");
    messages = client.drain();
    report = first_of_type(messages, "r");
    REQUIRE(report != nullptr);
    CHECK_EQ(report->get(531), std::string("0"));
    CHECK_EQ(report->get(532), std::string("0"));
}

// R10 (low, design). Every outbound session message (Heartbeat, TestRequest,
// ResendRequest, Logon, Reject) takes a resend-store slot; the store never
// evicts. A client flooding TestRequest(1) exhausts its own store and the
// session is then refused for the rest of the day unless it resets. With the
// default 65536 slots and HeartBtInt=1, an idle session dies after ~18 hours
// of heartbeats alone.
TEST_CASE(gateway_fix_review, r10_session_messages_do_not_exhaust_resend_store) {
    fix::FixUserLimits limits = small_limits();
    limits.store_max_messages = 32;
    FixTestExchange exchange(limits);
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    for (int index = 0; index < 64 && !client.connection().wants_disconnect(); ++index) {
        client.send("1", "112=P" + std::to_string(index) + "|");
    }
    CHECK(!client.connection().wants_disconnect());
    CHECK(!exchange.user(2).session_day_state().store_exhausted());
}

// R11 (low). A syntactically valid but unsupported application MsgType should
// get BusinessMessageReject(j) 380=3; Reject(3) 373=11 is for invalid MsgTypes.
TEST_CASE(gateway_fix_review, r11_unsupported_application_type_gets_business_reject) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send("AF", "584=Q1|585=1|");
    const auto messages = client.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "j");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(380), std::string("3"));
}

// R12 (low). FixConnection::start() on a connection that is still bound
// clears session_bound_ without ctx.unbind (fix_connection.hpp:62-66) and the
// destructor does the same (fix_connection.hpp:57). The user stays bound in
// the server registry and every later Logon is refused "already logged in".
TEST_CASE(gateway_fix_review, r12_restart_of_bound_connection_releases_session) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    TestByteSink other_sink;
    client.connection().start(other_sink, 77, exchange);
    CHECK(!exchange.is_bound(2));
}

// Coverage the existing suite lacks (expected to pass): an early Heartbeat
// whose TestReqID matches the outstanding probe clears it, and a Logout
// received while our Logout is outstanding is not answered again.
TEST_CASE(gateway_fix_review, coverage_early_matching_heartbeat_clears_probe) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(10, false, {});
    client.tick(15 * fix::kNanosecondsPerSecond);
    const auto messages = client.drain();
    const ParsedFixMessage* probe = first_of_type(messages, "1");
    REQUIRE(probe != nullptr);
    client.send_at("0", client.next_sequence + 3, "112=" + probe->get(112) + "|");
    CHECK(!client.connection().test_request_outstanding());
    client.tick(5 * fix::kNanosecondsPerSecond);
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_review, coverage_logout_reply_not_doubled) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    REQUIRE(client.connection().begin_logout("bye", exchange));
    REQUIRE_EQ(count_type(client.drain(), "5"), std::size_t{1});
    client.send("5", "");
    CHECK(client.drain().empty());
    CHECK(client.connection().wants_disconnect());
}

// R13 (low). OrderCancelReject(9) OrdStatus(39) is taken from the gateway's
// record of OrigClOrdID (fix_user_state.hpp:431-438) while CxlRejReason(102)
// comes from the engine. FIX 4.4 says 102=1 (Unknown order) goes with 39=8;
// a superseded ClOrdID of a live replaced order is reported 39=4 (Canceled)
// although the order is still working, and a filled order gets 102=1 39=2
// instead of 102=0 (Too late to cancel) 39=2.
TEST_CASE(gateway_fix_review, r13_cancel_reject_status_matches_reason) {
    FixTestExchange exchange;
    FixTestConnection buyer(exchange, "CLIENT1");
    FixTestConnection seller(exchange, "CLIENT2");
    buyer.logon();
    seller.logon(30, false, {});
    const std::string transact = "60=" + std::string(kStartTimestamp) + "|";

    buyer.send("D", new_order_fields("B1", "1", 100, "150"));
    buyer.send("G", "41=B1|11=B1R|55=AAPL|54=1|38=100|40=2|44=149|" + transact);
    buyer.drain();
    buyer.send("F", "41=B1|11=C1|55=AAPL|54=1|" + transact);  // superseded ClOrdID
    auto messages = buyer.drain();
    const ParsedFixMessage* reject = first_of_type(messages, "9");
    REQUIRE(reject != nullptr);
    CHECK_EQ(reject->get(102), std::string("1"));
    CHECK_EQ(reject->get(39), std::string("8"));

    buyer.send("D", new_order_fields("B2", "1", 10, "151"));
    seller.send("D", new_order_fields("S2", "2", 10, "151"));
    buyer.drain();
    buyer.send("F", "41=B2|11=C2|55=AAPL|54=1|" + transact);  // fully filled
    messages = buyer.drain();
    reject = first_of_type(messages, "9");
    REQUIRE(reject != nullptr);
    const bool consistent = (reject->get(102) == "0" && reject->get(39) == "2") ||
                            (reject->get(102) == "1" && reject->get(39) == "8");
    CHECK(consistent);
}
