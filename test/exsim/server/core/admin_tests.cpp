// ExchangeCore admin execution: every AdminOp, its reply text (lines ending
// with OK or ERR <message>) and its effect; market-data faults through each
// shape of MarketData policy.

#include "test/exsim/server/core/core_test_support.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

// Pushes `packets` one-byte packets through the injector; returns how many
// reached the sink.
std::size_t deliver_packets(sim::PacketFaultInjector& injector, std::size_t packets) {
    std::size_t delivered = 0;
    const std::array<char, 1> packet{'x'};
    for (std::size_t i = 0; i < packets; ++i) {
        injector.submit(std::span<const char>(packet), [&delivered](std::span<const char>) { ++delivered; });
    }
    return delivered;
}

}  // namespace

TEST_CASE(server_core_admin, help_lists_the_commands) {
    RecordingHarness h(parse_test_config());
    const std::string reply = h.admin("help");
    CHECK(contains(reply, "phase <SYMBOL|*>"));
    CHECK(contains(reply, "fault user <USER> disconnect"));
    CHECK(ends_with_ok(reply));
}

TEST_CASE(server_core_admin, parse_errors_are_single_err_lines) {
    RecordingHarness h(parse_test_config());
    for (const char* line : {"frobnicate", "phase AAPL sideways", "book", "fault md drop many", "bots maybe", ""}) {
        const std::string reply = h.admin(line);
        CHECK(is_error(reply));
        CHECK_EQ(std::count(reply.begin(), reply.end(), '\n'), 1);
    }
    CHECK_EQ(h.core.counters().commands_applied, 0u);
}

TEST_CASE(server_core_admin, phase_for_one_instrument) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("phase AAPL halted"), std::string("AAPL CONTINUOUS -> HALTED\nOK\n"));
    CHECK(h.core.engine().phase(kAapl) == Phase::Halted);
    CHECK(h.core.engine().phase(kMsft) == Phase::PreOpen);
    // Applied through submit: stamped, dispatched to market data.
    REQUIRE_EQ(h.recorder.batch_count(), 1u);
    CHECK(h.recorder.batch(0)[0].type == EventType::PhaseChanged);
    CHECK_EQ(h.core.counters().commands_applied, 1u);
}

TEST_CASE(server_core_admin, phase_star_applies_to_every_instrument) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("phase * continuous"),
             std::string("AAPL CONTINUOUS -> CONTINUOUS\nMSFT PRE_OPEN -> CONTINUOUS\nOK\n"));
    CHECK(h.core.engine().phase(kMsft) == Phase::Continuous);
    CHECK_EQ(h.recorder.batch_count(), 1u);  // one SetPhase{instrument 0}, one batch
}

TEST_CASE(server_core_admin, phase_errors) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("phase NOPE halted"), std::string("ERR unknown instrument NOPE\n"));
    CHECK_EQ(h.admin("phase TOOLONGSYMBOL halted"), std::string("ERR unknown instrument TOOLONGSYMBOL\n"));
    CHECK_EQ(h.recorder.batch_count(), 0u);
    h.core.shutdown();
    CHECK_EQ(h.admin("phase AAPL continuous"), std::string("ERR exchange is shut down\n"));
}

TEST_CASE(server_core_admin, book_prints_a_price_ladder) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("book AAPL"), std::string("book AAPL phase=CONTINUOUS\n(empty)\nOK\n"));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'100, 100)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::SELL, 1'500'100, 50)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::SELL, 1'500'200, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 2, kAapl, Side::BUY, 1'499'900, 30)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kAapl, Side::BUY, 1'499'800, 20)), SubmitResult::Applied);
    CHECK_EQ(h.admin("book AAPL"), std::string("book AAPL phase=CONTINUOUS\n"
                                               "ask 150.02 qty=10 orders=1\n"
                                               "ask 150.01 qty=150 orders=2\n"
                                               "bid 149.99 qty=30 orders=1\n"
                                               "bid 149.98 qty=20 orders=1\n"
                                               "OK\n"));
    CHECK_EQ(h.admin("book AAPL 1"), std::string("book AAPL phase=CONTINUOUS\n"
                                                 "ask 150.01 qty=150 orders=2\n"
                                                 "bid 149.99 qty=30 orders=1\n"
                                                 "OK\n"));
    CHECK_EQ(h.admin("book NOPE"), std::string("ERR unknown instrument NOPE\n"));
    AdminCommand zero_levels;
    zero_levels.op = AdminOp::Book;
    zero_levels.symbol = "AAPL";
    zero_levels.count = 0;  // not parseable, but execute() is public: shows one level
    std::string reply;
    h.core.execute(zero_levels, reply);
    CHECK(contains(reply, "ask 150.01 qty=150 orders=2\n"));
    CHECK(!contains(reply, "(empty)"));
}

TEST_CASE(server_core_admin, status_reports_every_part_of_the_core) {
    RecordingHarness h(parse_test_config(kTwoBots));
    REQUIRE(h.core.bind(1, 42));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'499'900, 30)), SubmitResult::Applied);
    const std::string reply = h.admin("status");
    CHECK(reply.rfind("exchange EXSIM shutdown=no\n", 0) == 0);
    CHECK(contains(reply, "trading_day start=00:00:00 end=24:00:00 last_command_ts=1000000000\n"));
    CHECK(contains(reply, "connections bound=1 users=5 bot_users=2\n"));
    CHECK(contains(reply, "user ALICE session=1 protocol=ouch42 bound=yes connection=42 "
                          "reject=0 delay_us=0 drop_out=0 disconnect=no\n"));
    CHECK(contains(reply, "user CAROL session=3 protocol=fix44 bound=no reject=0"));
    CHECK(contains(reply, "user MM1 session=100 protocol=ouch42 bot=yes\n"));
    CHECK(contains(reply, "orders live=1\n"));
    CHECK(contains(reply, "instrument AAPL id=1 phase=CONTINUOUS bid=149.99 ask=- last=-\n"));
    CHECK(contains(reply, "instrument MSFT id=2 phase=PRE_OPEN bid=- ask=- last=-\n"));
    CHECK(contains(reply, "md next_sequence=2\n"));
    CHECK(contains(reply, "md faults drop_ppm=0 dup_ppm=0 reorder_ppm=0 delay=1 seed=1 held=0 idle=yes"));
    CHECK(contains(reply, "bots on count=2\n"));
    CHECK(contains(reply, "bot maker type=market_maker user=MM1 instrument=AAPL enabled=yes inventory=0"));
    CHECK(contains(reply, "bot taker type=noise_taker user=NT1 instrument=AAPL enabled=yes"));
    CHECK(contains(reply, "journal state=disabled records=0 failures=0 on_failure=shutdown\n"));
    CHECK(contains(reply, "commands applied=1 outside_trading_day=0 after_shutdown=0 output_capacity_failures=0 deferred=0 "
                          "deferred_overflows=0\n"));
    CHECK(ends_with_ok(reply));
}

TEST_CASE(server_core_admin, status_without_optional_market_data_hooks) {
    HarnessWith<MinimalMarketData> h(parse_test_config());
    const std::string reply = h.admin("status");
    CHECK(contains(reply, "md next_sequence=unavailable\n"));
    CHECK(contains(reply, "md faults unavailable\n"));
    CHECK(contains(reply, "bots on count=0\n"));
    CHECK(ends_with_ok(reply));
}

TEST_CASE(server_core_admin, md_one_shot_faults_reach_the_injector) {
    RecordingHarness h(parse_test_config());
    sim::PacketFaultInjector& injector = h.market_data.injector;

    std::string reply = h.admin("fault md drop 2");
    CHECK(reply.rfind("md faults drop_ppm=0", 0) == 0);
    CHECK(contains(reply, " idle=no "));
    CHECK(ends_with_ok(reply));
    CHECK_EQ(deliver_packets(injector, 3), 1u);
    CHECK_EQ(injector.stats().dropped, 2u);

    CHECK(ends_with_ok(h.admin("fault md dup 1")));
    CHECK_EQ(deliver_packets(injector, 1), 2u);
    CHECK_EQ(injector.stats().duplicated, 1u);

    reply = h.admin("fault md reorder 1 3");
    CHECK(contains(reply, " delay=3 "));
    CHECK_EQ(injector.config().reorder_delay, 3u);
    CHECK_EQ(deliver_packets(injector, 1), 0u);  // held back
    CHECK_EQ(injector.held(), 1u);
    CHECK_EQ(deliver_packets(injector, 3), 4u);  // released after three later packets
    CHECK_EQ(injector.stats().reordered, 1u);
}

TEST_CASE(server_core_admin, md_rate_configures_random_faults) {
    RecordingHarness h(parse_test_config());
    const std::string reply = h.admin("fault md rate drop=1000 dup=20 reorder=300 delay=4 seed=9");
    CHECK(contains(reply, "md faults drop_ppm=1000 dup_ppm=20 reorder_ppm=300 delay=4 seed=9 "));
    CHECK(ends_with_ok(reply));
    const sim::PacketFaultConfig& configured = h.market_data.injector.config();
    CHECK_EQ(configured.drop_ppm, 1000u);
    CHECK_EQ(configured.duplicate_ppm, 20u);
    CHECK_EQ(configured.reorder_ppm, 300u);
    CHECK_EQ(configured.reorder_delay, 4u);
    CHECK_EQ(configured.seed, 9u);
}

TEST_CASE(server_core_admin, md_off_uses_the_reset_hook_when_present) {
    RecordingHarness h(parse_test_config());
    CHECK(ends_with_ok(h.admin("fault md rate drop=1000")));
    CHECK(ends_with_ok(h.admin("fault md drop 5")));
    const std::string reply = h.admin("fault md off");
    CHECK_EQ(h.market_data.resets, 1u);
    CHECK(h.market_data.injector.idle());
    CHECK(contains(reply, "drop_ppm=0 dup_ppm=0 reorder_ppm=0"));
    CHECK(!contains(reply, "note:"));
    CHECK(ends_with_ok(reply));
}

TEST_CASE(server_core_admin, md_off_without_reset_hook_clears_rates_and_reports_leftovers) {
    HarnessWith<NoResetMarketData> h(parse_test_config());
    CHECK(ends_with_ok(h.admin("fault md rate drop=1000 reorder=5")));
    std::string reply = h.admin("fault md off");
    CHECK(h.market_data.injector.idle());
    CHECK(!contains(reply, "note:"));
    CHECK(ends_with_ok(reply));

    CHECK(ends_with_ok(h.admin("fault md drop 2")));
    reply = h.admin("fault md off");
    CHECK(contains(reply, "note: pending one-shot faults or held packets remain"));
    CHECK(ends_with_ok(reply));
    CHECK_EQ(h.market_data.injector.config().drop_ppm, 0u);
}

TEST_CASE(server_core_admin, md_faults_without_an_injector_are_an_error) {
    HarnessWith<MinimalMarketData> h(parse_test_config());
    for (const char* line : {"fault md drop 1", "fault md dup 1", "fault md reorder 1", "fault md rate drop=5",
                             "fault md off"}) {
        CHECK_EQ(h.admin(line), std::string("ERR market data exposes no packet fault injector\n"));
    }
}

TEST_CASE(server_core_admin, user_fault_errors) {
    RecordingHarness h(parse_test_config(kTwoBots));
    CHECK_EQ(h.admin("fault user NOBODY reject 1"), std::string("ERR unknown user NOBODY\n"));
    CHECK_EQ(h.admin("fault user MM1 reject 1"),
             std::string("ERR user MM1 is driven by a bot; gateway faults do not apply\n"));
    CHECK_EQ(h.admin("fault user ALICE disconnect"), std::string("ERR user ALICE is not connected\n"));
    CHECK(!h.core.has_disconnect_requests());
}

TEST_CASE(server_core_admin, bots_on_and_off) {
    RecordingHarness h(parse_test_config(kTwoBots));
    CHECK(h.core.bots_enabled());
    CHECK_EQ(h.admin("bots off"), std::string("bots off (2 configured)\nOK\n"));
    CHECK(!h.core.bots_enabled());
    for (const auto& bot : h.core.bots()) CHECK(!bot.state->enabled());
    h.core.timer(h.clock.day);
    CHECK_EQ(h.recorder.batch_count(), 0u);  // disabled bots stay silent
    CHECK(contains(h.admin("status"), "bots off count=2\n"));

    CHECK_EQ(h.admin("bots on"), std::string("bots on (2 configured)\nOK\n"));
    CHECK(h.core.bots_enabled());
    h.core.timer(h.clock.day);
    CHECK(h.recorder.batch_count() > 0u);
}

TEST_CASE(server_core_admin, shutdown_closes_every_instrument_once) {
    RecordingHarness h(parse_test_config());
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'499'900, 30)), SubmitResult::Applied);
    CHECK_EQ(h.admin("shutdown"), std::string("all instruments closed\nOK\n"));
    CHECK(h.core.shutdown_requested());
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
    CHECK(h.core.engine().phase(kMsft) == Phase::Closed);
    CHECK_EQ(h.core.engine().live_order_count(), 0u);  // the DAY order expired
    const auto closing = h.recorder.batch(h.recorder.batch_count() - 1);
    CHECK(closing[0].type == EventType::Canceled);
    CHECK(closing[0].canceled.reason == CancelReason::EndOfDay);
    CHECK(h.recorder.sessions_of_batch(h.recorder.batch_count() - 1) == std::vector<SessionId>{1});
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK_EQ(h.recorder.market_data_shutdown_calls, 1u);

    CHECK_EQ(h.admin("shutdown"), std::string("already shut down\nOK\n"));
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK(contains(h.admin("status"), "exchange EXSIM shutdown=yes\n"));
}

TEST_CASE(server_core_admin, shutdown_at_the_last_nanosecond_reports_close_not_applied) {
    RecordingHarness h(parse_test_config("[exchange]\ntrading_day_end = 16:00\n"));
    const Timestamp day_end = 16 * 3600 * kSecond;
    h.clock.day = day_end - 1;
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'499'900, 30)), SubmitResult::Applied);
    CHECK_EQ(h.admin("shutdown"), std::string("SetPhase Closed not applied: outside trading day\nOK\n"));
    CHECK(h.core.shutdown_requested());
    CHECK(h.core.engine().phase(kAapl) == Phase::Continuous);
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
}
