// Session behaviour: replay, heartbeats, idle timeout, framing split at any
// byte, cancel-on-disconnect, store-full and send-buffer-full policies,
// fault hooks and the trading-day lifecycle.

#include "ouch_test_harness.hpp"

#include <random>

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;
constexpr Price kTick = 100;
constexpr Timestamp kOneMillisecond = 1'000'000;

std::string login_accepted_golden(std::uint64_t sequence) {
    return Golden().alpha("EXSIM", 10).numeric(sequence, 20).soup('A');
}

std::vector<std::string> only_sequenced(const std::vector<std::string>& frames) {
    std::vector<std::string> sequenced;
    for (const std::string& frame : frames) {
        if (frame_type(frame) == 'S') sequenced.push_back(frame);
    }
    return sequenced;
}

std::vector<std::string> stored_frames(const TestUserState& user_state, std::uint64_t first_sequence = 1) {
    std::vector<std::string> frames;
    for (std::uint64_t sequence = first_sequence; sequence <= user_state.store().last_seq(); ++sequence) {
        const gateway::SequencedStore::Message message = user_state.store().get(sequence);
        frames.emplace_back(message.bytes.data(), message.bytes.size());
    }
    return frames;
}

}  // namespace

TEST_CASE(gateway_ouch_session, replay_from_n_after_orders_completed_is_byte_identical) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    std::vector<std::string> bob_live_frames;
    {
        RawSession bob(exchange);
        REQUIRE(bob.login("bob", "bobpw"));
        bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));     // 2: A
        exchange.advance(10);
        alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));   // 3: E (ASK1 done)
        bob.send_message(enter_order("ASK2", 'S', 100, "AAPL", kHundredDollars + kTick));  // 4: A
        bob.send_message(replace_order("ASK2", "ASK2R", 50, kHundredDollars + kTick));      // 5: U
        bob.send_message(cancel_order("ASK2R", 0));                                         // 6: C (done)
        bob.send_message(enter_order("ASK3", 'S', 50, "AAPL", kHundredDollars + 2 * kTick));  // 7: A
        bob_live_frames = only_sequenced(bob.take_frames());
    }  // TCP loss; bob has no cancel-on-disconnect, ASK3 stays live
    REQUIRE_EQ(bob_live_frames.size(), 7u);
    CHECK_EQ(sequenced_types(bob_live_frames), "SAEAUCA");
    CHECK(bob_live_frames == stored_frames(exchange.user_state(2)));
    CHECK(exchange.find_live_order(2, padded_token("ASK3")).has_value());

    for (std::uint64_t first_sequence = 1; first_sequence <= 8; ++first_sequence) {
        RawSession bob(exchange);
        REQUIRE(bob.login("bob", "bobpw", first_sequence));
        const std::vector<std::string> frames = bob.take_frames();
        REQUIRE(!frames.empty());
        CHECK(frames[0] == login_accepted_golden(first_sequence));
        const std::vector<std::string> replayed(frames.begin() + 1, frames.end());
        const std::vector<std::string> expected(bob_live_frames.begin() + static_cast<std::ptrdiff_t>(first_sequence - 1),
                                                bob_live_frames.end());
        CHECK(replayed == expected);
        CHECK(!bob.connection().is_replaying());
        CHECK_EQ(bob.connection().next_sequence_to_send(), 8u);
    }

    RawSession bob(exchange);
    REQUIRE(bob.login("bob", "bobpw", 0));  // 0: only new messages
    const std::vector<std::string> frames = bob.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == login_accepted_golden(8));
}

TEST_CASE(gateway_ouch_session, messages_generated_while_disconnected_are_replayed) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    {
        RawSession bob(exchange);
        REQUIRE(bob.login("bob", "bobpw"));
        bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));
        CHECK_EQ(bob.take_types(), "SA");
    }
    alice.send_message(enter_order("BID1", 'B', 40, "AAPL", kHundredDollars));
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 3u);  // stored without a connection

    RawSession bob(exchange);
    REQUIRE(bob.login("bob", "bobpw", 3));
    const std::vector<std::string> frames = bob.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    const ouch42::OrderExecuted executed = decode_ouch<ouch42::OrderExecuted>(frames[1]);
    CHECK_EQ(token_text(executed.token), "ASK1");
    CHECK_EQ(executed.shares, Quantity{40});
    CHECK_EQ(executed.liquidity, 'A');
}

TEST_CASE(gateway_ouch_session, replay_pauses_on_a_full_sink_and_resumes_in_order) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    {
        RawSession bob(exchange);
        REQUIRE(bob.login("bob", "bobpw"));
        for (int index = 0; index < 40; ++index) {
            bob.send_message(enter_order("ASK" + std::to_string(index), 'S', 10, "AAPL", kHundredDollars + index * kTick));
        }
    }
    const std::uint64_t stored_before_replay = exchange.user_state(2).store().last_seq();
    REQUIRE_EQ(stored_before_replay, 41u);

    RawSession bob(exchange, 400);  // room for the Login Accepted and a few messages only
    REQUIRE(bob.login("bob", "bobpw", 1));
    CHECK(bob.connection().is_replaying());
    CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);

    // A live message while the replay is paused is queued behind it, not dropped.
    alice.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredDollars));
    CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);

    std::vector<std::string> received = bob.take_frames();
    REQUIRE(!received.empty());
    CHECK(received[0] == login_accepted_golden(1));
    received.erase(received.begin());
    int resumptions = 0;
    while (bob.connection().is_replaying() && resumptions < 100) {
        ++resumptions;
        exchange.advance(2 * kOneSecond);
        bob.send(control_frame<'R'>());  // the client stays active
        // Sink still full: the timer neither heartbeats nor closes mid-replay.
        bob.timer();
        CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);
        std::vector<std::string> drained = bob.take_frames();
        // Alternate the two resumption paths.
        if (resumptions % 2 == 0) {
            bob.connection().on_writable(exchange);
        } else {
            bob.timer();
        }
        for (std::string& frame : bob.take_frames()) drained.push_back(std::move(frame));
        for (std::string& frame : drained) {
            CHECK_EQ(frame_type(frame), 'S');
            received.push_back(std::move(frame));
        }
    }
    CHECK_GT(resumptions, 3);
    CHECK(!bob.connection().is_replaying());
    CHECK(received == stored_frames(exchange.user_state(2)));
    CHECK_EQ(sequenced_types(received).back(), 'E');
}

TEST_CASE(gateway_ouch_session, live_output_that_does_not_fit_closes_with_send_buffer_full) {
    TestExchange exchange;
    RawSession bob(exchange, 200);
    REQUIRE(bob.login("bob", "bobpw"));  // 33 + 13 bytes
    bob.send_message(enter_order("ASK1", 'S', 10, "AAPL", kHundredDollars));  // + 69
    bob.send_message(enter_order("ASK2", 'S', 10, "AAPL", kHundredDollars));  // + 69 = 184
    CHECK(bob.connection().state() == OuchConnectionState::LoggedIn);
    bob.send_message(enter_order("ASK3", 'S', 10, "AAPL", kHundredDollars));  // does not fit
    CHECK(bob.connection().close_reason() == OuchCloseReason::SendBufferFull);
    CHECK(!bob.connection().flush_before_close());
    CHECK(!exchange.is_bound(2));
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 4u);
    CHECK(exchange.find_live_order(2, padded_token("ASK3")).has_value());  // bob: no cancel-on-disconnect

    RawSession recovered(exchange);
    REQUIRE(recovered.login("bob", "bobpw", 4));
    const std::vector<std::string> frames = recovered.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK_EQ(token_text(decode_ouch<ouch42::OrderAccepted>(frames[1]).token), "ASK3");
}

TEST_CASE(gateway_ouch_session, server_heartbeat_after_an_interval_of_outbound_silence) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.take_frames();
    exchange.advance(kOneSecond - 1);
    alice.timer();
    CHECK(alice.take_frames().empty());
    exchange.advance(1);
    alice.timer();
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == control_frame<'H'>());

    // Any outbound message restarts the interval.
    exchange.advance(500 * kOneMillisecond);
    alice.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredDollars));
    alice.take_frames();
    exchange.advance(600 * kOneMillisecond);
    alice.timer();
    CHECK(alice.take_frames().empty());
    exchange.advance(400 * kOneMillisecond);
    alice.timer();
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == control_frame<'H'>());
}

TEST_CASE(gateway_ouch_session, client_idle_timeout_closes_and_cancels_on_disconnect) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredDollars));
    for (int second = 0; second < 10; ++second) {
        exchange.advance(kOneSecond);
        alice.timer();  // server heartbeats do not count as client activity
    }
    alice.send(control_frame<'R'>());  // client heartbeat resets the idle clock
    exchange.advance(14 * kOneSecond);
    alice.timer();
    CHECK(alice.connection().state() == OuchConnectionState::LoggedIn);
    exchange.advance(kOneSecond);
    alice.timer();
    CHECK(alice.connection().close_reason() == OuchCloseReason::ClientIdleTimeout);
    CHECK(alice.connection().flush_before_close());
    CHECK(!exchange.is_bound(1));
    CHECK(!exchange.find_live_order(1, padded_token("BID1")).has_value());
    const std::vector<std::string> stored = stored_frames(exchange.user_state(1));
    const ouch42::OrderCanceled canceled = decode_ouch<ouch42::OrderCanceled>(stored.back());
    CHECK_EQ(token_text(canceled.token), "BID1");
    CHECK_EQ(canceled.reason, 'U');
}

TEST_CASE(gateway_ouch_session, frames_split_at_arbitrary_boundaries_give_identical_output) {
    std::string script = login_frame("alice", "alicepw", "", 0);
    script += unsequenced_frame(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    script += unsequenced_frame(enter_order("ASK1", 'S', 40, "AAPL", kHundredDollars));  // self-trade, both legs
    script += control_frame<'R'>();
    script += Golden().alpha("debug", 5).soup('+');
    script += unsequenced_frame(replace_order("BID1", "BID2", 90, kHundredDollars - kTick));
    script += unsequenced_frame(cancel_order("BID2", 70));
    script += unsequenced_frame(enter_order("BAD1", 'B', 100, "ZZZZ", kHundredDollars));
    script += Golden().byte('M').alpha("BID2", 14).byte('B').u32(5).soup('U');
    script += unsequenced_frame(enter_order("IOC1", 'S', 500, "AAPL", kHundredDollars - kTick, 0));

    auto run = [&](const std::vector<std::size_t>& chunk_ends) {
        TestExchange exchange;
        RawSession session(exchange);
        std::size_t begin = 0;
        for (const std::size_t end : chunk_ends) {
            session.send(std::string_view(script).substr(begin, end - begin));
            begin = end;
        }
        return std::string(session.connection().sink().view());
    };

    const std::string whole_output = run({script.size()});
    const std::vector<std::string> frames = split_frames(whole_output);
    CHECK_EQ(frame_type(frames.front()), 'A');
    // IOC1 executes against alice's own BID2: both legs, buy first, then the IOC remainder.
    CHECK_EQ(sequenced_types(frames), "SAAEEUCJJAEEC");

    for (std::size_t chunk_size = 1; chunk_size <= 60; ++chunk_size) {
        std::vector<std::size_t> chunk_ends;
        for (std::size_t end = chunk_size; end < script.size(); end += chunk_size) chunk_ends.push_back(end);
        chunk_ends.push_back(script.size());
        CHECK(run(chunk_ends) == whole_output);
    }
    std::mt19937 random(20261103);
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::vector<std::size_t> chunk_ends;
        for (std::size_t end = 0;;) {
            end += 1 + random() % 70;
            if (end >= script.size()) break;
            chunk_ends.push_back(end);
        }
        chunk_ends.push_back(script.size());
        CHECK(run(chunk_ends) == whole_output);
    }
}

TEST_CASE(gateway_ouch_session, cancel_on_disconnect_on_tcp_loss) {
    TestExchange exchange;
    {
        RawSession alice(exchange);
        REQUIRE(alice.login("alice", "alicepw"));
        alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
        alice.send_message(enter_order("BID2", 'B', 200, "MSFT", kHundredDollars));
        alice.send_message(enter_order("BID3", 'B', 300, "AAPL", kHundredDollars - kTick));
        alice.send_message(cancel_order("BID3", 0));
    }
    {
        RawSession bob(exchange);
        REQUIRE(bob.login("bob", "bobpw"));
        bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars + kTick));
    }
    CHECK(!exchange.find_live_order(1, padded_token("BID1")).has_value());
    CHECK(!exchange.find_live_order(1, padded_token("BID2")).has_value());
    CHECK(exchange.find_live_order(2, padded_token("ASK1")).has_value());  // bob: no cancel-on-disconnect
    CHECK_EQ(exchange.engine().live_order_count(), 1u);

    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw", 6));
    const std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 3u);
    const ouch42::OrderCanceled first = decode_ouch<ouch42::OrderCanceled>(frames[1]);
    const ouch42::OrderCanceled second = decode_ouch<ouch42::OrderCanceled>(frames[2]);
    CHECK_EQ(token_text(first.token), "BID1");  // ascending order id
    CHECK_EQ(first.shares, Quantity{100});
    CHECK_EQ(first.reason, 'U');
    CHECK_EQ(token_text(second.token), "BID2");
    CHECK_EQ(second.shares, Quantity{200});
}

TEST_CASE(gateway_ouch_session, cancel_on_disconnect_on_logout) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    alice.take_frames();
    alice.send(control_frame<'O'>());
    CHECK(alice.connection().close_reason() == OuchCloseReason::ClientLogout);
    CHECK(alice.take_frames().empty());  // the cancel is stored, not written to the closing connection
    CHECK(!exchange.find_live_order(1, padded_token("BID1")).has_value());
    CHECK_EQ(ouch_type(stored_frames(exchange.user_state(1)).back()), 'C');
    alice.disconnect();  // the server's later on_disconnect does not unbind twice
    CHECK_EQ(exchange.unbind_count(), 1u);
}

TEST_CASE(gateway_ouch_session, store_admission_reserve_closes_instead_of_storing) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.store_messages = 8;
    config.limits.store_reserve_messages = 3;
    TestExchange exchange(config);
    RawSession bob(exchange);                // no cancel-on-disconnect
    REQUIRE(bob.login("bob", "bobpw"));      // 1: S
    for (int index = 1; index <= 4; ++index) {
        bob.send_message(enter_order("BID" + std::to_string(index), 'B', 10, "AAPL", kHundredDollars));
    }
    CHECK_EQ(bob.take_types(), "SAAAA");  // 5 stored, 3 free: at the reserve
    // A new order at the reserve stores nothing (no J): the connection closes.
    bob.send_message(enter_order("BID5", 'B', 10, "AAPL", kHundredDollars));
    CHECK(bob.take_frames().empty());
    CHECK(bob.connection().close_reason() == OuchCloseReason::StoreReserveReached);
    CHECK(bob.connection().flush_before_close());
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 5u);
    CHECK_EQ(exchange.user_state(2).statistics().requests_refused_at_store_reserve, 1u);
    CHECK_EQ(exchange.user_state(2).find_cl_ord_id(padded_token("BID5")), 0u);  // not interned: may be sent later
    bob.disconnect();

    // Normal reconnect; a replace at the reserve closes the same way.
    RawSession again(exchange);
    REQUIRE(again.login("bob", "bobpw", 6));
    CHECK(again.take_frames().size() == 1u);  // Login Accepted only
    again.send_message(replace_order("BID1", "BID1R", 20, kHundredDollars));
    CHECK(again.take_frames().empty());
    CHECK(again.connection().close_reason() == OuchCloseReason::StoreReserveReached);
    CHECK_EQ(exchange.user_state(2).statistics().requests_refused_at_store_reserve, 2u);
    CHECK_EQ(exchange.submitted_command_count(), 4u);
    again.disconnect();

    // Cancels of live orders use the reserve.
    RawSession bob_third(exchange);
    REQUIRE(bob_third.login("bob", "bobpw", 6));
    bob_third.take_frames();
    for (const char* token : {"BID1", "BID2", "BID3"}) bob_third.send_message(cancel_order(token, 0));
    CHECK_EQ(bob_third.take_types(), "CCC");  // messages 6 to 8: the store is now full
    CHECK(bob_third.connection().state() == OuchConnectionState::LoggedIn);

    // Hard overflow: the report is lost, the session ends for the day.
    bob_third.send_message(cancel_order("BID4", 0));
    const std::vector<std::string> frames = bob_third.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == control_frame<'Z'>());
    CHECK(bob_third.connection().close_reason() == OuchCloseReason::StoreFull);
    CHECK(bob_third.connection().flush_before_close());
    CHECK(exchange.user_state(2).store_overflowed());
    CHECK_EQ(exchange.user_state(2).statistics().messages_lost_to_store_overflow, 1u);
    CHECK_EQ(exchange.engine().live_order_count(), 0u);

    RawSession after_overflow(exchange);
    CHECK(!after_overflow.login("bob", "bobpw", 1));
    CHECK(after_overflow.take_frames().front() == Golden().byte('S').soup('J'));

    // Next trading day: statistics are per day too.
    after_overflow.disconnect();
    bob_third.disconnect();
    exchange.user_state(2).reset_for_new_trading_day();
    CHECK_EQ(exchange.user_state(2).statistics().requests_refused_at_store_reserve, 0u);
    CHECK_EQ(exchange.user_state(2).statistics().messages_lost_to_store_overflow, 0u);
    RawSession next_day(exchange);
    CHECK(next_day.login("bob", "bobpw", 1));
    CHECK_EQ(next_day.take_types(), "S");
}

// Admission comes before any gateway J: the token-table-full, unknown-stock
// and injected rejects stop at the reserve too.
TEST_CASE(gateway_ouch_session, store_admission_precedes_every_gateway_reject) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.max_tokens = 1;
    config.limits.store_messages = 6;
    config.limits.store_reserve_messages = 2;
    TestExchange exchange(config);
    RawSession bob(exchange);
    REQUIRE(bob.login("bob", "bobpw"));                                       // 1: S
    bob.send_message(enter_order("ASK", 'S', 10, "AAPL", kHundredDollars));  // 2: A, the only token
    bob.send_message(enter_order("FULL1", 'B', 1, "AAPL", kHundredDollars)); // 3: J, token table full
    bob.send_message(enter_order("FULL2", 'B', 1, "AAPL", kHundredDollars)); // 4: J, 2 free: at the reserve
    bob.send_message(enter_order("FULL3", 'B', 1, "AAPL", kHundredDollars)); // nothing stored, closed
    CHECK_EQ(bob.take_types(), "SAJJ");
    CHECK(bob.connection().close_reason() == OuchCloseReason::StoreReserveReached);
    CHECK_EQ(exchange.user_state(2).statistics().gateway_rejects, 2u);
    bob.disconnect();

    exchange.inject_gateway_rejects(2, 5);
    RawSession again(exchange);
    REQUIRE(again.login("bob", "bobpw", 5));
    again.send_message(enter_order("ZZZ", 'B', 1, "ZZZZ", kHundredDollars));
    CHECK(again.take_frames().size() == 1u);  // Login Accepted only
    CHECK(again.connection().close_reason() == OuchCloseReason::StoreReserveReached);
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 4u);
    again.disconnect();

    // The two reserved slots still carry the execution of the live ASK.
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.send_message(enter_order("BID", 'B', 10, "AAPL", kHundredDollars));
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 5u);
    CHECK_EQ(ouch_type(stored_frames(exchange.user_state(2)).back()), 'E');
    CHECK(!exchange.user_state(2).store_overflowed());
}

TEST_CASE(gateway_ouch_session, store_admission_counts_bytes_too) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.store_messages = 1000;
    config.limits.store_bytes = 4 * kMaxFramedOutboundBytes;  // 332 bytes
    config.limits.store_reserve_messages = 2;
    TestExchange exchange(config);
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));  // S: 13 bytes, 319 free
    alice.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredDollars));  // 250 free afterwards
    alice.send_message(enter_order("BID2", 'B', 10, "AAPL", kHundredDollars));  // admitted (250 / 83 = 3)
    alice.send_message(enter_order("BID3", 'B', 10, "AAPL", kHundredDollars));  // 181 / 83 = 2: refused
    CHECK_EQ(alice.take_types(), "SAA");
    CHECK(alice.connection().close_reason() == OuchCloseReason::StoreReserveReached);
}

TEST_CASE(gateway_ouch_session, fault_hooks_reject_and_drop) {
    TestExchange exchange;
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw"));
    alice.take_frames();
    exchange.inject_gateway_rejects(1, 2);
    for (const char* token : {"BID1", "BID2", "BID3"}) {
        alice.send_message(enter_order(token, 'B', 10, "AAPL", kHundredDollars));
    }
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "JJA");
    CHECK_EQ(decode_ouch<ouch42::OrderRejected>(frames[0]).reason, 'O');
    CHECK_EQ(exchange.submitted_command_count(), 1u);

    // drop_out 1: message 5 is stored, never written, and the link is cut
    // after it (SoupBinTCP numbers messages implicitly).
    exchange.drop_next_outbound_messages(1, 1);
    alice.send_message(enter_order("BID4", 'B', 10, "AAPL", kHundredDollars));
    CHECK(alice.take_frames().empty());
    CHECK(alice.connection().close_reason() == OuchCloseReason::FaultDropOut);
    CHECK(alice.connection().flush_before_close());
    CHECK_EQ(exchange.user_state(1).statistics().outbound_messages_dropped_by_fault, 1u);
    alice.disconnect();

    // The client saw 4 messages and asks for 5. Replay is never affected by
    // the drop fault and recovers the message.
    exchange.drop_next_outbound_messages(1, 5);
    RawSession recovered(exchange);
    REQUIRE(recovered.login("alice", "alicepw", 5));
    frames = recovered.take_frames();
    // BID4 (dropped live), then the cancel-on-disconnect of BID3 and BID4.
    REQUIRE_EQ(sequenced_types(frames), "ACC");
    CHECK_EQ(token_text(decode_ouch<ouch42::OrderAccepted>(frames[1]).token), "BID4");
    CHECK(recovered.connection().state() == OuchConnectionState::LoggedIn);
}

TEST_CASE(gateway_ouch_session, trading_day_lifecycle) {
    TestExchange exchange;
    {
        RawSession alice(exchange);
        REQUIRE(alice.login("alice", "alicepw"));
        alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
        alice.take_frames();
        exchange.set_phase(0, Phase::Closed);
        std::vector<std::string> frames = alice.take_frames();
        REQUIRE_EQ(frames.size(), 1u);
        CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[0]).reason, 'E');
        exchange.advance(kOneSecond);
        exchange.user_states().on_shutdown(exchange.now(), exchange);  // System Event 'E' for every OUCH user
        alice.connection().end_session(exchange);
        frames = alice.take_frames();
        REQUIRE_EQ(frames.size(), 2u);
        CHECK(frames[0] == Golden().byte('S').u64(kNineThirty + kOneSecond).byte('E').soup('S'));
        CHECK(frames[1] == control_frame<'Z'>());
    }
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 1u);  // bob never logged in: just the stored 'E'
    exchange.user_states().reset_for_new_trading_day();
    CHECK_EQ(exchange.user_state(1).store().last_seq(), 0u);
    CHECK_EQ(exchange.user_state(2).store().last_seq(), 0u);
    exchange.set_phase(0, Phase::Continuous);
    RawSession alice(exchange);
    REQUIRE(alice.login("alice", "alicepw", 1));
    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));  // the token is new again
    const std::vector<std::string> frames = alice.take_frames();
    CHECK(frames[0] == login_accepted_golden(1));
    CHECK_EQ(sequenced_types(frames), "SA");
}
