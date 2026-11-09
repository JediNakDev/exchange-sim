// OUCH order entry and ITCH market data through a running server over
// loopback sockets (server runtime).

#include "test/exsim/server/e2e/e2e_support.hpp"

using namespace exsim;
using namespace exsim::e2e;

namespace {

struct OuchPair {
    OuchRecorder alice_events;
    OuchRecorder bob_events;
    OuchSession alice{alice_events};
    OuchSession bob{bob_events};
};

}  // namespace

TEST_CASE(server_e2e_ouch, two_clients_trade_replace_cancel_and_hit_the_bots) {
    BookFeedHandler feed_handler;
    Feed feed(feed_handler, feed_receiver_config());
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    ServerUnderTest server(make_e2e_config(/*with_bots=*/true), feed.feed_endpoint().port, /*bots_enabled=*/false);
    REQUIRE(server.started());
    feed.set_retransmission_server(server.retransmission());

    OuchPair clients;
    auto pump = [&] {
        clients.alice.poll();
        clients.bob.poll();
        feed.poll();
    };
    REQUIRE(clients.alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(clients.bob.connect_and_login(server.ouch(), {"BOB", "bob", ""}, 0));
    REQUIRE(wait_until([&] { return clients.alice_events.logins_accepted == 1 && clients.bob_events.logins_accepted == 1; },
                       pump));

    // Enter, trade with each other.
    REQUIRE(clients.alice.client().enter_order(ouch_limit_order("A1", Side::BUY, 100, 150 * kDollar), net::monotonic_ns()));
    clients.alice.flush();
    REQUIRE(wait_until([&] { return clients.alice_events.has_accepted("A1"); }, pump));
    REQUIRE(clients.bob.client().enter_order(ouch_limit_order("B1", Side::SELL, 40, 150 * kDollar), net::monotonic_ns()));
    clients.bob.flush();
    REQUIRE(wait_until([&] {
        return clients.alice_events.executed_shares("A1") == 40 && clients.bob_events.executed_shares("B1") == 40;
    }, pump));
    CHECK_EQ(clients.alice_events.executed.back().price, 150 * kDollar);

    // Replace the remainder (60) down to 50 at 149.50, then cancel it.
    REQUIRE(clients.alice.client().replace_order(ouch_replace("A1", "A2", 50, 1495 * kDollar / 10), net::monotonic_ns()));
    clients.alice.flush();
    REQUIRE(wait_until([&] { return !clients.alice_events.replaced.empty(); }, pump));
    CHECK_EQ(clients.alice_events.replaced.back().token, std::string("A2"));
    CHECK_EQ(clients.alice_events.replaced.back().price, 1495 * kDollar / 10);
    REQUIRE(clients.alice.client().cancel_order(ouch_cancel("A2"), net::monotonic_ns()));
    clients.alice.flush();
    REQUIRE(wait_until([&] { return clients.alice_events.has_canceled("A2"); }, pump));

    // Bots on: the market maker quotes, ALICE lifts its offer.
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    CHECK(reply_ok(admin.request("bots on")));
    REQUIRE(wait_until([&] { return admin.request("book AAPL 5").find("ask ") != std::string::npos; }, pump));
    REQUIRE(clients.alice.client().enter_order(ouch_limit_order("A3", Side::BUY, 10, 200 * kDollar), net::monotonic_ns()));
    clients.alice.flush();
    REQUIRE(wait_until([&] { return clients.alice_events.executed_shares("A3") == 10; }, pump));

    // The ITCH-built book equals the engine's.
    CHECK(reply_ok(admin.request("bots off")));
    std::string mismatch;
    CHECK(feed_book_matches_admin_book(admin, feed, feed_handler, "AAPL", 1, pump, mismatch));
    if (!mismatch.empty()) testfw::report_failure(__FILE__, __LINE__, mismatch);
    CHECK_EQ(feed.stats().messages_lost, 0u);

    server.stop();
    std::string difference;
    CHECK(builder_matches_engine(feed_handler.builder, server.runtime_after_stop().core().engine(), 1, difference));
}

TEST_CASE(server_e2e_md, feed_rebuilds_the_engine_book_through_packet_faults_and_retransmission) {
    BookFeedHandler feed_handler;
    Feed feed(feed_handler, feed_receiver_config());
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    ServerUnderTest server(make_e2e_config(/*with_bots=*/true), feed.feed_endpoint().port, /*bots_enabled=*/true);
    REQUIRE(server.started());
    feed.set_retransmission_server(server.retransmission());
    auto pump = [&] { feed.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));

    // No faults first.
    REQUIRE(wait_until([&] { return feed_handler.messages > 50; }, pump));
    std::string mismatch;
    CHECK(reply_ok(admin.request("bots off")));
    CHECK(feed_book_matches_admin_book(admin, feed, feed_handler, "AAPL", 1, pump, mismatch));
    if (!mismatch.empty()) testfw::report_failure(__FILE__, __LINE__, mismatch);
    const std::uint64_t requests_before_faults = feed.stats().requests_sent;

    // One-shot drop, duplicate and reorder, then random faults of every kind.
    CHECK(reply_ok(admin.request("bots on")));
    CHECK(reply_ok(admin.request("fault md drop 3")));
    CHECK(reply_ok(admin.request("fault md dup 2")));
    CHECK(reply_ok(admin.request("fault md reorder 2 3")));
    const std::uint64_t messages_at_rate_start = feed_handler.messages;
    CHECK(reply_ok(admin.request("fault md rate drop=100000 dup=100000 reorder=100000 delay=2 seed=5")));
    REQUIRE(wait_until([&] { return feed_handler.messages > messages_at_rate_start + 300; }, pump));
    const std::string status_during_faults = admin.request("status");
    CHECK(status_during_faults.find("md faults drop_ppm=100000") != std::string::npos);

    // Faults off (held packets released), activity stops, books agree.
    CHECK(reply_ok(admin.request("fault md off")));
    CHECK(reply_ok(admin.request("bots off")));
    mismatch.clear();
    CHECK(feed_book_matches_admin_book(admin, feed, feed_handler, "AAPL", 1, pump, mismatch));
    if (!mismatch.empty()) testfw::report_failure(__FILE__, __LINE__, mismatch);
    CHECK_GT(feed.stats().requests_sent, requests_before_faults);  // gaps were recovered by request
    CHECK_GT(feed.stats().duplicate_messages, 0u);
    CHECK_EQ(feed.stats().messages_lost, 0u);

    // End of day reaches the feed (faults are off by then).
    CHECK(reply_ok(admin.request("shutdown")));
    REQUIRE(wait_until([&] { return feed.end_of_session_reached(); }, pump));
    REQUIRE(server.wait_finished());
    std::string difference;
    CHECK(builder_matches_engine(feed_handler.builder, server.runtime_after_stop().core().engine(), 1, difference));
    CHECK(builder_matches_engine(feed_handler.builder, server.runtime_after_stop().core().engine(), 2, difference));
    if (!difference.empty()) testfw::report_failure(__FILE__, __LINE__, difference);
}

TEST_CASE(server_e2e_admin, status_phase_and_book_commands) {
    BookFeedHandler feed_handler;
    Feed feed(feed_handler, feed_receiver_config());
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), feed.feed_endpoint().port);
    REQUIRE(server.started());
    feed.set_retransmission_server(server.retransmission());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    auto pump = [&] {
        alice.poll();
        feed.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1; }, pump));

    const std::string status = admin.request("status");
    CHECK(reply_ok(status));
    CHECK(status.find("user ALICE session=1 protocol=ouch42 bound=yes") != std::string::npos);
    CHECK(status.find("instrument MSFT id=2 phase=PRE_OPEN") != std::string::npos);
    CHECK_GT(md_next_sequence_from_status(status), 1u);

    // MSFT pre-open: an order rests; `phase` opens it (ITCH H 'T').
    REQUIRE(alice.client().enter_order(ouch_limit_order("M1", Side::BUY, 25, 399 * kDollar, "MSFT"), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("M1"); }, pump));
    CHECK(reply_ok(admin.request("phase MSFT continuous")));
    const std::string msft_book = admin.request("book MSFT");
    CHECK(msft_book.find("book MSFT phase=CONTINUOUS\nbid 399 qty=25 orders=1\nOK\n") != std::string::npos);
    REQUIRE(wait_until([&] { return feed_handler.builder.trading_state(2) == 'T'; }, pump));

    // Halt AAPL: new orders are rejected, the feed shows 'H'.
    CHECK(reply_ok(admin.request("phase AAPL halted")));
    REQUIRE(alice.client().enter_order(ouch_limit_order("H1", Side::BUY, 5, 150 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_rejected("H1"); }, pump));
    REQUIRE(wait_until([&] { return feed_handler.builder.trading_state(1) == 'H'; }, pump));
    CHECK(admin.request("book NOPE").rfind("ERR ", 0) != std::string::npos);
    CHECK(admin.request("frobnicate").find("ERR ") != std::string::npos);

    std::string mismatch;
    CHECK(feed_book_matches_admin_book(admin, feed, feed_handler, "MSFT", 2, pump, mismatch));
    if (!mismatch.empty()) testfw::report_failure(__FILE__, __LINE__, mismatch);
}

TEST_CASE(server_e2e_faults, user_reject_and_delay_keep_arrival_order) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    auto pump = [&] { alice.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1; }, pump));

    // reject 1: the gateway rejects the next new order itself (J 'O').
    CHECK(reply_ok(admin.request("fault user ALICE reject 1")));
    REQUIRE(alice.client().enter_order(ouch_limit_order("R1", Side::BUY, 10, 100 * kDollar), net::monotonic_ns()));
    REQUIRE(alice.client().enter_order(ouch_limit_order("R2", Side::BUY, 10, 100 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_rejected("R1") && alice_events.has_accepted("R2"); }, pump));
    CHECK_EQ(alice_events.rejected.back().reason, 'O');

    // delay 150 ms: input is held, then decoded in arrival order.
    CHECK(reply_ok(admin.request("fault user ALICE delay 150000")));
    const std::uint64_t sent_at = net::monotonic_ns();
    REQUIRE(alice.client().enter_order(ouch_limit_order("D1", Side::BUY, 10, 101 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(alice.client().enter_order(ouch_limit_order("D2", Side::BUY, 10, 102 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("D2"); }, pump));
    CHECK_GE(net::monotonic_ns() - sent_at, 140'000'000u);
    REQUIRE_EQ(alice_events.accepted.size(), 3u);
    CHECK_EQ(alice_events.accepted[1].token, std::string("D1"));
    CHECK_EQ(alice_events.accepted[2].token, std::string("D2"));

    // off: no more delay.
    CHECK(reply_ok(admin.request("fault user ALICE off")));
    const std::uint64_t undelayed_sent_at = net::monotonic_ns();
    REQUIRE(alice.client().enter_order(ouch_limit_order("D3", Side::BUY, 10, 103 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("D3"); }, pump));
    CHECK_LT(net::monotonic_ns() - undelayed_sent_at, 100'000'000u);
}

TEST_CASE(server_e2e_faults, drop_out_is_recovered_by_reconnecting_with_the_sequence_number) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    auto pump = [&] { alice.poll(); };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1; }, pump));
    REQUIRE(alice.client().enter_order(ouch_limit_order("P1", Side::BUY, 10, 100 * kDollar), net::monotonic_ns()));
    alice.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("P1"); }, pump));

    // The next two sequenced messages are stored but not transmitted.
    CHECK(reply_ok(admin.request("fault user ALICE drop_out 2")));
    REQUIRE(alice.client().enter_order(ouch_limit_order("P2", Side::BUY, 10, 101 * kDollar), net::monotonic_ns()));
    REQUIRE(alice.client().enter_order(ouch_limit_order("P3", Side::BUY, 10, 102 * kDollar), net::monotonic_ns()));
    alice.flush();
    const std::uint64_t expected_before_cut = alice.client().next_expected_sequence();

    // The gateway cuts the transport after the dropped messages; if it does
    // not, the client gives up waiting and reconnects on its own.
    wait_until([&] { return !alice.connected(); }, pump, 300);
    CHECK(!alice_events.has_accepted("P2"));
    CHECK(!alice_events.has_accepted("P3"));
    if (alice.connected()) alice.disconnect();
    REQUIRE(wait_until([&] { return admin.request("status").find("user ALICE session=1 protocol=ouch42 bound=no") !=
                                    std::string::npos; },
                       pump));

    // Reconnect asking for the first message not received: both are replayed.
    CHECK_EQ(alice.client().next_expected_sequence(), expected_before_cut);
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, alice.client().next_expected_sequence()));
    REQUIRE(wait_until([&] { return alice_events.has_accepted("P2") && alice_events.has_accepted("P3"); }, pump));
    for (std::size_t index = 1; index < alice_events.sequences.size(); ++index) {
        CHECK_EQ(alice_events.sequences[index], alice_events.sequences[index - 1] + 1);  // no gap, no repeat
    }
}

TEST_CASE(server_e2e_faults, admin_disconnect_closes_the_session_and_cancels_on_disconnect) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    OuchPair clients;
    auto pump = [&] {
        clients.alice.poll();
        clients.bob.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(clients.alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return clients.alice_events.logins_accepted == 1; }, pump));
    REQUIRE(clients.alice.client().enter_order(ouch_limit_order("C1", Side::BUY, 10, 140 * kDollar), net::monotonic_ns()));
    clients.alice.flush();
    REQUIRE(wait_until([&] { return clients.alice_events.has_accepted("C1"); }, pump));

    CHECK(reply_ok(admin.request("fault user ALICE disconnect")));
    REQUIRE(wait_until([&] { return !clients.alice.connected(); }, pump));
    REQUIRE(wait_until([&] { return admin.request("book AAPL").find("(empty)") != std::string::npos; }, pump));

    // The Order Canceled produced while unbound is stored and replayed.
    REQUIRE(clients.alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""},
                                            clients.alice.client().next_expected_sequence()));
    REQUIRE(wait_until([&] { return clients.alice_events.has_canceled("C1"); }, pump));
}

TEST_CASE(server_e2e_sessions, cancel_on_disconnect_follows_the_user_setting) {
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0);
    REQUIRE(server.started());
    OuchRecorder alice_events, keep_events;
    OuchSession alice(alice_events), keep(keep_events);
    auto pump = [&] {
        alice.poll();
        keep.poll();
    };
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(keep.connect_and_login(server.ouch(), {"KEEP", "keep", ""}, 0));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1 && keep_events.logins_accepted == 1; }, pump));
    REQUIRE(alice.client().enter_order(ouch_limit_order("A1", Side::BUY, 10, 140 * kDollar), net::monotonic_ns()));
    REQUIRE(keep.client().enter_order(ouch_limit_order("K1", Side::SELL, 20, 160 * kDollar), net::monotonic_ns()));
    alice.flush();
    keep.flush();
    REQUIRE(wait_until([&] { return alice_events.has_accepted("A1") && keep_events.has_accepted("K1"); }, pump));

    // Both drop the TCP connection without logging out.
    alice.disconnect();
    keep.disconnect();
    REQUIRE(wait_until([&] {
        return admin.request("book AAPL") == "book AAPL phase=CONTINUOUS\nask 160 qty=20 orders=1\nOK\n";
    }, pump));

    // ALICE gets the cancel on reconnect; KEEP's order is still live.
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, alice.client().next_expected_sequence()));
    REQUIRE(wait_until([&] { return alice_events.has_canceled("A1"); }, pump));
    REQUIRE(keep.connect_and_login(server.ouch(), {"KEEP", "keep", ""}, keep.client().next_expected_sequence()));
    REQUIRE(wait_until([&] { return keep_events.logins_accepted == 2; }, pump));
    REQUIRE(keep.client().cancel_order(ouch_cancel("K1"), net::monotonic_ns()));
    keep.flush();
    REQUIRE(wait_until([&] { return keep_events.has_canceled("K1"); }, pump));
}
