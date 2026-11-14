#include "../../framework/test.hpp"
#include <protocol/fix44.hpp>
#include <random>
using namespace exsim::protocol::fix44;
TEST_CASE(proto_fix, builder_rejects_zero_tag) {
    char buffer[64];
    Builder builder(buffer);
    CHECK(!builder.field(0, "invalid"));
    CHECK_EQ(builder.finish(), 0u);
}

TEST_CASE(proto_fix, decimal_boundaries) {
    exsim::Price p{};
    CHECK(parse_price("0.0001", p));
    CHECK_EQ(p, 1);
    CHECK(parse_price("-1.5", p));
    CHECK_EQ(p, -15000);
    CHECK(!parse_price("1.23456", p));
    CHECK(!parse_price("922337203685477.5808", p));
    CHECK(!parse_price("-922337203685477.5809", p));
    CHECK(parse_price("922337203685477.5807", p));
    CHECK_EQ(p, INT64_MAX);
    CHECK(parse_price("-922337203685477.5808", p));
    CHECK_EQ(p, INT64_MIN);
    for (auto bad : {"", ".", "1.", "+1", "--1", "1.2.3", " 1", "1e2"}) CHECK(!parse_price(bad, p));
    char b[64];
    CHECK_EQ(std::string_view(b, exsim::format_price(1502500, b, sizeof b, 0)), "150.25");
    CHECK_EQ(std::string_view(b, exsim::format_price(0, b, sizeof b, 0)), "0");
    std::mt19937_64 random(42);
    for (unsigned i = 0; i < 10000; ++i) {
        auto value = static_cast<exsim::Price>(random());
        auto n = exsim::format_price(value, b, sizeof b, 0);
        REQUIRE(parse_price({b, n}, p));
        CHECK_EQ(value, p);
    }
}
TEST_CASE(proto_fix, timestamps) {
    char b[21];
    std::uint64_t ns;
    CHECK_EQ(format_timestamp(0, b, sizeof b), 21U);
    CHECK_EQ(std::string_view(b, 21), "19700101-00:00:00.000");
    CHECK(parse_timestamp("20000229-23:59:59.999", ns));
    CHECK_EQ(format_timestamp(ns, b, sizeof b), 21U);
    CHECK_EQ(std::string_view(b, 21), "20000229-23:59:59.999");
    CHECK(!parse_timestamp("21000229-00:00:00.000", ns));
    CHECK(!parse_timestamp("20000101-24:00:00.000", ns));
    CHECK(!parse_timestamp("99991231-23:59:59.999", ns));
    CHECK(parse_timestamp("19700101-23:59:59", ns));
    CHECK_EQ(ns, 86399000000000ULL);
}
TEST_CASE(proto_fix, malformed_and_streams) {
    char b[1024], scratch[1024];
    Header h{"SENDER", "TARGET", 42, 0};
    Parser<> p;
    Builder builder(b);
    builder.header('0', h);
    builder.field(112, "PING");
    auto n = builder.finish();
    REQUIRE(n > 0);
    REQUIRE_EQ(p.parse(b, n), Status::ok);
    CHECK_EQ(p.get(8), "FIX.4.4");
    CHECK_EQ(p.get(35), "0");
    CHECK_EQ(p.get(112), "PING");
    CHECK_EQ(std::string_view(b, 10), std::string_view("8=FIX.4.4\1", 10));
    std::size_t body_start = 12;
    while (b[body_start] != soh) ++body_start;
    ++body_start;
    std::uint64_t body;
    REQUIRE(p.get_uint(9, body));
    CHECK_EQ(body, n - body_start - 7);
    unsigned sum = 0;
    for (std::size_t i = 0; i < n - 7; ++i) sum += static_cast<unsigned char>(b[i]);
    std::uint64_t check;
    CHECK(p.get_uint(10, check));
    CHECK_EQ(check, sum % 256);
    std::mt19937 rng(12);
    for (unsigned run = 0; run < 100; ++run) {
        FrameReader reader(scratch);
        std::size_t pos = 0;
        std::span<const char> msg;
        while (pos < n) {
            auto chunk = std::min(n - pos, run == 0 ? std::size_t(1) : std::size_t(rng() % 16 + 1));
            std::size_t consumed;
            auto status = reader.feed({b + pos, chunk}, consumed, msg);
            pos += consumed;
            CHECK_EQ(status, pos == n ? Status::ok : Status::incomplete);
        }
        CHECK_EQ(std::string_view(msg.data(), msg.size()), std::string_view(b, n));
    }
    b[n - 2] = b[n - 2] == '0' ? '1' : '0';
    CHECK_EQ(p.parse(b, n), Status::checksum);
    b[n - 2] = b[n - 2] == '0' ? '1' : '0';
    Builder duplicate(b);
    duplicate.header('0', h);
    duplicate.field(112, "A");
    duplicate.field(112, "B");
    n = duplicate.finish();
    CHECK_EQ(p.parse(b, n), Status::duplicate_field);
    Builder missing(b);
    missing.header('1', h);
    n = missing.finish();
    TestRequest test;
    CHECK_EQ(decode(b, n, test), Status::missing_field);
    Builder early(b);
    early.header('0', h);
    early.field(10, "000");
    n = early.finish();
    CHECK_EQ(p.parse(b, n), Status::invalid_length);
    Builder valid(b);
    valid.header('0', h);
    n = valid.finish();
    b[n] = 'X';
    CHECK_EQ(p.parse(b, n + 1), Status::invalid_length);
    b[12] = '9';
    CHECK_NE(p.parse(b, n), Status::ok);
    Parser<2> small;
    Builder again(b);
    again.header('0', h);
    n = again.finish();
    CHECK_EQ(small.parse(b, n), Status::full);
}
TEST_CASE(proto_fix, exact_capacity_and_pipeline) {
    char buffer[512], exact[512], scratch[512];
    Header header{"S", "T", 1, 0};
    Heartbeat m;
    auto n = encode(m, header, buffer, sizeof buffer);
    REQUIRE(n > 0);
    CHECK_EQ(encode(m, header, exact, n), n);
    CHECK_EQ(encode(m, header, exact, n - 1), 0U);
    std::memcpy(buffer + n, buffer, n);
    FrameReader reader(scratch);
    std::size_t consumed;
    std::span<const char> message;
    CHECK_EQ(reader.feed({buffer, n * 2}, consumed, message), Status::ok);
    CHECK_EQ(consumed, n);
    CHECK_EQ(message.size(), n);
    CHECK_EQ(reader.feed({buffer + n, n}, consumed, message), Status::ok);
    CHECK_EQ(consumed, n);
}
