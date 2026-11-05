#include "../../framework/test.hpp"
#include <exsim/protocol/fix44.hpp>
#include <random>
using namespace exsim::protocol::fix44;
TEST_CASE(proto_fix_golden, Logon) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    Logon m, d;
    char b[2048], again[2048];
    m.encrypt_method = random();
    m.heartbeat_interval = random();
    m.has_reset_sequence = true;
    m.reset_sequence = 'Y';
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    std::uint64_t value_98;
    CHECK(parser.get_uint(98, value_98));
    CHECK_EQ(value_98, m.encrypt_method);
    std::uint64_t value_108;
    CHECK(parser.get_uint(108, value_108));
    CHECK_EQ(value_108, m.heartbeat_interval);
    CHECK_EQ(parser.get(141), "Y");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {98U, 108U}) {
      Builder bad(again);
      bad.header('A', h);
      if (skip != 98)
        bad.field(98, parser.get(98));
      if (skip != 108)
        bad.field(108, parser.get(108));
      if (skip != 141)
        bad.field(141, parser.get(141));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, Heartbeat) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    Heartbeat m, d;
    char b[2048], again[2048];
    m.has_test_request_id = true;
    m.test_request_id.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(112), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
  }
}
TEST_CASE(proto_fix_golden, TestRequest) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    TestRequest m, d;
    char b[2048], again[2048];
    m.test_request_id.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(112), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {112U}) {
      Builder bad(again);
      bad.header('1', h);
      if (skip != 112)
        bad.field(112, parser.get(112));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, ResendRequest) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    ResendRequest m, d;
    char b[2048], again[2048];
    m.begin_sequence = random();
    m.end_sequence = random();
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    std::uint64_t value_7;
    CHECK(parser.get_uint(7, value_7));
    CHECK_EQ(value_7, m.begin_sequence);
    std::uint64_t value_16;
    CHECK(parser.get_uint(16, value_16));
    CHECK_EQ(value_16, m.end_sequence);
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {7U, 16U}) {
      Builder bad(again);
      bad.header('2', h);
      if (skip != 7)
        bad.field(7, parser.get(7));
      if (skip != 16)
        bad.field(16, parser.get(16));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, Reject) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    Reject m, d;
    char b[2048], again[2048];
    m.reference_sequence = random();
    m.reference_tag = random();
    m.reference_type.assign("VALUE");
    m.reason = random();
    m.text.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    std::uint64_t value_45;
    CHECK(parser.get_uint(45, value_45));
    CHECK_EQ(value_45, m.reference_sequence);
    std::uint64_t value_371;
    CHECK(parser.get_uint(371, value_371));
    CHECK_EQ(value_371, m.reference_tag);
    CHECK_EQ(parser.get(372), "VALUE");
    std::uint64_t value_373;
    CHECK(parser.get_uint(373, value_373));
    CHECK_EQ(value_373, m.reason);
    CHECK_EQ(parser.get(58), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {45U, 371U, 372U, 373U, 58U}) {
      Builder bad(again);
      bad.header('3', h);
      if (skip != 45)
        bad.field(45, parser.get(45));
      if (skip != 371)
        bad.field(371, parser.get(371));
      if (skip != 372)
        bad.field(372, parser.get(372));
      if (skip != 373)
        bad.field(373, parser.get(373));
      if (skip != 58)
        bad.field(58, parser.get(58));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, SequenceReset) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    SequenceReset m, d;
    char b[2048], again[2048];
    m.gap_fill = 'Y';
    m.new_sequence = random();
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(123), "Y");
    std::uint64_t value_36;
    CHECK(parser.get_uint(36, value_36));
    CHECK_EQ(value_36, m.new_sequence);
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {123U, 36U}) {
      Builder bad(again);
      bad.header('4', h);
      if (skip != 123)
        bad.field(123, parser.get(123));
      if (skip != 36)
        bad.field(36, parser.get(36));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, Logout) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    Logout m, d;
    char b[2048], again[2048];
    m.has_text = true;
    m.text.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(58), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
  }
}
TEST_CASE(proto_fix_golden, NewOrderSingle) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    NewOrderSingle m, d;
    char b[2048], again[2048];
    m.client_order_id.assign("VALUE");
    m.has_account = true;
    m.account.assign("VALUE");
    m.symbol.assign("VALUE");
    m.side = exsim::Side::SELL;
    m.quantity = random();
    m.order_type = exsim::OrdType::Limit;
    m.has_price = true;
    m.price = 1502500;
    m.time_in_force = exsim::TimeInForce::FOK;
    m.has_exec_inst = true;
    m.exec_inst.assign("VALUE");
    m.transact_time = 0;
    m.stp_mode = exsim::StpMode::CancelBoth;
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(1), "VALUE");
    CHECK_EQ(parser.get(55), "VALUE");
    CHECK_EQ(parser.get(54), "2");
    std::uint64_t value_38;
    CHECK(parser.get_uint(38, value_38));
    CHECK_EQ(value_38, m.quantity);
    CHECK_EQ(parser.get(40), "2");
    CHECK_EQ(parser.get(44), "150.25");
    CHECK_EQ(parser.get(59), "4");
    CHECK_EQ(parser.get(18), "VALUE");
    CHECK_EQ(parser.get(60), "19700101-00:00:00.000");
    CHECK_EQ(parser.get(7928), "B");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {11U, 55U, 54U, 38U, 40U, 59U, 60U, 7928U}) {
      Builder bad(again);
      bad.header('D', h);
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 1)
        bad.field(1, parser.get(1));
      if (skip != 55)
        bad.field(55, parser.get(55));
      if (skip != 54)
        bad.field(54, parser.get(54));
      if (skip != 38)
        bad.field(38, parser.get(38));
      if (skip != 40)
        bad.field(40, parser.get(40));
      if (skip != 44)
        bad.field(44, parser.get(44));
      if (skip != 59)
        bad.field(59, parser.get(59));
      if (skip != 18)
        bad.field(18, parser.get(18));
      if (skip != 60)
        bad.field(60, parser.get(60));
      if (skip != 7928)
        bad.field(7928, parser.get(7928));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, OrderCancelRequest) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    OrderCancelRequest m, d;
    char b[2048], again[2048];
    m.original_client_order_id.assign("VALUE");
    m.client_order_id.assign("VALUE");
    m.symbol.assign("VALUE");
    m.side = exsim::Side::SELL;
    m.transact_time = 0;
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(41), "VALUE");
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(55), "VALUE");
    CHECK_EQ(parser.get(54), "2");
    CHECK_EQ(parser.get(60), "19700101-00:00:00.000");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {41U, 11U, 55U, 54U, 60U}) {
      Builder bad(again);
      bad.header('F', h);
      if (skip != 41)
        bad.field(41, parser.get(41));
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 55)
        bad.field(55, parser.get(55));
      if (skip != 54)
        bad.field(54, parser.get(54));
      if (skip != 60)
        bad.field(60, parser.get(60));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, OrderCancelReplaceRequest) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    OrderCancelReplaceRequest m, d;
    char b[2048], again[2048];
    m.original_client_order_id.assign("VALUE");
    m.client_order_id.assign("VALUE");
    m.symbol.assign("VALUE");
    m.side = exsim::Side::SELL;
    m.quantity = random();
    m.order_type = exsim::OrdType::Limit;
    m.has_price = true;
    m.price = 1502500;
    m.transact_time = 0;
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(41), "VALUE");
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(55), "VALUE");
    CHECK_EQ(parser.get(54), "2");
    std::uint64_t value_38;
    CHECK(parser.get_uint(38, value_38));
    CHECK_EQ(value_38, m.quantity);
    CHECK_EQ(parser.get(40), "2");
    CHECK_EQ(parser.get(44), "150.25");
    CHECK_EQ(parser.get(60), "19700101-00:00:00.000");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {41U, 11U, 55U, 54U, 38U, 40U, 60U}) {
      Builder bad(again);
      bad.header('G', h);
      if (skip != 41)
        bad.field(41, parser.get(41));
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 55)
        bad.field(55, parser.get(55));
      if (skip != 54)
        bad.field(54, parser.get(54));
      if (skip != 38)
        bad.field(38, parser.get(38));
      if (skip != 40)
        bad.field(40, parser.get(40));
      if (skip != 44)
        bad.field(44, parser.get(44));
      if (skip != 60)
        bad.field(60, parser.get(60));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, OrderMassCancelRequest) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    OrderMassCancelRequest m, d;
    char b[2048], again[2048];
    m.client_order_id.assign("VALUE");
    m.request_type = '1';
    m.has_symbol = true;
    m.symbol.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(530), "1");
    CHECK_EQ(parser.get(55), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {11U, 530U}) {
      Builder bad(again);
      bad.header('q', h);
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 530)
        bad.field(530, parser.get(530));
      if (skip != 55)
        bad.field(55, parser.get(55));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, ExecutionReport) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    ExecutionReport m, d;
    char b[2048], again[2048];
    m.order_id.assign("VALUE");
    m.client_order_id.assign("VALUE");
    m.has_original_client_order_id = true;
    m.original_client_order_id.assign("VALUE");
    m.execution_id.assign("VALUE");
    m.execution_type = '4';
    m.order_status = '4';
    m.symbol.assign("VALUE");
    m.side = exsim::Side::SELL;
    m.quantity = random();
    m.price = 1502500;
    m.last_quantity = random();
    m.last_price = 1502500;
    m.leaves_quantity = random();
    m.cumulative_quantity = random();
    m.average_price = 1502500;
    m.has_reject_reason = true;
    m.reject_reason = random();
    m.has_text = true;
    m.text.assign("VALUE");
    m.transact_time = 0;
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(37), "VALUE");
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(41), "VALUE");
    CHECK_EQ(parser.get(17), "VALUE");
    CHECK_EQ(parser.get(150), "4");
    CHECK_EQ(parser.get(39), "4");
    CHECK_EQ(parser.get(55), "VALUE");
    CHECK_EQ(parser.get(54), "2");
    std::uint64_t value_38;
    CHECK(parser.get_uint(38, value_38));
    CHECK_EQ(value_38, m.quantity);
    CHECK_EQ(parser.get(44), "150.25");
    std::uint64_t value_32;
    CHECK(parser.get_uint(32, value_32));
    CHECK_EQ(value_32, m.last_quantity);
    CHECK_EQ(parser.get(31), "150.25");
    std::uint64_t value_151;
    CHECK(parser.get_uint(151, value_151));
    CHECK_EQ(value_151, m.leaves_quantity);
    std::uint64_t value_14;
    CHECK(parser.get_uint(14, value_14));
    CHECK_EQ(value_14, m.cumulative_quantity);
    CHECK_EQ(parser.get(6), "150.25");
    std::uint64_t value_103;
    CHECK(parser.get_uint(103, value_103));
    CHECK_EQ(value_103, m.reject_reason);
    CHECK_EQ(parser.get(58), "VALUE");
    CHECK_EQ(parser.get(60), "19700101-00:00:00.000");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {37U, 11U, 17U, 150U, 39U, 55U, 54U, 38U, 32U, 31U,
                          151U, 14U, 6U, 60U}) {
      Builder bad(again);
      bad.header('8', h);
      if (skip != 37)
        bad.field(37, parser.get(37));
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 41)
        bad.field(41, parser.get(41));
      if (skip != 17)
        bad.field(17, parser.get(17));
      if (skip != 150)
        bad.field(150, parser.get(150));
      if (skip != 39)
        bad.field(39, parser.get(39));
      if (skip != 55)
        bad.field(55, parser.get(55));
      if (skip != 54)
        bad.field(54, parser.get(54));
      if (skip != 38)
        bad.field(38, parser.get(38));
      if (skip != 44)
        bad.field(44, parser.get(44));
      if (skip != 32)
        bad.field(32, parser.get(32));
      if (skip != 31)
        bad.field(31, parser.get(31));
      if (skip != 151)
        bad.field(151, parser.get(151));
      if (skip != 14)
        bad.field(14, parser.get(14));
      if (skip != 6)
        bad.field(6, parser.get(6));
      if (skip != 103)
        bad.field(103, parser.get(103));
      if (skip != 58)
        bad.field(58, parser.get(58));
      if (skip != 60)
        bad.field(60, parser.get(60));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, OrderCancelReject) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    OrderCancelReject m, d;
    char b[2048], again[2048];
    m.order_id.assign("VALUE");
    m.client_order_id.assign("VALUE");
    m.original_client_order_id.assign("VALUE");
    m.order_status = '4';
    m.response_to = '1';
    m.reason = random();
    m.has_text = true;
    m.text.assign("VALUE");
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(37), "VALUE");
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(41), "VALUE");
    CHECK_EQ(parser.get(39), "4");
    CHECK_EQ(parser.get(434), "1");
    std::uint64_t value_102;
    CHECK(parser.get_uint(102, value_102));
    CHECK_EQ(value_102, m.reason);
    CHECK_EQ(parser.get(58), "VALUE");
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {37U, 11U, 41U, 39U, 434U, 102U}) {
      Builder bad(again);
      bad.header('9', h);
      if (skip != 37)
        bad.field(37, parser.get(37));
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 41)
        bad.field(41, parser.get(41));
      if (skip != 39)
        bad.field(39, parser.get(39));
      if (skip != 434)
        bad.field(434, parser.get(434));
      if (skip != 102)
        bad.field(102, parser.get(102));
      if (skip != 58)
        bad.field(58, parser.get(58));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, OrderMassCancelReport) {
  std::mt19937 random(123);
  Header h{"S", "T", 1, 0};
  for (unsigned run = 0; run < 25; ++run) {
    OrderMassCancelReport m, d;
    char b[2048], again[2048];
    m.client_order_id.assign("VALUE");
    m.order_id.assign("VALUE");
    m.request_type = '1';
    m.response = '1';
    m.has_reject_reason = true;
    m.reject_reason = 1;
    m.total_affected_orders = random();
    auto n = encode(m, h, b, sizeof b);
    REQUIRE(n > 0);
    Parser<> parser;
    REQUIRE_EQ(parser.parse(b, n), Status::ok);
    CHECK_EQ(parser.get(11), "VALUE");
    CHECK_EQ(parser.get(37), "VALUE");
    CHECK_EQ(parser.get(530), "1");
    CHECK_EQ(parser.get(531), "1");
    CHECK_EQ(parser.get(532), "1");
    std::uint64_t value_533;
    CHECK(parser.get_uint(533, value_533));
    CHECK_EQ(value_533, m.total_affected_orders);
    REQUIRE_EQ(decode(parser, d), Status::ok);
    auto n2 = encode(d, h, again, sizeof again);
    CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
    for (unsigned skip : {11U, 37U, 530U, 531U, 533U}) {
      Builder bad(again);
      bad.header('r', h);
      if (skip != 11)
        bad.field(11, parser.get(11));
      if (skip != 37)
        bad.field(37, parser.get(37));
      if (skip != 530)
        bad.field(530, parser.get(530));
      if (skip != 531)
        bad.field(531, parser.get(531));
      if (skip != 532)
        bad.field(532, parser.get(532));
      if (skip != 533)
        bad.field(533, parser.get(533));
      auto bad_size = bad.finish();
      CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
    }
  }
}
TEST_CASE(proto_fix_golden, ExecutionReportWithoutPrice) {
  Header h{"S", "T", 1, 0};
  ExecutionReport m, d;
  char b[2048], again[2048];
  m.order_id.assign("1");
  m.client_order_id.assign("MKT");
  m.execution_id.assign("1");
  m.execution_type = 'C';
  m.order_status = 'C';
  m.symbol.assign("AAPL");
  m.side = exsim::Side::BUY;
  m.quantity = 10;
  m.has_price = false;
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  Parser<> parser;
  REQUIRE_EQ(parser.parse(b, n), Status::ok);
  CHECK(!parser.has(44));
  CHECK_EQ(parser.get(150), "C");
  CHECK_EQ(parser.get(39), "C");
  REQUIRE_EQ(decode(parser, d), Status::ok);
  CHECK(!d.has_price);
  auto n2 = encode(d, h, again, sizeof again);
  CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
}
TEST_CASE(proto_fix_golden, BusinessMessageReject) {
  Header h{"S", "T", 7, 0};
  BusinessMessageReject m, d;
  char b[2048], again[2048];
  m.has_reference_sequence = true;
  m.reference_sequence = 5;
  m.reference_type.assign("AF");
  m.reason = 3;
  m.has_text = true;
  m.text.assign("unsupported");
  auto n = encode(m, h, b, sizeof b);
  REQUIRE(n > 0);
  Parser<> parser;
  REQUIRE_EQ(parser.parse(b, n), Status::ok);
  CHECK_EQ(parser.get(35), "j");
  CHECK_EQ(parser.get(45), "5");
  CHECK_EQ(parser.get(372), "AF");
  CHECK(!parser.has(379));
  CHECK_EQ(parser.get(380), "3");
  CHECK_EQ(parser.get(58), "unsupported");
  REQUIRE_EQ(decode(parser, d), Status::ok);
  auto n2 = encode(d, h, again, sizeof again);
  CHECK_EQ(std::string_view(b, n), std::string_view(again, n2));
  for (unsigned skip : {372U, 380U}) {
    Builder bad(again);
    bad.header('j', h);
    bad.field(45, parser.get(45));
    if (skip != 372)
      bad.field(372, parser.get(372));
    if (skip != 380)
      bad.field(380, parser.get(380));
    auto bad_size = bad.finish();
    CHECK_EQ(decode(again, bad_size, d), Status::missing_field);
  }
}
