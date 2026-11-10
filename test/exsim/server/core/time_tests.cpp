// ExchangeCore time handling: strictly increasing command timestamps whatever
// the clock does, a monotonic now(), the trading-day window and the
// end-of-day shutdown the timer triggers.

#include "test/exsim/server/core/core_test_support.hpp"

#include <string>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr Timestamp kHour = 3600 * kSecond;
constexpr Timestamp kDayStart = 9 * kHour + 30 * 60 * kSecond;  // 09:30
constexpr Timestamp kDayEnd = 16 * kHour;                        // 16:00

const char* const kTradingDay = R"(
[exchange]
trading_day_start = 09:30
trading_day_end = 16:00
)";

// One stamp per applied command (every event of a batch shares it).
std::vector<Timestamp> stamps_of_batches(const Recorder& recorder) {
    std::vector<Timestamp> stamps;
    for (std::size_t index = 0; index < recorder.batch_count(); ++index) {
        const auto batch = recorder.batch(index);
        stamps.push_back(batch[0].ts);
        for (const Event& event : batch) CHECK_EQ(event.ts, batch[0].ts);
    }
    return stamps;
}

}  // namespace

TEST_CASE(server_core_time, command_timestamps_strictly_increase_when_the_clock_repeats) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 5 * kSecond;
    for (ClOrdId token = 1; token <= 3; ++token) {
        REQUIRE_EQ(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    }
    CHECK(stamps_of_batches(h.recorder) ==
          (std::vector<Timestamp>{5 * kSecond, 5 * kSecond + 1, 5 * kSecond + 2}));
    CHECK_EQ(h.core.last_command_timestamp(), 5 * kSecond + 2);
}

TEST_CASE(server_core_time, command_timestamps_strictly_increase_when_the_clock_steps_back) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 5 * kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.clock.day = 2 * kSecond;  // NTP step backwards
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.clock.day = 10 * kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 3, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(stamps_of_batches(h.recorder) ==
          (std::vector<Timestamp>{5 * kSecond, 5 * kSecond + 1, 10 * kSecond}));
}

TEST_CASE(server_core_time, first_command_may_be_stamped_at_midnight) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 0;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(stamps_of_batches(h.recorder) == (std::vector<Timestamp>{0, 1}));
}

TEST_CASE(server_core_time, every_event_of_a_batch_carries_the_command_stamp) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 7 * kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'500'000, 10)), SubmitResult::Applied);
    const auto batch = h.recorder.batch(1);
    REQUIRE_EQ(batch.size(), 2u);
    CHECK_EQ(batch[0].ts, 7 * kSecond + 1);
    CHECK_EQ(batch[1].ts, 7 * kSecond + 1);
}

TEST_CASE(server_core_time, now_is_monotonic_and_never_behind_a_stamped_command) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 5 * kSecond;
    CHECK_EQ(h.core.now(), 5 * kSecond);
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.now(), 5 * kSecond + 1);
    h.clock.day = kSecond;
    CHECK_EQ(h.core.now(), 5 * kSecond + 1);
    h.clock.day = 6 * kSecond;
    CHECK_EQ(h.core.now(), 6 * kSecond);
    CHECK_EQ(h.core.realtime_ns(), h.clock.real);
}

TEST_CASE(server_core_time, commands_outside_the_trading_day_are_refused_untouched) {
    RecordingHarness h(parse_test_config(kTradingDay));
    h.clock.day = kDayStart - 1;
    CHECK_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::OutsideTradingDay);
    h.clock.day = kDayEnd;
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::OutsideTradingDay);
    CHECK_EQ(h.recorder.batch_count(), 0u);
    CHECK(h.recorder.session_calls.empty());
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
    CHECK_EQ(h.core.counters().commands_outside_trading_day, 2u);
    CHECK_EQ(h.core.counters().commands_applied, 0u);

    h.clock.day = kDayStart;
    CHECK_EQ(h.core.submit(limit_order(1, 3, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.clock.day = kDayEnd - 1;
    CHECK_EQ(h.core.submit(limit_order(1, 4, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.engine().live_order_count(), 2u);
}

TEST_CASE(server_core_time, a_stamp_pushed_past_the_day_end_is_refused) {
    RecordingHarness h(parse_test_config(kTradingDay));
    h.clock.day = kDayEnd - 1;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    // Same clock reading: the strictly-increasing stamp would be kDayEnd.
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::OutsideTradingDay);
    CHECK_EQ(h.core.last_command_timestamp(), kDayEnd - 1);
}

TEST_CASE(server_core_time, admin_phase_outside_the_trading_day_reports_the_reason) {
    RecordingHarness h(parse_test_config(kTradingDay));
    h.clock.day = kDayStart - kSecond;
    std::string reply;
    h.core.execute_line("phase AAPL halted", reply);
    CHECK_EQ(reply, std::string("ERR phase not applied: outside trading day\n"));
    CHECK(h.core.engine().phase(kAapl) == Phase::Continuous);
}

TEST_CASE(server_core_time, timer_at_the_day_end_closes_everything_inside_the_day) {
    RecordingHarness h(parse_test_config(kTradingDay));
    h.clock.day = kDayStart + kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.core.timer(kDayEnd - 1);
    CHECK(!h.core.shutdown_requested());

    h.clock.day = kDayEnd + 5 * kSecond;
    h.core.timer(kDayEnd + 5 * kSecond);
    CHECK(h.core.shutdown_requested());
    const auto closing = h.recorder.batch(h.recorder.batch_count() - 1);
    std::size_t phase_changes = 0;
    for (const Event& event : closing) {
        CHECK_EQ(event.ts, kDayEnd - 1);  // clamped to the last nanosecond of the day
        phase_changes += event.type == EventType::PhaseChanged ? 1 : 0;
    }
    CHECK_EQ(phase_changes, 2u);
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
    CHECK(h.core.engine().phase(2) == Phase::Closed);
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK_EQ(h.recorder.market_data_shutdown_calls, 1u);

    // Idempotent; later commands are refused as after shutdown, which wins
    // over the trading-day check.
    h.core.timer(kDayEnd + 6 * kSecond);
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::ExchangeShutDown);
    CHECK_EQ(h.core.counters().commands_after_shutdown, 1u);
}

TEST_CASE(server_core_time, submit_after_admin_shutdown_is_refused_and_not_journaled) {
    RecordingHarness h(parse_test_config());
    h.clock.day = kDayStart + kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.core.shutdown();
    REQUIRE(h.core.shutdown_requested());
    const std::size_t batches_before = h.recorder.batch_count();
    h.clock.day += kSecond;
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::ExchangeShutDown);
    CHECK_EQ(h.recorder.batch_count(), batches_before);
    CHECK_EQ(h.core.counters().commands_after_shutdown, 1u);
    CHECK_EQ(server::to_string(SubmitResult::ExchangeShutDown), std::string_view("exchange shut down"));
}

TEST_CASE(server_core_time, timer_value_handed_on_never_goes_backwards) {
    RecordingHarness h(parse_test_config());
    h.clock.day = 9 * kSecond;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    // A loop time behind the last stamped command is lifted to it, so bots
    // and market-data timers never see time run backwards.
    h.core.timer(kSecond);
    h.core.timer(12 * kSecond);
    h.core.timer(2 * kSecond);
    CHECK(h.recorder.market_data_timer_times == (std::vector<Timestamp>{9 * kSecond, 12 * kSecond, 12 * kSecond}));
    CHECK(!h.core.shutdown_requested());
}
