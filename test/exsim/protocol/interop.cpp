#include "../../framework/test.hpp"
#include <protocol/itch50.hpp>
#include <protocol/ouch42.hpp>
using exsim::protocol::Status;
TEST_CASE(proto_itch, add_order_golden) {
  namespace i = exsim::protocol::itch50;
  char b[128];
  i::AddOrder a;
  a.timestamp = 123456;
  a.order_reference = 0x0102030405060708ULL;
  a.side = exsim::Side::BUY;
  a.shares = 0x11223344;
  a.price = 1502500;
  a.stock = exsim::Symbol("AAPL");
  CHECK_EQ(i::encode(a, b, sizeof b), 36U);
  CHECK_EQ(static_cast<unsigned char>(b[11]), 1);
  CHECK_EQ(static_cast<unsigned char>(b[18]), 8);
  CHECK_EQ(b[19], 'B');
  CHECK_EQ(static_cast<unsigned char>(b[20]), 0x11);
  CHECK_EQ(static_cast<unsigned char>(b[23]), 0x44);
  CHECK_EQ(std::string_view(b + 24, 8), "AAPL    ");
  CHECK_EQ(static_cast<unsigned char>(b[32]), 0);
  CHECK_EQ(static_cast<unsigned char>(b[33]), 0x16);
  CHECK_EQ(static_cast<unsigned char>(b[34]), 0xed);
  CHECK_EQ(static_cast<unsigned char>(b[35]), 0x24);
}
TEST_CASE(proto_ouch, enter_golden) {
  namespace o = exsim::protocol::ouch42;
  o::EnterOrder m, d;
  char b[49];
  m.side = 'T';
  m.shares = 0x12345678;
  m.stock = exsim::Symbol("MSFT");
  m.price = 0x10203040;
  m.time_in_force = 99998;
  m.minimum_quantity = 0xaabbccdd;
  m.cross_type = 'N';
  m.customer_type = 'R';
  CHECK_EQ(o::encode(m, b, sizeof b), 49U);
  CHECK_EQ(b[15], 'T');
  CHECK_EQ(static_cast<unsigned char>(b[16]), 0x12);
  CHECK_EQ(static_cast<unsigned char>(b[19]), 0x78);
  CHECK_EQ(std::string_view(b + 20, 8), "MSFT    ");
  CHECK_EQ(static_cast<unsigned char>(b[28]), 0x10);
  CHECK_EQ(static_cast<unsigned char>(b[31]), 0x40);
  CHECK_EQ(static_cast<unsigned char>(b[33]), 1);
  CHECK_EQ(static_cast<unsigned char>(b[34]), 0x86);
  CHECK_EQ(static_cast<unsigned char>(b[35]), 0x9e);
  CHECK_EQ(static_cast<unsigned char>(b[43]), 0xaa);
  CHECK_EQ(b[47], 'N');
  CHECK_EQ(b[48], 'R');
  CHECK_EQ(o::decode(b, 49, d), Status::ok);
  CHECK_EQ(d.minimum_quantity, m.minimum_quantity);
}
TEST_CASE(proto_binary, invalid_fields) {
  char b[128];
  exsim::protocol::ouch42::EnterOrder order, decoded;
  order.price = -1;
  CHECK_EQ(exsim::protocol::ouch42::encode(order, b, sizeof b), 0U);
  order.price = exsim::Price(UINT32_MAX) + 1;
  CHECK_EQ(exsim::protocol::ouch42::encode(order, b, sizeof b), 0U);
  order.price = 0;
  order.side = 'B';
  REQUIRE_EQ(exsim::protocol::ouch42::encode(order, b, sizeof b), 49U);
  b[15] = '?';
  CHECK_EQ(exsim::protocol::ouch42::decode(b, 49, decoded),
           Status::invalid_field);
  exsim::protocol::itch50::AddOrder add, read;
  add.timestamp = 0x1000000000000ULL;
  CHECK_EQ(exsim::protocol::itch50::encode(add, b, sizeof b), 0U);
  add.timestamp = 0;
  REQUIRE_EQ(exsim::protocol::itch50::encode(add, b, sizeof b), 36U);
  b[19] = '?';
  CHECK_EQ(exsim::protocol::itch50::decode(b, 36, read), Status::invalid_field);
}
