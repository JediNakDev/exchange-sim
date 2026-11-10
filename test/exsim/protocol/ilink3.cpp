#include "protocol/ilink3.hpp"
#include "test/framework/test.hpp"

namespace wire = exsim::protocol::ilink3;
namespace schema = wire::schema;
namespace cme = exsim::protocol::cme;
using exsim::protocol::Status;

TEST_CASE(proto_ilink3, cme_new_order_example_with_version9_extensions) {
    // CME's published v0 NewOrderSingle packet, with its header advanced
    // to v9 and the two appended optional PRICE9 fields set to null.
    // https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/714113056
    constexpr std::string_view hex =
        "8000FECA740002020800000000E876481700000001000000CBA70D000101000000"
        "437563756D626572000000000000000000000000595A373334000000000000000000000000000000"
        "7B00000000000000DE02000000000000D7096EAB9B27BB15FFFFFFFFFFFFFF7F4D696E736B"
        "0000000000000000FFFF3200000000FFFFFF";
    std::vector<char> expected;
    auto digit = [](char value) { return value >= '0' && value <= '9' ? value - '0' : value - 'A' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2) expected.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    REQUIRE_EQ(expected.size(), 128u);
    expected[0] = char(144); expected[4] = char(132); expected[10] = 9;
    for (unsigned field = 0; field < 2; ++field) {
        expected.insert(expected.end(), 7, char(0xff)); expected.push_back(char(0x7f));
    }
    using M = schema::NewOrderSingle514;
    wire::Root<M> root;
    root.set(M::Price, 100000000000ll); root.set(M::OrderQty, 1); root.set(M::SecurityID, 894923);
    root.set(M::Side, 1); root.set(M::SeqNum, 1); root.set(M::SenderID, "Cucumber"); root.set(M::ClOrdID, "YZ734");
    root.set(M::PartyDetailsListReqID, 123); root.set(M::OrderRequestID, 734);
    root.set(M::SendingTimeEpoch, 1565888844990908887ull); root.set(M::Location, "Minsk");
    root.set(M::MinQty, 0); root.set(M::DisplayQty, 0); root.set(M::OrdType, '2'); root.set(M::TimeInForce, 0);
    std::array<char, 144> encoded{};
    REQUIRE_EQ(root.encode(encoded), encoded.size());
    CHECK_EQ(std::vector<char>(encoded.begin(), encoded.end()), expected);
    cme::MessageView decoded;
    REQUIRE_EQ(cme::decode_ilink(encoded, decoded), Status::ok);
    REQUIRE(wire::valid<M>(decoded));
    CHECK_EQ(wire::get(decoded, M::ClOrdID), "YZ734");
    CHECK_EQ(wire::get(decoded, M::DiscretionPrice), cme::kNullPrice9);
    CHECK_EQ(wire::get(decoded, M::ReservationPrice), cme::kNullPrice9);
}

TEST_CASE(proto_ilink3, fixed_text_validation_and_credentials_tail) {
    using M = schema::Negotiate500;
    wire::Root<M> root;
    REQUIRE(root.set(M::AccessKeyID, "KEY"));
    CHECK(!root.set(M::Session, "TOO-LONG"));
    std::array<char, 128> bytes{};
    const auto size = root.encode(bytes);
    CHECK_EQ(size, 90u);
    cme::MessageView view;
    REQUIRE_EQ(cme::decode_ilink(std::span(bytes).first(size), view), Status::ok);
    CHECK_EQ(view.trailing().size(), 2u);
    CHECK(wire::valid_text(view, M::AccessKeyID));
    bytes[12 + M::AccessKeyID.offset + 5] = 'X';
    CHECK(!wire::valid_text(view, M::AccessKeyID));
    view.header.version = 8; CHECK(!wire::valid<M>(view));
}
