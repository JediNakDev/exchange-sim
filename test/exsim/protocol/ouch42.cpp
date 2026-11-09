#include "../../framework/test.hpp"
#include <protocol/ouch42.hpp>
#include <lob/protocol/itch.hpp>
using namespace exsim::protocol::ouch42;
TEST_CASE(proto_ouch42, EnterOrder) {
  EnterOrder m{}, d{};
  char b[50]{};
  CHECK_EQ(encode(m, b, sizeof b), 49U);
  CHECK_EQ(b[0], 'O');
  CHECK_EQ(decode(b, 49, d), Status::ok);
  for (std::size_t i = 0; i < 49; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 50, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 48), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 49, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, ReplaceOrder) {
  ReplaceOrder m{}, d{};
  char b[48]{};
  CHECK_EQ(encode(m, b, sizeof b), 47U);
  CHECK_EQ(b[0], 'U');
  CHECK_EQ(decode(b, 47, d), Status::ok);
  for (std::size_t i = 0; i < 47; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 48, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 46), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 47, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, CancelOrder) {
  CancelOrder m{}, d{};
  char b[20]{};
  CHECK_EQ(encode(m, b, sizeof b), 19U);
  CHECK_EQ(b[0], 'X');
  CHECK_EQ(decode(b, 19, d), Status::ok);
  for (std::size_t i = 0; i < 19; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 20, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 18), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 19, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, ModifyOrder) {
  ModifyOrder m{}, d{};
  char b[21]{};
  CHECK_EQ(encode(m, b, sizeof b), 20U);
  CHECK_EQ(b[0], 'M');
  CHECK_EQ(decode(b, 20, d), Status::ok);
  for (std::size_t i = 0; i < 20; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 21, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 19), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 20, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, SystemEvent) {
  SystemEvent m{}, d{};
  char b[11]{};
  CHECK_EQ(encode(m, b, sizeof b), 10U);
  CHECK_EQ(b[0], 'S');
  CHECK_EQ(decode(b, 10, d), Status::ok);
  for (std::size_t i = 0; i < 10; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 11, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 9), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 10, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderAccepted) {
  OrderAccepted m{}, d{};
  char b[67]{};
  CHECK_EQ(encode(m, b, sizeof b), 66U);
  CHECK_EQ(b[0], 'A');
  CHECK_EQ(decode(b, 66, d), Status::ok);
  for (std::size_t i = 0; i < 66; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 67, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 65), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 66, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderCanceled) {
  OrderCanceled m{}, d{};
  char b[29]{};
  CHECK_EQ(encode(m, b, sizeof b), 28U);
  CHECK_EQ(b[0], 'C');
  CHECK_EQ(decode(b, 28, d), Status::ok);
  for (std::size_t i = 0; i < 28; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 29, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 27), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 28, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderExecuted) {
  OrderExecuted m{}, d{};
  char b[41]{};
  CHECK_EQ(encode(m, b, sizeof b), 40U);
  CHECK_EQ(b[0], 'E');
  CHECK_EQ(decode(b, 40, d), Status::ok);
  for (std::size_t i = 0; i < 40; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 41, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 39), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 40, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderRejected) {
  OrderRejected m{}, d{};
  char b[25]{};
  CHECK_EQ(encode(m, b, sizeof b), 24U);
  CHECK_EQ(b[0], 'J');
  CHECK_EQ(decode(b, 24, d), Status::ok);
  for (std::size_t i = 0; i < 24; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 25, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 23), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 24, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, CancelPending) {
  CancelPending m{}, d{};
  char b[24]{};
  CHECK_EQ(encode(m, b, sizeof b), 23U);
  CHECK_EQ(b[0], 'P');
  CHECK_EQ(decode(b, 23, d), Status::ok);
  for (std::size_t i = 0; i < 23; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 24, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 22), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 23, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, CancelReject) {
  CancelReject m{}, d{};
  char b[24]{};
  CHECK_EQ(encode(m, b, sizeof b), 23U);
  CHECK_EQ(b[0], 'I');
  CHECK_EQ(decode(b, 23, d), Status::ok);
  for (std::size_t i = 0; i < 23; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 24, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 22), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 23, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderModified) {
  OrderModified m{}, d{};
  char b[29]{};
  CHECK_EQ(encode(m, b, sizeof b), 28U);
  CHECK_EQ(b[0], 'M');
  CHECK_EQ(decode(b, 28, d), Status::ok);
  for (std::size_t i = 0; i < 28; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 29, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 27), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 28, d), Status::invalid_type);
}
TEST_CASE(proto_ouch42, OrderReplaced) {
  OrderReplaced m{}, d{};
  char b[81]{};
  CHECK_EQ(encode(m, b, sizeof b), 80U);
  CHECK_EQ(b[0], 'U');
  CHECK_EQ(decode(b, 80, d), Status::ok);
  for (std::size_t i = 0; i < 80; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 81, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 79), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 80, d), Status::invalid_type);
}
