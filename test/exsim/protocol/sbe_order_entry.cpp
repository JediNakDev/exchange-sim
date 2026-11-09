#include <test/framework/test.hpp>
#include "protocol/sbe_order_entry.hpp"
#include <array>
#include <cstring>
namespace sbe = exsim::protocol::sbe_order_entry;

template<class Message> void check_round_trip() {
    Message original;
    std::array<char, 1024> encoded{}, reencoded{};
    if constexpr (Message::kHasVariableLengthField) sbe::set_variable_length_field(original, "test-data");
    auto size = sbe::encode(original, encoded.data(), encoded.size());
    REQUIRE(size != 0);
    CHECK_EQ(sbe::encode(original, reencoded.data(), size - 1), 0u);
    sbe::FrameView view;
    CHECK(sbe::decode_frame(encoded.data(), size - 1, view) == sbe::DecodeStatus::Incomplete);
    REQUIRE(sbe::decode_frame(encoded.data(), size, view) == sbe::DecodeStatus::Ok);
    Message decoded;
    REQUIRE(sbe::decode(view, decoded) == sbe::DecodeStatus::Ok);
    CHECK_EQ(sbe::encode(decoded, reencoded.data(), reencoded.size()), size);
    CHECK(std::memcmp(encoded.data(), reencoded.data(), size) == 0);
}
TEST_CASE(sbe_codec, round_trip_Negotiate) { check_round_trip<sbe::Negotiate>(); }
TEST_CASE(sbe_codec, round_trip_NegotiationResponse) { check_round_trip<sbe::NegotiationResponse>(); }
TEST_CASE(sbe_codec, round_trip_NegotiationReject) { check_round_trip<sbe::NegotiationReject>(); }
TEST_CASE(sbe_codec, round_trip_Establish) { check_round_trip<sbe::Establish>(); }
TEST_CASE(sbe_codec, round_trip_EstablishmentAck) { check_round_trip<sbe::EstablishmentAck>(); }
TEST_CASE(sbe_codec, round_trip_EstablishmentReject) { check_round_trip<sbe::EstablishmentReject>(); }
TEST_CASE(sbe_codec, round_trip_Sequence) { check_round_trip<sbe::Sequence>(); }
TEST_CASE(sbe_codec, round_trip_UnsequencedHeartbeat) { check_round_trip<sbe::UnsequencedHeartbeat>(); }
TEST_CASE(sbe_codec, round_trip_RetransmitRequest) { check_round_trip<sbe::RetransmitRequest>(); }
TEST_CASE(sbe_codec, round_trip_Retransmission) { check_round_trip<sbe::Retransmission>(); }
TEST_CASE(sbe_codec, round_trip_RetransmitReject) { check_round_trip<sbe::RetransmitReject>(); }
TEST_CASE(sbe_codec, round_trip_Terminate) { check_round_trip<sbe::Terminate>(); }
TEST_CASE(sbe_codec, round_trip_NotApplied) { check_round_trip<sbe::NotApplied>(); }
TEST_CASE(sbe_codec, round_trip_NewOrderSingle) { check_round_trip<sbe::NewOrderSingle>(); }
TEST_CASE(sbe_codec, round_trip_OrderCancelRequest) { check_round_trip<sbe::OrderCancelRequest>(); }
TEST_CASE(sbe_codec, round_trip_OrderCancelReplaceRequest) { check_round_trip<sbe::OrderCancelReplaceRequest>(); }
TEST_CASE(sbe_codec, round_trip_OrderMassCancelRequest) { check_round_trip<sbe::OrderMassCancelRequest>(); }
TEST_CASE(sbe_codec, round_trip_ExecutionReportNew) { check_round_trip<sbe::ExecutionReportNew>(); }
TEST_CASE(sbe_codec, round_trip_ExecutionReportTrade) { check_round_trip<sbe::ExecutionReportTrade>(); }
TEST_CASE(sbe_codec, round_trip_ExecutionReportCancel) { check_round_trip<sbe::ExecutionReportCancel>(); }
TEST_CASE(sbe_codec, round_trip_ExecutionReportReplace) { check_round_trip<sbe::ExecutionReportReplace>(); }
TEST_CASE(sbe_codec, round_trip_ExecutionReportReject) { check_round_trip<sbe::ExecutionReportReject>(); }
TEST_CASE(sbe_codec, round_trip_OrderCancelReject) { check_round_trip<sbe::OrderCancelReject>(); }
TEST_CASE(sbe_codec, round_trip_OrderMassCancelReport) { check_round_trip<sbe::OrderMassCancelReport>(); }
TEST_CASE(sbe_codec, round_trip_BusinessMessageReject) { check_round_trip<sbe::BusinessMessageReject>(); }

TEST_CASE(sbe_codec, handwritten_sequence_vector) {
    // SOFH length 26, EB50; block 8, template 8, schema 22597, version 1.
    constexpr unsigned char golden[] = {0,0,0,26,0xeb,0x50,8,0,8,0,0x45,0x58,1,0,0,0,0,0,
                                        8,7,6,5,4,3,2,1};
    char bytes[sizeof golden];
    sbe::Sequence message; message.next_seq_no = 0x0102030405060708ull;
    REQUIRE(sbe::encode(message, bytes, sizeof bytes) == sizeof golden);
    CHECK(std::memcmp(bytes, golden, sizeof golden) == 0);
}
TEST_CASE(sbe_codec, handwritten_new_vector) {
    constexpr unsigned char golden[] = {0,0,0,51,0xeb,0x50,33,0,100,0,0x45,0x58,1,0,0,0,0,0,
      1,0,0,0,0,0,0,0, 0x10,0x27,0,0,0,0,0,0, 100,0,0,0,
      'A','A','P','L',0,0,0,0, '1','2','0',0,0};
    sbe::NewOrderSingle message; message.cl_ord_id=1; message.price_mantissa=10000;
    message.order_qty=100; message.symbol={'A','A','P','L',0,0,0,0};
    char bytes[sizeof golden];
    REQUIRE(sbe::encode(message, bytes, sizeof bytes) == sizeof golden);
    CHECK(std::memcmp(bytes, golden, sizeof golden) == 0);
}
TEST_CASE(sbe_codec, malformed_and_extension) {
    std::array<char, 1024> bytes{};
    sbe::Sequence sequence; sequence.next_seq_no=42;
    auto size=sbe::encode(sequence,bytes.data(),bytes.size());
    sbe::FrameView view;
    for(std::size_t prefix=0; prefix<size; ++prefix)
        CHECK(sbe::decode_frame(bytes.data(),prefix,view)==sbe::DecodeStatus::Incomplete);
    bytes[11]=0; CHECK(sbe::decode_frame(bytes.data(),size,view)==sbe::DecodeStatus::UnknownSchemaId);
    bytes[11]=0x58; bytes[12]=0;
    CHECK(sbe::decode_frame(bytes.data(),size,view)==sbe::DecodeStatus::UnsupportedSchemaVersion);
    bytes[12]=1; bytes[6]=7;
    REQUIRE(sbe::decode_frame(bytes.data(),size,view)==sbe::DecodeStatus::Ok);
    CHECK(sbe::decode(view,sequence)==sbe::DecodeStatus::BlockLengthTooShort);
    bytes[6]=8; bytes[8]=99;
    REQUIRE(sbe::decode_frame(bytes.data(),size,view)==sbe::DecodeStatus::Ok);
    CHECK(sbe::decode(view,sequence)==sbe::DecodeStatus::TemplateMismatch);
    bytes[8]=8; bytes[3]=1;
    CHECK(sbe::decode_frame(bytes.data(),size,view)==sbe::DecodeStatus::FrameLengthTooSmall);
    bytes[3]=28; bytes[6]=10; bytes[12]=2;
    REQUIRE(sbe::decode_frame(bytes.data(),size+2,view)==sbe::DecodeStatus::Ok);
    REQUIRE(sbe::decode(view,sequence)==sbe::DecodeStatus::Ok);
    CHECK_EQ(sequence.next_seq_no,42u);
}

TEST_CASE(sbe_codec, handwritten_cancel_and_negotiate_vectors) {
    // Cancel ID 2, original 1, total quantity 300. Not a leaves quantity.
    constexpr unsigned char cancel_golden[]={0,0,0,38,0xeb,0x50,20,0,101,0,0x45,0x58,1,0,0,0,0,0,
        2,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 0x2c,1,0,0};
    sbe::OrderCancelRequest cancel; cancel.cl_ord_id=2; cancel.orig_cl_ord_id=1; cancel.reduce_to_total_qty=300;
    std::array<char,128> bytes{};
    REQUIRE_EQ(sbe::encode(cancel,bytes.data(),bytes.size()),sizeof cancel_golden);
    CHECK(std::memcmp(bytes.data(),cancel_golden,sizeof cancel_golden)==0);
    constexpr unsigned char negotiate_golden[]={0,0,0,48,0xeb,0x50,25,0,1,0,0x45,0x58,1,0,0,0,1,0,
        1,2,3,4,5,6,0x47,8,0x89,10,11,12,13,14,15,16,
        8,7,6,5,4,3,2,1, 1, 3,0,'a',':','b'};
    sbe::Negotiate negotiate; negotiate.session_id={1,2,3,4,5,6,0x47,8,0x89,10,11,12,13,14,15,16};
    negotiate.timestamp_ns=0x0102030405060708ull; negotiate.credentials="a:b";
    REQUIRE_EQ(sbe::encode(negotiate,bytes.data(),bytes.size()),sizeof negotiate_golden);
    CHECK(std::memcmp(bytes.data(),negotiate_golden,sizeof negotiate_golden)==0);
    // Appended root bytes precede the variable credentials in a later schema.
    std::memmove(bytes.data()+45,bytes.data()+43,5);
    bytes[43]=99; bytes[44]=98; bytes[3]=50; bytes[6]=27; bytes[12]=2;
    sbe::FrameView view; REQUIRE(sbe::decode_frame(bytes.data(),50,view)==sbe::DecodeStatus::Ok);
    sbe::Negotiate decoded; REQUIRE(sbe::decode(view,decoded)==sbe::DecodeStatus::Ok);
    CHECK_EQ(decoded.credentials,"a:b");
    bytes[45]=127; // declared credentials length overruns the frame
    REQUIRE(sbe::decode_frame(bytes.data(),50,view)==sbe::DecodeStatus::Ok);
    CHECK(sbe::decode(view,decoded)==sbe::DecodeStatus::VariableLengthFieldOverrun);
}
