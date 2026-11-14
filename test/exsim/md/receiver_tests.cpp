// MoldUdp64Receiver: in-order delivery, duplicates, reordering, gap
// requests, retries, loss declaration, session handling, End of Session,
// plus typed ITCH dispatch.

#include "test/exsim/support/clients/itch_dispatch.hpp"
#include "test/exsim/support/clients/moldudp64_receiver.hpp"
#include <test/exsim/md/md_test_support.hpp>
#include <test/framework/test.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

Bytes message_for(std::uint64_t sequence, std::size_t length = 12) {
    Bytes message(length, static_cast<char>('a' + sequence % 26));
    message[0] = 'S';
    return message;
}

// Data packet carrying sequences [first, first + count).
Bytes data_packet(std::uint64_t first, std::uint16_t count, std::string_view session = "EXSIM",
                  std::size_t message_length = 12) {
    Bytes storage(65535);
    protocol::moldudp64::PacketBuilder builder(storage, storage.size());
    REQUIRE(builder.reset(session, first));
    for (std::uint64_t sequence = first; sequence < first + count; ++sequence)
        REQUIRE(builder.append(message_for(sequence, message_length)) == protocol::Status::ok);
    const auto packet = builder.packet();
    return Bytes(packet.begin(), packet.end());
}

Bytes header_packet(std::uint64_t sequence, bool end_of_session, std::string_view session = "EXSIM") {
    Bytes storage(20);
    protocol::moldudp64::PacketBuilder builder(storage, storage.size());
    REQUIRE(builder.reset(session, sequence, end_of_session));
    const auto packet = builder.packet();
    return Bytes(packet.begin(), packet.end());
}

struct RecordingHandler {
    std::vector<std::pair<std::uint64_t, Bytes>> delivered;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lost_runs;
    std::vector<std::uint64_t> end_of_session_calls;

    void on_message(std::uint64_t sequence, std::span<const char> message) {
        delivered.emplace_back(sequence, Bytes(message.begin(), message.end()));
    }
    void on_messages_lost(std::uint64_t first, std::uint64_t count) { lost_runs.emplace_back(first, count); }
    void on_end_of_session(std::uint64_t next_sequence) { end_of_session_calls.push_back(next_sequence); }

    // Delivered sequences are exactly [first, last] in order, with the right bytes.
    bool delivered_range(std::uint64_t first, std::uint64_t last) const {
        if (delivered.size() != last - first + 1) return false;
        for (std::uint64_t sequence = first; sequence <= last; ++sequence) {
            const auto& [delivered_sequence, bytes] = delivered[sequence - first];
            if (delivered_sequence != sequence || bytes != message_for(sequence)) return false;
        }
        return true;
    }
};

using TestReceiver = md::MoldUdp64Receiver<RecordingHandler, PacketQueueSink>;

struct ReceiverFixture {
    RecordingHandler handler;
    std::deque<Bytes> requests;
    TestReceiver receiver;

    explicit ReceiverFixture(const md::ReceiverConfig& config = default_config())
        : receiver(config, handler, PacketQueueSink{&requests}) {}

    static md::ReceiverConfig default_config() {
        md::ReceiverConfig config;
        config.session = "EXSIM";
        config.reorder_window_messages = 64;
        config.request_timeout_ns = 100;
        config.max_request_attempts = 3;
        return config;
    }

    md::PacketStatus feed(const Bytes& packet, std::uint64_t now = 0) { return receiver.on_packet(packet, now); }

    protocol::moldudp64::Request request(std::size_t index) const {
        protocol::moldudp64::Request decoded;
        REQUIRE(index < requests.size());
        REQUIRE(protocol::moldudp64::decode(requests[index].data(), requests[index].size(), decoded) ==
                protocol::Status::ok);
        return decoded;
    }
};

}  // namespace

TEST_CASE(md_receiver, config_rules) {
    RecordingHandler handler;
    std::deque<Bytes> requests;
    auto construct_throws = [&](const md::ReceiverConfig& config) {
        try {
            TestReceiver receiver(config, handler, PacketQueueSink{&requests});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    md::ReceiverConfig config;
    CHECK(!construct_throws(config));
    config.reorder_window_messages = 0;
    CHECK(construct_throws(config));
    config = {};
    config.max_buffered_message_size = 0;
    CHECK(construct_throws(config));
    config = {};
    config.session = "ABCDEFGHIJK";
    CHECK(construct_throws(config));
}

TEST_CASE(md_receiver, in_order_delivery_and_duplicates) {
    ReceiverFixture fixture;
    CHECK(fixture.feed(data_packet(1, 2)) == md::PacketStatus::Accepted);
    CHECK(fixture.feed(data_packet(3, 1)) == md::PacketStatus::Accepted);
    CHECK(fixture.handler.delivered_range(1, 3));
    CHECK(fixture.feed(data_packet(1, 2)) == md::PacketStatus::Accepted);
    CHECK(fixture.feed(data_packet(2, 3)) == md::PacketStatus::Accepted);  // 2, 3 old; 4 new
    CHECK(fixture.handler.delivered_range(1, 4));
    CHECK_EQ(fixture.receiver.stats().duplicate_messages, 4u);
    CHECK_EQ(fixture.receiver.next_sequence(), 5u);
    CHECK(!fixture.receiver.gap_open());
    CHECK(fixture.requests.empty());
}

TEST_CASE(md_receiver, reordered_packets_are_buffered_and_gap_requested) {
    ReceiverFixture fixture;
    fixture.feed(data_packet(3, 2), 10);
    CHECK(fixture.handler.delivered.empty());
    CHECK_EQ(fixture.receiver.buffered_messages(), 2u);
    CHECK(fixture.receiver.gap_open());
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.requests[0].size(), md::kMoldRequestSize);
    const auto request = fixture.request(0);
    CHECK_EQ(hex_of(request.session), hex_of(hex_bytes("45 58 53 49 4D 20 20 20 20 20")));
    CHECK_EQ(request.sequence, 1u);
    CHECK_EQ(request.count, 2u);  // up to the first buffered message
    CHECK(fixture.receiver.request_outstanding());

    fixture.feed(data_packet(3, 2), 20);  // buffered duplicates
    CHECK_EQ(fixture.receiver.stats().duplicate_messages, 2u);
    fixture.feed(data_packet(1, 2), 30);
    CHECK(fixture.handler.delivered_range(1, 4));
    CHECK_EQ(fixture.receiver.buffered_messages(), 0u);
    CHECK(!fixture.receiver.request_outstanding());
    CHECK_EQ(fixture.requests.size(), 1u);
}

TEST_CASE(md_receiver, heartbeat_reveals_tail_gap) {
    ReceiverFixture fixture;
    fixture.feed(data_packet(1, 2));
    fixture.feed(header_packet(5, false));
    CHECK_EQ(fixture.receiver.stats().heartbeats, 1u);
    CHECK_EQ(fixture.receiver.known_end_sequence(), 5u);
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.request(0).sequence, 3u);
    CHECK_EQ(fixture.request(0).count, 2u);
    // A heartbeat at the next expected sequence is not a gap.
    ReceiverFixture caught_up;
    caught_up.feed(data_packet(1, 2));
    caught_up.feed(header_packet(3, false));
    CHECK(!caught_up.receiver.gap_open());
    CHECK(caught_up.requests.empty());
}

TEST_CASE(md_receiver, unanswered_request_retries_then_declares_loss) {
    ReceiverFixture fixture;  // timeout 100, three attempts
    fixture.feed(data_packet(4, 2), 1'000);
    REQUIRE_EQ(fixture.requests.size(), 1u);
    fixture.receiver.poll(1'099);
    CHECK_EQ(fixture.requests.size(), 1u);
    fixture.receiver.poll(1'100);
    CHECK_EQ(fixture.requests.size(), 2u);
    fixture.receiver.poll(1'150);
    CHECK_EQ(fixture.requests.size(), 2u);
    fixture.receiver.poll(1'200);
    CHECK_EQ(fixture.requests.size(), 3u);
    CHECK_EQ(fixture.request(2).sequence, 1u);
    CHECK_EQ(fixture.request(2).count, 3u);
    CHECK(fixture.handler.lost_runs.empty());
    fixture.receiver.poll(1'300);
    REQUIRE_EQ(fixture.handler.lost_runs.size(), 1u);
    CHECK_EQ(fixture.handler.lost_runs[0].first, 1u);
    CHECK_EQ(fixture.handler.lost_runs[0].second, 3u);
    CHECK_EQ(fixture.receiver.stats().messages_lost, 3u);
    CHECK_EQ(fixture.receiver.stats().request_retries, 2u);
    CHECK_EQ(fixture.receiver.stats().requests_sent, 3u);
    // Delivery resumes with the buffered messages after the lost run.
    REQUIRE_EQ(fixture.handler.delivered.size(), 2u);
    CHECK_EQ(fixture.handler.delivered[0].first, 4u);
    CHECK_EQ(fixture.handler.delivered[1].first, 5u);
    CHECK(!fixture.receiver.gap_open());
    CHECK(!fixture.receiver.request_outstanding());
    // Late answers for the lost run are duplicates now.
    fixture.feed(data_packet(1, 3), 1'400);
    CHECK_EQ(fixture.handler.delivered.size(), 2u);
    CHECK_EQ(fixture.receiver.stats().duplicate_messages, 3u);
}

TEST_CASE(md_receiver, partial_answer_re_requests_the_remainder_at_once) {
    ReceiverFixture fixture;
    fixture.feed(data_packet(11, 1), 0);
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.request(0).sequence, 1u);
    CHECK_EQ(fixture.request(0).count, 10u);
    fixture.feed(data_packet(1, 4), 10);  // the server sent what fitted
    REQUIRE_EQ(fixture.requests.size(), 2u);
    CHECK_EQ(fixture.request(1).sequence, 5u);
    CHECK_EQ(fixture.request(1).count, 6u);
    CHECK_EQ(fixture.receiver.stats().request_retries, 0u);
    fixture.feed(data_packet(5, 6), 20);
    CHECK(fixture.handler.delivered_range(1, 11));
    CHECK_EQ(fixture.requests.size(), 2u);
}

TEST_CASE(md_receiver, request_count_is_capped) {
    md::ReceiverConfig config = ReceiverFixture::default_config();
    config.max_request_messages = 4;
    ReceiverFixture fixture(config);
    fixture.feed(header_packet(101, false));
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.request(0).sequence, 1u);
    CHECK_EQ(fixture.request(0).count, 4u);
    // Loss is declared one capped run at a time.
    for (std::uint64_t now = 100; now <= 300; now += 100) fixture.receiver.poll(now);
    REQUIRE_EQ(fixture.handler.lost_runs.size(), 1u);
    CHECK_EQ(fixture.handler.lost_runs[0].second, 4u);
    CHECK_EQ(fixture.receiver.next_sequence(), 5u);
    CHECK(fixture.receiver.request_outstanding());  // a fresh request for 5..8
    CHECK_EQ(fixture.request(fixture.requests.size() - 1).sequence, 5u);
}

TEST_CASE(md_receiver, session_filtering_and_adoption) {
    ReceiverFixture fixture;
    CHECK(fixture.feed(data_packet(1, 1, "OTHER")) == md::PacketStatus::WrongSession);
    CHECK(fixture.handler.delivered.empty());
    CHECK_EQ(fixture.receiver.stats().wrong_session_packets, 1u);
    CHECK_EQ(fixture.receiver.known_end_sequence(), 0u);

    md::ReceiverConfig adopt = ReceiverFixture::default_config();
    adopt.session = {};
    ReceiverFixture adopting(adopt);
    CHECK(adopting.feed(data_packet(2, 1, "FEED2")) == md::PacketStatus::Accepted);
    CHECK(adopting.feed(data_packet(1, 1, "EXSIM")) == md::PacketStatus::WrongSession);
    REQUIRE_EQ(adopting.requests.size(), 1u);
    CHECK_EQ(hex_of(adopting.request(0).session), hex_of(Bytes{'F', 'E', 'E', 'D', '2', ' ', ' ', ' ', ' ', ' '}));
    CHECK(adopting.feed(data_packet(1, 1, "FEED2")) == md::PacketStatus::Accepted);
    CHECK(adopting.handler.delivered_range(1, 2));
}

TEST_CASE(md_receiver, late_join_starts_at_first_packet) {
    md::ReceiverConfig config = ReceiverFixture::default_config();
    config.first_sequence = 0;
    ReceiverFixture fixture(config);
    fixture.feed(data_packet(500, 2));
    CHECK(fixture.requests.empty());
    REQUIRE_EQ(fixture.handler.delivered.size(), 2u);
    CHECK_EQ(fixture.handler.delivered[0].first, 500u);
    fixture.feed(data_packet(499, 1));
    CHECK_EQ(fixture.receiver.stats().duplicate_messages, 1u);
    CHECK_EQ(fixture.receiver.next_sequence(), 502u);
}

TEST_CASE(md_receiver, end_of_session_completes_after_gap_is_filled) {
    ReceiverFixture fixture;
    fixture.feed(data_packet(1, 2));
    fixture.feed(header_packet(4, true));
    CHECK(fixture.receiver.end_of_session_seen());
    CHECK(!fixture.receiver.end_of_session_reached());
    CHECK(fixture.handler.end_of_session_calls.empty());
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.request(0).sequence, 3u);
    CHECK_EQ(fixture.request(0).count, 1u);
    fixture.feed(data_packet(3, 1));
    CHECK(fixture.receiver.end_of_session_reached());
    REQUIRE_EQ(fixture.handler.end_of_session_calls.size(), 1u);
    CHECK_EQ(fixture.handler.end_of_session_calls[0], 4u);
    fixture.feed(header_packet(4, true));  // repeated End of Session
    CHECK_EQ(fixture.handler.end_of_session_calls.size(), 1u);
    CHECK_EQ(fixture.receiver.stats().end_of_session_packets, 2u);

    // With nothing missing it completes immediately.
    ReceiverFixture complete;
    complete.feed(data_packet(1, 3));
    complete.feed(header_packet(4, true));
    CHECK(complete.receiver.end_of_session_reached());
    CHECK(complete.requests.empty());

    // A run declared lost also lets the session complete.
    ReceiverFixture lossy;
    lossy.feed(header_packet(3, true), 0);
    for (std::uint64_t now = 100; now <= 300; now += 100) lossy.receiver.poll(now);
    CHECK(lossy.receiver.end_of_session_reached());
    REQUIRE_EQ(lossy.handler.end_of_session_calls.size(), 1u);
    CHECK_EQ(lossy.handler.end_of_session_calls[0], 3u);
    CHECK_EQ(lossy.receiver.stats().messages_lost, 2u);
}

TEST_CASE(md_receiver, messages_beyond_window_or_slot_are_dropped_then_recovered) {
    md::ReceiverConfig config = ReceiverFixture::default_config();
    config.reorder_window_messages = 4;
    config.max_buffered_message_size = 16;
    ReceiverFixture fixture(config);
    fixture.feed(data_packet(10, 1));  // 9 ahead of next: outside the window
    CHECK_EQ(fixture.receiver.stats().messages_dropped_outside_window, 1u);
    CHECK_EQ(fixture.receiver.buffered_messages(), 0u);
    CHECK_EQ(fixture.receiver.known_end_sequence(), 11u);
    REQUIRE_EQ(fixture.requests.size(), 1u);
    CHECK_EQ(fixture.request(0).count, 10u);
    fixture.feed(data_packet(2, 3));  // 2..4 fit the window
    CHECK_EQ(fixture.receiver.buffered_messages(), 3u);
    fixture.feed(data_packet(1, 10));
    CHECK(fixture.handler.delivered_range(1, 10));

    // A message larger than a slot cannot be buffered but is delivered in order.
    ReceiverFixture small(config);
    small.feed(data_packet(2, 1, "EXSIM", 20));
    CHECK_EQ(small.receiver.stats().messages_dropped_outside_window, 1u);
    small.feed(data_packet(1, 2, "EXSIM", 20));
    REQUIRE_EQ(small.handler.delivered.size(), 2u);
    CHECK_EQ(small.handler.delivered[1].second.size(), 20u);
}

TEST_CASE(md_receiver, malformed_datagrams_are_rejected) {
    ReceiverFixture fixture;
    CHECK(fixture.feed(Bytes(19, 'x')) == md::PacketStatus::Malformed);
    Bytes truncated = data_packet(1, 2);
    truncated.pop_back();
    CHECK(fixture.feed(truncated) == md::PacketStatus::Malformed);
    Bytes trailing = data_packet(1, 1);
    trailing.push_back(0);
    CHECK(fixture.feed(trailing) == md::PacketStatus::Malformed);
    CHECK(fixture.feed(data_packet(0, 1)) == md::PacketStatus::Malformed);
    CHECK_EQ(fixture.receiver.stats().malformed_packets, 4u);
    CHECK(fixture.handler.delivered.empty());
    CHECK(fixture.requests.empty());
}

namespace {

struct TypedRecorder {
    std::string seen;
    std::uint64_t last_add_reference = 0;
    std::uint64_t end_of_session_next = 0;
    std::size_t unknown = 0;
    void on_itch(std::uint64_t, const protocol::itch50::SystemEvent& message) { seen.push_back(message.event); }
    void on_itch(std::uint64_t, const protocol::itch50::AddOrder& message) {
        seen.push_back('A');
        last_add_reference = message.order_reference;
    }
    void on_unknown_itch(std::uint64_t, std::span<const char>) { ++unknown; }
    void on_end_of_session(std::uint64_t next) { end_of_session_next = next; }
};

}  // namespace

TEST_CASE(md_receiver, typed_dispatch_through_receiver) {
    TypedRecorder recorder;
    md::TypedItchHandler<TypedRecorder> typed{recorder};
    std::deque<Bytes> requests;
    md::ReceiverConfig config;
    md::MoldUdp64Receiver<md::TypedItchHandler<TypedRecorder>, PacketQueueSink> receiver(config, typed,
                                                                                          PacketQueueSink{&requests});
    const Bytes system = hex_bytes("53 0000 0000 000000000010 4F");
    const Bytes add = hex_bytes("41 0001 0000 000000000020 0000000000000007 42 00000064 4141412020202020 000F4240");
    const Bytes delete_message = hex_bytes("44 0001 0000 000000000020 0000000000000007");  // no on_itch overload
    const Bytes unknown = hex_bytes("5A 0001");
    const Bytes truncated_add = hex_bytes("41 0001 0000");
    Bytes storage(1400);
    protocol::moldudp64::PacketBuilder builder(storage, storage.size());
    REQUIRE(builder.reset("EXSIM", 1));
    for (const Bytes* message : {&system, &add, &delete_message, &unknown, &truncated_add})
        REQUIRE(builder.append(*message) == protocol::Status::ok);
    REQUIRE(receiver.on_packet(builder.packet(), 0) == md::PacketStatus::Accepted);
    CHECK_EQ(recorder.seen, std::string("OA"));
    CHECK_EQ(recorder.last_add_reference, 7u);
    CHECK_EQ(recorder.unknown, 1u);
    CHECK_EQ(typed.decode_failures, 2u);  // unknown type and truncated A
    REQUIRE(receiver.on_packet(header_packet(6, true), 0) == md::PacketStatus::Accepted);
    CHECK_EQ(recorder.end_of_session_next, 6u);
}
