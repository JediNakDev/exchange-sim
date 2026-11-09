#include "../../framework/test.hpp"
#include <exsim/protocol/fix44.hpp>
using namespace exsim::protocol::fix44;
TEST_CASE(proto_fix_messages, Logon) {
  Logon m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, Heartbeat) {
  Heartbeat m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, TestRequest) {
  TestRequest m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.test_request_id.assign("TEST");
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, ResendRequest) {
  ResendRequest m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, Reject) {
  Reject m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.reference_type.assign("TEST");
  m.text.assign("TEST");
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, SequenceReset) {
  SequenceReset m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.gap_fill = 'Y';
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, Logout) {
  Logout m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, NewOrderSingle) {
  NewOrderSingle m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.client_order_id.assign("TEST");
  m.symbol.assign("TEST");
  m.has_price = true;
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, OrderCancelRequest) {
  OrderCancelRequest m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.original_client_order_id.assign("TEST");
  m.client_order_id.assign("TEST");
  m.symbol.assign("TEST");
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, OrderCancelReplaceRequest) {
  OrderCancelReplaceRequest m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.original_client_order_id.assign("TEST");
  m.client_order_id.assign("TEST");
  m.symbol.assign("TEST");
  m.has_price = true;
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, OrderMassCancelRequest) {
  OrderMassCancelRequest m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.client_order_id.assign("TEST");
  m.request_type = '7';
  m.request_type = '7';
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, ExecutionReport) {
  ExecutionReport m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.order_id.assign("TEST");
  m.client_order_id.assign("TEST");
  m.execution_id.assign("TEST");
  m.execution_type = '0';
  m.order_status = '0';
  m.symbol.assign("TEST");
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, OrderCancelReject) {
  OrderCancelReject m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.order_id.assign("TEST");
  m.client_order_id.assign("TEST");
  m.original_client_order_id.assign("TEST");
  m.order_status = '0';
  m.response_to = '1';
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
TEST_CASE(proto_fix_messages, OrderMassCancelReport) {
  OrderMassCancelReport m, d;
  char b[2048];
  Header h{"S", "T", 1, 0};
  m.client_order_id.assign("TEST");
  m.order_id.assign("TEST");
  m.request_type = '7';
  m.response = '0';
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  CHECK_EQ(decode(b, n, d), Status::ok);
  CHECK_EQ(encode(m, h, b, 10), 0U);
  CHECK_EQ(decode(b, n - 1, d), Status::incomplete);
}
