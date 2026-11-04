// Outbound mapping (GATEWAYS.md section 2, outbound table): engine events to
// OUCH sequenced messages, byte for byte, and the order lifecycle scenarios.

#include "ouch_test_harness.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;
constexpr Price kTick = 100;
constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

struct AcceptedFields {
    Timestamp timestamp = kNineThirty;
    std::string token;
    char side = 'B';
    std::uint32_t shares = 0;
    std::string stock = "AAPL";
    std::uint32_t price = kHundredDollars;
    std::uint32_t time_in_force = 99999;
    std::uint64_t order_reference = 0;
    char display = 'Y';
};

std::string order_accepted_golden(const AcceptedFields& fields) {
    return Golden()
        .byte('A')
        .u64(fields.timestamp)
        .alpha(fields.token, 14)
        .byte(fields.side)
        .u32(fields.shares)
        .alpha(fields.stock, 8)
        .u32(fields.price)
        .u32(fields.time_in_force)
        .alpha("FRM1", 4)
        .byte(fields.display)
        .u64(fields.order_reference)
        .byte('A')   // capacity
        .byte('N')   // intermarket sweep
        .u32(0)      // minimum quantity
        .byte('N')   // cross type
        .byte('L')   // order state: live
        .byte(' ')   // BBO weight indicator
        .soup('S');
}

std::string order_executed_golden(Timestamp timestamp, std::string_view token, std::uint32_t shares,
                                  std::uint32_t price, char liquidity, std::uint64_t match_number) {
    return Golden()
        .byte('E')
        .u64(timestamp)
        .alpha(token, 14)
        .u32(shares)
        .u32(price)
        .byte(liquidity)
        .u64(match_number)
        .soup('S');
}

std::string order_canceled_golden(Timestamp timestamp, std::string_view token, std::uint32_t shares, char reason) {
    return Golden().byte('C').u64(timestamp).alpha(token, 14).u32(shares).byte(reason).soup('S');
}

std::string order_rejected_golden(Timestamp timestamp, std::string_view token, char reason) {
    return Golden().byte('J').u64(timestamp).alpha(token, 14).byte(reason).soup('S');
}

struct ReplacedFields {
    Timestamp timestamp = kNineThirty;
    std::string replacement_token;
    char side = 'B';
    std::uint32_t shares = 0;
    std::uint32_t price = kHundredDollars;
    std::uint32_t time_in_force = 99999;
    std::uint64_t order_reference = 0;
    std::string previous_token;
};

std::string order_replaced_golden(const ReplacedFields& fields) {
    return Golden()
        .byte('U')
        .u64(fields.timestamp)
        .alpha(fields.replacement_token, 14)
        .byte(fields.side)
        .u32(fields.shares)
        .alpha("AAPL", 8)
        .u32(fields.price)
        .u32(fields.time_in_force)
        .alpha("FRM1", 4)
        .byte('Y')
        .u64(fields.order_reference)
        .byte('A')
        .byte('N')
        .u32(0)
        .byte('N')
        .byte('L')
        .alpha(fields.previous_token, 14)
        .byte(' ')
        .soup('S');
}

// Two logged-in raw sessions (alice and bob) with their login output discarded.
struct AliceAndBob {
    TestExchange exchange;
    RawSession alice{exchange};
    RawSession bob{exchange};

    AliceAndBob() {
        REQUIRE(alice.login("alice", "alicepw"));
        REQUIRE(bob.login("bob", "bobpw"));
        alice.take_frames();
        bob.take_frames();
    }
};

}  // namespace

TEST_CASE(gateway_ouch_outbound, order_accepted_golden) {
    AliceAndBob fixture;
    fixture.exchange.advance(11);
    fixture.alice.send_message(enter_order("BUY1", 'B', 300, "AAPL", kHundredDollars, 99998));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    AcceptedFields expected;
    expected.timestamp = kNineThirty + 11;
    expected.token = "BUY1";
    expected.shares = 300;
    expected.time_in_force = 99998;
    expected.order_reference = 1;
    CHECK(frames[0] == order_accepted_golden(expected));
    CHECK(fixture.bob.take_frames().empty());
}

TEST_CASE(gateway_ouch_outbound, order_accepted_echoes_sell_short_side_and_post_only_display) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("SHORT1", 'T', 50, "MSFT", kHundredDollars, 99999, 'P'));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    AcceptedFields expected;
    expected.token = "SHORT1";
    expected.side = 'T';
    expected.shares = 50;
    expected.stock = "MSFT";
    expected.order_reference = 1;
    expected.display = 'P';
    CHECK(frames[0] == order_accepted_golden(expected));
}

TEST_CASE(gateway_ouch_outbound, engine_rejects_map_to_order_rejected_reasons) {
    AliceAndBob fixture;
    TestExchange& exchange = fixture.exchange;
    RawSession& alice = fixture.alice;
    fixture.bob.send_message(enter_order("ASK", 'S', 100, "AAPL", kHundredDollars));

    const struct {
        const char* token;
        Quantity shares;
        Price price;
        std::uint32_t time_in_force;
        char display;
        char reason;
    } cases[] = {
        {"OFFTICK", 100, kHundredDollars + 50, 99999, 'Y', 'X'},
        {"TOOHIGH", 100, 2'000'100, 99999, 'Y', 'X'},
        {"ZERO", 0, kHundredDollars, 99999, 'Y', 'Z'},
        {"TOOBIG", 1'000'001, kHundredDollars, 99999, 'Y', 'Z'},
        {"POSTX", 100, kHundredDollars, 99999, 'P', 'O'},  // post-only would cross the ask
        {"POSTIOC", 100, kHundredDollars - kTick, 0, 'P', 'O'},  // post-only IOC: invalid field
    };
    for (const auto& reject_case : cases) {
        alice.send_message(enter_order(reject_case.token, 'B', reject_case.shares, "AAPL", reject_case.price,
                                       reject_case.time_in_force, reject_case.display));
        const std::vector<std::string> frames = alice.take_frames();
        REQUIRE_EQ(frames.size(), 1u);
        CHECK(frames[0] == order_rejected_golden(kNineThirty, reject_case.token, reject_case.reason));
    }

    exchange.set_phase(kMsft, Phase::Halted);
    alice.send_message(enter_order("HALT", 'B', 100, "MSFT", kHundredDollars));
    CHECK(alice.take_frames().front() == order_rejected_golden(kNineThirty, "HALT", 'H'));

    exchange.set_phase(kMsft, Phase::Closed);
    alice.send_message(enter_order("CLOSED", 'B', 100, "MSFT", kHundredDollars));
    CHECK(alice.take_frames().front() == order_rejected_golden(kNineThirty, "CLOSED", 'C'));

    exchange.set_phase(kMsft, Phase::PreOpen);
    alice.send_message(enter_order("IOCPRE", 'B', 100, "MSFT", kHundredDollars, 0));
    CHECK(alice.take_frames().front() == order_rejected_golden(kNineThirty, "IOCPRE", 'R'));
    alice.send_message(enter_order("MKTPRE", 'B', 100, "MSFT", 0x7FFFFFFF, 99999));
    CHECK(alice.take_frames().front() == order_rejected_golden(kNineThirty, "MKTPRE", 'R'));
    alice.send_message(enter_order("DAYPRE", 'B', 100, "MSFT", kHundredDollars, 99999));
    CHECK_EQ(alice.take_types(), "A");
}

TEST_CASE(gateway_ouch_outbound, order_executed_golden_for_both_counterparties) {
    AliceAndBob fixture;
    fixture.bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));
    CHECK_EQ(fixture.bob.take_types(), "A");
    fixture.exchange.advance(20);
    fixture.alice.send_message(enter_order("BID1", 'B', 60, "AAPL", kHundredDollars + 10 * kTick));
    const Timestamp fill_time = kNineThirty + 20;

    const std::vector<std::string> alice_frames = fixture.alice.take_frames();
    REQUIRE_EQ(alice_frames.size(), 2u);
    CHECK_EQ(ouch_type(alice_frames[0]), 'A');
    CHECK(alice_frames[1] == order_executed_golden(fill_time, "BID1", 60, kHundredDollars, 'R', 1));

    const std::vector<std::string> bob_frames = fixture.bob.take_frames();
    REQUIRE_EQ(bob_frames.size(), 1u);
    CHECK(bob_frames[0] == order_executed_golden(fill_time, "ASK1", 60, kHundredDollars, 'A', 1));
}

TEST_CASE(gateway_ouch_outbound, order_canceled_golden) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.alice.take_frames();
    fixture.exchange.advance(9);
    fixture.alice.send_message(cancel_order("BID1", 0));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty + 9, "BID1", 100, 'U'));
    CHECK(!fixture.exchange.find_live_order(1, padded_token("BID1")).has_value());
}

TEST_CASE(gateway_ouch_outbound, order_replaced_golden) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.alice.take_frames();
    fixture.exchange.advance(4);
    fixture.alice.send_message(replace_order("BID1", "BID2", 150, kHundredDollars - kTick, 99998));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    ReplacedFields expected;
    expected.timestamp = kNineThirty + 4;
    expected.replacement_token = "BID2";
    expected.shares = 150;
    expected.price = kHundredDollars - kTick;
    expected.time_in_force = 99998;
    expected.order_reference = 2;  // OUCH replace always loses priority: new order id
    expected.previous_token = "BID1";
    CHECK(frames[0] == order_replaced_golden(expected));
    const std::optional<OrderInfo> order = fixture.exchange.find_live_order(1, padded_token("BID2"));
    REQUIRE(order.has_value());
    CHECK_EQ(order->order_id, OrderId{2});
    CHECK_EQ(order->leaves_qty, Quantity{150});
}

TEST_CASE(gateway_ouch_outbound, partial_fill_then_replace_then_cancel) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.bob.send_message(enter_order("ASK1", 'S', 30, "AAPL", kHundredDollars));
    CHECK_EQ(fixture.alice.take_types(), "AE");
    CHECK_EQ(fixture.bob.take_types(), "AE");

    // Replace to a total of 80: leaves = 80 - 30 executed = 50.
    fixture.alice.send_message(replace_order("BID1", "BID2", 80, kHundredDollars - kTick));
    std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    const ouch42::OrderReplaced replaced = decode_ouch<ouch42::OrderReplaced>(frames[0]);
    CHECK_EQ(token_text(replaced.replacement_token), "BID2");
    CHECK_EQ(token_text(replaced.previous_token), "BID1");
    CHECK_EQ(replaced.shares, Quantity{50});
    CHECK_EQ(replaced.price, kHundredDollars - kTick);
    CHECK_EQ(replaced.order_reference, 3u);

    // The old token no longer names a live order.
    fixture.alice.send_message(cancel_order("BID1", 0));
    CHECK(fixture.alice.take_frames().empty());

    fixture.alice.send_message(cancel_order("BID2", 0));
    frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    const ouch42::OrderCanceled canceled = decode_ouch<ouch42::OrderCanceled>(frames[0]);
    CHECK_EQ(token_text(canceled.token), "BID2");
    CHECK_EQ(canceled.shares, Quantity{50});
    CHECK_EQ(canceled.reason, 'U');
}

TEST_CASE(gateway_ouch_outbound, replace_to_total_at_or_below_executed_cancels_with_the_old_token) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.bob.send_message(enter_order("ASK1", 'S', 40, "AAPL", kHundredDollars));
    fixture.alice.take_frames();
    fixture.alice.send_message(replace_order("BID1", "BID2", 40, kHundredDollars));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 60, 'U'));
}

TEST_CASE(gateway_ouch_outbound, engine_rejected_replace_keeps_the_original_order) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.alice.take_frames();
    fixture.alice.send_message(replace_order("BID1", "BID2", 100, kHundredDollars + 1));
    fixture.alice.send_message(replace_order("BID1", "BID3", 0, kHundredDollars));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0] == order_rejected_golden(kNineThirty, "BID2", 'X'));
    CHECK(frames[1] == order_rejected_golden(kNineThirty, "BID3", 'Z'));
    CHECK(fixture.exchange.find_live_order(1, padded_token("BID1")).has_value());
}

TEST_CASE(gateway_ouch_outbound, replace_to_a_crossing_price_executes_after_the_replaced_message) {
    AliceAndBob fixture;
    fixture.bob.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars - kTick));
    fixture.alice.take_frames();
    fixture.bob.take_frames();
    fixture.alice.send_message(replace_order("BID1", "BID2", 100, kHundredDollars));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    CHECK_EQ(sequenced_types(frames), "UE");
    const ouch42::OrderExecuted executed = decode_ouch<ouch42::OrderExecuted>(frames[1]);
    CHECK_EQ(token_text(executed.token), "BID2");
    CHECK_EQ(executed.liquidity, 'R');
    CHECK_EQ(fixture.bob.take_types(), "E");
}

TEST_CASE(gateway_ouch_outbound, cancel_reduces_cancels_fully_or_does_nothing) {
    AliceAndBob fixture;
    TestExchange& exchange = fixture.exchange;
    RawSession& alice = fixture.alice;
    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    alice.send_message(enter_order("BID2", 'B', 100, "AAPL", kHundredDollars));
    fixture.bob.send_message(enter_order("ASK1", 'S', 30, "AAPL", kHundredDollars));
    CHECK_EQ(alice.take_types(), "AAE");  // BID1: executed 30, leaves 70
    const OrderId original_order_id = exchange.find_live_order(1, padded_token("BID1"))->order_id;

    // Intended total 80: leaves become 80 - 30 = 50, a 20-share decrement.
    alice.send_message(cancel_order("BID1", 80));
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 20, 'U'));
    std::optional<OrderInfo> order = exchange.find_live_order(1, padded_token("BID1"));
    REQUIRE(order.has_value());
    CHECK_EQ(order->leaves_qty, Quantity{50});
    CHECK_EQ(order->order_id, original_order_id);  // reduction keeps the order and its priority

    // Priority is kept: the next sell still executes against BID1 before BID2.
    fixture.bob.send_message(enter_order("ASK2", 'S', 10, "AAPL", kHundredDollars));
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(token_text(decode_ouch<ouch42::OrderExecuted>(frames[0]).token), "BID1");

    // Now executed 40, leaves 40. Intended total 120 is above 40 + 40: no-op, no message.
    alice.send_message(cancel_order("BID1", 120));
    CHECK(alice.take_frames().empty());
    CHECK_EQ(exchange.find_live_order(1, padded_token("BID1"))->leaves_qty, Quantity{40});

    // Intended total at or below executed: cancels the rest.
    alice.send_message(cancel_order("BID1", 40));
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 40, 'U'));
    CHECK(!exchange.find_live_order(1, padded_token("BID1")).has_value());

    // Shares 0 always cancels fully.
    alice.send_message(cancel_order("BID2", 0));
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID2", 100, 'U'));
}

TEST_CASE(gateway_ouch_outbound, halted_instrument_allows_reductions_only) {
    AliceAndBob fixture;
    TestExchange& exchange = fixture.exchange;
    RawSession& alice = fixture.alice;
    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    alice.take_frames();
    exchange.set_phase(kAapl, Phase::Halted);
    CHECK(alice.take_frames().empty());  // PhaseChanged has no OUCH message

    alice.send_message(cancel_order("BID1", 60));
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 40, 'U'));

    alice.send_message(replace_order("BID1", "BID2", 60, kHundredDollars - kTick));
    alice.send_message(enter_order("BID3", 'B', 10, "AAPL", kHundredDollars));
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0] == order_rejected_golden(kNineThirty, "BID2", 'H'));
    CHECK(frames[1] == order_rejected_golden(kNineThirty, "BID3", 'H'));

    alice.send_message(cancel_order("BID1", 0));
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 60, 'U'));
}

TEST_CASE(gateway_ouch_outbound, ioc_order_reports_accepted_executed_canceled_in_order) {
    AliceAndBob fixture;
    fixture.bob.send_message(enter_order("ASK1", 'S', 50, "AAPL", kHundredDollars));
    fixture.alice.send_message(enter_order("IOC1", 'B', 80, "AAPL", kHundredDollars, 0));
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "AEC");
    CHECK_EQ(decode_ouch<ouch42::OrderAccepted>(frames[0]).time_in_force, 0u);
    CHECK(frames[1] == order_executed_golden(kNineThirty, "IOC1", 50, kHundredDollars, 'R', 1));
    CHECK(frames[2] == order_canceled_golden(kNineThirty, "IOC1", 30, 'I'));
    CHECK(!fixture.exchange.find_live_order(1, padded_token("IOC1")).has_value());
}

TEST_CASE(gateway_ouch_outbound, both_fill_legs_in_one_session_report_buy_leg_first) {
    AliceAndBob fixture;
    RawSession& alice = fixture.alice;
    // Alice's buy aggresses her own resting sell (STP None).
    alice.send_message(enter_order("SELL1", 'S', 100, "AAPL", kHundredDollars));
    alice.send_message(enter_order("BUY1", 'B', 100, "AAPL", kHundredDollars));
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "AAEE");
    CHECK(frames[2] == order_executed_golden(kNineThirty, "BUY1", 100, kHundredDollars, 'R', 1));
    CHECK(frames[3] == order_executed_golden(kNineThirty, "SELL1", 100, kHundredDollars, 'A', 1));

    // Sell aggressor: still the buy leg first.
    alice.send_message(enter_order("BUY2", 'B', 70, "AAPL", kHundredDollars));
    alice.send_message(enter_order("SELL2", 'S', 70, "AAPL", kHundredDollars));
    frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "AAEE");
    CHECK(frames[2] == order_executed_golden(kNineThirty, "BUY2", 70, kHundredDollars, 'A', 2));
    CHECK(frames[3] == order_executed_golden(kNineThirty, "SELL2", 70, kHundredDollars, 'R', 2));
    CHECK(fixture.bob.take_frames().empty());
}

TEST_CASE(gateway_ouch_outbound, self_trade_prevention_cancels_the_resting_order) {
    TestExchange exchange;
    RawSession alice(exchange);
    RawSession dave(exchange);  // same owner as alice, STP CancelResting
    REQUIRE(alice.login("alice", "alicepw"));
    REQUIRE(dave.login("dave", "davepw"));
    alice.take_frames();
    dave.take_frames();
    alice.send_message(enter_order("ASK1", 'S', 100, "AAPL", kHundredDollars));
    alice.take_frames();
    dave.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    std::vector<std::string> alice_frames = alice.take_frames();
    REQUIRE_EQ(alice_frames.size(), 1u);
    CHECK(alice_frames[0] == order_canceled_golden(kNineThirty, "ASK1", 100, 'Q'));
    CHECK_EQ(dave.take_types(), "A");  // dave's order rests, nothing executed
    const std::optional<OrderInfo> dave_order = exchange.find_live_order(4, padded_token("BID1"));
    REQUIRE(dave_order.has_value());
    CHECK_EQ(dave_order->leaves_qty, Quantity{100});
}

TEST_CASE(gateway_ouch_outbound, auction_fills_carry_cross_liquidity_flags) {
    AliceAndBob fixture;
    TestExchange& exchange = fixture.exchange;
    RawSession& alice = fixture.alice;
    RawSession& bob = fixture.bob;

    // Opening cross: PreOpen -> Continuous.
    exchange.set_phase(kAapl, Phase::PreOpen);
    alice.send_message(enter_order("OPENB", 'B', 100, "AAPL", kHundredDollars + kTick));
    bob.send_message(enter_order("OPENS", 'S', 100, "AAPL", kHundredDollars));
    alice.take_frames();
    bob.take_frames();
    exchange.set_phase(kAapl, Phase::Continuous);
    std::vector<std::string> alice_frames = alice.take_frames();
    std::vector<std::string> bob_frames = bob.take_frames();
    REQUIRE_EQ(alice_frames.size(), 1u);
    REQUIRE_EQ(bob_frames.size(), 1u);
    const ouch42::OrderExecuted opening_buy = decode_ouch<ouch42::OrderExecuted>(alice_frames[0]);
    CHECK_EQ(opening_buy.liquidity, 'O');
    CHECK_EQ(opening_buy.shares, Quantity{100});
    CHECK_EQ(decode_ouch<ouch42::OrderExecuted>(bob_frames[0]).liquidity, 'O');
    CHECK_EQ(decode_ouch<ouch42::OrderExecuted>(bob_frames[0]).match_number, opening_buy.match_number);

    // Closing cross: PreClose -> Closed; the unmatched remainder expires end of day.
    exchange.set_phase(kAapl, Phase::PreClose);
    alice.send_message(enter_order("CLOSEB", 'B', 150, "AAPL", kHundredDollars + kTick));
    bob.send_message(enter_order("CLOSES", 'S', 100, "AAPL", kHundredDollars));
    alice.take_frames();
    bob.take_frames();
    exchange.set_phase(kAapl, Phase::Closed);
    alice_frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(alice_frames), "EC");
    CHECK_EQ(decode_ouch<ouch42::OrderExecuted>(alice_frames[0]).liquidity, 'C');
    const ouch42::OrderCanceled expired = decode_ouch<ouch42::OrderCanceled>(alice_frames[1]);
    CHECK_EQ(expired.shares, Quantity{50});
    CHECK_EQ(expired.reason, 'E');
    bob_frames = bob.take_frames();
    REQUIRE_EQ(sequenced_types(bob_frames), "E");
    CHECK_EQ(decode_ouch<ouch42::OrderExecuted>(bob_frames[0]).liquidity, 'C');
}

TEST_CASE(gateway_ouch_outbound, uncross_after_a_halt_is_flagged_h_per_instrument) {
    AliceAndBob fixture;
    TestExchange& exchange = fixture.exchange;
    RawSession& alice = fixture.alice;
    RawSession& bob = fixture.bob;
    exchange.set_phase(0, Phase::PreOpen);
    for (const char* stock : {"AAPL", "MSFT"}) {
        alice.send_message(enter_order(std::string("B") + stock, 'B', 100, stock, kHundredDollars + kTick));
        bob.send_message(enter_order(std::string("S") + stock, 'S', 100, stock, kHundredDollars));
    }
    exchange.set_phase(kMsft, Phase::Halted);  // keeps its crossed book without matching
    alice.take_frames();
    bob.take_frames();

    // One all-instrument SetPhase batch: AAPL leaves PreOpen ('O'), MSFT leaves Halted ('H').
    exchange.set_phase(0, Phase::Continuous);
    const std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "EE");
    const ouch42::OrderExecuted aapl = decode_ouch<ouch42::OrderExecuted>(frames[0]);
    const ouch42::OrderExecuted msft = decode_ouch<ouch42::OrderExecuted>(frames[1]);
    CHECK_EQ(token_text(aapl.token), "BAAPL");
    CHECK_EQ(aapl.liquidity, 'O');
    CHECK_EQ(token_text(msft.token), "BMSFT");
    CHECK_EQ(msft.liquidity, 'H');
    CHECK_EQ(sequenced_types(bob.take_frames()), "EE");
}

TEST_CASE(gateway_ouch_outbound, end_of_day_system_event_golden) {
    AliceAndBob fixture;
    fixture.exchange.advance(42);
    fixture.exchange.user_state(1).append_end_of_day_event(fixture.exchange.now(), fixture.exchange);
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == Golden().byte('S').u64(kNineThirty + 42).byte('E').soup('S'));
}

TEST_CASE(gateway_ouch_outbound, rested_mass_cancel_done_and_phase_changes_send_nothing) {
    AliceAndBob fixture;
    fixture.exchange.set_phase(0, Phase::PreOpen);
    fixture.exchange.set_phase(0, Phase::Continuous);
    MassCancel mass_cancel;
    mass_cancel.session = 1;
    fixture.exchange.submit(Command{mass_cancel});  // no live orders: only MassCancelDone
    CHECK(fixture.alice.take_frames().empty());
    CHECK(fixture.bob.take_frames().empty());
}

TEST_CASE(gateway_ouch_outbound, mass_cancel_reports_user_cancels) {
    AliceAndBob fixture;
    fixture.alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredDollars));
    fixture.alice.send_message(enter_order("BID2", 'B', 200, "MSFT", kHundredDollars));
    fixture.alice.take_frames();
    MassCancel mass_cancel;
    mass_cancel.session = 1;
    fixture.exchange.submit(Command{mass_cancel});
    const std::vector<std::string> frames = fixture.alice.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0] == order_canceled_golden(kNineThirty, "BID1", 100, 'U'));
    CHECK(frames[1] == order_canceled_golden(kNineThirty, "BID2", 200, 'U'));
}

TEST_CASE(gateway_ouch_outbound, sequence_numbers_are_implicit_and_contiguous) {
    AliceAndBob fixture;
    for (int index = 0; index < 5; ++index) {
        fixture.alice.send_message(enter_order("B" + std::to_string(index), 'B', 10, "AAPL", kHundredDollars));
    }
    const gateway::SequencedStore& store = fixture.exchange.user_state(1).store();
    CHECK_EQ(store.last_seq(), 6u);  // S + 5 A
    CHECK_EQ(fixture.alice.connection().next_sequence_to_send(), 7u);
    // What was written equals the stored frames, in sequence order.
    std::string stored;
    for (std::uint64_t sequence = 2; sequence <= store.last_seq(); ++sequence) {
        const gateway::SequencedStore::Message message = store.get(sequence);
        stored.append(message.bytes.data(), message.bytes.size());
    }
    CHECK(fixture.alice.connection().sink().view() == stored);
}
