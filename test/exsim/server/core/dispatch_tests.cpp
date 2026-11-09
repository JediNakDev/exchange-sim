// ExchangeCore dispatch: each distinct session of a batch exactly once, in
// first-appearance order, before market data (on_events + flush per batch).

#include "test/exsim/server/core/core_test_support.hpp"

#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

// Every session call of batch `index` saw that whole batch.
bool calls_saw_whole_batch(const Recorder& recorder, std::size_t index) {
    for (const SessionCall& call : recorder.session_calls) {
        if (call.batch_number == index && call.batch_size != recorder.batch(index).size()) return false;
    }
    return true;
}

}  // namespace

TEST_CASE(server_core_dispatch, two_leg_fill_reaches_aggressor_then_resting_session_once) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'000, 100)), SubmitResult::Applied);
    REQUIRE_EQ(h.recorder.batch_count(), 1u);
    CHECK(h.recorder.sessions_of_batch(0) == std::vector<SessionId>{1});

    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'500'000, 40)), SubmitResult::Applied);
    REQUIRE_EQ(h.recorder.batch_count(), 2u);
    const auto batch = h.recorder.batch(1);
    REQUIRE_EQ(batch.size(), 2u);
    CHECK(batch[0].type == EventType::Accepted);
    CHECK(batch[1].type == EventType::Fill);
    CHECK(h.recorder.sessions_of_batch(1) == (std::vector<SessionId>{2, 1}));
    CHECK(calls_saw_whole_batch(h.recorder, 1));
    CHECK_EQ(h.recorder.flushes, 2u);
    CHECK_EQ(h.core.counters().session_dispatches, 3u);
}

TEST_CASE(server_core_dispatch, sweep_across_three_sessions_dispatches_each_once) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'000, 60)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kAapl, Side::SELL, 1'500'100, 50)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::SELL, 1'500'100, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'500'100, 200)), SubmitResult::Applied);
    const std::size_t sweep = h.recorder.batch_count() - 1;
    std::size_t fills = 0;
    for (const Event& e : h.recorder.batch(sweep)) fills += e.type == EventType::Fill ? 1 : 0;
    CHECK_EQ(fills, 3u);  // ALICE, CAROL, ALICE again: ALICE still dispatched once
    CHECK(h.recorder.sessions_of_batch(sweep) == (std::vector<SessionId>{2, 1, 3}));
    CHECK(calls_saw_whole_batch(h.recorder, sweep));
}

TEST_CASE(server_core_dispatch, self_trade_prevention_cancel_reaches_resting_owner) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kAapl, Side::SELL, 1'510'000, 10, 1)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'510'000, 10, 1, StpMode::CancelResting)),
               SubmitResult::Applied);
    const auto batch = h.recorder.batch(1);
    REQUIRE_EQ(batch.size(), 3u);
    CHECK(batch[1].type == EventType::Canceled);
    CHECK(batch[1].canceled.reason == CancelReason::SelfTradePrevention);
    CHECK_EQ(batch[1].canceled.session, 3);
    CHECK(h.recorder.sessions_of_batch(1) == (std::vector<SessionId>{1, 3}));
}

TEST_CASE(server_core_dispatch, auction_uncross_dispatches_buy_then_sell_sessions_once) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.engine().phase(kMsft) == Phase::PreOpen);
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kMsft, Side::BUY, 4'000'000, 100)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kMsft, Side::SELL, 3'990'000, 60)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kMsft, Side::SELL, 4'000'000, 40)), SubmitResult::Applied);
    SetPhase open{};
    open.instrument = kMsft;
    open.phase = Phase::Continuous;
    REQUIRE_EQ(h.core.submit(Command{open}), SubmitResult::Applied);
    const std::size_t uncross = h.recorder.batch_count() - 1;
    const auto batch = h.recorder.batch(uncross);
    REQUIRE_EQ(batch.size(), 4u);
    CHECK(batch[0].type == EventType::Fill);
    CHECK(batch[0].fill.aggressor == Aggressor::None);
    CHECK(batch[2].type == EventType::AuctionSummary);
    CHECK(batch[3].type == EventType::PhaseChanged);
    CHECK(h.recorder.sessions_of_batch(uncross) == (std::vector<SessionId>{1, 2, 3}));
    CHECK(calls_saw_whole_batch(h.recorder, uncross));
}

TEST_CASE(server_core_dispatch, mass_cancel_dispatches_its_session_once) {
    RecordingHarness h(parse_test_config());
    for (ClOrdId token = 1; token <= 4; ++token) {
        REQUIRE_EQ(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'400'000 + static_cast<Price>(token) * 100, 10)),
                   SubmitResult::Applied);
    }
    MassCancel cancel_all{};
    cancel_all.session = 1;
    cancel_all.cl_ord_id = 99;
    REQUIRE_EQ(h.core.submit(Command{cancel_all}), SubmitResult::Applied);
    const std::size_t index = h.recorder.batch_count() - 1;
    CHECK_EQ(h.recorder.batch(index).size(), 5u);
    CHECK(h.recorder.sessions_of_batch(index) == std::vector<SessionId>{1});
}

TEST_CASE(server_core_dispatch, end_of_day_expiry_dispatches_in_first_appearance_order) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kAapl, Side::BUY, 1'400'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'400'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(3, 2, kAapl, Side::SELL, 1'600'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kMsft, Side::BUY, 3'000'000, 10)), SubmitResult::Applied);
    SetPhase close_all{};
    close_all.instrument = 0;
    close_all.phase = Phase::Closed;
    REQUIRE_EQ(h.core.submit(Command{close_all}), SubmitResult::Applied);
    const std::size_t index = h.recorder.batch_count() - 1;
    CHECK_EQ(h.recorder.batch(index).size(), 6u);  // 4 EndOfDay cancels + 2 PhaseChanged
    CHECK(h.recorder.sessions_of_batch(index) == (std::vector<SessionId>{3, 1, 2}));
}

TEST_CASE(server_core_dispatch, empty_batch_is_not_dispatched) {
    RecordingHarness h(parse_test_config());
    SetPhase same{};
    same.instrument = kAapl;
    same.phase = Phase::Continuous;  // already continuous: no events
    CHECK_EQ(h.core.submit(Command{same}), SubmitResult::Applied);
    CHECK_EQ(h.recorder.batch_count(), 0u);
    CHECK_EQ(h.recorder.flushes, 0u);
    CHECK(h.recorder.session_calls.empty());
    CHECK_EQ(h.core.counters().commands_applied, 1u);
}

TEST_CASE(server_core_dispatch, rejection_reaches_requesting_session) {
    RecordingHarness h(parse_test_config());
    CancelOrder unknown{};
    unknown.session = 2;
    unknown.orig_cl_ord_id = 42;
    unknown.cl_ord_id = 43;
    CHECK_EQ(h.core.submit(Command{unknown}), SubmitResult::Applied);
    REQUIRE_EQ(h.recorder.batch_count(), 1u);
    CHECK(h.recorder.batch(0)[0].type == EventType::Rejected);
    CHECK(h.recorder.sessions_of_batch(0) == std::vector<SessionId>{2});
    // A SetPhase rejection carries session 0: market data only.
    SetPhase bad{};
    bad.instrument = 9;
    bad.phase = Phase::Halted;
    CHECK_EQ(h.core.submit(Command{bad}), SubmitResult::Applied);
    REQUIRE_EQ(h.recorder.batch_count(), 2u);
    CHECK(h.recorder.sessions_of_batch(1).empty());
}

TEST_CASE(server_core_dispatch, unconfigured_session_is_still_dispatched) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(77, 1, kAapl, Side::BUY, 1'400'000, 10)), SubmitResult::Applied);
    CHECK(h.recorder.sessions_of_batch(0) == std::vector<SessionId>{77});
}

TEST_CASE(server_core_dispatch, output_capacity_failure_applies_nothing_and_core_recovers) {
    ServerConfig config = parse_test_config();
    config.journal_path = temp_journal_path("capacity");
    RecordingHarness h(config, CoreOptions{4});
    const std::size_t capacity = h.core.event_buffer_capacity();
    REQUIRE(capacity >= 4u);
    REQUIRE(capacity < 64u);
    // Engines may use different proven preflight bounds. Reach the tiny
    // buffer's limit without baking one engine's headroom into this core test.
    ClOrdId refused_token = 0;
    for (ClOrdId token = 1; token <= capacity + 1; ++token) {
        const auto live_before = h.core.engine().live_order_count();
        const auto batches_before = h.recorder.batch_count();
        const auto journaled_before = h.core.counters().journal_records;
        const auto timestamp_before = h.core.last_command_timestamp();
        h.clock.day += kSecond;
        const auto result = h.core.submit(limit_order(1, token, kAapl, Side::BUY,
                                                    1'000'000 + static_cast<Price>(token) * 100, 1));
        if (result == SubmitResult::Applied) continue;
        REQUIRE_EQ(result, SubmitResult::OutputCapacityExceeded);
        CHECK_EQ(h.core.engine().live_order_count(), live_before);
        CHECK_EQ(h.recorder.batch_count(), batches_before);
        CHECK_EQ(h.core.counters().journal_records, journaled_before);
        CHECK_EQ(h.core.last_command_timestamp(), timestamp_before);
        refused_token = token;
        break;
    }
    REQUIRE(refused_token > 1);
    CHECK_EQ(h.core.counters().output_capacity_failures, 1u);

    // A cancel needs one slot; afterwards the refused order fits and gets
    // the next OrderId: the failed attempt consumed nothing.
    CancelOrder cancel{};
    cancel.session = 1;
    cancel.orig_cl_ord_id = refused_token - 1;
    REQUIRE_EQ(h.core.submit(Command{cancel}), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, refused_token, kAapl, Side::BUY,
                                       1'000'000 + static_cast<Price>(refused_token) * 100, 1)), SubmitResult::Applied);
    const auto batch = h.recorder.batch(h.recorder.batch_count() - 1);
    REQUIRE(batch[0].type == EventType::Accepted);
    CHECK_EQ(batch[0].accepted.order_id, static_cast<OrderId>(refused_token));
    ::unlink(config.journal_path.c_str());
}
