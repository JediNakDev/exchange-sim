#include "ipc/strategy_wire.hpp"
#include "test/framework/test.hpp"

TEST_CASE(strategy_wire, uses_explicit_big_endian_fields_without_struct_padding) {
    exsim::ipc::Frame frame;
    std::uint32_t number = 0x01020304;
    std::int64_t negative = -2;
    bool flag = true;
    frame.fields(number,negative,flag);
    REQUIRE_EQ(frame.size,13u);
    CHECK_EQ(frame.bytes[0],1u);
    CHECK_EQ(frame.bytes[3],4u);
    CHECK_EQ(frame.bytes[11],254u);
    frame.reading = true;
    number = 0; negative = 0; flag = false;
    frame.fields(number,negative,flag); frame.finish();
    CHECK_EQ(number,0x01020304u); CHECK_EQ(negative,-2); CHECK(flag);
}
TEST_CASE(strategy_wire, rejects_truncated_fields_and_invalid_booleans) {
    exsim::ipc::Frame frame;
    frame.reading = true;
    bool rejected = false;
    std::uint64_t number = 0;
    try { frame.field(number); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
    frame.size = 1; frame.cursor = 0; frame.bytes[0] = 2;
    bool flag = false; rejected = false;
    try { frame.field(flag); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
TEST_CASE(strategy_wire, decision_reasons_decode_to_stable_storage) {
    exsim::research::ExecutionDecision row;
    row.reason = "no_delivered_touch"; row.headroom = -17;
    exsim::ipc::Frame frame; frame.field(row);
    frame.reading = true;
    exsim::research::ExecutionDecision decoded; frame.field(decoded); frame.finish();
    frame.bytes.fill(0);
    CHECK_EQ(std::string(decoded.reason),"no_delivered_touch");
    CHECK_EQ(decoded.headroom,-17);
}
