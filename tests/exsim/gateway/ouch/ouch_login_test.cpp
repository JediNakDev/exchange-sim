// SoupBinTCP login, logout and session-control behaviour of OuchConnection.

#include "ouch_test_harness.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

std::string login_accepted_golden(std::uint64_t sequence) {
    return Golden().alpha("EXSIM", 10).numeric(sequence, 20).soup('A');
}

std::string system_event_golden(Timestamp timestamp, char event) {
    return Golden().byte('S').u64(timestamp).byte(event).soup('S');
}

std::string login_rejected_golden(char reason) { return Golden().byte(reason).soup('J'); }

}  // namespace

TEST_CASE(gateway_ouch_login, accepted_login_writes_golden_login_accepted_then_start_of_day) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0] == login_accepted_golden(1));
    CHECK(frames[1] == system_event_golden(kNineThirty, 'S'));
    CHECK(exchange.is_bound(1));
    CHECK(exchange.user_state(1).has_attached_connection());
    CHECK(exchange.user_state(1).start_of_day_sent());
    CHECK_EQ(session.connection().next_sequence_to_send(), 2u);
    CHECK(!session.connection().is_replaying());
}

TEST_CASE(gateway_ouch_login, login_request_golden_bytes_are_accepted) {
    TestExchange exchange;
    RawSession session(exchange);
    // Independent byte layout: username(6) password(10) session(10) sequence(20).
    session.send(Golden().alpha("alice", 6).alpha("alicepw", 10).alpha("", 10).numeric(0, 20).soup('L'));
    CHECK(session.connection().state() == OuchConnectionState::LoggedIn);
    CHECK(session.take_frames().front() == login_accepted_golden(1));
}

TEST_CASE(gateway_ouch_login, start_of_day_is_sent_once_per_trading_day) {
    TestExchange exchange;
    {
        RawSession first(exchange);
        REQUIRE(first.login("bob", "bobpw"));
        CHECK_EQ(first.take_types(), "S");
    }
    exchange.advance(kOneSecond);
    RawSession second(exchange);
    REQUIRE(second.login("bob", "bobpw"));
    const std::vector<std::string> frames = second.take_frames();
    REQUIRE_EQ(frames.size(), 1u);  // requested 0: no replay, no second S
    CHECK(frames[0] == login_accepted_golden(2));
}

TEST_CASE(gateway_ouch_login, bad_password_is_rejected_not_authorized) {
    TestExchange exchange;
    RawSession session(exchange);
    CHECK(!session.login("alice", "wrong"));
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == login_rejected_golden('A'));
    CHECK(session.connection().close_requested());
    CHECK(session.connection().close_reason() == OuchCloseReason::LoginRejected);
    CHECK(session.connection().flush_before_close());
    CHECK(!exchange.is_bound(1));
    CHECK_EQ(exchange.unbind_count(), 0u);
}

TEST_CASE(gateway_ouch_login, unknown_user_is_rejected_not_authorized) {
    TestExchange exchange;
    RawSession session(exchange);
    CHECK(!session.login("eve", "x"));
    CHECK(session.take_frames().front() == login_rejected_golden('A'));
}

TEST_CASE(gateway_ouch_login, fix_user_cannot_log_in_over_ouch) {
    TestExchange exchange;
    RawSession session(exchange);
    CHECK(!session.login("carol", "carolpw"));
    CHECK(session.take_frames().front() == login_rejected_golden('A'));
    CHECK(!exchange.is_bound(3));
}

TEST_CASE(gateway_ouch_login, second_concurrent_login_is_rejected_session_not_available) {
    TestExchange exchange;
    RawSession first(exchange);
    REQUIRE(first.login("alice", "alicepw"));
    first.take_frames();
    RawSession second(exchange);
    CHECK(!second.login("alice", "alicepw"));
    CHECK(second.take_frames().front() == login_rejected_golden('S'));
    // The first connection is untouched and still owns the session.
    CHECK(first.connection().state() == OuchConnectionState::LoggedIn);
    CHECK(exchange.is_bound(1));
    CHECK(exchange.user_state(1).attached_connection() == &first.connection());
    second.disconnect();
    CHECK(exchange.is_bound(1));
    CHECK_EQ(exchange.unbind_count(), 0u);
}

TEST_CASE(gateway_ouch_login, requested_session_must_match_the_current_session) {
    TestExchange exchange;
    {
        RawSession mismatched(exchange);
        CHECK(!mismatched.login("alice", "alicepw", 0, "OTHER"));
        CHECK(mismatched.take_frames().front() == login_rejected_golden('S'));
        CHECK(!exchange.is_bound(1));
    }
    {
        RawSession named(exchange);
        CHECK(named.login("alice", "alicepw", 0, "EXSIM"));
    }
    RawSession blank(exchange);
    CHECK(blank.login("alice", "alicepw", 0, ""));
}

TEST_CASE(gateway_ouch_login, requested_sequence_is_clamped_to_the_next_message) {
    TestExchange exchange;
    {
        RawSession first(exchange);
        REQUIRE(first.login("bob", "bobpw"));  // stores S as message 1
    }
    RawSession session(exchange);
    REQUIRE(session.login("bob", "bobpw", 100));
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == login_accepted_golden(2));
}

TEST_CASE(gateway_ouch_login, data_before_login_is_a_protocol_violation) {
    TestExchange exchange;
    RawSession session(exchange);
    session.send_message(enter_order("T1", 'B', 100, "AAPL", 1'000'000));
    CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 0u);
}

TEST_CASE(gateway_ouch_login, heartbeat_and_debug_before_login_are_ignored) {
    TestExchange exchange;
    RawSession session(exchange);
    session.send(control_frame<'R'>());
    session.send(Golden().alpha("hello", 5).soup('+'));
    CHECK(session.connection().state() == OuchConnectionState::AwaitingLogin);
    CHECK(session.login("alice", "alicepw"));
}

TEST_CASE(gateway_ouch_login, second_login_on_a_logged_in_connection_is_a_protocol_violation) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.send(login_frame("alice", "alicepw", "", 0));
    CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
    CHECK(!exchange.is_bound(1));  // released immediately
    CHECK(!exchange.user_state(1).has_attached_connection());
}

TEST_CASE(gateway_ouch_login, server_only_packet_types_are_protocol_violations) {
    for (const std::string& frame : {control_frame<'H'>(), control_frame<'Z'>(), login_accepted_golden(1)}) {
        TestExchange exchange;
        RawSession session(exchange);
        REQUIRE(session.login("alice", "alicepw"));
        session.send(frame);
        CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
    }
}

TEST_CASE(gateway_ouch_login, malformed_login_request_is_a_protocol_violation) {
    TestExchange exchange;
    RawSession session(exchange);
    // Sequence field with a letter in it.
    session.send(Golden().alpha("alice", 6).alpha("alicepw", 10).alpha("", 10).alpha("12x", 20).soup('L'));
    CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
    CHECK(session.take_frames().empty());
}

TEST_CASE(gateway_ouch_login, zero_length_frame_is_a_protocol_violation) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.send(std::string_view("\0\0", 2));
    CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
}

TEST_CASE(gateway_ouch_login, oversized_inbound_frame_is_a_protocol_violation) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.send(Golden().alpha("", 300).soup('+'));
    CHECK(session.connection().close_reason() == OuchCloseReason::ProtocolViolation);
}

TEST_CASE(gateway_ouch_login, logout_request_closes_and_releases_the_session) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.send(control_frame<'O'>());
    CHECK(session.connection().close_reason() == OuchCloseReason::ClientLogout);
    CHECK(session.connection().flush_before_close());
    CHECK(!exchange.is_bound(1));
    CHECK(!exchange.user_state(1).has_attached_connection());
    CHECK_EQ(exchange.unbind_count(), 1u);
    // The user can log in again on a new connection.
    RawSession again(exchange);
    CHECK(again.login("alice", "alicepw"));
}

TEST_CASE(gateway_ouch_login, bytes_after_close_are_ignored) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.send(control_frame<'O'>());
    session.take_frames();
    session.send_message(enter_order("T1", 'B', 100, "AAPL", 1'000'000));
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 1u);  // only the logout's cancel-on-disconnect
}

TEST_CASE(gateway_ouch_login, login_deadline_uses_the_idle_timeout) {
    TestExchange exchange;
    RawSession session(exchange);
    exchange.advance(15 * kOneSecond - 1);
    session.timer();
    CHECK(session.connection().state() == OuchConnectionState::AwaitingLogin);
    exchange.advance(1);
    session.timer();
    CHECK(session.connection().close_reason() == OuchCloseReason::ClientIdleTimeout);
    CHECK(session.take_frames().empty());
}

TEST_CASE(gateway_ouch_login, end_session_sends_end_of_session_and_releases) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("alice", "alicepw"));
    session.take_frames();
    session.connection().end_session(exchange);
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == control_frame<'Z'>());
    CHECK(session.connection().close_reason() == OuchCloseReason::EndOfSession);
    CHECK(!exchange.is_bound(1));
}

TEST_CASE(gateway_ouch_login, session_settings_come_from_the_exchange_section) {
    server::ExchangeSection exchange_section;
    exchange_section.soup_session = "DAY1";
    exchange_section.heartbeat_interval_ms = 250;
    const OuchSessionSettings settings = OuchSessionSettings::from(exchange_section, 5000);
    CHECK(std::string_view(settings.soup_session.data(), 10) == "DAY1      ");
    CHECK_EQ(settings.server_heartbeat_interval_ns, Timestamp{250'000'000});
    CHECK_EQ(settings.client_idle_timeout_ns, Timestamp{5'000'000'000});

    server::EngineSection engine_section;
    engine_section.max_tokens_per_session = 77;
    engine_section.store_messages_per_session = 320;
    engine_section.store_bytes_per_session = 9000;
    const OuchUserStateLimits limits = OuchUserStateLimits::from(engine_section);
    CHECK_EQ(limits.max_tokens, 77u);
    CHECK_EQ(limits.store_messages, 320u);
    CHECK_EQ(limits.store_bytes, 9000u);
    CHECK_EQ(limits.store_reserve_messages, 20u);
}
