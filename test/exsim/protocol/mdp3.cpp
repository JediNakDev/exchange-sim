#include "protocol/mdp3.hpp"
#include "test/framework/test.hpp"

#include <vector>

namespace mdp = exsim::protocol::mdp3;
namespace cme = exsim::protocol::cme;
using exsim::protocol::Status;

TEST_CASE(proto_mdp3, book46_schema13_golden) {
    mdp::BookEntry entry;
    entry.price = 100000000000ll;
    entry.quantity = 10;
    entry.security_id = 894923;
    entry.report_sequence = 1;
    entry.order_count = 2;
    entry.level = 1;
    const std::array entries{entry};
    std::array<char, 64> bytes{};
    REQUIRE_EQ(mdp::encode_book(1, mdp::kEndOfEvent, entries, bytes), bytes.size());
    // Packed from the pinned XML offsets, including root alignment padding,
    // optional TradeableSize and the empty eight-byte order group header.
    constexpr unsigned char expected[] = {
        0x40,0, 0x0b,0, 0x2e,0, 1,0, 0x0d,0,
        1,0,0,0,0,0,0,0, 0x80,0,0, 0x20,0,1,
        0,0xe8,0x76,0x48,0x17,0,0,0, 0x0a,0,0,0,
        0xcb,0xa7,0x0d,0, 1,0,0,0, 2,0,0,0,
        1,0,0x30,0xff,0xff,0xff,0x7f,0,
        0x18,0,0,0,0,0,0,0};
    static_assert(sizeof expected == bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        CHECK_EQ(static_cast<unsigned char>(bytes[i]), expected[i]);
    cme::MessageView frame;
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    mdp::IncrementalView message;
    REQUIRE_EQ(mdp::decode_incremental(frame, message), Status::ok);
    CHECK_EQ(message.transact_time, 1u);
    CHECK_EQ(message.event_indicator, mdp::kEndOfEvent);
    CHECK_EQ(message.entries.count, 1);
    CHECK_EQ(message.orders.count, 0);
    CHECK_EQ(message.orders.block_length, 24);
}

TEST_CASE(proto_mdp3, trade48_order_id_group_and_nulls) {
    const std::array entries{mdp::TradeEntry{2000000000, 7, 9001, 22, 2, 1, mdp::UpdateAction::New, 111}};
    const std::array orders{mdp::TradeOrder{51, 7}, mdp::TradeOrder{52, 7}};
    std::array<char, 96> bytes{};
    REQUIRE_EQ(mdp::encode_trades(123456, mdp::kEndOfEvent | mdp::kLastTrade, entries, orders, bytes), bytes.size());
    cme::MessageView frame;
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    CHECK_EQ(frame.header.template_id, 48);
    mdp::IncrementalView message;
    REQUIRE_EQ(mdp::decode_incremental(frame, message), Status::ok);
    CHECK_EQ(message.orders.encoded_size, 40u);
    CHECK_EQ(message.orders.count, 2);
    CHECK_EQ(cme::load_le<std::uint64_t>(message.orders.entry(0).data()), 51u);
    CHECK_EQ(cme::load_le<std::uint64_t>(message.orders.entry(1).data()), 52u);
    CHECK_EQ(cme::load_le<std::int32_t>(message.orders.entry(1).data() + 8), 7);
    CHECK_EQ(cme::load_le<std::uint32_t>(message.entries.entry(0).data() + 26), 111u);
    // Constant MDEntryType='2' has no wire byte in template 48.
    CHECK_EQ(bytes[24 + 30], 0);
    // A count at byte 2 is padding in groupSize8Byte, not numInGroup.
    bytes[58] = 50;
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    REQUIRE_EQ(mdp::decode_incremental(frame, message), Status::ok);
    CHECK_EQ(message.orders.count, 2);
    bytes[63] = 3;
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    CHECK_EQ(mdp::decode_incremental(frame, message), Status::invalid_length);
}

TEST_CASE(proto_mdp3, snapshot52_sequence_boundary_and_empty_book) {
    mdp::Snapshot snapshot;
    snapshot.last_packet_sequence = 123;
    snapshot.total_reports = 2;
    snapshot.security_id = 98765;
    snapshot.report_sequence = 19;
    snapshot.transact_time = 500;
    snapshot.last_update_time = 250;
    snapshot.trade_date = 20000;
    std::array<char, 94> bytes{};
    REQUIRE_EQ(mdp::encode_snapshot(snapshot, {}, bytes), 72u);
    cme::MessageView frame;
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    mdp::Snapshot decoded;
    cme::GroupView group;
    REQUIRE_EQ(mdp::decode_snapshot(frame, decoded, group), Status::ok);
    CHECK_EQ(decoded.last_packet_sequence, 123u);
    CHECK_EQ(decoded.report_sequence, 19u);
    CHECK_EQ(decoded.security_id, 98765);
    CHECK_EQ(decoded.high_limit, cme::kNullPrice9);
    CHECK_EQ(decoded.low_limit, cme::kNullPrice9);
    CHECK_EQ(group.count, 0);
    const std::array levels{mdp::SnapshotEntry{100000000, 5, 1, 1, UINT16_MAX, UINT8_MAX, 0, '1'}};
    REQUIRE_EQ(mdp::encode_snapshot(snapshot, levels, bytes), bytes.size());
    REQUIRE_EQ(cme::decode_mdp_message(bytes, frame), Status::ok);
    REQUIRE_EQ(mdp::decode_snapshot(frame, decoded, group), Status::ok);
    CHECK_EQ(group.count, 1);
    CHECK_EQ(group.entry(0)[21], '1');
    CHECK_EQ(cme::load_le<std::uint16_t>(group.entry(0).data() + 17), UINT16_MAX);
}

TEST_CASE(proto_mdp3, decoders_reject_unpinned_schemas_and_missing_groups) {
    std::array<char, 128> bytes{};
    const auto size = mdp::encode_book(1, 0, {}, bytes);
    cme::MessageView frame;
    REQUIRE_EQ(cme::decode_mdp_message(std::span(bytes).first(size), frame), Status::ok);
    mdp::IncrementalView message;
    REQUIRE_EQ(mdp::decode_incremental(frame, message), Status::ok);
    frame.header.version = 12;
    CHECK_EQ(mdp::decode_incremental(frame, message), Status::invalid_type);
    frame.header.version = 13;
    frame.header.schema_id = 8;
    CHECK_EQ(mdp::decode_incremental(frame, message), Status::invalid_type);
    frame.header.schema_id = 1;
    frame.body = frame.body.first(frame.body.size() - 1);
    CHECK_EQ(mdp::decode_incremental(frame, message), Status::invalid_length);
    frame.header.block_length = 10;
    CHECK_EQ(mdp::decode_incremental(frame, message), Status::invalid_length);
}

TEST_CASE(proto_mdp3, bounded_group_counts_and_no_writes_on_failure) {
    std::vector<mdp::BookEntry> entries(256);
    std::vector<char> output(9000, 'x');
    CHECK_EQ(mdp::encode_book(1, 0, entries, output), 0u);
    CHECK_EQ(output[0], 'x');
    entries.resize(255);
    REQUIRE_EQ(mdp::encode_book(1, 0, entries, output), 8192u);
    output.assign(output.size(), 'x');
    CHECK_EQ(mdp::encode_book(1, 0, entries, std::span(output).first(8191)), 0u);
    CHECK_EQ(output[0], 'x');
    CHECK_EQ(output[8191], 'x');
    std::array<mdp::TradeOrder, 256> orders{};
    CHECK_EQ(mdp::encode_trades(1, 0, {}, orders, output), 0u);
    CHECK_EQ(output[0], 'x');
    std::array<mdp::SnapshotEntry, 256> levels{};
    CHECK_EQ(mdp::encode_snapshot({}, levels, output), 0u);
    CHECK_EQ(output[0], 'x');
    std::array<char, 10> heartbeat{};
    REQUIRE_EQ(mdp::encode_heartbeat(heartbeat), 10u);
    cme::MessageView frame;
    REQUIRE_EQ(cme::decode_mdp_message(heartbeat, frame), Status::ok);
    CHECK_EQ(frame.header.template_id, 12);
    CHECK(frame.body.empty());
}
