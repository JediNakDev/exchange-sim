#include "../../framework/test.hpp"
#include <protocol/itch50.hpp>
using namespace exsim::protocol::itch50;
TEST_CASE(proto_itch50, SystemEvent) {
  SystemEvent m{}, d{};
  char b[13]{};
  CHECK_EQ(encode(m, b, sizeof b), 12U);
  CHECK_EQ(b[0], 'S');
  CHECK_EQ(decode(b, 12, d), Status::ok);
  for (std::size_t i = 0; i < 12; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 13, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 11), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 12, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, StockDirectory) {
  StockDirectory m{}, d{};
  char b[40]{};
  CHECK_EQ(encode(m, b, sizeof b), 39U);
  CHECK_EQ(b[0], 'R');
  CHECK_EQ(decode(b, 39, d), Status::ok);
  for (std::size_t i = 0; i < 39; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 40, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 38), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 39, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, StockTradingAction) {
  StockTradingAction m{}, d{};
  char b[26]{};
  CHECK_EQ(encode(m, b, sizeof b), 25U);
  CHECK_EQ(b[0], 'H');
  CHECK_EQ(decode(b, 25, d), Status::ok);
  for (std::size_t i = 0; i < 25; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 26, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 24), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 25, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, AddOrder) {
  AddOrder m{}, d{};
  char b[37]{};
  CHECK_EQ(encode(m, b, sizeof b), 36U);
  CHECK_EQ(b[0], 'A');
  CHECK_EQ(decode(b, 36, d), Status::ok);
  for (std::size_t i = 0; i < 36; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 37, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 35), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 36, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, AddOrderMPID) {
  AddOrderMPID m{}, d{};
  char b[41]{};
  CHECK_EQ(encode(m, b, sizeof b), 40U);
  CHECK_EQ(b[0], 'F');
  CHECK_EQ(decode(b, 40, d), Status::ok);
  for (std::size_t i = 0; i < 40; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 41, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 39), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 40, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, OrderExecuted) {
  OrderExecuted m{}, d{};
  char b[32]{};
  CHECK_EQ(encode(m, b, sizeof b), 31U);
  CHECK_EQ(b[0], 'E');
  CHECK_EQ(decode(b, 31, d), Status::ok);
  for (std::size_t i = 0; i < 31; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 32, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 30), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 31, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, OrderExecutedWithPrice) {
  OrderExecutedWithPrice m{}, d{};
  char b[37]{};
  CHECK_EQ(encode(m, b, sizeof b), 36U);
  CHECK_EQ(b[0], 'C');
  CHECK_EQ(decode(b, 36, d), Status::ok);
  for (std::size_t i = 0; i < 36; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 37, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 35), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 36, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, OrderCancel) {
  OrderCancel m{}, d{};
  char b[24]{};
  CHECK_EQ(encode(m, b, sizeof b), 23U);
  CHECK_EQ(b[0], 'X');
  CHECK_EQ(decode(b, 23, d), Status::ok);
  for (std::size_t i = 0; i < 23; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 24, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 22), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 23, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, OrderDelete) {
  OrderDelete m{}, d{};
  char b[20]{};
  CHECK_EQ(encode(m, b, sizeof b), 19U);
  CHECK_EQ(b[0], 'D');
  CHECK_EQ(decode(b, 19, d), Status::ok);
  for (std::size_t i = 0; i < 19; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 20, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 18), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 19, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, OrderReplace) {
  OrderReplace m{}, d{};
  char b[36]{};
  CHECK_EQ(encode(m, b, sizeof b), 35U);
  CHECK_EQ(b[0], 'U');
  CHECK_EQ(decode(b, 35, d), Status::ok);
  for (std::size_t i = 0; i < 35; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 36, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 34), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 35, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, Trade) {
  Trade m{}, d{};
  char b[45]{};
  CHECK_EQ(encode(m, b, sizeof b), 44U);
  CHECK_EQ(b[0], 'P');
  CHECK_EQ(decode(b, 44, d), Status::ok);
  for (std::size_t i = 0; i < 44; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 45, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 43), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 44, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, CrossTrade) {
  CrossTrade m{}, d{};
  char b[41]{};
  CHECK_EQ(encode(m, b, sizeof b), 40U);
  CHECK_EQ(b[0], 'Q');
  CHECK_EQ(decode(b, 40, d), Status::ok);
  for (std::size_t i = 0; i < 40; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 41, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 39), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 40, d), Status::invalid_type);
}
TEST_CASE(proto_itch50, NetOrderImbalance) {
  NetOrderImbalance m{}, d{};
  char b[51]{};
  CHECK_EQ(encode(m, b, sizeof b), 50U);
  CHECK_EQ(b[0], 'I');
  CHECK_EQ(decode(b, 50, d), Status::ok);
  for (std::size_t i = 0; i < 50; ++i)
    CHECK_EQ(decode(b, i, d), Status::incomplete);
  CHECK_EQ(decode(b, 51, d), Status::invalid_length);
  CHECK_EQ(encode(m, b, 49), 0U);
  b[0] = '?';
  CHECK_EQ(decode(b, 50, d), Status::invalid_type);
}
