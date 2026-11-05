// FIX 4.4 order entry, end of day (every protocol) and journal replay
// through a running server over loopback sockets.

#include "exsim/sim/canonical.hpp"
#include "exsim/sim/journal.hpp"
#include "tests/exsim/server/e2e/e2e_support.hpp"

using namespace exsim;
using namespace exsim::e2e;

namespace {

bool send_fix_order(FixSession& session, const fix44::NewOrderSingle& order) {
    return session.client().send_new_order_single(order, client::fix_time_now()) && session.flush();
}

}  // namespace

TEST_CASE(server_e2e_fix, enter_replace_cancel_and_trade_against_an_ouch_client) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixRecorder carol_events;
    FixSession carol(carol_events, fix_settings("CAROL", "carol"));
    OuchRecorder bob_events;
    OuchSession bob(bob_events);
    auto pump = [&] {
        carol.poll();
        bob.poll();
    };
    REQUIRE(carol.connect_and_logon(server.fix(), /*reset_sequence_numbers=*/false));
    REQUIRE(bob.connect_and_login(server.ouch(), {"BOB", "bob", ""}, 0));
    REQUIRE(wait_until([&] { return carol_events.logons == 1 && bob_events.logins_accepted == 1; }, pump));

    REQUIRE(send_fix_order(carol, fix_limit_order("F1", Side::BUY, 100, 150 * kDollar)));
    REQUIRE(wait_until([&] { return carol_events.count("F1", '0') == 1; }, pump));
    REQUIRE(carol.client().send_order_cancel_replace_request(fix_replace("F1", "F2", Side::BUY, 80, 1499 * kDollar / 10),
                                                             client::fix_time_now()));
    carol.flush();
    REQUIRE(wait_until([&] { return carol_events.count("F2", '5') == 1; }, pump));
    REQUIRE(carol.client().send_order_cancel_request(fix_cancel("F2", "F3", Side::BUY), client::fix_time_now()));
    carol.flush();
    REQUIRE(wait_until([&] { return carol_events.count("F3", '4') == 1; }, pump));

    // CAROL bids, BOB (OUCH) sells into it.
    REQUIRE(send_fix_order(carol, fix_limit_order("F4", Side::BUY, 30, 151 * kDollar)));
    REQUIRE(wait_until([&] { return carol_events.count("F4", '0') == 1; }, pump));
    REQUIRE(bob.client().enter_order(ouch_limit_order("B1", Side::SELL, 30, 151 * kDollar), net::monotonic_ns()));
    bob.flush();
    REQUIRE(wait_until([&] { return carol_events.filled("F4") == 30 && bob_events.executed_shares("B1") == 30; }, pump));
    CHECK_EQ(carol_events.reports.back().order_status, '2');  // filled
    CHECK_EQ(carol_events.session_rejects, 0);
}

TEST_CASE(server_e2e_fix, drop_out_is_recovered_through_resend_request) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    FixRecorder carol_events;
    FixSession carol(carol_events, fix_settings("CAROL", "carol"));
    auto pump = [&] { carol.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(carol.connect_and_logon(server.fix(), false));
    REQUIRE(wait_until([&] { return carol_events.logons == 1; }, pump));
    REQUIRE(send_fix_order(carol, fix_limit_order("G1", Side::BUY, 10, 100 * kDollar)));
    REQUIRE(wait_until([&] { return carol_events.count("G1", '0') == 1; }, pump));

    // G2's Execution Report is stored but not sent; G3's reveals the gap.
    CHECK(reply_ok(admin.request("fault user CAROL drop_out 1")));
    REQUIRE(send_fix_order(carol, fix_limit_order("G2", Side::BUY, 10, 101 * kDollar)));
    REQUIRE(send_fix_order(carol, fix_limit_order("G3", Side::BUY, 10, 102 * kDollar)));
    REQUIRE(wait_until([&] { return carol_events.count("G2", '0') == 1 && carol_events.count("G3", '0') == 1; }, pump));
    bool resent_as_possible_duplicate = false;
    for (const FixRecorder::Report& report : carol_events.reports) {
        if (report.client_order_id == "G2") resent_as_possible_duplicate = report.possible_duplicate;
    }
    CHECK(resent_as_possible_duplicate);
    CHECK(carol.connected());  // FIX recovers in-session: no reconnect
}

TEST_CASE(server_e2e_lifecycle, shutdown_ends_every_session_and_the_feed) {
    BookFeedHandler feed_handler;
    Feed feed(feed_handler, feed_receiver_config());
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    ServerUnderTest server(make_e2e_config(/*with_bots=*/true), feed.feed_endpoint().port);
    REQUIRE(server.started());
    feed.set_retransmission_server(server.retransmission());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    FixRecorder carol_events;
    FixSession carol(carol_events, fix_settings("CAROL", "carol"));
    FixpSbeRecorder erin_events;
    FixpSbeSession erin(erin_events, fixp_sbe_settings("ERIN:erin"));
    auto pump = [&] {
        alice.poll();
        carol.poll();
        erin.poll();
        feed.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(carol.connect_and_logon(server.fix(), false));
    REQUIRE(erin.connect_and_negotiate(server.fixp_sbe(), client::random_fixp_session_uuid()));
    REQUIRE(wait_until([&] {
        return alice_events.logins_accepted == 1 && carol_events.logons == 1 && erin_events.establishment_acks == 1;
    }, pump));
    REQUIRE(alice.client().enter_order(ouch_limit_order("S1", Side::BUY, 10, 140 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("S1"); }, pump));

    const std::string reply = admin.request("shutdown");
    CHECK(reply.find("all instruments closed") != std::string::npos);
    CHECK(reply_ok(reply));

    // OUCH: System Event 'E' then SoupBinTCP End of Session.
    REQUIRE(wait_until([&] { return alice_events.end_of_session; }, pump));
    REQUIRE(!alice_events.system_events.empty());
    CHECK_EQ(alice_events.system_events.back(), 'E');
    // FIX: Logout from the exchange.
    REQUIRE(wait_until([&] { return !carol_events.logout_texts.empty(); }, pump));
    CHECK_EQ(carol_events.logout_texts.back(), std::string("end of day"));
    // SBE over FIXP: Terminate (Finished) from the exchange.
    REQUIRE(wait_until([&] { return !erin_events.terminate_codes.empty(); }, pump));
    CHECK(erin_events.terminate_codes.back() == sbe::TerminationCode::Finished);
    // ITCH: S 'E', S 'C', then MoldUDP64 End of Session.
    REQUIRE(wait_until([&] { return feed.end_of_session_reached(); }, pump));
    REQUIRE(feed_handler.system_events.size() >= 2u);
    CHECK_EQ(feed_handler.system_events[feed_handler.system_events.size() - 2], 'E');
    CHECK_EQ(feed_handler.system_events.back(), 'C');
    CHECK_EQ(feed_handler.builder.trading_state(1), 'H');

    REQUIRE(server.wait_finished());
    // A command after shutdown is refused by the core (nothing is journaled).
    CHECK(server.runtime_after_stop().core().shutdown_requested());
}

TEST_CASE(server_e2e_lifecycle, journal_replays_to_the_published_event_stream) {
    TemporaryJournalPath journal;
    ServerUnderTest server(make_e2e_config(/*with_bots=*/true, journal.path), 0);
    REQUIRE(server.started());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    FixRecorder dave_events;
    FixSession dave(dave_events, fix_settings("DAVE", "dave"));
    auto pump = [&] {
        alice.poll();
        dave.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(dave.connect_and_logon(server.fix(), false));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1 && dave_events.logons == 1; }, pump));
    for (int round = 0; round < 20; ++round) {
        const std::string ouch_token = "J" + std::to_string(round);
        const std::string fix_id = "K" + std::to_string(round);
        REQUIRE(alice.client().enter_order(ouch_limit_order(ouch_token, Side::BUY, 10, (150 + round % 3) * kDollar),
                                           net::monotonic_ns()));
        alice.flush();
        REQUIRE(send_fix_order(dave, fix_limit_order(fix_id, Side::SELL, 7, (149 + round % 4) * kDollar)));
        REQUIRE(wait_until([&] { return alice_events.has_accepted(ouch_token) && dave_events.count(fix_id, '0') == 1; },
                           pump));
    }
    CHECK(reply_ok(admin.request("phase MSFT continuous")));
    wait_until([] { return false; }, pump, 100);  // let the bots trade a little longer
    CHECK(reply_ok(admin.request("shutdown")));
    // Answer the exchange's Logout so the server need not wait for its drain timeout.
    REQUIRE(wait_until([&] { return alice_events.end_of_session && !dave_events.logout_texts.empty(); }, pump));
    REQUIRE(server.wait_finished());
    const std::uint64_t published_hash = server.runtime_after_stop().market_data().event_stream_hash();
    const std::uint64_t published_events = server.runtime_after_stop().market_data().stats().events_published;

    sim::JournalReader reader(journal.path);
    MatchingEngine engine(reader.setup().engine);
    for (const InstrumentConfig& instrument : reader.setup().instruments) REQUIRE(engine.add_instrument(instrument) != 0);
    EventBuffer events(required_event_capacity(reader.setup().engine));
    Command command;
    std::uint64_t replayed_hash = 14695981039346656037ull;
    std::uint64_t replayed_events = 0;
    std::uint64_t commands = 0;
    while (reader.next(command)) {
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        ++commands;
        replayed_events += events.size();
        for (const Event& event : events) replayed_hash = sim::hash_event(event, replayed_hash);
    }
    CHECK_GT(commands, 40u);
    CHECK_EQ(replayed_events, published_events);
    CHECK_EQ(replayed_hash, published_hash);
}
