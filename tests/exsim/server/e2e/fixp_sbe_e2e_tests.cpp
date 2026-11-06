// SBE 1.0 over FIXP 1.0 order entry through a running server over loopback
// sockets: Negotiate/Establish, business messages, a trade against an OUCH
// client, user faults, admin disconnect and end of day.

#include "tests/exsim/server/e2e/e2e_support.hpp"

using namespace exsim;
using namespace exsim::e2e;

namespace {

constexpr std::string_view kErinCredentials = "ERIN:erin";

// Connects with a fresh logical session and waits for the EstablishmentAck.
bool establish_new_session(FixpSbeSession& session, const FixpSbeRecorder& events, net::Ipv4Endpoint server) {
    const int acks_before = events.establishment_acks;
    if (!session.connect_and_negotiate(server, client::random_fixp_session_uuid())) return false;
    return wait_until([&] { return events.establishment_acks == acks_before + 1; }, [&] { session.poll(); });
}

std::uint8_t reason_code(sbe::ExsimRejectReasonCode reason) { return static_cast<std::uint8_t>(reason); }
std::uint8_t reason_code(sbe::ExsimCancelReasonCode reason) { return static_cast<std::uint8_t>(reason); }

}  // namespace

TEST_CASE(server_e2e_fixp_sbe, enter_replace_cancel_and_trade_against_an_ouch_client) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings(kErinCredentials));
    OuchRecorder bob_events;
    OuchSession bob(bob_events);
    auto pump = [&] {
        erin.poll();
        bob.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(establish_new_session(erin, erin_events, server.fixp_sbe()));
    CHECK_EQ(erin_events.negotiation_responses, 1);
    CHECK_EQ(erin_events.last_establishment_next_sequence_number, 1u);
    CHECK(admin.request("status").find("user ERIN session=6 protocol=fixp_sbe bound=yes") != std::string::npos);
    REQUIRE(bob.connect_and_login(server.ouch(), {"BOB", "bob", ""}, 0));
    REQUIRE(wait_until([&] { return bob_events.logins_accepted == 1; }, pump));

    REQUIRE(send_sbe(erin, sbe_limit_order(1, Side::BUY, 100, 150 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 1); }, pump));
    REQUIRE(send_sbe(erin, sbe_replace(1, 2, 80, 1499 * kDollar / 10)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportReplace, 2); }, pump));
    CHECK_EQ(erin_events.find(sbe::TemplateId::ExecutionReportReplace, 2)->leaves_quantity, Quantity{80});
    CHECK(admin.request("book AAPL").find("bid 149.9 qty=80 orders=1") != std::string::npos);
    REQUIRE(send_sbe(erin, sbe_cancel(2, 3)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportCancel, 3); }, pump));
    CHECK(admin.request("book AAPL").find("(empty)") != std::string::npos);

    // ERIN bids, BOB (OUCH) sells into it.
    REQUIRE(send_sbe(erin, sbe_limit_order(4, Side::BUY, 30, 151 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 4); }, pump));
    REQUIRE(bob.client().enter_order(ouch_limit_order("B1", Side::SELL, 30, 151 * kDollar), net::monotonic_ns()));
    bob.flush();
    REQUIRE(wait_until([&] { return erin_events.filled(4) == 30 && bob_events.executed_shares("B1") == 30; }, pump));
    CHECK_EQ(erin_events.reports.back().leaves_quantity, Quantity{0});

    // Server flow: every business message once, numbered 1, 2, 3, ... in order.
    for (std::size_t index = 0; index < erin_events.reports.size(); ++index) {
        CHECK_EQ(erin_events.reports[index].server_sequence_number, std::uint64_t(index + 1));
        CHECK(!erin_events.reports[index].retransmitted);
    }
    CHECK_EQ(erin_events.cancel_rejects, 0);
    CHECK_EQ(erin_events.business_message_rejects, 0);
    CHECK(erin.established());
}

TEST_CASE(server_e2e_fixp_sbe, user_reject_and_delay_keep_arrival_order) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings(kErinCredentials));
    auto pump = [&] { erin.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(establish_new_session(erin, erin_events, server.fixp_sbe()));

    // reject 1: the gateway rejects the next new order itself.
    CHECK(reply_ok(admin.request("fault user ERIN reject 1")));
    REQUIRE(send_sbe(erin, sbe_limit_order(10, Side::BUY, 10, 100 * kDollar)));
    REQUIRE(send_sbe(erin, sbe_limit_order(11, Side::BUY, 10, 100 * kDollar)));
    REQUIRE(wait_until([&] {
        return erin_events.has(sbe::TemplateId::ExecutionReportReject, 10) &&
               erin_events.has(sbe::TemplateId::ExecutionReportNew, 11);
    }, pump));
    CHECK_EQ(erin_events.find(sbe::TemplateId::ExecutionReportReject, 10)->reason,
             reason_code(sbe::ExsimRejectReasonCode::InjectedReject));

    // delay 150 ms: input is held, then decoded in arrival order.
    CHECK(reply_ok(admin.request("fault user ERIN delay 150000")));
    const std::uint64_t sent_at = net::monotonic_ns();
    REQUIRE(send_sbe(erin, sbe_limit_order(12, Side::BUY, 10, 101 * kDollar)));
    REQUIRE(send_sbe(erin, sbe_limit_order(13, Side::BUY, 10, 102 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 13); }, pump));
    CHECK_GE(net::monotonic_ns() - sent_at, 140'000'000u);
    REQUIRE_EQ(erin_events.reports.size(), 4u);
    CHECK_EQ(erin_events.reports[2].cl_ord_id, 12u);
    CHECK_EQ(erin_events.reports[3].cl_ord_id, 13u);

    // off: no more delay.
    CHECK(reply_ok(admin.request("fault user ERIN off")));
    const std::uint64_t undelayed_sent_at = net::monotonic_ns();
    REQUIRE(send_sbe(erin, sbe_limit_order(14, Side::BUY, 10, 103 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 14); }, pump));
    CHECK_LT(net::monotonic_ns() - undelayed_sent_at, 100'000'000u);
    CHECK(erin.established());
}

TEST_CASE(server_e2e_fixp_sbe, drop_out_is_recovered_through_retransmit_request) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings(kErinCredentials));
    auto pump = [&] { erin.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(establish_new_session(erin, erin_events, server.fixp_sbe()));
    REQUIRE(send_sbe(erin, sbe_limit_order(20, Side::BUY, 10, 100 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 20); }, pump));

    // 21's report is stored but not sent; 22's is preceded by Sequence, which
    // reveals the gap, and the client asks for 21 again.
    CHECK(reply_ok(admin.request("fault user ERIN drop_out 1")));
    REQUIRE(send_sbe(erin, sbe_limit_order(21, Side::BUY, 10, 101 * kDollar)));
    REQUIRE(send_sbe(erin, sbe_limit_order(22, Side::BUY, 10, 102 * kDollar)));
    REQUIRE(wait_until([&] {
        return erin_events.has(sbe::TemplateId::ExecutionReportNew, 21) &&
               erin_events.has(sbe::TemplateId::ExecutionReportNew, 22);
    }, pump));
    const FixpSbeRecorder::Report* recovered = erin_events.find(sbe::TemplateId::ExecutionReportNew, 21);
    CHECK(recovered->retransmitted);
    CHECK_EQ(recovered->server_sequence_number, 2u);
    CHECK_GE(erin.client().statistics().retransmit_requests_sent, 1u);
    // Delivered exactly once and in sequence order.
    for (std::size_t index = 0; index < erin_events.reports.size(); ++index) {
        CHECK_EQ(erin_events.reports[index].server_sequence_number, std::uint64_t(index + 1));
    }
    CHECK(erin.established());  // FIXP recovers in-session: no reconnect
}

TEST_CASE(server_e2e_fixp_sbe, admin_disconnect_closes_the_session_and_cancels_on_disconnect) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings(kErinCredentials));
    auto pump = [&] { erin.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(establish_new_session(erin, erin_events, server.fixp_sbe()));
    REQUIRE(send_sbe(erin, sbe_limit_order(30, Side::BUY, 10, 140 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 30); }, pump));

    CHECK(reply_ok(admin.request("fault user ERIN disconnect")));
    REQUIRE(wait_until([&] { return !erin.connected(); }, pump));
    REQUIRE(wait_until([&] { return admin.request("book AAPL").find("(empty)") != std::string::npos; }, pump));
    CHECK(admin.request("status").find("user ERIN session=6 protocol=fixp_sbe bound=no") != std::string::npos);

    // Cancel-on-disconnect stored the cancel (2) and the mass cancel report
    // (3, ClOrdID 0) while unbound: Establish on a new transport announces
    // NextSeqNo 4 and the client retransmits both.
    REQUIRE(erin.reconnect_and_establish(server.fixp_sbe()));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::OrderMassCancelReport, 0); }, pump));
    CHECK_EQ(erin_events.establishment_acks, 2);
    CHECK_EQ(erin_events.last_establishment_next_sequence_number, 4u);
    const FixpSbeRecorder::Report* canceled = erin_events.find(sbe::TemplateId::ExecutionReportCancel, 30);
    REQUIRE(canceled != nullptr);
    CHECK_EQ(canceled->reason, reason_code(sbe::ExsimCancelReasonCode::CancelOnDisconnect));
    CHECK_EQ(canceled->server_sequence_number, 2u);
    CHECK(canceled->retransmitted);
    CHECK(erin_events.find(sbe::TemplateId::OrderMassCancelReport, 0)->retransmitted);

    // The session carries on: new orders are numbered after the recovered ones.
    REQUIRE(send_sbe(erin, sbe_limit_order(31, Side::BUY, 10, 141 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 31); }, pump));
    CHECK_EQ(erin_events.find(sbe::TemplateId::ExecutionReportNew, 31)->server_sequence_number, 4u);
}

TEST_CASE(server_e2e_fixp_sbe, shutdown_ends_the_session_with_terminate) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings(kErinCredentials));
    auto pump = [&] { erin.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(establish_new_session(erin, erin_events, server.fixp_sbe()));
    REQUIRE(send_sbe(erin, sbe_limit_order(40, Side::BUY, 10, 140 * kDollar)));
    REQUIRE(wait_until([&] { return erin_events.has(sbe::TemplateId::ExecutionReportNew, 40); }, pump));

    const std::uint64_t shutdown_at = net::monotonic_ns();
    CHECK(reply_ok(admin.request("shutdown")));
    REQUIRE(wait_until([&] { return !erin_events.terminate_codes.empty(); }, pump));
    CHECK(erin_events.terminate_codes.back() == sbe::TerminationCode::Finished);
    REQUIRE(wait_until([&] { return !erin.connected(); }, pump));
    REQUIRE(server.wait_finished());
    // The server closed the session itself: it did not wait for the drain timeout.
    CHECK_LT(net::monotonic_ns() - shutdown_at, 1'500'000'000u);
}
