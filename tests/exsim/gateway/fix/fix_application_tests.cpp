// Application mapping (docs/GATEWAYS.md 3): golden ExecutionReport,
// OrderCancelReject and OrderMassCancelReport for every path, AvgPx and
// cum/leaves through partial fills and replaces, gateway-level rejects that
// never reach the engine, STP, auction fills, cancel-on-disconnect, drop_out.

#include "tests/exsim/gateway/fix/fix_test_support.hpp"

#include <string>
#include <vector>

using namespace fix_test;

namespace {

using Tokens = std::vector<std::string>;
constexpr std::string_view kTransactTime = "60=20261103-09:30:00.000|";

std::string er(std::string_view fields) { return std::string(fields) + std::string(kTransactTime); }

void check_golden(const ParsedFixMessage& message, std::string_view type, std::uint64_t sequence,
                  const std::string& body, std::string_view target) {
    CHECK_EQ(message.pipes(), with_pipes(exchange_message(type, sequence, body, target)));
}

std::string cancel_fields(std::string_view original, std::string_view request, std::string_view side = "1") {
    return "41=" + std::string(original) + "|11=" + std::string(request) + "|55=AAPL|54=" + std::string(side) +
           "|60=20261103-09:30:00.000|";
}

std::string replace_fields(std::string_view original, std::string_view request, std::uint64_t quantity,
                           std::string_view price, std::string_view side = "1") {
    return "41=" + std::string(original) + "|11=" + std::string(request) + "|55=AAPL|54=" + std::string(side) +
           "|38=" + std::to_string(quantity) + "|40=2|44=" + std::string(price) + "|60=20261103-09:30:00.000|";
}

struct TwoClients {
    FixTestExchange exchange;
    FixTestConnection buyer{exchange, "CLIENT1"};
    FixTestConnection seller{exchange, "CLIENT2"};
    TwoClients() {
        buyer.logon();
        seller.logon(30, false, {});
    }
    explicit TwoClients(exsim::Phase phase) : exchange(small_limits(), default_users(), phase) {
        buyer.logon();
        seller.logon(30, false, {});
    }
};

}  // namespace

TEST_CASE(gateway_fix_application, avgpx_and_cum_leaves_through_partial_fills_and_replace) {
    TwoClients clients;
    clients.seller.send("D", new_order_fields("S1", "2", 100, "150"));
    clients.seller.send("D", new_order_fields("S2", "2", 100, "150.10"));
    auto seller_messages = clients.seller.drain();
    REQUIRE_EQ(seller_messages.size(), std::size_t{2});
    check_golden(seller_messages[0], "8", 2,
                 er("37=1|11=S1|17=1|150=0|39=0|55=AAPL|54=2|38=100|44=150|32=0|31=0|151=100|14=0|6=0|"),
                 "CLIENT2");

    clients.buyer.send("D", new_order_fields("B1", "1", 300, "151"));
    auto buyer_messages = clients.buyer.drain();
    REQUIRE_EQ(buyer_messages.size(), std::size_t{3});
    check_golden(buyer_messages[0], "8", 2,
                 er("37=3|11=B1|17=1|150=0|39=0|55=AAPL|54=1|38=300|44=151|32=0|31=0|151=300|14=0|6=0|"),
                 "CLIENT1");
    check_golden(buyer_messages[1], "8", 3,
                 er("37=3|11=B1|17=2|150=F|39=1|55=AAPL|54=1|38=300|44=151|32=100|31=150|151=200|14=100|6=150|"),
                 "CLIENT1");
    check_golden(
        buyer_messages[2], "8", 4,
        er("37=3|11=B1|17=3|150=F|39=1|55=AAPL|54=1|38=300|44=151|32=100|31=150.1|151=100|14=200|6=150.05|"),
        "CLIENT1");
    seller_messages = clients.seller.drain();
    REQUIRE_EQ(seller_messages.size(), std::size_t{2});
    check_golden(seller_messages[0], "8", 4,
                 er("37=1|11=S1|17=3|150=F|39=2|55=AAPL|54=2|38=100|44=150|32=100|31=150|151=0|14=100|6=150|"),
                 "CLIENT2");

    // Replace up to 400 total: AvgPx and CumQty carry over to the new ClOrdID.
    clients.buyer.send("G", replace_fields("B1", "B1R", 400, "151"));
    buyer_messages = clients.buyer.drain();
    REQUIRE_EQ(buyer_messages.size(), std::size_t{1});
    const std::string new_order_id = buyer_messages[0].get(37);
    check_golden(buyer_messages[0], "8", 5,
                 er("37=" + new_order_id +
                    "|11=B1R|41=B1|17=4|150=5|39=1|55=AAPL|54=1|38=400|44=151|32=0|31=0|151=200|14=200|6=150.05|"),
                 "CLIENT1");

    clients.seller.send("D", new_order_fields("S3", "2", 200, "150.20"));
    buyer_messages = clients.buyer.drain();
    REQUIRE_EQ(buyer_messages.size(), std::size_t{1});
    // (100 x 150 + 100 x 150.10 + 200 x 151) / 400 = 150.525
    check_golden(buyer_messages[0], "8", 6,
                 er("37=" + new_order_id +
                    "|11=B1R|17=5|150=F|39=2|55=AAPL|54=1|38=400|44=151|32=200|31=151|151=0|14=400|6=150.525|"),
                 "CLIENT1");
    const auto* record = clients.exchange.user(1).order_record("B1R");
    REQUIRE(record != nullptr);
    CHECK_EQ(record->cum_qty, 400u);
    CHECK_EQ(record->order_status, '2');
    CHECK_EQ(clients.exchange.user(1).order_record("B1")->order_status, '4');

    // Cancel of the completed order: engine UnknownOrder -> 9 with 102=1.
    clients.buyer.send("F", cancel_fields("B1R", "C1"));
    buyer_messages = clients.buyer.drain();
    REQUIRE_EQ(buyer_messages.size(), std::size_t{1});
    check_golden(buyer_messages[0], "9", 7,
                 "37=" + new_order_id + "|11=C1|41=B1R|39=2|434=1|102=1|58=UNKNOWN_ORDER|", "CLIENT1");
}

TEST_CASE(gateway_fix_application, avgpx_rounds_to_nearest_ten_thousandth) {
    TwoClients clients;
    clients.seller.send("D", new_order_fields("S1", "2", 1, "150.01"));
    clients.seller.send("D", new_order_fields("S2", "2", 2, "150.02"));
    clients.buyer.send("D", new_order_fields("B1", "1", 3, "151"));
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{3});
    CHECK_EQ(messages[1].get(6), std::string("150.01"));
    // (1 x 150.01 + 2 x 150.02) / 3 = 150.016666... -> 150.0167
    CHECK_EQ(messages[2].get(6), std::string("150.0167"));
}

TEST_CASE(gateway_fix_application, cancel_and_replace_goldens) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("B1", "1", 50, "149"));
    clients.buyer.drain();
    clients.buyer.send("G", replace_fields("B1", "B1R", 40, "149"));
    auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    // Size reduction at the same price keeps priority and the OrderID.
    check_golden(messages[0], "8", 3,
                 er("37=1|11=B1R|41=B1|17=2|150=5|39=0|55=AAPL|54=1|38=40|44=149|32=0|31=0|151=40|14=0|6=0|"),
                 "CLIENT1");
    clients.buyer.send("F", cancel_fields("B1R", "C1"));
    messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    check_golden(messages[0], "8", 4,
                 er("37=1|11=C1|41=B1R|17=3|150=4|39=4|55=AAPL|54=1|38=40|44=149|32=0|31=0|151=0|14=0|6=0|"),
                 "CLIENT1");
}

TEST_CASE(gateway_fix_application, replace_to_quantity_at_or_below_cum_cancels) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("B1", "1", 100, "150"));
    clients.seller.send("D", new_order_fields("S1", "2", 60, "150"));
    clients.buyer.drain();
    clients.buyer.send("G", replace_fields("B1", "B1R", 50, "150"));
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(150), std::string("4"));
    CHECK_EQ(messages[0].get(39), std::string("4"));
    CHECK_EQ(messages[0].get(11), std::string("B1R"));
    CHECK_EQ(messages[0].get(41), std::string("B1"));
    CHECK_EQ(messages[0].get(14), std::string("60"));
    CHECK_EQ(messages[0].get(151), std::string("0"));
    CHECK_EQ(messages[0].get(6), std::string("150"));
    CHECK(!messages[0].has(58));
}

TEST_CASE(gateway_fix_application, cancel_reject_and_replace_reject_434_102) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("B1", "1", 50, "149"));
    clients.buyer.drain();

    clients.buyer.send("F", cancel_fields("NOPE", "C1"));
    clients.buyer.send("G", replace_fields("NOPE", "R1", 10, "149"));
    clients.buyer.send("F", cancel_fields("B1", "B1"));  // request token already used
    clients.buyer.send("G", replace_fields("B1", "R2", 10, "149.001"));  // engine: off tick
    clients.buyer.send("G", "41=B1|11=R3|55=AAPL|54=1|38=10|40=1|60=20261103-09:30:00.000|");
    clients.buyer.send("G", replace_fields("B1", "R1", 10, "149"));  // R1 was used by the rejected replace
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{6});
    check_golden(messages[0], "9", 3, "37=NONE|11=C1|41=NOPE|39=8|434=1|102=1|58=unknown order|", "CLIENT1");
    check_golden(messages[1], "9", 4, "37=NONE|11=R1|41=NOPE|39=8|434=2|102=1|58=unknown order|", "CLIENT1");
    check_golden(messages[2], "9", 5, "37=1|11=B1|41=B1|39=0|434=1|102=6|58=duplicate ClOrdID|", "CLIENT1");
    check_golden(messages[3], "9", 6, "37=1|11=R2|41=B1|39=0|434=2|102=99|58=INVALID_PRICE|", "CLIENT1");
    check_golden(messages[4], "9", 7,
                 "37=1|11=R3|41=B1|39=0|434=2|102=99|58=replace supports OrdType 2 (limit) only|", "CLIENT1");
    check_golden(messages[5], "9", 8, "37=1|11=R1|41=B1|39=0|434=2|102=6|58=duplicate ClOrdID|", "CLIENT1");
    // Only the off-tick replace reached the engine.
    CHECK_EQ(engine_command_tokens(clients.exchange, 1), (Tokens{"D:B1", "G:R2"}));
}

TEST_CASE(gateway_fix_application, new_order_rejects_goldens) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("B1", "1", 50, "149"));
    clients.buyer.drain();
    clients.buyer.send("D", new_order_fields("B1", "1", 50, "149"));
    clients.buyer.send("D", new_order_fields("B2", "1", 50, "149", "ZZZZ"));
    clients.buyer.send("D", "11=B3|55=AAPL|54=1|38=50|40=3|44=149|59=0|60=20261103-09:30:00.000|");
    clients.buyer.send("D", "11=B4|55=AAPL|54=1|38=50|40=2|44=149|59=6|60=20261103-09:30:00.000|");
    clients.buyer.send("D", new_order_fields("B5", "1", 0, "149"));
    clients.buyer.send("D", new_order_fields("B67890123456789012345", "1", 50, "149"));
    clients.buyer.send("D", new_order_fields("B7", "1", 5'000'000'000ull, "149"));
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{7});
    check_golden(messages[0], "8", 3,
                 er("37=NONE|11=B1|17=2|150=8|39=8|55=AAPL|54=1|38=50|44=149|32=0|31=0|151=0|14=0|6=0|103=6|"
                    "58=duplicate ClOrdID|"),
                 "CLIENT1");
    check_golden(messages[1], "8", 4,
                 er("37=NONE|11=B2|17=3|150=8|39=8|55=ZZZZ|54=1|38=50|44=149|32=0|31=0|151=0|14=0|6=0|103=1|"
                    "58=unknown symbol|"),
                 "CLIENT1");
    check_golden(messages[2], "8", 5,
                 er("37=NONE|11=B3|17=4|150=8|39=8|55=AAPL|54=1|38=50|44=149|32=0|31=0|151=0|14=0|6=0|103=99|"
                    "58=unsupported OrdType|"),
                 "CLIENT1");
    CHECK_EQ(messages[3].get(58), std::string("unsupported TimeInForce"));
    CHECK_EQ(messages[3].get(103), std::string("99"));
    // Zero quantity reaches the engine: InvalidQuantity -> 99 with Text.
    check_golden(messages[4], "8", 7,
                 er("37=NONE|11=B5|17=6|150=8|39=8|55=AAPL|54=1|38=0|44=149|32=0|31=0|151=0|14=0|6=0|103=99|"
                    "58=INVALID_QUANTITY|"),
                 "CLIENT1");
    CHECK_EQ(messages[5].get(58), std::string("ClOrdID longer than 20 characters"));
    CHECK_EQ(messages[6].get(58), std::string("INVALID_QUANTITY"));
    CHECK_EQ(messages[6].get(38), std::string("0"));
    CHECK_EQ(engine_command_tokens(clients.exchange, 1), (Tokens{"D:B1", "D:B5"}));
}

TEST_CASE(gateway_fix_application, market_closed_and_injected_rejects) {
    TwoClients clients(exsim::Phase::Closed);
    clients.buyer.send("D", new_order_fields("B1", "1", 50, "149"));
    clients.exchange.user(1).inject_new_order_rejects(1);
    clients.buyer.send("D", new_order_fields("B2", "1", 50, "149"));
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].get(103), std::string("2"));
    CHECK_EQ(messages[0].get(58), std::string("MARKET_CLOSED"));
    CHECK_EQ(messages[1].get(103), std::string("99"));
    CHECK_EQ(messages[1].get(58), std::string("injected"));
    CHECK_EQ(engine_command_tokens(clients.exchange, 1), (Tokens{"D:B1"}));
}

TEST_CASE(gateway_fix_application, market_order_and_flags) {
    TwoClients clients;
    clients.seller.send("D", new_order_fields("S1", "2", 100, "150"));
    clients.buyer.send("D", "11=M1|55=AAPL|54=1|38=30|40=1|59=3|60=20261103-09:30:00.000|");
    auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].get(44), std::string("0"));
    CHECK_EQ(messages[1].get(150), std::string("F"));
    CHECK_EQ(messages[1].get(31), std::string("150"));
    // Post-only that would cross is rejected by the engine.
    clients.buyer.send("D", new_order_fields("P1", "1", 10, "150", "AAPL", "18=6|"));
    messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(58), std::string("POST_ONLY_WOULD_CROSS"));
    const auto commands = clients.exchange.commands_of(1);
    REQUIRE_EQ(commands.size(), std::size_t{2});
    CHECK(commands[0].new_order.type == exsim::OrdType::Market);
    CHECK(commands[0].new_order.tif == exsim::TimeInForce::IOC);
    CHECK(commands[1].new_order.post_only);
    CHECK_EQ(commands[1].new_order.price, 150 * kDollar);
}

TEST_CASE(gateway_fix_application, mass_cancel_reports) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("B1", "1", 10, "149"));
    clients.buyer.send("D", new_order_fields("B2", "1", 10, "148", "MSFT"));
    clients.buyer.send("D", new_order_fields("B3", "1", 10, "147"));
    clients.buyer.drain();

    clients.buyer.send("q", "11=MC1|530=1|55=AAPL|60=20261103-09:30:00.000|");
    auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{3});
    check_golden(
        messages[0], "8", 5,
        er("37=1|11=MC1|41=B1|17=4|150=4|39=4|55=AAPL|54=1|38=10|44=149|32=0|31=0|151=0|14=0|6=0|58=MASS_CANCEL|"),
        "CLIENT1");
    CHECK_EQ(messages[1].get(41), std::string("B3"));
    check_golden(messages[2], "r", 7, "11=MC1|37=6|530=1|531=1|533=2|", "CLIENT1");

    clients.buyer.send("q", "11=MC2|530=7|60=20261103-09:30:00.000|");
    messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].get(41), std::string("B2"));
    CHECK_EQ(messages[0].get(55), std::string("MSFT"));
    check_golden(messages[1], "r", 9, "11=MC2|37=8|530=7|531=7|533=1|", "CLIENT1");

    // Nothing left: still a successful report with 533=0.
    clients.buyer.send("q", "11=MC3|530=7|");
    // Unknown symbol and reused ClOrdID: rejected at the gateway (531=0).
    clients.buyer.send("q", "11=MC4|530=1|55=ZZZZ|");
    clients.buyer.send("q", "11=MC1|530=7|");
    // 530=1 without Symbol: session reject.
    clients.buyer.send("q", "11=MC5|530=1|");
    messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{4});
    check_golden(messages[0], "r", 10, "11=MC3|37=9|530=7|531=7|533=0|", "CLIENT1");
    check_golden(messages[1], "r", 11, "11=MC4|37=10|530=1|531=0|533=0|", "CLIENT1");
    check_golden(messages[2], "r", 12, "11=MC1|37=11|530=7|531=0|533=0|", "CLIENT1");
    CHECK_EQ(messages[3].type(), std::string("3"));
    CHECK_EQ(messages[3].get(371), std::string("55"));
    CHECK_EQ(engine_command_tokens(clients.exchange, 1),
             (Tokens{"D:B1", "D:B2", "D:B3", "q:MC1", "q:MC2", "q:MC3"}));
}

TEST_CASE(gateway_fix_application, self_trade_prevention_default_and_override) {
    FixTestExchange exchange;
    FixTestConnection resting(exchange, "CLIENT1");
    FixTestConnection aggressor(exchange, "CLIENT3");
    resting.logon();
    aggressor.logon(30, false, {});
    // CLIENT3's configured default is CancelResting.
    resting.send("D", new_order_fields("S1", "2", 100, "150"));
    aggressor.send("D", new_order_fields("A1", "1", 100, "150"));
    auto resting_messages = resting.drain();
    REQUIRE_EQ(resting_messages.size(), std::size_t{2});
    check_golden(resting_messages[1], "8", 3,
                 er("37=1|11=S1|17=2|150=4|39=4|55=AAPL|54=2|38=100|44=150|32=0|31=0|151=0|14=0|6=0|"
                    "58=SELF_TRADE_PREVENTION|"),
                 "CLIENT1");
    auto aggressor_messages = aggressor.drain();
    REQUIRE_EQ(aggressor_messages.size(), std::size_t{1});
    CHECK_EQ(aggressor_messages[0].get(150), std::string("0"));

    // 7928=A overrides: the incoming order is canceled instead.
    resting.send("D", new_order_fields("B2", "1", 50, "150", "AAPL", "7928=A|"));
    resting_messages = resting.drain();
    REQUIRE_EQ(resting_messages.size(), std::size_t{1});  // rests behind A1 at 150
    CHECK_EQ(resting_messages[0].get(150), std::string("0"));
    aggressor.send("D", new_order_fields("S3", "2", 50, "150", "AAPL", "7928=A|"));
    aggressor_messages = aggressor.drain();
    // S3 would first meet CLIENT3's own resting A1 (same owner): the aggressor is canceled.
    REQUIRE_EQ(aggressor_messages.size(), std::size_t{2});
    CHECK_EQ(aggressor_messages[0].get(150), std::string("0"));
    CHECK_EQ(aggressor_messages[1].get(150), std::string("4"));
    CHECK_EQ(aggressor_messages[1].get(11), std::string("S3"));
    CHECK_EQ(aggressor_messages[1].get(58), std::string("SELF_TRADE_PREVENTION"));
    const auto commands = exchange.commands_of(3);
    REQUIRE_EQ(commands.size(), std::size_t{2});
    CHECK(commands[0].new_order.stp == exsim::StpMode::CancelResting);
    CHECK(commands[1].new_order.stp == exsim::StpMode::CancelAggressor);
    CHECK_EQ(commands[1].new_order.owner, 7u);
}

TEST_CASE(gateway_fix_application, same_session_fill_reports_buy_leg_first) {
    TwoClients clients;
    clients.buyer.send("D", new_order_fields("S1", "2", 100, "150"));
    clients.buyer.send("D", new_order_fields("B1", "1", 100, "150"));
    const auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{4});
    CHECK_EQ(messages[2].get(11), std::string("B1"));
    CHECK_EQ(messages[2].get(17), std::string("3"));
    CHECK_EQ(messages[3].get(11), std::string("S1"));
    CHECK_EQ(messages[3].get(17), std::string("4"));
    CHECK_EQ(messages[2].get(39), std::string("2"));
    CHECK_EQ(messages[3].get(39), std::string("2"));
}

TEST_CASE(gateway_fix_application, auction_uncross_fills_both_sessions) {
    TwoClients clients(exsim::Phase::PreOpen);
    clients.buyer.send("D", new_order_fields("B1", "1", 100, "151"));
    clients.seller.send("D", new_order_fields("S1", "2", 60, "150"));
    CHECK_EQ(clients.buyer.drain().size(), std::size_t{1});
    CHECK_EQ(clients.seller.drain().size(), std::size_t{1});
    clients.exchange.submit(exsim::SetPhase{clients.exchange.now(), 0, exsim::Phase::Continuous});
    const auto buyer_messages = clients.buyer.drain();
    const auto seller_messages = clients.seller.drain();
    REQUIRE_EQ(buyer_messages.size(), std::size_t{1});
    REQUIRE_EQ(seller_messages.size(), std::size_t{1});
    CHECK_EQ(buyer_messages[0].get(150), std::string("F"));
    CHECK_EQ(buyer_messages[0].get(39), std::string("1"));
    CHECK_EQ(buyer_messages[0].get(32), std::string("60"));
    CHECK_EQ(buyer_messages[0].get(151), std::string("40"));
    CHECK_EQ(seller_messages[0].get(39), std::string("2"));
    CHECK_EQ(buyer_messages[0].get(31), seller_messages[0].get(31));
    CHECK_EQ(buyer_messages[0].get(6), buyer_messages[0].get(31));
}

TEST_CASE(gateway_fix_application, cancel_on_disconnect_is_stored_for_recovery) {
    FixTestExchange exchange;
    FixTestConnection with_cod(exchange, "CLIENT1");
    FixTestConnection without_cod(exchange, "CLIENT3");
    with_cod.logon();
    without_cod.logon(30, false, {});
    with_cod.send("D", new_order_fields("B1", "1", 10, "149"));
    without_cod.send("D", new_order_fields("K1", "1", 10, "148"));
    with_cod.drain();
    without_cod.drain();

    with_cod.disconnect();
    without_cod.disconnect();
    CHECK(!exchange.is_bound(1));
    // The cancel was sequenced and stored at 3 while no connection was bound.
    CHECK_EQ(exchange.user(1).session_day_state().store().last_seq(), 3u);
    CHECK_EQ(exchange.user(3).session_day_state().store().last_seq(), 2u);
    CHECK(!exchange.engine().find_order(1, 1).has_value());
    CHECK(exchange.engine().find_order(3, 1).has_value());

    with_cod.reconnect();
    const ParsedFixMessage logon_response = with_cod.logon();
    CHECK_EQ(logon_response.sequence(), 4u);  // the client sees a gap of 3
    with_cod.send("2", "7=3|16=3|");
    const auto messages = with_cod.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(43), std::string("Y"));
    CHECK_EQ(messages[0].get(150), std::string("4"));
    CHECK_EQ(messages[0].get(11), std::string("B1"));
    CHECK(!messages[0].has(41));
    CHECK_EQ(messages[0].get(58), std::string("CANCEL_ON_DISCONNECT"));
}

TEST_CASE(gateway_fix_application, drop_out_stores_without_transmitting) {
    TwoClients clients;
    clients.exchange.user(1).drop_out_transmissions(1);
    clients.buyer.send("D", new_order_fields("B1", "1", 10, "149"));
    clients.buyer.send("D", new_order_fields("B2", "1", 10, "149"));
    auto messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].sequence(), 3u);
    CHECK_EQ(clients.exchange.user(1).session_day_state().transmissions_suppressed(), 1u);
    clients.buyer.send("2", "7=2|16=2|");
    messages = clients.buyer.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].get(11), std::string("B1"));
    CHECK_EQ(messages[0].get(43), std::string("Y"));
}
