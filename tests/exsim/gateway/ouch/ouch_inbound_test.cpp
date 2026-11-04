// Inbound mapping (GATEWAYS.md section 2, inbound table): OUCH request bytes
// to engine commands, and the requests the gateway answers or ignores itself.

#include "ouch_test_harness.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;

struct EnterOrderBytes {
    std::string token = "T1";
    char side = 'B';
    std::uint32_t shares = 100;
    std::string stock = "AAPL";
    std::uint32_t price = kHundredDollars;
    std::uint32_t time_in_force = 99999;
    std::string firm = "FRM1";
    char display = 'Y';
    char capacity = 'A';
    char intermarket_sweep = 'N';
    std::uint32_t minimum_quantity = 0;
    char cross_type = 'N';
    char customer_type = ' ';

    // Independent of the codec: OUCH 4.2 Enter Order layout, framed as
    // SoupBinTCP Unsequenced Data.
    [[nodiscard]] std::string frame() const {
        return Golden()
            .byte('O')
            .alpha(token, 14)
            .byte(side)
            .u32(shares)
            .alpha(stock, 8)
            .u32(price)
            .u32(time_in_force)
            .alpha(firm, 4)
            .byte(display)
            .byte(capacity)
            .byte(intermarket_sweep)
            .u32(minimum_quantity)
            .byte(cross_type)
            .byte(customer_type)
            .soup('U');
    }
};

std::string replace_order_frame(std::string_view existing, std::string_view replacement, std::uint32_t shares,
                                std::uint32_t price, std::uint32_t time_in_force = 99999, char display = 'Y') {
    return Golden()
        .byte('U')
        .alpha(existing, 14)
        .alpha(replacement, 14)
        .u32(shares)
        .u32(price)
        .u32(time_in_force)
        .byte(display)
        .byte('N')
        .u32(0)
        .soup('U');
}

std::string cancel_order_frame(std::string_view token, std::uint32_t shares) {
    return Golden().byte('X').alpha(token, 14).u32(shares).soup('U');
}

std::string order_rejected_golden(Timestamp timestamp, std::string_view token, char reason) {
    return Golden().byte('J').u64(timestamp).alpha(token, 14).byte(reason).soup('S');
}

// Logs alice in and discards the login output.
void login_alice(RawSession& session) {
    REQUIRE(session.login("alice", "alicepw"));
    session.take_frames();
}

}  // namespace

TEST_CASE(gateway_ouch_inbound, enter_order_golden_bytes_map_to_new_order) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    exchange.advance(5);
    session.send(EnterOrderBytes{}.frame());
    REQUIRE_EQ(exchange.submitted_command_count(), 1u);
    const Command& command = exchange.last_command();
    REQUIRE(command.type == CommandType::NewOrder);
    const NewOrder& order = command.new_order;
    CHECK_EQ(order.ts, kNineThirty + 5);
    CHECK_EQ(order.instrument, InstrumentId{1});
    CHECK_EQ(order.session, SessionId{1});
    CHECK_EQ(order.owner, OwnerId{1});
    CHECK_EQ(order.cl_ord_id, ClOrdId{1});
    CHECK(order.side == Side::BUY);
    CHECK(order.type == OrdType::Limit);
    CHECK(order.tif == TimeInForce::Day);
    CHECK(order.stp == StpMode::None);
    CHECK(!order.post_only);
    CHECK_EQ(order.price, kHundredDollars);
    CHECK_EQ(order.qty, Quantity{100});
    CHECK_EQ(exchange.user_state(1).find_cl_ord_id(padded_token("T1")), ClOrdId{1});
}

TEST_CASE(gateway_ouch_inbound, owner_and_stp_come_from_the_user_config) {
    TestExchange exchange;
    RawSession session(exchange);
    REQUIRE(session.login("dave", "davepw"));
    session.send(EnterOrderBytes{}.frame());
    CHECK_EQ(exchange.last_command().new_order.session, SessionId{4});
    CHECK_EQ(exchange.last_command().new_order.owner, OwnerId{1});
    CHECK(exchange.last_command().new_order.stp == StpMode::CancelResting);
}

TEST_CASE(gateway_ouch_inbound, sell_side_codes_map_to_sell) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    const std::string tokens[] = {"S", "T", "E"};
    for (const std::string& code : tokens) {
        EnterOrderBytes request;
        request.token = "SIDE" + code;
        request.side = code[0];
        session.send(request.frame());
        CHECK(exchange.last_command().new_order.side == Side::SELL);
    }
    EnterOrderBytes buy;
    buy.token = "SIDEB";
    session.send(buy.frame());
    CHECK(exchange.last_command().new_order.side == Side::BUY);
}

TEST_CASE(gateway_ouch_inbound, time_in_force_maps_to_ioc_or_day) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    const std::pair<std::uint32_t, TimeInForce> cases[] = {
        {0, TimeInForce::IOC}, {99998, TimeInForce::Day}, {99999, TimeInForce::Day}};
    int index = 0;
    for (const auto& [ouch_time_in_force, engine_time_in_force_expected] : cases) {
        EnterOrderBytes request;
        request.token = "TIF" + std::to_string(index++);
        request.time_in_force = ouch_time_in_force;
        session.send(request.frame());
        CHECK(exchange.last_command().new_order.tif == engine_time_in_force_expected);
    }
}

TEST_CASE(gateway_ouch_inbound, market_price_maps_to_a_market_order) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    EnterOrderBytes request;
    request.price = 0x7FFFFFFF;
    request.time_in_force = 0;
    session.send(request.frame());
    CHECK(exchange.last_command().new_order.type == OrdType::Market);
    // Empty book: accepted, then the remainder is cancelled as immediate-or-cancel.
    const std::vector<std::string> frames = session.take_frames();
    CHECK_EQ(sequenced_types(frames), "AC");
    CHECK_EQ(decode_ouch<ouch42::OrderAccepted>(frames[0]).price, Price{0x7FFFFFFF});
    CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[1]).reason, 'I');
}

TEST_CASE(gateway_ouch_inbound, display_p_maps_to_post_only) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    EnterOrderBytes request;
    request.display = 'P';
    session.send(request.frame());
    CHECK(exchange.last_command().new_order.post_only);
}

TEST_CASE(gateway_ouch_inbound, unknown_symbol_is_rejected_at_the_gateway_with_reason_s) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    EnterOrderBytes request;
    request.stock = "ZZZZ";
    session.send(request.frame());
    CHECK_EQ(exchange.submitted_command_count(), 0u);
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_rejected_golden(kNineThirty, "T1", 'S'));
    CHECK_EQ(exchange.user_state(1).statistics().gateway_rejects, 1u);
}

TEST_CASE(gateway_ouch_inbound, unsupported_instructions_are_rejected_with_reason_o) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    std::vector<EnterOrderBytes> requests;
    auto add = [&](auto mutate) {
        EnterOrderBytes request;
        request.token = "BAD" + std::to_string(requests.size());
        mutate(request);
        requests.push_back(request);
    };
    add([](EnterOrderBytes& r) { r.time_in_force = 30; });       // timed orders are not supported
    add([](EnterOrderBytes& r) { r.display = 'N'; });            // non-displayed
    add([](EnterOrderBytes& r) { r.display = 'A'; });            // attributable
    add([](EnterOrderBytes& r) { r.minimum_quantity = 100; });   // minimum quantity
    add([](EnterOrderBytes& r) { r.cross_type = 'O'; });         // opening cross order
    add([](EnterOrderBytes& r) { r.capacity = 'X'; });           // unknown capacity
    add([](EnterOrderBytes& r) { r.intermarket_sweep = 'Q'; });  // unknown ISE flag
    add([](EnterOrderBytes& r) { r.customer_type = 'Q'; });      // unknown customer type
    add([](EnterOrderBytes& r) { r.side = 'X'; });               // unknown side
    for (const EnterOrderBytes& request : requests) {
        session.send(request.frame());
        const std::vector<std::string> frames = session.take_frames();
        REQUIRE_EQ(frames.size(), 1u);
        CHECK(frames[0] == order_rejected_golden(kNineThirty, request.token, 'O'));
    }
    CHECK_EQ(exchange.submitted_command_count(), 0u);
    CHECK(session.connection().state() == OuchConnectionState::LoggedIn);
}

TEST_CASE(gateway_ouch_inbound, standard_capacity_and_customer_codes_are_accepted) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    int index = 0;
    for (const char capacity : {'A', 'P', 'R', 'O'}) {
        for (const char customer_type : {'R', 'N', ' '}) {
            EnterOrderBytes request;
            request.token = "OK" + std::to_string(index++);
            request.capacity = capacity;
            request.customer_type = customer_type;
            request.intermarket_sweep = index % 2 == 0 ? 'Y' : 'N';
            session.send(request.frame());
        }
    }
    CHECK_EQ(exchange.submitted_command_count(), 12u);
}

TEST_CASE(gateway_ouch_inbound, duplicate_tokens_are_ignored_for_the_whole_day) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    CHECK_EQ(session.take_types(), "A");
    // Live order: same token again is suppressed.
    session.send(EnterOrderBytes{}.frame());
    CHECK(session.take_frames().empty());
    // After the order is gone the token is still used for the day.
    session.send(cancel_order_frame("T1", 0));
    CHECK_EQ(session.take_types(), "C");
    session.send(EnterOrderBytes{}.frame());
    CHECK(session.take_frames().empty());
    // A token that was rejected at the gateway is used as well.
    EnterOrderBytes unknown_symbol;
    unknown_symbol.token = "T2";
    unknown_symbol.stock = "ZZZZ";
    session.send(unknown_symbol.frame());
    CHECK_EQ(session.take_types(), "J");
    EnterOrderBytes valid_reuse;
    valid_reuse.token = "T2";
    session.send(valid_reuse.frame());
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 2u);
    CHECK_EQ(exchange.user_state(1).statistics().duplicate_tokens_ignored, 3u);
}

TEST_CASE(gateway_ouch_inbound, tokens_are_byte_exact_and_case_sensitive) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    EnterOrderBytes lower;
    lower.token = "abc";
    EnterOrderBytes upper;
    upper.token = "ABC";
    session.send(lower.frame());
    session.send(upper.frame());
    CHECK_EQ(session.take_types(), "AA");
    CHECK_EQ(exchange.user_state(1).find_cl_ord_id(padded_token("abc")), ClOrdId{1});
    CHECK_EQ(exchange.user_state(1).find_cl_ord_id(padded_token("ABC")), ClOrdId{2});
}

TEST_CASE(gateway_ouch_inbound, full_token_table_rejects_with_reason_o) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.max_tokens = 2;
    TestExchange exchange(config);
    RawSession session(exchange);
    login_alice(session);
    for (const char* token : {"T1", "T2", "T3"}) {
        EnterOrderBytes request;
        request.token = token;
        session.send(request.frame());
    }
    const std::vector<std::string> frames = session.take_frames();
    CHECK_EQ(sequenced_types(frames), "AAJ");
    CHECK(frames[2] == order_rejected_golden(kNineThirty, "T3", 'O'));
    CHECK_EQ(exchange.submitted_command_count(), 2u);
}

TEST_CASE(gateway_ouch_inbound, replace_golden_bytes_map_to_replace_order) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    exchange.advance(7);
    session.send(replace_order_frame("T1", "T1R", 250, 999'900, 99998));
    REQUIRE_EQ(exchange.submitted_command_count(), 2u);
    const Command& command = exchange.last_command();
    REQUIRE(command.type == CommandType::Replace);
    CHECK_EQ(command.replace.ts, kNineThirty + 7);
    CHECK_EQ(command.replace.session, SessionId{1});
    CHECK_EQ(command.replace.orig_cl_ord_id, ClOrdId{1});
    CHECK_EQ(command.replace.cl_ord_id, ClOrdId{2});
    CHECK_EQ(command.replace.price, Price{999'900});
    CHECK_EQ(command.replace.qty, Quantity{250});
    CHECK(command.replace.force_priority_loss);
}

TEST_CASE(gateway_ouch_inbound, replace_of_an_unknown_token_is_ignored) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(replace_order_frame("NOPE", "NEW", 100, kHundredDollars));
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 0u);
    CHECK_EQ(exchange.user_state(1).statistics().unknown_token_requests_ignored, 1u);
}

TEST_CASE(gateway_ouch_inbound, replace_with_a_used_replacement_token_is_ignored) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    EnterOrderBytes second;
    second.token = "T2";
    session.send(second.frame());
    session.take_frames();
    session.send(replace_order_frame("T1", "T2", 300, kHundredDollars));
    session.send(replace_order_frame("T1", "T1", 300, kHundredDollars));
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 2u);
    CHECK_EQ(exchange.user_state(1).statistics().duplicate_tokens_ignored, 2u);
}

TEST_CASE(gateway_ouch_inbound, replace_that_changes_engine_tif_or_post_only_is_rejected_with_reason_o) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    session.take_frames();
    session.send(replace_order_frame("T1", "R1", 100, kHundredDollars, 0));       // DAY -> IOC
    session.send(replace_order_frame("T1", "R2", 100, kHundredDollars, 99999, 'P'));  // add post-only
    session.send(replace_order_frame("T1", "R3", 100, kHundredDollars, 99999, 'N'));  // non-displayed
    session.send(replace_order_frame("T1", "R4", 100, kHundredDollars, 30));      // timed
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 4u);
    CHECK(frames[0] == order_rejected_golden(kNineThirty, "R1", 'O'));
    CHECK(frames[1] == order_rejected_golden(kNineThirty, "R2", 'O'));
    CHECK(frames[2] == order_rejected_golden(kNineThirty, "R3", 'O'));
    CHECK(frames[3] == order_rejected_golden(kNineThirty, "R4", 'O'));
    CHECK_EQ(exchange.submitted_command_count(), 1u);
    // 99998 and 99999 are both DAY: switching between them is allowed.
    session.send(replace_order_frame("T1", "R5", 150, kHundredDollars, 99998));
    CHECK_EQ(session.take_types(), "U");
}

TEST_CASE(gateway_ouch_inbound, cancel_golden_bytes_map_to_cancel_order) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    exchange.advance(3);
    session.send(cancel_order_frame("T1", 40));
    const Command& command = exchange.last_command();
    REQUIRE(command.type == CommandType::Cancel);
    CHECK_EQ(command.cancel.ts, kNineThirty + 3);
    CHECK_EQ(command.cancel.session, SessionId{1});
    CHECK_EQ(command.cancel.orig_cl_ord_id, ClOrdId{1});
    CHECK_EQ(command.cancel.cl_ord_id, ClOrdId{0});
    CHECK_EQ(command.cancel.reduce_to_total_qty, Quantity{40});
}

TEST_CASE(gateway_ouch_inbound, cancel_of_an_unknown_token_is_ignored) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(cancel_order_frame("NOPE", 0));
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 0u);
    CHECK_EQ(exchange.user_state(1).statistics().unknown_token_requests_ignored, 1u);
}

TEST_CASE(gateway_ouch_inbound, cancel_of_a_dead_order_produces_nothing) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    session.send(cancel_order_frame("T1", 0));
    session.take_frames();
    session.send(cancel_order_frame("T1", 0));  // engine rejects UnknownOrder; OUCH sends nothing
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.submitted_command_count(), 3u);
}

TEST_CASE(gateway_ouch_inbound, modify_order_is_rejected_with_reason_o) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(EnterOrderBytes{}.frame());
    session.take_frames();
    session.send(Golden().byte('M').alpha("T1", 14).byte('S').u32(50).soup('U'));
    const std::vector<std::string> frames = session.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_rejected_golden(kNineThirty, "T1", 'O'));
    CHECK_EQ(exchange.submitted_command_count(), 1u);
}

TEST_CASE(gateway_ouch_inbound, malformed_application_messages_are_ignored_and_counted) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    std::string short_enter_order = EnterOrderBytes{}.frame();
    short_enter_order = Golden().alpha(std::string_view(short_enter_order).substr(3, 48), 48).soup('U');
    session.send(short_enter_order);                                     // 48-byte Enter Order
    session.send(Golden().byte('Q').alpha("whatever", 20).soup('U'));    // unknown OUCH type
    session.send(Golden().soup('U'));                                    // empty Unsequenced Data
    session.send(Golden().byte('X').alpha("T1", 14).soup('U'));          // short Cancel
    CHECK(session.take_frames().empty());
    CHECK_EQ(exchange.user_state(1).statistics().malformed_messages_ignored, 4u);
    CHECK(session.connection().state() == OuchConnectionState::LoggedIn);
    CHECK_EQ(exchange.submitted_command_count(), 0u);
}

TEST_CASE(gateway_ouch_inbound, client_heartbeat_and_debug_are_ignored_after_login) {
    TestExchange exchange;
    RawSession session(exchange);
    login_alice(session);
    session.send(control_frame<'R'>());
    session.send(Golden().alpha("debug text", 10).soup('+'));
    CHECK(session.take_frames().empty());
    CHECK(session.connection().state() == OuchConnectionState::LoggedIn);
}
