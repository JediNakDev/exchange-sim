// Garbled framing (bad CheckSum, bad BodyLength, junk, partial frames) and
// the heartbeat / TestRequest / timeout timers on the injected clock.

#include "test/exsim/gateway/fix/fix_test_support.hpp"

#include <string>
#include <vector>

using namespace fix_test;

namespace {

using Tokens = std::vector<std::string>;
constexpr exsim::Timestamp kSecond = fix::kNanosecondsPerSecond;

std::string order(std::string_view client_order_id) {
    return new_order_fields(client_order_id, "1", 100, "150", "AAPL");
}

std::string with_bad_checksum(std::string frame) {
    frame[frame.size() - 2] = frame[frame.size() - 2] == '0' ? '1' : '0';
    return frame;
}

std::uint64_t expected_inbound(FixTestExchange& exchange) {
    return exchange.user(2).session_day_state().expected_inbound_seq();
}

}  // namespace

TEST_CASE(gateway_fix_garbled, bad_checksum_is_discarded_without_sequence_change) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.feed(with_bad_checksum(client_message("D", 2, order("G2"), "CLIENT2")));
    CHECK_EQ(expected_inbound(exchange), 2u);
    CHECK(client.drain().empty());
    CHECK_EQ(client.connection().counters().garbled_frames, 1u);
    CHECK(engine_command_tokens(exchange, 2).empty());
    // The same sequence number sent correctly is accepted.
    client.send_at("D", 2, order("G2"));
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2"}));
}

TEST_CASE(gateway_fix_garbled, next_valid_high_sequence_triggers_recovery) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.feed(with_bad_checksum(client_message("D", 2, order("G2"), "CLIENT2")));
    client.send_at("D", 3, order("G3"));
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("2", 2, "7=2|16=0|", "CLIENT2")));
    client.send_at("D", 2, order("G2"), true);
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2", "D:G3"}));
}

TEST_CASE(gateway_fix_garbled, bad_body_length_and_junk_resynchronise) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    std::string wrong_length = client_message("D", 2, order("G2"), "CLIENT2");
    const std::size_t length_position = wrong_length.find("9=") + 2;
    wrong_length[length_position] = static_cast<char>(wrong_length[length_position] - 1);
    std::string stream = "garbage\001bytes" + wrong_length + "more junk" + client_message("D", 2, order("G2B"), "CLIENT2") +
                         client_message("D", 3, order("G3"), "CLIENT2");
    CHECK_EQ(client.feed(stream), stream.size());
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2B", "D:G3"}));
    CHECK_EQ(client.connection().counters().garbled_frames, 2u);  // leading junk; bad frame plus trailing junk
    CHECK(!client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_garbled, body_length_too_long_swallowing_next_frame_recovers) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    // BodyLength claims 40 more bytes than the frame has: the declared frame
    // runs into the next real frame, fails its CheckSum, and the parser
    // resynchronises on the next "8=FIX.4.4" start.
    std::string too_long = client_message("D", 2, order("G2"), "CLIENT2");
    const std::size_t length_position = too_long.find("9=") + 2;
    const std::size_t length_end = too_long.find('\001', length_position);
    const unsigned declared = static_cast<unsigned>(std::stoul(too_long.substr(length_position, length_end)));
    too_long.replace(length_position, length_end - length_position, std::to_string(declared + 40));
    const std::string valid = client_message("D", 2, order("G2B"), "CLIENT2");
    const std::string stream = too_long + valid;
    CHECK_EQ(client.feed(stream), stream.size());
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2B"}));
}

TEST_CASE(gateway_fix_garbled, partial_frames_wait_for_more_bytes) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    const std::string frame = client_message("D", 2, order("G2"), "CLIENT2");
    for (std::size_t split = 1; split < frame.size(); split += 7) {
        // Feeding a prefix consumes nothing; the remainder completes it.
        CHECK_EQ(client.feed(frame.substr(0, split)), std::size_t{0});
    }
    CHECK_EQ(client.feed(frame), frame.size());
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2"}));
}

TEST_CASE(gateway_fix_garbled, oversized_frame_is_discarded) {
    FixTestExchange exchange;
    fix::FixSessionSettings settings;
    settings.max_inbound_message_bytes = 256;
    FixTestConnection client(exchange, "CLIENT2", settings);
    client.logon(30, false, {});
    const std::string huge = client_message("0", 2, "58=" + std::string(400, 'x') + "|", "CLIENT2");
    const std::string valid = client_message("D", 2, order("G2"), "CLIENT2");
    const std::string stream = huge + valid;
    CHECK_EQ(client.feed(stream), stream.size());
    CHECK_EQ(engine_command_tokens(exchange, 2), (Tokens{"D:G2"}));
}

TEST_CASE(gateway_fix_timers, heartbeat_after_outbound_silence) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.tick(29 * kSecond);
    CHECK(client.drain().empty());
    client.send("0", "");  // inbound traffic does not reset the outbound timer
    client.tick(kSecond);
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(fix_frame("35=0|49=EXSIM|56=CLIENT2|34=2|52=20261103-09:30:30.000|")));
}

TEST_CASE(gateway_fix_timers, test_request_then_timeout_at_twice_the_interval) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(10, false, {});
    client.tick(10 * kSecond);
    CHECK_EQ(message_types(client.drain()), (Tokens{"0"}));
    client.tick(5 * kSecond);
    auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(fix_frame("35=1|49=EXSIM|56=CLIENT2|34=3|52=20261103-09:30:15.000|112=TEST1|")));
    CHECK(client.connection().test_request_outstanding());
    // Unrelated traffic and a non-matching Heartbeat do not clear the probe.
    client.send("0", "");
    client.send("0", "112=OTHER|");
    CHECK(client.connection().test_request_outstanding());
    client.tick(5 * kSecond - 1);
    CHECK(!client.connection().wants_disconnect());
    client.tick(1);
    messages = client.drain();
    REQUIRE(!messages.empty());
    CHECK_EQ(messages.back().type(), std::string("5"));
    CHECK_EQ(messages.back().get(58), std::string("heartbeat timeout"));
    CHECK(client.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_timers, matching_heartbeat_clears_probe) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(10, false, {});
    client.tick(15 * kSecond);
    client.drain();
    CHECK(client.connection().test_request_outstanding());
    client.send("0", "112=TEST1|");
    CHECK(!client.connection().test_request_outstanding());
    client.tick(5 * kSecond);
    CHECK(!client.connection().wants_disconnect());
    // The next probe uses a new TestReqID.
    client.tick(10 * kSecond);
    const auto messages = client.drain();
    REQUIRE(!messages.empty());
    CHECK_EQ(messages.back().get(112), std::string("TEST2"));
}

TEST_CASE(gateway_fix_timers, inbound_traffic_postpones_test_request) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(10, false, {});
    client.tick(14 * kSecond);
    client.send("0", "");
    client.tick(14 * kSecond);
    for (const auto& message : client.drain()) {
        CHECK(message.type() != "1");
    }
    CHECK(!client.connection().test_request_outstanding());
}

TEST_CASE(gateway_fix_timers, test_request_from_client_is_echoed) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(30, false, {});
    client.send("1", "112=PING-7|");
    const auto messages = client.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("0", 2, "112=PING-7|", "CLIENT2")));
}

TEST_CASE(gateway_fix_timers, zero_heartbeat_interval_disables_heartbeats) {
    FixTestExchange exchange;
    FixTestConnection client(exchange, "CLIENT2");
    client.logon(0, false, {});
    client.tick(3600 * kSecond);
    CHECK(client.drain().empty());
    CHECK(!client.connection().wants_disconnect());
}
