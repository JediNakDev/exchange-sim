#include "../../framework/test.hpp"
#include <exsim/protocol/moldudp64.hpp>
#include <exsim/protocol/soupbintcp.hpp>
#include <random>
using exsim::protocol::Status;
namespace soup = exsim::protocol::soupbintcp;
namespace mold = exsim::protocol::moldudp64;
namespace wire = exsim::protocol::wire;
TEST_CASE(proto_wire, extremes) {
  char b[32];
  wire::store64(b, UINT64_MAX);
  CHECK_EQ(wire::load64(b), UINT64_MAX);
  wire::store48(b, 0x123456789abcULL);
  CHECK_EQ(static_cast<unsigned char>(b[0]), 0x12);
  CHECK_EQ(wire::load48(b), 0x123456789abcULL);
  wire::store32(b, 0x12345678);
  CHECK_EQ(wire::load32(b), 0x12345678U);
  wire::store16(b, 0xabcd);
  CHECK_EQ(wire::load16(b), 0xabcd);
  CHECK(wire::alpha(b, 8, "ABC"));
  CHECK_EQ(std::string_view(b, 8), "ABC     ");
  CHECK_EQ(wire::alpha(b, 8), "ABC");
  CHECK(!wire::alpha(b, 2, "ABC"));
  CHECK(wire::numeric(b, 20, UINT64_MAX));
  std::uint64_t v{};
  CHECK(wire::numeric({b, 20}, v));
  CHECK_EQ(v, UINT64_MAX);
  CHECK(!wire::numeric("18446744073709551616", v));
  CHECK(!wire::numeric("12X", v));
  CHECK(!wire::numeric(b, 1, 10));
}
TEST_CASE(proto_soup, every_packet_and_chunks) {
  std::mt19937 rng(42);
  for (char type : std::string_view("US+ROHZJ")) {
    for (unsigned run = 0; run < 100; ++run) {
      char wire_bytes[256], scratch[256];
      std::string_view body = (type == 'U' || type == 'S' || type == '+')
                                  ? "test payload"
                              : type == 'J' ? "A"
                                            : "";
      auto n = soup::encode({type, body}, wire_bytes, sizeof wire_bytes);
      REQUIRE(n > 0);
      CHECK_EQ(static_cast<unsigned char>(wire_bytes[0]), 0U);
      CHECK_EQ(static_cast<unsigned char>(wire_bytes[1]), body.size() + 1);
      CHECK_EQ(wire_bytes[2], type);
      soup::Packet p;
      CHECK_EQ(soup::decode(wire_bytes, n, p), Status::ok);
      CHECK_EQ(p.payload, body);
      for (std::size_t i = 0; i < n; ++i)
        CHECK_EQ(soup::decode(wire_bytes, i, p), Status::incomplete);
      CHECK_EQ(soup::decode(wire_bytes, n + 1, p), Status::invalid_length);
      soup::FrameReader reader(scratch);
      std::size_t pos = 0;
      while (pos < n) {
        auto chunk = std::min(n - pos, run == 0 ? std::size_t(1)
                                                : std::size_t(rng() % 9 + 1));
        std::size_t consumed;
        auto s = reader.feed({wire_bytes + pos, chunk}, consumed, p);
        CHECK_EQ(consumed, chunk);
        pos += consumed;
        CHECK_EQ(s, pos == n ? Status::ok : Status::incomplete);
      }
      CHECK_EQ(p.type, type);
      CHECK_EQ(p.payload, body);
    }
  }
}
TEST_CASE(proto_soup, login_and_guards) {
  char b[64];
  soup::LoginRequest m, d;
  wire::alpha(m.username.data(), 6, "USER");
  wire::alpha(m.password.data(), 10, "PASS");
  wire::alpha(m.session.data(), 10, "SESSION");
  m.sequence = UINT64_MAX;
  CHECK_EQ(soup::encode(m, b, sizeof b), 49U);
  CHECK_EQ(std::string_view(b + 3, 6), "USER  ");
  CHECK_EQ(std::string_view(b + 9, 10), "PASS      ");
  CHECK_EQ(std::string_view(b + 19, 10), "SESSION   ");
  CHECK_EQ(std::string_view(b + 29, 20), "18446744073709551615");
  CHECK_EQ(soup::decode(b, 49, d), Status::ok);
  CHECK_EQ(d.sequence, m.sequence);
  b[48] = 'X';
  CHECK_EQ(soup::decode(b, 49, d), Status::invalid_field);
  soup::LoginAccepted a, e;
  a.sequence = 1;
  CHECK_EQ(soup::encode(a, b, sizeof b), 33U);
  CHECK_EQ(std::string_view(b + 13, 20), "                   1");
  CHECK_EQ(soup::decode(b, 33, e), Status::ok);
  soup::FrameReader reader({b, 64}, 10);
  char over[] = {0, 11};
  std::size_t consumed;
  soup::Packet pkt;
  CHECK_EQ(reader.feed(over, consumed, pkt), Status::overflow);
  reader.reset();
  char zero[] = {0, 0};
  CHECK_EQ(reader.feed(zero, consumed, pkt), Status::invalid_length);
}
TEST_CASE(proto_mold, randomized_packets) {
  std::mt19937 rng(7);
  char b[1400];
  char message[100];
  for (unsigned run = 0; run < 200; ++run) {
    mold::PacketBuilder builder(b);
    auto seq = std::uint64_t(rng()) << 32 | rng();
    REQUIRE(builder.reset("SESSION", seq));
    unsigned count = rng() % 10;
    std::array<std::size_t, 10> lengths{};
    for (unsigned i = 0; i < count; ++i) {
      lengths[i] = rng() % 100 + 1;
      std::fill_n(message, lengths[i], static_cast<char>('A' + i));
      CHECK_EQ(builder.append({message, lengths[i]}), Status::ok);
    }
    auto packet = builder.packet();
    CHECK_EQ(std::string_view(packet.data(), 10), "SESSION   ");
    CHECK_EQ(wire::load64(packet.data() + 10), seq);
    CHECK_EQ(wire::load16(packet.data() + 18), count);
    mold::PacketReader reader;
    REQUIRE_EQ(reader.reset(packet), Status::ok);
    std::span<const char> msg;
    for (unsigned i = 0; i < count; ++i) {
      CHECK_EQ(reader.next(msg), Status::ok);
      CHECK_EQ(msg.size(), lengths[i]);
      for (char c : msg)
        CHECK_EQ(c, static_cast<char>('A' + i));
    }
    CHECK_EQ(reader.next(msg), Status::end);
    for (std::size_t i = 0; i < packet.size(); ++i)
      CHECK_EQ(reader.reset(packet.first(i)), Status::incomplete);
  }
  mold::PacketBuilder builder(b, 24);
  CHECK(builder.reset("S", 1));
  CHECK_EQ(builder.append(std::span<const char>("ab", 2)), Status::ok);
  CHECK_EQ(builder.append(std::span<const char>("c", 1)), Status::full);
  CHECK(builder.reset("S", 1, true));
  mold::PacketReader reader;
  CHECK_EQ(reader.reset(builder.packet()), Status::ok);
  CHECK_EQ(reader.header.count, 65535);
  mold::Request r, d;
  r.sequence = UINT64_MAX;
  r.count = 65535;
  CHECK_EQ(mold::encode(r, b, sizeof b), 20U);
  CHECK_EQ(mold::decode(b, 20, d), Status::ok);
  CHECK_EQ(d.sequence, r.sequence);
  CHECK_EQ(d.count, r.count);
  CHECK_EQ(mold::decode(b, 19, d), Status::incomplete);
  CHECK_EQ(mold::decode(b, 21, d), Status::invalid_length);
}
TEST_CASE(proto_soup, typed_and_pipeline) {
  char b[128], scratch[128];
  soup::ClientHeartbeat hb, decoded;
  CHECK_EQ(soup::encode(hb, b, sizeof b), 3U);
  CHECK_EQ(soup::decode(b, 3, decoded), Status::ok);
  soup::LoginRejected rejected{'S'}, rejected_decoded;
  CHECK_EQ(soup::encode(rejected, b + 3, sizeof(b) - 3), 4U);
  CHECK_EQ(soup::decode(b + 3, 4, rejected_decoded), Status::ok);
  CHECK_EQ(rejected_decoded.reason, 'S');
  soup::FrameReader reader(scratch);
  soup::Packet packet;
  std::size_t consumed;
  CHECK_EQ(reader.feed({b, 7}, consumed, packet), Status::ok);
  CHECK_EQ(consumed, 3U);
  CHECK_EQ(packet.type, 'R');
  CHECK_EQ(reader.feed({b + 3, 4}, consumed, packet), Status::ok);
  CHECK_EQ(consumed, 4U);
  CHECK_EQ(packet.type, 'J');
  rejected.reason = '?';
  CHECK_EQ(soup::encode(rejected, b, sizeof b), 0U);
  char tiny[2];
  soup::FrameReader small(tiny);
  const char too_large[] = {0, 2};
  CHECK_EQ(small.feed(too_large, consumed, packet), Status::full);
}
TEST_CASE(proto_mold, malformed) {
  char b[32]{};
  mold::PacketReader reader;
  wire::store16(b + 18, 1);
  wire::store16(b + 20, 0);
  CHECK_EQ(reader.reset({b, 22}), Status::invalid_length);
  wire::store16(b + 20, 65535);
  CHECK_EQ(reader.reset({b, 22}), Status::incomplete);
  wire::store16(b + 18, 0);
  CHECK_EQ(reader.reset({b, 21}), Status::invalid_length);
  mold::PacketBuilder builder(b);
  CHECK(builder.reset("SESSION", 0));
  CHECK_EQ(builder.append({}), Status::invalid_length);
  CHECK(!builder.reset("TOO LONG SESSION", 0));
}
