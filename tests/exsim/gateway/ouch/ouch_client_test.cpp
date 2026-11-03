// OuchClient: wire bytes it sends, login and recovery with a requested
// sequence number, heartbeats, server-silence detection and typed callbacks.

#include "ouch_test_harness.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;

using RecordingClient = OuchClient<MemoryByteSink, RecordingHandler>;

std::span<const char> bytes_of(const std::string& text) { return {text.data(), text.size()}; }

}  // namespace

TEST_CASE(gateway_ouch_client, sends_golden_bytes_for_every_request) {
    RecordingHandler handler;
    RecordingClient client(MemoryByteSink(4096), handler);
    CHECK(!client.enter_order(enter_order("T1", 'B', 100, "AAPL", kHundredDollars), 0));  // not logged in
    CHECK(client.transport().size() == 0);

    REQUIRE(client.login({"alice", "alicepw", "EXSIM"}, 7, 0));
    CHECK(client.transport().view() ==
          Golden().alpha("alice", 6).alpha("alicepw", 10).alpha("EXSIM", 10).numeric(7, 20).soup('L'));
    CHECK(client.state() == OuchClientState::AwaitingLoginResponse);
    CHECK(!client.login({"alice", "alicepw", ""}, 7, 0));  // one login at a time
    client.transport().clear();

    client.on_data(bytes_of(Golden().alpha("EXSIM", 10).numeric(7, 20).soup('A')), 0);
    REQUIRE(client.state() == OuchClientState::LoggedIn);
    CHECK_EQ(client.next_expected_sequence(), 7u);

    REQUIRE(client.enter_order(enter_order("T1", 'B', 100, "AAPL", kHundredDollars, 99998, 'P'), 0));
    CHECK(client.transport().view() == Golden()
                                           .byte('O')
                                           .alpha("T1", 14)
                                           .byte('B')
                                           .u32(100)
                                           .alpha("AAPL", 8)
                                           .u32(kHundredDollars)
                                           .u32(99998)
                                           .alpha("FRM1", 4)
                                           .byte('P')
                                           .byte('A')
                                           .byte('N')
                                           .u32(0)
                                           .byte('N')
                                           .byte(' ')
                                           .soup('U'));
    client.transport().clear();

    REQUIRE(client.replace_order(replace_order("T1", "T2", 150, kHundredDollars - 100), 0));
    CHECK(client.transport().view() == Golden()
                                           .byte('U')
                                           .alpha("T1", 14)
                                           .alpha("T2", 14)
                                           .u32(150)
                                           .u32(kHundredDollars - 100)
                                           .u32(99999)
                                           .byte('Y')
                                           .byte('N')
                                           .u32(0)
                                           .soup('U'));
    client.transport().clear();

    REQUIRE(client.cancel_order(cancel_order("T2", 25), 0));
    CHECK(client.transport().view() == Golden().byte('X').alpha("T2", 14).u32(25).soup('U'));
    client.transport().clear();

    ouch42::ModifyOrder modify;
    modify.token = token14("T2");
    modify.side = 'S';
    modify.shares = 10;
    REQUIRE(client.modify_order(modify, 0));
    CHECK(client.transport().view() == Golden().byte('M').alpha("T2", 14).byte('S').u32(10).soup('U'));
    client.transport().clear();

    REQUIRE(client.send_heartbeat(0));
    CHECK(client.transport().view() == control_frame<'R'>());
    client.transport().clear();

    REQUIRE(client.logout(0));
    CHECK(client.transport().view() == control_frame<'O'>());
    CHECK(client.state() == OuchClientState::LoggedOut);
    CHECK(!client.enter_order(enter_order("T3", 'B', 1, "AAPL", kHundredDollars), 0));
}

TEST_CASE(gateway_ouch_client, typed_callbacks_carry_sequence_numbers) {
    TestExchange exchange;
    RecordingHandler alice_handler;
    RecordingHandler bob_handler;
    ClientLink<RecordingHandler> alice(exchange, alice_handler);
    ClientLink<RecordingHandler> bob(exchange, bob_handler);
    REQUIRE(alice.login("alice", "alicepw", 1));
    REQUIRE(bob.login("bob", "bobpw", 1));
    CHECK(alice_handler.login_accepted_sequences == std::vector<std::uint64_t>{1});

    REQUIRE(bob.client().enter_order(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();
    REQUIRE(alice.client().enter_order(enter_order("BID1", 'B', 30, "AAPL", kHundredDollars), exchange.now()));
    alice.pump();
    bob.pump();
    REQUIRE(alice.client().replace_order(replace_order("BID1", "BID2", 50, kHundredDollars), exchange.now()));
    alice.pump();
    REQUIRE(alice.client().modify_order(ouch42::ModifyOrder{}, exchange.now()));
    alice.pump();

    CHECK_EQ(alice_handler.types(), "SAEJ");  // BID1 fully executed; its replace is ignored; Modify: J
    for (std::size_t index = 0; index < alice_handler.messages.size(); ++index) {
        CHECK_EQ(alice_handler.messages[index].sequence, index + 1);
    }
    CHECK_EQ(alice_handler.messages[0].system_event.event, 'S');
    const ouch42::OrderAccepted& accepted = alice_handler.messages[1].accepted;
    CHECK_EQ(token_text(accepted.token), "BID1");
    CHECK_EQ(accepted.order_reference, 2u);
    CHECK_EQ(accepted.order_state, 'L');
    CHECK_EQ(alice_handler.messages[2].executed.shares, Quantity{30});
    CHECK_EQ(alice_handler.messages[2].executed.liquidity, 'R');
    CHECK_EQ(alice_handler.messages[3].rejected.reason, 'O');

    CHECK_EQ(bob_handler.types(), "SAE");
    CHECK_EQ(bob_handler.messages[2].executed.liquidity, 'A');
    CHECK_EQ(alice.client().next_expected_sequence(), 5u);
    CHECK_EQ(alice.client().sequenced_messages_received(), 4u);
    CHECK(alice_handler.sequence_mismatches.empty());
}

TEST_CASE(gateway_ouch_client, recovers_missed_messages_after_transport_loss) {
    TestExchange exchange;
    RecordingHandler alice_handler;
    RecordingHandler bob_handler;
    ClientLink<RecordingHandler> alice(exchange, alice_handler);
    ClientLink<RecordingHandler> bob(exchange, bob_handler);
    REQUIRE(alice.login("alice", "alicepw", 1));
    REQUIRE(bob.login("bob", "bobpw", 1));
    REQUIRE(bob.client().enter_order(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars), exchange.now()));
    bob.pump();

    bob.drop_transport();
    CHECK(bob.client().state() == OuchClientState::Disconnected);
    CHECK_EQ(bob.client().next_expected_sequence(), 3u);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(alice.client().enter_order(enter_order("BID" + std::to_string(index), 'B', 20, "AAPL", kHundredDollars),
                                           exchange.now()));
        alice.pump();
    }

    REQUIRE(bob.login("bob", "bobpw", bob.client().next_expected_sequence()));
    CHECK_EQ(bob_handler.types(), "SAEEE");
    for (std::size_t index = 0; index < bob_handler.messages.size(); ++index) {
        CHECK_EQ(bob_handler.messages[index].sequence, index + 1);
    }
    CHECK(bob_handler.sequence_mismatches.empty());
    CHECK(bob_handler.login_accepted_sequences == (std::vector<std::uint64_t>{1, 3}));
    CHECK_EQ(bob.client().next_expected_sequence(), exchange.user_state(2).store().next_seq());
}

TEST_CASE(gateway_ouch_client, reports_a_sequence_mismatch_when_the_server_clamps) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> bob(exchange, handler);
    REQUIRE(bob.login("bob", "bobpw", 50));
    REQUIRE_EQ(handler.sequence_mismatches.size(), 1u);
    CHECK_EQ(handler.sequence_mismatches[0].first, 50u);
    CHECK_EQ(handler.sequence_mismatches[0].second, 1u);
    CHECK_EQ(handler.types(), "S");
    CHECK_EQ(bob.client().next_expected_sequence(), 2u);
}

TEST_CASE(gateway_ouch_client, login_rejected_is_reported) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> link(exchange, handler);
    CHECK(!link.login("alice", "wrong", 0));
    CHECK(link.client().state() == OuchClientState::LoginRejected);
    CHECK(handler.login_rejections == std::vector<char>{'A'});
    CHECK(link.connection().close_reason() == OuchCloseReason::LoginRejected);
}

TEST_CASE(gateway_ouch_client, heartbeats_keep_both_sides_alive_and_silence_times_out) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> link(exchange, handler);
    REQUIRE(link.login("alice", "alicepw", 1));

    exchange.advance(kOneSecond);
    link.client().on_timer(exchange.now());
    CHECK(link.client().transport().view() == control_frame<'R'>());
    link.pump();

    for (int second = 0; second < 60; ++second) {
        exchange.advance(kOneSecond);
        link.timers();
        link.pump();
    }
    CHECK(link.client().state() == OuchClientState::LoggedIn);
    CHECK(link.connection().state() == OuchConnectionState::LoggedIn);
    CHECK_GE(handler.server_heartbeats, 50);

    // The server goes silent (nothing is pumped any more).
    for (int second = 1; second < 15; ++second) {
        exchange.advance(kOneSecond);
        link.client().on_timer(exchange.now());
    }
    CHECK(link.client().state() == OuchClientState::LoggedIn);
    exchange.advance(kOneSecond);
    link.client().on_timer(exchange.now());
    CHECK(link.client().state() == OuchClientState::ServerTimedOut);
    CHECK_EQ(handler.server_timeouts, 1);
}

TEST_CASE(gateway_ouch_client, end_of_session_and_logout) {
    TestExchange exchange;
    RecordingHandler handler;
    ClientLink<RecordingHandler> link(exchange, handler);
    REQUIRE(link.login("alice", "alicepw", 1));
    link.connection().end_session(exchange);
    link.pump();
    CHECK(link.client().state() == OuchClientState::SessionEnded);
    CHECK_EQ(handler.end_of_sessions, 1);

    link.drop_transport();
    REQUIRE(link.login("alice", "alicepw", link.client().next_expected_sequence()));
    REQUIRE(link.client().logout(exchange.now()));
    link.pump();
    CHECK(link.connection().close_reason() == OuchCloseReason::ClientLogout);
    CHECK(link.client().state() == OuchClientState::LoggedOut);
}

TEST_CASE(gateway_ouch_client, parses_server_bytes_split_at_any_boundary) {
    // Capture a server byte stream with every sequenced message type the gateway sends.
    TestExchange exchange;
    RawSession alice(exchange);
    RawSession bob(exchange);
    REQUIRE(alice.login("alice", "alicepw", 1));
    REQUIRE(bob.login("bob", "bobpw"));
    bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));
    alice.send_message(enter_order("BID1", 'B', 60, "AAPL", kHundredDollars + 100));
    alice.send_message(enter_order("BID2", 'B', 60, "AAPL", kHundredDollars - 100));
    alice.send_message(replace_order("BID2", "BID3", 70, kHundredDollars - 200));
    alice.send_message(cancel_order("BID3", 0));
    alice.send_message(enter_order("BAD", 'B', 60, "ZZZZ", kHundredDollars));
    exchange.advance(kOneSecond);
    alice.timer();
    const std::string stream(alice.connection().sink().view());

    auto run = [&](std::size_t chunk_size) {
        RecordingHandler handler;
        RecordingClient client(MemoryByteSink(4096), handler);
        REQUIRE(client.login({"alice", "alicepw", ""}, 1, 0));
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk_size) {
            client.on_data(bytes_of(stream.substr(offset, chunk_size)), 0);
        }
        CHECK(client.state() == OuchClientState::LoggedIn);
        CHECK_EQ(handler.server_heartbeats, 1);
        return handler;
    };
    const RecordingHandler whole = run(stream.size());
    CHECK_EQ(whole.types(), "SAEAUCJ");
    for (std::size_t chunk_size = 1; chunk_size <= 90; ++chunk_size) {
        const RecordingHandler chunked = run(chunk_size);
        REQUIRE_EQ(chunked.types(), whole.types());
        for (std::size_t index = 0; index < whole.messages.size(); ++index) {
            CHECK_EQ(chunked.messages[index].sequence, whole.messages[index].sequence);
        }
    }
}

TEST_CASE(gateway_ouch_client, protocol_errors_stop_the_client) {
    RecordingHandler handler;
    RecordingClient client(MemoryByteSink(4096), handler);
    REQUIRE(client.login({"alice", "alicepw", ""}, 1, 0));
    client.on_data(bytes_of(Golden().byte('S').u64(1).byte('S').soup('S')), 0);  // data before Login Accepted
    CHECK(client.state() == OuchClientState::ProtocolError);
    CHECK(handler.messages.empty());

    RecordingHandler second_handler;
    RecordingClient second(MemoryByteSink(4096), second_handler);
    REQUIRE(second.login({"alice", "alicepw", ""}, 1, 0));
    second.on_data(bytes_of(Golden().alpha("EXSIM", 10).numeric(1, 20).soup('A')), 0);
    second.on_data(bytes_of(control_frame<'R'>()), 0);  // a client-only packet type from the server
    CHECK(second.state() == OuchClientState::ProtocolError);
}

TEST_CASE(gateway_ouch_client, unknown_sequenced_payloads_still_advance_the_sequence) {
    RecordingHandler handler;
    RecordingClient client(MemoryByteSink(4096), handler);
    REQUIRE(client.login({"alice", "alicepw", ""}, 1, 0));
    client.on_data(bytes_of(Golden().alpha("EXSIM", 10).numeric(1, 20).soup('A')), 0);
    client.on_data(bytes_of(Golden().byte('Q').u32(7).soup('S')), 0);
    client.on_data(bytes_of(Golden().byte('S').u64(1).byte('S').soup('S')), 0);
    CHECK_EQ(handler.unknown_messages, 1);
    REQUIRE_EQ(handler.messages.size(), 1u);
    CHECK_EQ(handler.messages[0].sequence, 2u);
    CHECK_EQ(client.next_expected_sequence(), 3u);
}

TEST_CASE(gateway_ouch_client, every_callback_is_optional) {
    struct NoCallbacks {};
    NoCallbacks handler;
    TestExchange exchange;
    ClientLink<NoCallbacks> link(exchange, handler);
    REQUIRE(link.login("alice", "alicepw", 1));
    REQUIRE(link.client().enter_order(enter_order("BID1", 'B', 10, "AAPL", kHundredDollars), exchange.now()));
    link.pump();
    CHECK_EQ(link.client().next_expected_sequence(), 3u);
    link.client().reset_for_new_trading_day();
    CHECK_EQ(link.client().next_expected_sequence(), 1u);
}
