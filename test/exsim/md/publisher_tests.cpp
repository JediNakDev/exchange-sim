// MarketDataPublisher: configuration rules, the session state machine,
// packing into max_payload, per-batch flushing, timers and the ring.

#include <engine/matching_engine.hpp>
#include <md/publisher.hpp>
#include <test/exsim/md/md_test_support.hpp>
#include <test/framework/test.hpp>

#include <limits>
#include <stdexcept>
#include <string>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

md::PublisherConfig small_config() {
    md::PublisherConfig config;
    config.max_instruments = 4;
    config.retransmit_buffer_messages = 4096;
    config.noii_interval_ns = 0;
    return config;
}

Event rested_event(Timestamp ts, InstrumentId instrument, OrderId order_id, Price price, Quantity leaves) {
    Event event{};
    event.type = EventType::Rested;
    event.instrument = instrument;
    event.ts = ts;
    event.rested = RestedEvent{order_id, 1, 1, Side::BUY, price, leaves};
    return event;
}

struct PublisherFixture {
    EngineConfig engine_config{4, 1024};
    MatchingEngine engine{engine_config};
    EventBuffer events{required_event_capacity(engine_config)};
    std::deque<Bytes> packets;
    md::MarketDataPublisher<PacketQueueSink> publisher;

    explicit PublisherFixture(md::PublisherConfig config = small_config(), std::size_t instrument_count = 1)
        : publisher(config, PacketQueueSink{&packets}) {
        const char* symbols[] = {"AAA", "BBB", "CCC", "DDD"};
        for (std::size_t index = 0; index < instrument_count; ++index)
            REQUIRE(engine.add_instrument(test_instrument(symbols[index])) != 0);
        REQUIRE(publisher.add_instruments_from(engine) == md::PublishStatus::Ok);
    }

    md::PublishStatus submit(const Command& command) {
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        return publisher.on_events(events);
    }
};

}  // namespace

TEST_CASE(md_publisher, config_validation) {
    md::PublisherConfig config;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::Ok);

    config.max_payload = md::kMinimumMaxPayload - 1;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config.max_payload = md::kMinimumMaxPayload;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::Ok);
    config.max_payload = md::kMaximumMaxPayload;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::Ok);
    config.max_payload = md::kMaximumMaxPayload + 1;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config = {};

    config.session = "ABCDEFGHIJ";
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::Ok);
    config.session = "ABCDEFGHIJK";
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config = {};

    config.retransmit_buffer_messages = 0;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config = {};
    config.max_instruments = 0;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config.max_instruments = std::size_t{std::numeric_limits<InstrumentId>::max()} + 1;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config = {};
    config.heartbeat_interval_ns = 0;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config = {};
    config.day_origin = std::numeric_limits<Timestamp>::max() - md::kNanosecondsPerTradingDay + 1;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::InvalidConfig);
    config.day_origin = std::numeric_limits<Timestamp>::max() - md::kNanosecondsPerTradingDay;
    CHECK(md::validate_publisher_config(config) == md::PublishStatus::Ok);

    std::deque<Bytes> packets;
    md::PublisherConfig rejected;
    rejected.max_payload = 10;
    bool threw = false;
    try {
        md::MarketDataPublisher<PacketQueueSink> publisher(rejected, PacketQueueSink{&packets});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(md_publisher, instrument_registration_rules) {
    std::deque<Bytes> packets;
    md::MarketDataPublisher<PacketQueueSink> publisher(small_config(), PacketQueueSink{&packets});
    InstrumentConfig instrument = test_instrument("AAA");
    CHECK(publisher.add_instrument(0, instrument) == md::PublishStatus::InvalidInstrument);
    CHECK(publisher.add_instrument(5, instrument) == md::PublishStatus::InvalidInstrument);
    InstrumentConfig too_wide = instrument;
    too_wide.max_price = md::kMaxItchPrice + 1;
    CHECK(publisher.add_instrument(1, too_wide) == md::PublishStatus::InvalidInstrument);
    InstrumentConfig invalid = instrument;
    invalid.tick_size = 0;
    CHECK(publisher.add_instrument(1, invalid) == md::PublishStatus::InvalidInstrument);
    CHECK(publisher.add_instrument(1, instrument) == md::PublishStatus::Ok);
    CHECK(publisher.add_instrument(1, instrument) == md::PublishStatus::DuplicateInstrument);
    CHECK(publisher.add_instrument(4, test_instrument("DDD")) == md::PublishStatus::Ok);

    // Nothing may be published before the session starts.
    const Event event = rested_event(10, 1, 1, 1'000'000, 5);
    CHECK(publisher.on_events(std::span<const Event>(&event, 1)) == md::PublishStatus::NotAllowedInState);
    CHECK(publisher.poll_heartbeat(10) == md::PublishStatus::NotAllowedInState);
    CHECK(publisher.end_of_day(10) == md::PublishStatus::NotAllowedInState);
    CHECK(publisher.end_session(10) == md::PublishStatus::NotAllowedInState);
    CHECK(packets.empty());

    REQUIRE(publisher.start_session(10) == md::PublishStatus::Ok);
    CHECK(publisher.state() == md::PublisherState::Live);
    CHECK(publisher.add_instrument(2, test_instrument("BBB")) == md::PublishStatus::NotAllowedInState);
    CHECK(publisher.start_session(11) == md::PublishStatus::NotAllowedInState);
    // Startup lists only registered locates, ascending: S R H R H S.
    CHECK_EQ(message_types(messages_of(packets)), std::string("SRHRHS"));
    const auto messages = messages_of(packets);
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(messages[3].bytes[2])), 4);  // second R has locate 4
}

TEST_CASE(md_publisher, start_before_day_origin_stays_in_setup) {
    md::PublisherConfig config = small_config();
    config.day_origin = 1'000;
    PublisherFixture fixture(config);
    CHECK(fixture.publisher.start_session(999) == md::PublishStatus::TimestampOutsideDay);
    CHECK(fixture.publisher.state() == md::PublisherState::Setup);
    CHECK(fixture.publisher.start_session(1'000 + md::kNanosecondsPerTradingDay) ==
          md::PublishStatus::TimestampOutsideDay);
    CHECK(fixture.packets.empty());
    CHECK(fixture.publisher.start_session(1'000) == md::PublishStatus::Ok);
    CHECK(fixture.publisher.state() == md::PublisherState::Live);
}

TEST_CASE(md_publisher, out_of_day_batch_stops_session_without_output) {
    PublisherFixture fixture;
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    fixture.packets.clear();
    const std::uint64_t next_before = fixture.publisher.next_sequence();

    // The first event is fine, the second is past the end of the day: the
    // whole batch is withheld.
    const Event batch[] = {rested_event(10, 1, 1, 1'000'000, 5),
                           rested_event(md::kNanosecondsPerTradingDay, 1, 2, 1'000'000, 5)};
    CHECK(fixture.publisher.on_events(batch) == md::PublishStatus::TimestampOutsideDay);
    CHECK(fixture.packets.empty());
    CHECK_EQ(fixture.publisher.next_sequence(), next_before);
    CHECK(fixture.publisher.state() == md::PublisherState::Stopped);
    CHECK(fixture.publisher.stop_reason() == md::PublishStatus::TimestampOutsideDay);

    CHECK(fixture.publisher.on_events(std::span<const Event>(batch, 1)) == md::PublishStatus::NotAllowedInState);
    CHECK(fixture.publisher.poll(md::kNanosecondsPerTradingDay - 1, fixture.engine) ==
          md::PublishStatus::NotAllowedInState);  // no heartbeats from a stopped session
    CHECK(fixture.publisher.end_of_day(20) == md::PublishStatus::NotAllowedInState);
    CHECK(fixture.packets.empty());

    CHECK(fixture.publisher.end_session(30) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 1u);
    const DecodedPacket end = decode_packet(fixture.packets[0]);
    CHECK_EQ(end.count, md::kMoldEndOfSessionCount);
    CHECK_EQ(end.sequence, next_before);
    CHECK(fixture.publisher.state() == md::PublisherState::Ended);
}

TEST_CASE(md_publisher, unregistered_instrument_or_unrepresentable_price_stops_session) {
    {
        PublisherFixture fixture;
        REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
        fixture.packets.clear();
        const Event event = rested_event(10, 3, 1, 1'000'000, 5);
        CHECK(fixture.publisher.on_events(std::span<const Event>(&event, 1)) == md::PublishStatus::UnknownInstrument);
        CHECK(fixture.packets.empty());
        CHECK(fixture.publisher.state() == md::PublisherState::Stopped);
    }
    {
        PublisherFixture fixture;
        REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
        fixture.packets.clear();
        const Event event = rested_event(10, 1, 1, md::kMaxItchPrice + 1, 5);
        CHECK(fixture.publisher.on_events(std::span<const Event>(&event, 1)) ==
              md::PublishStatus::UnrepresentableField);
        CHECK(fixture.packets.empty());
        CHECK(fixture.publisher.stop_reason() == md::PublishStatus::UnrepresentableField);
    }
    {
        // Events that produce no ITCH output are not validated.
        PublisherFixture fixture;
        REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
        Event rejected{};
        rejected.type = EventType::Rejected;
        rejected.instrument = 9;
        rejected.ts = md::kNanosecondsPerTradingDay * 2;
        CHECK(fixture.publisher.on_events(std::span<const Event>(&rejected, 1)) == md::PublishStatus::Ok);
        CHECK(fixture.publisher.state() == md::PublisherState::Live);
    }
}

TEST_CASE(md_publisher, tiny_payload_never_exceeds_limit_and_keeps_sequences_contiguous) {
    md::PublisherConfig config = small_config();
    config.max_payload = md::kMinimumMaxPayload;
    PublisherFixture fixture(config, 3);
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    for (ClOrdId token = 1; token <= 6; ++token)
        REQUIRE(fixture.submit(limit_order(token * 10, 1, 1, token, Side::BUY, 1'000'000, 10)) ==
                md::PublishStatus::Ok);
    REQUIRE(fixture.submit(MassCancel{100, 1, 0, 50, false}) == md::PublishStatus::Ok);
    std::uint64_t expected_sequence = 1;
    for (const Bytes& packet : fixture.packets) {
        CHECK_LE(packet.size(), md::kMinimumMaxPayload);
        const DecodedPacket decoded = decode_packet(packet);
        CHECK_EQ(decoded.sequence, expected_sequence);
        CHECK_EQ(decoded.messages.size(), std::size_t{decoded.count});
        CHECK_GE(decoded.count, 1u);
        expected_sequence += decoded.count;
    }
    CHECK_EQ(expected_sequence, fixture.publisher.next_sequence());
    // 8 startup + 6 A + 6 D.
    CHECK_EQ(fixture.publisher.next_sequence(), 21u);
}

TEST_CASE(md_publisher, mass_cancel_spans_packets_at_exact_capacity) {
    // D is 19 bytes: three fit exactly in 20 + 3 * (2 + 19) = 83 bytes.
    md::PublisherConfig config = small_config();
    config.max_payload = 83;
    PublisherFixture fixture(config);
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    for (ClOrdId token = 1; token <= 7; ++token)
        REQUIRE(fixture.submit(limit_order(token * 10, 1, 1, token, Side::SELL, 1'000'100, 10)) ==
                md::PublishStatus::Ok);
    fixture.packets.clear();
    REQUIRE(fixture.submit(MassCancel{100, 1, 1, 50, false}) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 3u);
    CHECK_EQ(fixture.packets[0].size(), 83u);
    CHECK_EQ(decode_packet(fixture.packets[0]).count, 3u);
    CHECK_EQ(decode_packet(fixture.packets[1]).count, 3u);
    CHECK_EQ(decode_packet(fixture.packets[2]).count, 1u);
    const auto messages = messages_of(fixture.packets);
    CHECK_EQ(message_types(messages), std::string("DDDDDDD"));
    // Deleted in book order (time priority), one per order.
    for (std::size_t index = 0; index < messages.size(); ++index) {
        protocol::itch50::OrderDelete deleted;
        REQUIRE(protocol::itch50::decode(messages[index].bytes, deleted) == protocol::Status::ok);
        CHECK_EQ(deleted.order_reference, OrderId{index + 1});
    }
}

TEST_CASE(md_publisher, each_batch_is_flushed_in_its_own_packet) {
    PublisherFixture fixture;
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    fixture.packets.clear();
    REQUIRE(fixture.submit(limit_order(10, 1, 1, 1, Side::BUY, 1'000'000, 10)) == md::PublishStatus::Ok);
    REQUIRE(fixture.submit(limit_order(20, 1, 1, 2, Side::BUY, 1'000'000, 10)) == md::PublishStatus::Ok);
    CHECK_EQ(fixture.packets.size(), 2u);
    // A batch without ITCH output sends nothing (not even an empty packet).
    REQUIRE(fixture.submit(CancelOrder{30, 1, 99, 3, 0}) == md::PublishStatus::Ok);
    CHECK_EQ(fixture.packets.size(), 2u);
    fixture.publisher.flush();
    CHECK_EQ(fixture.packets.size(), 2u);
    CHECK_EQ(fixture.publisher.stats().packets_sent, 3u);  // startup + 2
}

TEST_CASE(md_publisher, heartbeat_after_silence_does_not_consume_sequences) {
    md::PublisherConfig config = small_config();
    config.heartbeat_interval_ns = 1'000;
    PublisherFixture fixture(config);
    REQUIRE(fixture.publisher.start_session(100) == md::PublishStatus::Ok);
    fixture.packets.clear();
    CHECK(fixture.publisher.poll(1'099, fixture.engine) == md::PublishStatus::Ok);
    CHECK(fixture.packets.empty());
    CHECK(fixture.publisher.poll(1'100, fixture.engine) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 1u);
    DecodedPacket heartbeat = decode_packet(fixture.packets[0]);
    CHECK_EQ(heartbeat.count, 0u);
    CHECK_EQ(heartbeat.sequence, 5u);
    CHECK_EQ(fixture.packets[0].size(), md::kMoldHeaderSize);

    // A data packet restarts the silence timer.
    REQUIRE(fixture.submit(limit_order(1'500, 1, 1, 1, Side::BUY, 1'000'000, 10)) == md::PublishStatus::Ok);
    CHECK(fixture.publisher.poll(2'499, fixture.engine) == md::PublishStatus::Ok);
    CHECK_EQ(fixture.packets.size(), 2u);
    CHECK(fixture.publisher.poll(2'500, fixture.engine) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 3u);
    heartbeat = decode_packet(fixture.packets[2]);
    CHECK_EQ(heartbeat.count, 0u);
    CHECK_EQ(heartbeat.sequence, 6u);
    // A clock that goes backwards never fires the timer.
    CHECK(fixture.publisher.poll(10, fixture.engine) == md::PublishStatus::Ok);
    CHECK_EQ(fixture.packets.size(), 3u);
    CHECK_EQ(fixture.publisher.stats().heartbeats_sent, 2u);
}

TEST_CASE(md_publisher, imbalance_timer_covers_every_call_phase_instrument) {
    md::PublisherConfig config = small_config();
    config.noii_interval_ns = 500;
    config.heartbeat_interval_ns = 1'000'000;
    PublisherFixture fixture(config, 3);
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    fixture.packets.clear();
    // All continuous: the timer fires but produces nothing.
    CHECK(fixture.publisher.poll(500, fixture.engine) == md::PublishStatus::Ok);
    CHECK(fixture.packets.empty());
    REQUIRE(fixture.submit(SetPhase{600, 1, Phase::PreOpen}) == md::PublishStatus::Ok);
    REQUIRE(fixture.submit(SetPhase{600, 3, Phase::PreClose}) == md::PublishStatus::Ok);
    fixture.packets.clear();
    CHECK(fixture.publisher.poll(999, fixture.engine) == md::PublishStatus::Ok);
    CHECK(fixture.packets.empty());
    CHECK(fixture.publisher.poll(1'000, fixture.engine) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 1u);  // both I in one packet
    const auto messages = messages_of(fixture.packets);
    REQUIRE_EQ(message_types(messages), std::string("II"));
    protocol::itch50::NetOrderImbalance first, second;
    REQUIRE(protocol::itch50::decode(messages[0].bytes, first) == protocol::Status::ok);
    REQUIRE(protocol::itch50::decode(messages[1].bytes, second) == protocol::Status::ok);
    CHECK_EQ(first.stock_locate, 1u);
    CHECK_EQ(first.cross_type, 'O');
    CHECK_EQ(second.stock_locate, 3u);
    CHECK_EQ(second.cross_type, 'C');
    CHECK_EQ(fixture.publisher.stats().imbalance_messages_sent, 2u);

    // noii_interval_ns 0 disables the imbalance feed.
    md::PublisherConfig disabled = small_config();
    PublisherFixture quiet(disabled);
    REQUIRE(quiet.publisher.start_session(0) == md::PublishStatus::Ok);
    REQUIRE(quiet.submit(SetPhase{10, 1, Phase::PreOpen}) == md::PublishStatus::Ok);
    quiet.packets.clear();
    CHECK(quiet.publisher.publish_imbalances(5'000'000'000, quiet.engine) == md::PublishStatus::Ok);
    CHECK(quiet.packets.empty());
}

TEST_CASE(md_publisher, ring_holds_exactly_the_published_bytes) {
    md::PublisherConfig config = small_config();
    config.retransmit_buffer_messages = 8;
    PublisherFixture fixture(config);
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    for (ClOrdId token = 1; token <= 10; ++token)
        REQUIRE(fixture.submit(limit_order(token, 1, 1, token, Side::BUY, 1'000'000, 10)) == md::PublishStatus::Ok);
    const auto messages = messages_of(fixture.packets);
    REQUIRE_EQ(messages.size(), 14u);
    const md::RetransmissionRing& ring = fixture.publisher.ring();
    CHECK_EQ(ring.newest_sequence(), 14u);
    CHECK_EQ(ring.oldest_sequence(), 7u);
    CHECK(ring.find(6).empty());
    for (const SequencedMessage& message : messages) {
        if (message.sequence < 7) continue;
        const auto held = ring.find(message.sequence);
        CHECK_EQ(hex_of(held), hex_of(message.bytes));
    }
}

TEST_CASE(md_publisher, end_of_day_ends_session_and_repeats_end_of_session) {
    md::PublisherConfig config = small_config();
    config.heartbeat_interval_ns = 100;
    PublisherFixture fixture(config);
    REQUIRE(fixture.publisher.start_session(0) == md::PublishStatus::Ok);
    fixture.packets.clear();
    REQUIRE(fixture.publisher.end_of_day(50) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 2u);
    CHECK_EQ(message_types(messages_of({fixture.packets[0]})), std::string("SS"));
    const DecodedPacket end = decode_packet(fixture.packets[1]);
    CHECK_EQ(end.count, md::kMoldEndOfSessionCount);
    CHECK_EQ(end.sequence, 7u);
    CHECK(fixture.publisher.state() == md::PublisherState::Ended);
    CHECK(fixture.publisher.end_of_day(60) == md::PublishStatus::NotAllowedInState);
    CHECK(fixture.publisher.end_session(60) == md::PublishStatus::NotAllowedInState);
    CHECK(fixture.publisher.poll(149, fixture.engine) == md::PublishStatus::Ok);
    CHECK_EQ(fixture.packets.size(), 2u);
    CHECK(fixture.publisher.poll(150, fixture.engine) == md::PublishStatus::Ok);
    REQUIRE_EQ(fixture.packets.size(), 3u);
    CHECK(fixture.packets[2] == fixture.packets[1]);
    CHECK_EQ(fixture.publisher.stats().end_of_session_packets_sent, 2u);
}
