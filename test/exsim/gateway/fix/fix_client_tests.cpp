// FixClient against a real FixConnection in memory: Logon golden, typed
// callbacks, heartbeats and TestRequests in both directions, gap recovery on
// both sides, reconnect, refused Logon, Logout.

#include "test/exsim/gateway/fix/fix_test_support.hpp"

#include <string>
#include <vector>

using namespace fix_test;

namespace {

using Strings = std::vector<std::string>;
constexpr exsim::Timestamp kSecond = fix::kNanosecondsPerSecond;

std::string possdup_suffix(const fix::FixInboundMessageInfo& info) { return info.possible_duplicate ? ":dup" : ""; }

struct RecordingHandler {
    Strings events;
    std::string last_logout_text;

    void on_logon(const fix::FixInboundMessageInfo& info) { events.push_back("A:" + std::to_string(info.sequence)); }
    void on_execution_report(const fix44::ExecutionReport& report, const fix::FixInboundMessageInfo& info) {
        events.push_back("8:" + std::to_string(info.sequence) + ":" + std::string(report.client_order_id.view()) +
                         ":" + std::string(1, report.execution_type) + ":" +
                         std::to_string(report.leaves_quantity) + possdup_suffix(info));
    }
    void on_order_cancel_reject(const fix44::OrderCancelReject& reject, const fix::FixInboundMessageInfo& info) {
        events.push_back("9:" + std::to_string(info.sequence) + ":" + std::string(reject.client_order_id.view()) +
                         ":" + std::string(1, reject.response_to) + ":" + std::to_string(reject.reason));
    }
    void on_order_mass_cancel_report(const fix44::OrderMassCancelReport& report,
                                     const fix::FixInboundMessageInfo& info) {
        events.push_back("r:" + std::to_string(info.sequence) + ":" + std::string(report.client_order_id.view()) +
                         ":" + std::string(1, report.response) + ":" + std::to_string(report.total_affected_orders));
    }
    void on_session_reject(const fix44::Reject& reject, const fix::FixInboundMessageInfo& info) {
        events.push_back("3:" + std::to_string(info.sequence) + ":" + std::to_string(reject.reference_tag) + ":" +
                         std::to_string(reject.reason));
    }
    void on_business_message_reject(const fix44::BusinessMessageReject& reject,
                                    const fix::FixInboundMessageInfo& info) {
        events.push_back("j:" + std::to_string(info.sequence) + ":" + std::string(reject.reference_type.view()) +
                         ":" + std::to_string(reject.reason));
    }
    void on_logout(std::string_view text) {
        last_logout_text = std::string(text);
        events.push_back("5");
    }
};

using Wire = FixClientWire<RecordingHandler>;

struct ConnectedClient {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire{exchange, handler, client_settings("CLIENT1", "secret1")};
    ConnectedClient() {
        wire.connect(false);
        wire.pump();
    }
};

}  // namespace

TEST_CASE(gateway_fix_client, logon_golden_and_handshake) {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire(exchange, handler, client_settings("CLIENT1", "secret1"));
    wire.connect(false);
    CHECK_EQ(with_pipes(wire.client_to_exchange().bytes()),
             with_pipes(fix_frame("35=A|49=CLIENT1|56=EXSIM|34=1|52=20261103-09:30:00.000|98=0|108=30|554=secret1|")));
    wire.pump();
    CHECK(wire.client().active());
    CHECK(wire.connection().phase() == fix::FixConnectionPhase::Active);
    CHECK_EQ(handler.events, (Strings{"A:1"}));
    CHECK_EQ(wire.client().session_day_state().expected_inbound_seq(), 2u);
    CHECK_EQ(wire.client().session_day_state().next_outbound_seq(), 2u);
}

TEST_CASE(gateway_fix_client, reset_logon_golden) {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire(exchange, handler, client_settings("CLIENT2", {}, 10));
    wire.connect(true);
    CHECK_EQ(with_pipes(wire.client_to_exchange().bytes()),
             with_pipes(fix_frame("35=A|49=CLIENT2|56=EXSIM|34=1|52=20261103-09:30:00.000|98=0|108=10|141=Y|")));
    wire.pump();
    CHECK(wire.client().active());
}

TEST_CASE(gateway_fix_client, typed_callbacks_for_every_application_message) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    const auto now = connected.exchange.fix_time();
    CHECK(client.send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 100, 150 * kDollar), now));
    CHECK(client.send_order_cancel_replace_request(
        make_replace_request("B1", "B1R", exsim::Side::BUY, 80, 150 * kDollar), now));
    CHECK(client.send_order_cancel_request(make_cancel_request("NOPE", "C1", exsim::Side::BUY), now));
    CHECK(client.send_new_order_single(make_new_order_single("B2", exsim::Side::BUY, 10, 149 * kDollar), now));
    CHECK(client.send_order_mass_cancel_request(make_mass_cancel_request("MC1"), now));
    // Empty Symbol: the exchange answers with a session Reject (373=4).
    CHECK(client.send_new_order_single(make_new_order_single("B3", exsim::Side::BUY, 10, 149 * kDollar, ""), now));
    connected.wire.pump();
    CHECK_EQ(connected.handler.events, (Strings{"A:1", "8:2:B1:0:100", "8:3:B1R:5:80", "9:4:C1:1:1", "8:5:B2:0:10",
                                                "8:6:MC1:4:0", "8:7:MC1:4:0", "r:8:MC1:7:2", "3:9:55:4"}));
    CHECK_EQ(engine_command_tokens(connected.exchange, 1),
             (Strings{"D:B1", "G:B1R", "D:B2", "q:MC1"}));
}

TEST_CASE(gateway_fix_client, sends_refused_when_not_active) {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire(exchange, handler, client_settings("CLIENT1", "secret1"));
    CHECK(!wire.client().send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 1, kDollar),
                                               exchange.fix_time()));
    wire.connect(false);  // Logon sent, response not yet delivered
    CHECK(!wire.client().send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 1, kDollar),
                                               exchange.fix_time()));
}

TEST_CASE(gateway_fix_client, heartbeats_and_test_requests_both_directions) {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire(exchange, handler, client_settings("CLIENT1", "secret1", 10));
    wire.connect(false);
    wire.pump();
    for (int second = 0; second < 120; ++second) {
        wire.tick(kSecond);
    }
    // Both sides heartbeat every 10 s, so neither ever probes the other.
    CHECK(!wire.wants_disconnect());
    CHECK_EQ(wire.client().counters().test_requests_sent, 0u);
    CHECK_EQ(wire.connection().counters().test_requests_sent, 0u);
    CHECK_GE(wire.client().counters().heartbeats_sent, 11u);
    CHECK_GE(wire.connection().counters().heartbeats_sent, 11u);

    // The exchange goes silent: the client probes after 15 s of inbound silence.
    int silent_seconds = 0;
    while (!wire.client().test_request_outstanding() && silent_seconds < 16) {
        exchange.advance(kSecond);
        wire.client().on_timer(exchange.fix_time());
        ++silent_seconds;
    }
    CHECK(wire.client().test_request_outstanding());
    CHECK(!wire.client().wants_disconnect());
    // The exchange answers the probe with a matching Heartbeat.
    wire.pump();
    CHECK(!wire.client().test_request_outstanding());
    CHECK(!wire.wants_disconnect());
    // Silent again: probe at 15 s, disconnect 5 s later (2 x HeartBtInt).
    for (int second = 0; second < 19; ++second) {
        exchange.advance(kSecond);
        wire.client().on_timer(exchange.fix_time());
    }
    CHECK(wire.client().test_request_outstanding());
    CHECK(!wire.client().wants_disconnect());
    exchange.advance(kSecond);
    wire.client().on_timer(exchange.fix_time());
    CHECK(wire.client().wants_disconnect());
    CHECK_EQ(with_pipes(wire.client_to_exchange().bytes()).find("58=heartbeat timeout") != std::string::npos, true);
}

TEST_CASE(gateway_fix_client, client_recovers_dropped_exchange_messages) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    connected.exchange.user(1).drop_out_transmissions(1);
    const auto now = connected.exchange.fix_time();
    CHECK(client.send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 100, 150 * kDollar), now));
    CHECK(client.send_new_order_single(make_new_order_single("B2", exsim::Side::BUY, 100, 150 * kDollar), now));
    connected.wire.pump();
    // ER 2 was stored but not sent; ER 3 revealed the gap; the client asked
    // for 2.. and delivered 2 (PossDup) then the held 3, each once.
    CHECK_EQ(connected.handler.events, (Strings{"A:1", "8:2:B1:0:100:dup", "8:3:B2:0:100"}));
    CHECK_EQ(client.counters().resend_requests_sent, 1u);
    CHECK_EQ(client.counters().duplicates_suppressed, 1u);  // ER 3 resent as PossDup
    CHECK(!client.resend_request_outstanding());
}

TEST_CASE(gateway_fix_client, client_answers_resend_request_for_dropped_orders) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    const auto now = connected.exchange.fix_time();
    CHECK(client.send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 100, 150 * kDollar), now));
    connected.wire.client_to_exchange().clear();  // lost in transit
    CHECK(client.send_new_order_single(make_new_order_single("B2", exsim::Side::BUY, 100, 150 * kDollar), now));
    connected.wire.pump();
    CHECK_EQ(engine_command_tokens(connected.exchange, 1), (Strings{"D:B1", "D:B2"}));
    CHECK_EQ(connected.wire.connection().counters().resend_requests_sent, 1u);
    CHECK_EQ(connected.exchange.user(1).session_day_state().expected_inbound_seq(), 4u);
    CHECK_EQ(connected.handler.events, (Strings{"A:1", "8:3:B1:0:100", "8:4:B2:0:100"}));  // 2 is the ResendRequest
}

TEST_CASE(gateway_fix_client, reconnect_continues_sequences_and_recovers) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    CHECK(client.send_new_order_single(make_new_order_single("B1", exsim::Side::BUY, 100, 150 * kDollar),
                                       connected.exchange.fix_time()));
    connected.wire.pump();
    connected.wire.disconnect();  // cancel-on-disconnect stores ER 3 for the client
    CHECK(!client.active());
    connected.wire.connect(false);
    CHECK_EQ(connected.wire.client_to_exchange().bytes().find("34=3\001") != std::string_view::npos, true);
    connected.wire.pump();
    CHECK(client.active());
    CHECK_EQ(connected.handler.events,
             (Strings{"A:1", "8:2:B1:0:100", "A:4", "8:3:B1:4:0:dup"}));
    CHECK_EQ(client.session_day_state().expected_inbound_seq(), 5u);
}

TEST_CASE(gateway_fix_client, refused_logon_reports_logout_text) {
    FixTestExchange exchange;
    RecordingHandler handler;
    Wire wire(exchange, handler, client_settings("CLIENT1", "wrong"));
    wire.connect(false);
    wire.pump();
    CHECK(!wire.client().active());
    CHECK(wire.client().wants_disconnect());
    CHECK_EQ(handler.last_logout_text, std::string("invalid SenderCompID or Password"));
}

TEST_CASE(gateway_fix_client, logout_handshake) {
    ConnectedClient connected;
    CHECK(connected.wire.client().logout("done", connected.exchange.fix_time()));
    connected.wire.pump();
    CHECK(connected.wire.client().wants_disconnect());
    CHECK(connected.wire.connection().wants_disconnect());
    CHECK_EQ(connected.handler.events, (Strings{"A:1", "5"}));
}

TEST_CASE(gateway_fix_client, business_message_reject_reaches_handler) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    const std::string business_reject = exchange_message("j", 2, "45=7|372=AF|380=3|58=unsupported MsgType|");
    client.on_data({business_reject.data(), business_reject.size()}, connected.exchange.fix_time());
    CHECK_EQ(connected.handler.events, (Strings{"A:1", "j:2:AF:3"}));
    CHECK(connected.wire.client_to_exchange().bytes().empty());
    CHECK_EQ(client.session_day_state().expected_inbound_seq(), 3u);
}

TEST_CASE(gateway_fix_client, unsupported_and_undecodable_messages_are_rejected) {
    ConnectedClient connected;
    auto& client = connected.wire.client();
    const std::string unsupported = exchange_message("Z", 2, "");
    client.on_data({unsupported.data(), unsupported.size()}, connected.exchange.fix_time());
    const std::string undecodable = exchange_message("8", 3, "37=1|");
    client.on_data({undecodable.data(), undecodable.size()}, connected.exchange.fix_time());
    const auto messages = split_frames(connected.wire.client_to_exchange().bytes());
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(fix_frame("35=3|49=CLIENT1|56=EXSIM|34=2|52=20261103-09:30:00.000|45=2|371=35|372=Z|373=11|"
                                  "58=unsupported MsgType|")));
    CHECK_EQ(messages[1].get(373), std::string("1"));
    CHECK_EQ(messages[1].get(45), std::string("3"));
    CHECK_EQ(client.session_day_state().expected_inbound_seq(), 4u);
}
