#include "exsim/research/position_ledger.hpp"
#include "exsim/sim/random.hpp"
#include "test/framework/test.hpp"

#include <limits>
#include <stdexcept>

using namespace exsim;
using namespace exsim::research;

TEST_CASE(position_ledger, fifo_partial_closes_with_fees_and_rebates) {
    PositionLedger ledger(10'000);
    REQUIRE(ledger.record_fill(Side::BUY, 100, 10, 2) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::BUY, 110, 5, -1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::SELL, 120, 12, 3) == LedgerStatus::Ok);
    const auto state = ledger.state();
    CHECK_EQ(state.cash, 9839);
    CHECK_EQ(state.position, 3);
    CHECK_EQ(state.gross_realized_pnl, 220);
    CHECK_EQ(state.fees, 51);
    CHECK_EQ(state.fill_count, 3u);
    CHECK_EQ(state.bought_quantity, 15u);
    CHECK_EQ(state.sold_quantity, 12u);
    CHECK_EQ(ledger.open_lot_count(), 1u);
    const auto value = ledger.valuation(115);
    REQUIRE(value.has_value());
    CHECK_EQ(value->equity, 10'184);
    CHECK_EQ(value->unrealized_pnl, 15);
    CHECK_EQ(value->net_pnl, state.gross_realized_pnl + value->unrealized_pnl - state.fees);
}

TEST_CASE(position_ledger, short_cover_and_reverse_to_long) {
    PositionLedger ledger(10'000);
    REQUIRE(ledger.record_fill(Side::SELL, 100, 10, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::BUY, 90, 4, 2) == LedgerStatus::Ok);
    const auto short_value = ledger.valuation(80);
    REQUIRE(short_value.has_value());
    CHECK_EQ(short_value->net_pnl, 142);
    CHECK_EQ(short_value->unrealized_pnl, 120);
    REQUIRE(ledger.record_fill(Side::BUY, 110, 10) == LedgerStatus::Ok);
    CHECK_EQ(ledger.state().position, 4);
    CHECK_EQ(ledger.state().gross_realized_pnl, -20);
    const auto long_value = ledger.valuation(120);
    REQUIRE(long_value.has_value());
    CHECK_EQ(long_value->net_pnl, 2);
    CHECK_EQ(long_value->unrealized_pnl, 40);
}

TEST_CASE(position_ledger, capacity_rejection_coalescing_and_ring_reuse) {
    PositionLedger ledger(100'000, 1);
    REQUIRE(ledger.record_fill(Side::BUY, 100, 2) == LedgerStatus::Ok);
    CHECK(ledger.record_fill(Side::BUY, 101, 1, 3) == LedgerStatus::CapacityExceeded);
    CHECK_EQ(ledger.state().cash, 99'800);
    CHECK_EQ(ledger.state().position, 2);
    CHECK_EQ(ledger.state().fill_count, 1u);
    CHECK_EQ(ledger.state().fees, 0);
    REQUIRE(ledger.record_fill(Side::BUY, 100, 1) == LedgerStatus::Ok);
    CHECK_EQ(ledger.open_lot_count(), 1u);
    REQUIRE(ledger.record_fill(Side::SELL, 110, 5) == LedgerStatus::Ok);
    CHECK_EQ(ledger.state().position, -2);
    CHECK_EQ(ledger.state().gross_realized_pnl, 30);
    REQUIRE(ledger.record_fill(Side::BUY, 105, 2) == LedgerStatus::Ok);
    CHECK_EQ(ledger.open_lot_count(), 0u);
    CHECK_EQ(ledger.state().gross_realized_pnl, 40);
    const auto flat_value = ledger.valuation(100);
    REQUIRE(flat_value.has_value());
    CHECK_EQ(flat_value->net_pnl, 40);
    CHECK_EQ(flat_value->unrealized_pnl, 0);
}

TEST_CASE(position_ledger, fifo_wraparound_preserves_lot_order) {
    PositionLedger ledger(100'000, 2);
    REQUIRE(ledger.record_fill(Side::BUY, 100, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::BUY, 200, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::SELL, 150, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::BUY, 300, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::SELL, 250, 1) == LedgerStatus::Ok);
    CHECK_EQ(ledger.state().gross_realized_pnl, 100);
    const auto value = ledger.valuation(300);
    REQUIRE(value.has_value());
    CHECK_EQ(value->unrealized_pnl, 0);
    CHECK_EQ(value->net_pnl, 100);
}

TEST_CASE(position_ledger, invalid_fills_and_overflows_do_not_change_state) {
    PositionLedger ledger(10'000);
    CHECK(ledger.record_fill(Side::BUY, 0, 1) == LedgerStatus::InvalidInput);
    CHECK(ledger.record_fill(Side::SELL, 10, 0) == LedgerStatus::InvalidInput);
    CHECK(ledger.record_fill(static_cast<Side>(255), 10, 1) == LedgerStatus::InvalidInput);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    CHECK(ledger.record_fill(Side::BUY, maximum, 2) == LedgerStatus::Overflow);
    CHECK(ledger.record_fill(Side::SELL, 1, 2, maximum) == LedgerStatus::Overflow);
    CHECK_EQ(ledger.state().cash, 10'000);
    CHECK_EQ(ledger.state().position, 0);
    CHECK_EQ(ledger.state().fill_count, 0u);
    CHECK(!ledger.valuation(0).has_value());
    CHECK(!ledger.valuation(-1).has_value());

    PositionLedger overflowing_cash(maximum);
    CHECK(overflowing_cash.record_fill(Side::SELL, 1, 1) == LedgerStatus::Overflow);
    CHECK_EQ(overflowing_cash.state().position, 0);
    PositionLedger overflowing_fees(maximum);
    REQUIRE(overflowing_fees.record_fill(Side::BUY, 1, 1, maximum) == LedgerStatus::Ok);
    CHECK(overflowing_fees.record_fill(Side::BUY, 1, 1, 1) == LedgerStatus::Overflow);
    CHECK_EQ(overflowing_fees.state().fill_count, 1u);
    CHECK_EQ(overflowing_fees.state().fees, maximum);
}

TEST_CASE(position_ledger, realized_overflow_is_transactional) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    PositionLedger ledger(maximum - 1, 1);
    REQUIRE(ledger.record_fill(Side::SELL, 1, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::BUY, maximum, 1) == LedgerStatus::Ok);
    REQUIRE(ledger.record_fill(Side::SELL, 1, 1) == LedgerStatus::Ok);
    CHECK(ledger.record_fill(Side::BUY, maximum, 1) == LedgerStatus::Overflow);
    CHECK_EQ(ledger.state().cash, 1);
    CHECK_EQ(ledger.state().position, -1);
    CHECK_EQ(ledger.state().gross_realized_pnl, 1 - maximum);
    CHECK_EQ(ledger.state().fill_count, 3u);
    CHECK_EQ(ledger.open_lot_count(), 1u);
}

TEST_CASE(position_ledger, absent_valuation_when_marked_value_overflows) {
    PositionLedger ledger(1000);
    REQUIRE(ledger.record_fill(Side::BUY, 1, 2) == LedgerStatus::Ok);
    CHECK(!ledger.valuation(std::numeric_limits<std::int64_t>::max()).has_value());
    const auto valid = ledger.valuation(1);
    REQUIRE(valid.has_value());
    CHECK_EQ(valid->net_pnl, 0);
}

TEST_CASE(position_ledger, zero_lot_capacity_is_invalid) {
    bool rejected = false;
    try { PositionLedger ledger(1000, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

TEST_CASE(position_ledger, seeded_fill_stream_reconciles_cash_inventory_and_pnl) {
    PositionLedger ledger(100'000'000, 64);
    sim::Rng random(6197);
    std::int64_t expected_cash = 100'000'000;
    std::int64_t expected_position = 0;
    std::int64_t expected_fees = 0;
    std::uint64_t expected_fills = 0;
    for (int iteration = 0; iteration < 20'000; ++iteration) {
        const auto side = random.below(2) == 0 ? Side::BUY : Side::SELL;
        const auto quantity = static_cast<Quantity>(random.below(20) + 1);
        const Price price = static_cast<Price>(random.below(101) + 100);
        const auto fee = static_cast<std::int64_t>(random.below(6)) - 2;
        const auto status = ledger.record_fill(side, price, quantity, fee);
        REQUIRE(status == LedgerStatus::Ok || status == LedgerStatus::CapacityExceeded);
        if (status == LedgerStatus::Ok) {
            const std::int64_t signed_quantity = side == Side::BUY ? quantity : -static_cast<std::int64_t>(quantity);
            expected_position += signed_quantity;
            expected_cash -= signed_quantity * price + fee * quantity;
            expected_fees += fee * quantity;
            ++expected_fills;
        }
        CHECK_EQ(ledger.state().cash, expected_cash);
        CHECK_EQ(ledger.state().position, expected_position);
        CHECK_EQ(ledger.state().fees, expected_fees);
        CHECK_EQ(ledger.state().fill_count, expected_fills);
        const auto value = ledger.valuation(150);
        REQUIRE(value.has_value());
        CHECK_EQ(value->equity, expected_cash + expected_position * 150);
        CHECK_EQ(value->net_pnl, ledger.state().gross_realized_pnl + value->unrealized_pnl - expected_fees);
    }
}
