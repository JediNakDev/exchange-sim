#include "test/framework/test.hpp"
#include "protocol/cme_sbe.hpp"

#include <array>
#include <string>
#include <vector>

using namespace exsim::protocol;
namespace cme = exsim::protocol::cme;

namespace {
// CME's MDP 3.0 SBE Decoding Example, schema 1 version 9, template 50.
// This fixture comes from the published packet, not our encoder.
// https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457638169
std::vector<char> limits_packet() {
    constexpr unsigned char bytes[] = {
        0xa6, 0xbb, 0x0a, 0x00, 0x5b, 0x19, 0x01, 0x72, 0x1e, 0xef, 0xa9, 0x16,
        0x38, 0x00, 0x0b, 0x00, 0x32, 0x00, 0x01, 0x00, 0x09, 0x00,
        0x4b, 0x52, 0xe8, 0x71, 0x1e, 0xef, 0xa9, 0x16, 0x00, 0x00, 0x00,
        0x20, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
        0x00, 0x90, 0xcd, 0x79, 0x2f, 0x08, 0x00, 0x00,
        0x00, 0xe4, 0x0b, 0x54, 0x02, 0x00, 0x00, 0x00,
        0xf4, 0x15, 0x00, 0x00, 0x4d, 0x07, 0x00, 0x00};
    return {std::begin(bytes), std::end(bytes)};
}
}

TEST_CASE(proto_cme_sbe, little_endian_signed_and_unsigned) {
    constexpr char bytes[] = {char(0x08), char(0x07), char(0x06), char(0x05),
                              char(0x04), char(0x03), char(0x02), char(0x81)};
    CHECK_EQ(cme::load_le<std::uint16_t>(bytes), 0x0708);
    CHECK_EQ(cme::load_le<std::uint32_t>(bytes), 0x05060708u);
    CHECK_EQ(cme::load_le<std::uint64_t>(bytes), 0x8102030405060708ull);
    std::array<char, 8> output{};
    cme::store_le(output.data(), cme::load_le<std::int64_t>(bytes));
    CHECK_EQ(std::string_view(output.data(), 8), std::string_view(bytes, 8));
    cme::store_le(output.data(), INT64_MIN);
    CHECK_EQ(cme::load_le<std::int64_t>(output.data()), INT64_MIN);
    cme::store_le(output.data(), std::int32_t{-1});
    CHECK_EQ(cme::load_le<std::uint32_t>(output.data()), UINT32_MAX);
}

TEST_CASE(proto_cme_sbe, ilink_framing_has_four_byte_sofh) {
    constexpr char bytes[] = {0x10, 0x00, char(0xfe), char(0xca),
                              0x02, 0x00, char(0xf4), 0x01, 0x08, 0x00, 0x09, 0x00,
                              0x11, 0x22, 0x33, 0x44};
    cme::MessageView message;
    REQUIRE_EQ(cme::decode_ilink(bytes, message), Status::ok);
    CHECK_EQ(message.frame_size, sizeof bytes);
    CHECK_EQ(message.header.block_length, 2);
    CHECK_EQ(message.header.template_id, 500);
    CHECK_EQ(message.header.schema_id, 8);
    CHECK_EQ(message.header.version, 9);
    CHECK_EQ(message.root().size(), 2u);
    CHECK_EQ(message.trailing().size(), 2u);
    std::array<char, sizeof bytes> output{};
    REQUIRE_EQ(cme::encode_ilink(message.header, message.body, output), sizeof bytes);
    CHECK_EQ(std::string_view(output.data(), output.size()), std::string_view(bytes, sizeof bytes));
}

TEST_CASE(proto_cme_sbe, ilink_partial_and_coalesced_frames) {
    std::array<char, 64> bytes{};
    const std::array<char, 3> body{1, 2, 3};
    const auto size = cme::encode_ilink({3, 500, 8, 9}, body, bytes);
    REQUIRE_EQ(size, 15u);
    cme::MessageView message;
    for (std::size_t length = 0; length < size; ++length) {
        message.frame_size = 123;
        CHECK_EQ(cme::decode_ilink(std::span(bytes).first(length), message), Status::incomplete);
        CHECK_EQ(message.frame_size, 123u);
    }
    REQUIRE_EQ(cme::encode_ilink({3, 503, 8, 9}, body, std::span(bytes).subspan(size)), size);
    REQUIRE_EQ(cme::decode_ilink(bytes, message), Status::ok);
    CHECK_EQ(message.header.template_id, 500);
    REQUIRE_EQ(cme::decode_ilink(std::span(bytes).subspan(message.frame_size), message), Status::ok);
    CHECK_EQ(message.header.template_id, 503);
    bytes[2] = 0;
    CHECK_EQ(cme::decode_ilink(bytes, message), Status::invalid_type);
    cme::store_le(bytes.data(), std::uint16_t{11});
    CHECK_EQ(cme::decode_ilink(bytes, message), Status::invalid_length);
    cme::store_le(bytes.data(), std::uint16_t{12});
    cme::store_le(bytes.data() + 2, cme::kEncoding);
    cme::store_le(bytes.data() + 4, std::uint16_t{1});
    CHECK_EQ(cme::decode_ilink(bytes, message), Status::invalid_length);
}

TEST_CASE(proto_cme_sbe, published_mdp_packet_and_group_padding) {
    const auto bytes = limits_packet();
    cme::MdpPacketView packet;
    REQUIRE_EQ(cme::decode_mdp_packet(bytes, packet), Status::ok);
    CHECK_EQ(packet.sequence, 703398u);
    CHECK_EQ(packet.sending_time, 1633099253939247451ull);
    cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), Status::ok);
    CHECK_EQ(message.header.template_id, 50);
    CHECK_EQ(message.header.schema_id, 1);
    CHECK_EQ(message.header.version, 9);
    CHECK_EQ(message.header.block_length, 11);
    CHECK_EQ(cme::load_le<std::uint64_t>(message.root().data()), 1633099253937623627ull);
    cme::GroupView group;
    REQUIRE_EQ(cme::decode_group(message.trailing(), 32, group), Status::ok);
    REQUIRE_EQ(group.count, 1);
    REQUIRE_EQ(group.block_length, 32);
    const auto entry = group.entry(0);
    CHECK_EQ(cme::load_le<std::int64_t>(entry.data()), cme::kNullPrice9);
    CHECK_EQ(cme::load_le<std::int64_t>(entry.data() + 8), 9000000000000ll);
    CHECK_EQ(cme::load_le<std::int32_t>(entry.data() + 24), 5620);
    CHECK_EQ(cme::load_le<std::uint32_t>(entry.data() + 28), 1869u);
    CHECK(group.entry(1).empty());
    CHECK_EQ(group.encoded_size, message.trailing().size());
    std::vector<char> encoded(bytes.size());
    REQUIRE_EQ(cme::encode_mdp_packet(packet.sequence, packet.sending_time, packet.messages, encoded), bytes.size());
    CHECK_EQ(encoded, bytes);
    std::vector<char> encoded_message(message.frame_size);
    REQUIRE_EQ(cme::encode_mdp_message(message.header, message.body, encoded_message), message.frame_size);
    CHECK_EQ(std::string_view(encoded_message.data(), encoded_message.size()),
             std::string_view(packet.messages.data(), packet.messages.size()));
}

TEST_CASE(proto_cme_sbe, mdp_rejects_truncated_and_corrupt_packets_atomically) {
    auto bytes = limits_packet();
    cme::MdpPacketView packet;
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        packet.sequence = 99;
        CHECK_EQ(cme::decode_mdp_packet(std::span(bytes).first(length), packet), Status::invalid_length);
        CHECK_EQ(packet.sequence, 99u);
    }
    // Valid first message followed by a corrupt second message.
    bytes.push_back(0);
    bytes.push_back(0);
    CHECK_EQ(cme::decode_mdp_packet(bytes, packet), Status::invalid_length);
    bytes.resize(bytes.size() - 2);
    cme::store_le(bytes.data() + 14, std::uint16_t{55});
    CHECK_EQ(cme::decode_mdp_packet(bytes, packet), Status::invalid_length);
    cme::MessageView message;
    CHECK_EQ(cme::decode_mdp_message(std::span(bytes).subspan(12), message), Status::invalid_length);
    std::vector<char> oversized(65508);
    CHECK_EQ(cme::decode_mdp_packet(oversized, packet), Status::invalid_length);
}

TEST_CASE(proto_cme_sbe, mdp_multiple_messages) {
    auto bytes = limits_packet();
    const std::vector<char> second(bytes.begin() + 12, bytes.end());
    bytes.insert(bytes.end(), second.begin(), second.end());
    cme::MdpPacketView packet;
    REQUIRE_EQ(cme::decode_mdp_packet(bytes, packet), Status::ok);
    auto remaining = packet.messages;
    unsigned count = 0;
    while (!remaining.empty()) {
        cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_message(remaining, message), Status::ok);
        remaining = remaining.subspan(message.frame_size);
        ++count;
    }
    CHECK_EQ(count, 2u);
}

TEST_CASE(proto_cme_sbe, group_checks_lengths_and_uses_acting_stride) {
    std::array<char, 15> bytes{};
    cme::store_le(bytes.data(), std::uint16_t{6});
    bytes[2] = 2;
    bytes[3] = 11;
    bytes[9] = 22;
    cme::GroupView group;
    REQUIRE_EQ(cme::decode_group(bytes, 4, group), Status::ok);
    CHECK_EQ(group.entry(1).front(), 22);
    CHECK_EQ(group.entry(1).size(), 6u);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK_EQ(cme::decode_group(std::span(bytes).first(length), 4, group), Status::incomplete);
    CHECK_EQ(cme::decode_group(bytes, 7, group), Status::invalid_length);
    bytes[2] = 0;
    REQUIRE_EQ(cme::decode_group(bytes, 4, group), Status::ok);
    CHECK(group.entries.empty());
    CHECK_EQ(group.encoded_size, 3u);
}

TEST_CASE(proto_cme_sbe, encoders_do_not_write_on_failure) {
    std::array<char, 20> output;
    output.fill('x');
    const auto original = output;
    std::array<char, 21> body{};
    CHECK_EQ(cme::encode_ilink({21, 1, 1, 1}, body, output), 0u);
    CHECK_EQ(cme::encode_mdp_message({21, 1, 1, 1}, body, output), 0u);
    CHECK_EQ(cme::encode_ilink({1, 1, 1, 1}, {}, output), 0u);
    CHECK_EQ(cme::encode_mdp_message({1, 1, 1, 1}, {}, output), 0u);
    CHECK_EQ(cme::encode_mdp_packet(1, 1, body, output), 0u);
    CHECK_EQ(output, original);
    std::vector<char> oversized(UINT16_MAX);
    std::vector<char> large_output(UINT16_MAX + 12, 'x');
    CHECK_EQ(cme::encode_ilink({}, oversized, large_output), 0u);
    CHECK_EQ(cme::encode_mdp_message({}, oversized, large_output), 0u);
    CHECK_EQ(large_output.front(), 'x');
    CHECK_EQ(large_output.back(), 'x');
}

TEST_CASE(proto_cme_sbe, price9_exact_conversion_and_boundaries) {
    exsim::Price price = 123;
    CHECK(!cme::price_from_price9(cme::kNullPrice9, price));
    CHECK_EQ(price, 123);
    CHECK(!cme::price_from_price9(100001, price));
    CHECK(!cme::price_from_price9(-100001, price));
    REQUIRE(cme::price_from_price9(-1250000000, price));
    CHECK_EQ(price, -12500);
    std::int64_t mantissa = 321;
    CHECK(!cme::price_to_price9(INT64_MAX, mantissa));
    CHECK(!cme::price_to_price9(INT64_MIN, mantissa));
    CHECK_EQ(mantissa, 321);
    for (const auto value : {INT64_MIN / 100000, INT64_MAX / 100000, std::int64_t{0}, std::int64_t{-1}}) {
        REQUIRE(cme::price_to_price9(value, mantissa));
        REQUIRE(cme::price_from_price9(mantissa, price));
        CHECK_EQ(price, value);
    }
    CHECK(!cme::price_to_price9(INT64_MAX / 100000 + 1, mantissa));
    CHECK(!cme::price_to_price9(INT64_MIN / 100000 - 1, mantissa));
}
