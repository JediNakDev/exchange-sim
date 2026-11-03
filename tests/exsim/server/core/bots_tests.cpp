// Bots inside ExchangeCore: driven by core.timer(), submitting through the
// same path as clients, seeing every batch, never dispatched to Sessions.

#include "tests/exsim/server/core/core_test_support.hpp"

#include <cstdint>
#include <optional>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr SessionId kMakerSession = 100;
constexpr SessionId kTakerSession = 101;

// Advances clock and core together, one loop iteration per millisecond.
template <typename Harness>
void run_loop(Harness& h, Timestamp from, Timestamp until) {
    for (Timestamp now = from; now < until; now += kMillisecond) {
        h.clock.day = now;
        h.core.timer(now);
    }
}

std::uint64_t hash_of_all_events(const Recorder& recorder) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const Event& event : recorder.events) hash = sim::hash_event(event, hash);
    return hash;
}

std::size_t fills_between(const Recorder& recorder, SessionId first, SessionId second) {
    std::size_t fills = 0;
    for (const Event& event : recorder.events) {
        if (event.type != EventType::Fill) continue;
        const bool matches = (event.fill.buy.session == first && event.fill.sell.session == second) ||
                             (event.fill.buy.session == second && event.fill.sell.session == first);
        fills += matches ? 1 : 0;
    }
    return fills;
}

}  // namespace

TEST_CASE(server_core_bots, configured_bots_are_registered_on_their_users) {
    RecordingHarness h(parse_test_config(kTwoBots));
    REQUIRE_EQ(h.core.bots().size(), 2u);
    CHECK(h.core.bots()[0].name == "maker");
    CHECK(h.core.bots()[0].type == BotType::MarketMaker);
    CHECK(h.core.bots()[0].user == "MM1");
    CHECK(h.core.bots()[0].instrument_symbol == "AAPL");
    CHECK(h.core.bots()[1].name == "taker");
    CHECK(h.core.bots()[1].type == BotType::NoiseTaker);
    CHECK(h.core.bots()[1].state != nullptr);
}

TEST_CASE(server_core_bots, bots_trade_with_each_other_deterministically) {
    RecordingHarness first(parse_test_config(kTwoBots));
    RecordingHarness second(parse_test_config(kTwoBots));
    run_loop(first, kSecond, 4 * kSecond);
    run_loop(second, kSecond, 4 * kSecond);

    CHECK(fills_between(first.recorder, kMakerSession, kTakerSession) > 0u);
    CHECK(first.core.bots()[0].state->stats().fills_received > 0u);
    CHECK(first.core.bots()[1].state->stats().fills_received > 0u);
    // Bot sessions are never handed to the gateways' Sessions policy.
    CHECK(first.recorder.session_calls.empty());
    // Every bot command went through submit: stamped and counted.
    CHECK_EQ(first.core.counters().commands_applied,
             first.core.bots()[0].state->stats().commands_submitted +
                 first.core.bots()[1].state->stats().commands_submitted);
    // Same configuration and clock: the same event stream.
    CHECK_EQ(first.recorder.events.size(), second.recorder.events.size());
    CHECK_EQ(hash_of_all_events(first.recorder), hash_of_all_events(second.recorder));
    CHECK_EQ(first.recorder.batch_count(), second.recorder.batch_count());
}

TEST_CASE(server_core_bots, bots_trade_against_a_client_order) {
    RecordingHarness h(parse_test_config(kTwoBots));
    REQUIRE(h.core.bind(2, 1));
    run_loop(h, kSecond, kSecond + 200 * kMillisecond);  // the maker is quoting
    const std::optional<Price> best_ask = h.core.engine().best_ask(kAapl);
    REQUIRE(best_ask.has_value());
    const std::uint64_t maker_fills_before = h.core.bots()[0].state->stats().fills_received;

    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, *best_ask, 10)), SubmitResult::Applied);
    const std::size_t last = h.recorder.batch_count() - 1;
    const auto batch = h.recorder.batch(last);
    REQUIRE(batch.size() >= 2u);
    CHECK(batch[1].type == EventType::Fill);
    CHECK_EQ(batch[1].fill.buy.session, 2);
    CHECK_EQ(batch[1].fill.sell.session, kMakerSession);
    CHECK_EQ(batch[1].fill.price, *best_ask);
    CHECK(h.recorder.sessions_of_batch(last) == std::vector<SessionId>{2});  // not the maker
    CHECK_EQ(h.core.bots()[0].state->stats().fills_received, maker_fills_before + 1);
}

TEST_CASE(server_core_bots, bots_do_not_trade_after_shutdown) {
    RecordingHarness h(parse_test_config(kTwoBots));
    run_loop(h, kSecond, kSecond + 100 * kMillisecond);
    h.core.shutdown();
    const std::size_t batches_at_shutdown = h.recorder.batch_count();
    run_loop(h, kSecond + 100 * kMillisecond, kSecond + 300 * kMillisecond);
    CHECK_EQ(h.recorder.batch_count(), batches_at_shutdown);
}

TEST_CASE(server_core_bots, disabling_bots_pulls_the_makers_quotes) {
    RecordingHarness h(parse_test_config(kTwoBots));
    run_loop(h, kSecond, kSecond + 100 * kMillisecond);
    REQUIRE(h.core.bots()[0].state->orders().size() > 0u);
    h.core.set_bots_enabled(false);
    run_loop(h, kSecond + 100 * kMillisecond, kSecond + 200 * kMillisecond);
    CHECK_EQ(h.core.bots()[0].state->orders().size(), 0u);
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
}
