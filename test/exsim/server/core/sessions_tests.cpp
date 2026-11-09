// ExchangeCore as the gateway Context: authentication, at most one live
// binding per session, unbind with and without cancel-on-disconnect, and
// commands submitted from inside a dispatch (deferred, never re-entered).

#include "test/exsim/server/core/core_test_support.hpp"

#include <string>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

const char* const kPasswordlessUser = R"(
[user OPEN]
session_id = 9
owner = 9
)";

std::size_t count_events(std::span<const Event> batch, EventType type) {
    std::size_t count = 0;
    for (const Event& event : batch) count += event.type == type ? 1 : 0;
    return count;
}

}  // namespace

TEST_CASE(server_core_sessions, authenticate_checks_name_password_and_protocol) {
    RecordingHarness h(parse_test_config(kPasswordlessUser));
    const UserConfig* alice = h.core.authenticate("ALICE", "alice", Protocol::Ouch42);
    REQUIRE(alice != nullptr);
    CHECK_EQ(alice->session_id, 1);
    CHECK(h.core.authenticate("ALICE", "wrong", Protocol::Ouch42) == nullptr);
    CHECK(h.core.authenticate("ALICE", "alice", Protocol::Fix44) == nullptr);
    CHECK(h.core.authenticate("alice", "alice", Protocol::Ouch42) == nullptr);  // names are exact
    CHECK(h.core.authenticate("NOBODY", "x", Protocol::Ouch42) == nullptr);
    const UserConfig* carol = h.core.authenticate("CAROL", "carol", Protocol::Fix44);
    REQUIRE(carol != nullptr);
    CHECK_EQ(carol->session_id, 3);
    // An empty configured password accepts any password.
    const UserConfig* open = h.core.authenticate("OPEN", "anything", Protocol::Ouch42);
    REQUIRE(open != nullptr);
    CHECK_EQ(open->session_id, 9);
    CHECK(h.core.user(9) == open);
    CHECK(h.core.user(55) == nullptr);
}

TEST_CASE(server_core_sessions, bot_users_cannot_log_in_or_bind) {
    RecordingHarness without_bots(parse_test_config());
    CHECK(without_bots.core.authenticate("MM1", "mm1", Protocol::Ouch42) != nullptr);
    CHECK(!without_bots.core.is_bot_session(100));

    RecordingHarness with_bots(parse_test_config(kTwoBots));
    CHECK(with_bots.core.authenticate("MM1", "mm1", Protocol::Ouch42) == nullptr);
    CHECK(with_bots.core.authenticate("NT1", "nt1", Protocol::Ouch42) == nullptr);
    CHECK(with_bots.core.is_bot_session(100));
    CHECK(with_bots.core.is_bot_session(101));
    CHECK(!with_bots.core.is_bot_session(1));
    CHECK(!with_bots.core.bind(100, 5));
    CHECK_EQ(with_bots.core.bound_connection_count(), 0u);
}

TEST_CASE(server_core_sessions, one_live_binding_per_session) {
    RecordingHarness h(parse_test_config());
    CHECK(!h.core.is_bound(1));
    CHECK(h.core.bind(1, 77));
    CHECK(h.core.is_bound(1));
    REQUIRE(h.core.connection(1).has_value());
    CHECK_EQ(*h.core.connection(1), 77u);
    CHECK(!h.core.bind(1, 78));  // second login for the same session
    CHECK_EQ(*h.core.connection(1), 77u);
    CHECK(h.core.bind(2, 79));
    CHECK_EQ(h.core.bound_connection_count(), 2u);
    CHECK(!h.core.bind(55, 80));  // no such user
    CHECK(!h.core.bind(0, 81));
    CHECK_EQ(h.core.bound_connection_count(), 2u);

    h.core.unbind(1, 77);
    CHECK(!h.core.is_bound(1));
    CHECK(!h.core.connection(1).has_value());
    CHECK_EQ(h.core.bound_connection_count(), 1u);
    CHECK(h.core.bind(1, 78));
    CHECK_EQ(*h.core.connection(1), 78u);
}

TEST_CASE(server_core_sessions, unbind_cancels_everything_when_cancel_on_disconnect) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 7));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::SELL, 2'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 3, kMsft, Side::BUY, 3'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    const std::size_t before = h.recorder.batch_count();

    h.core.unbind(1, 7);
    REQUIRE_EQ(h.recorder.batch_count(), before + 1);
    const auto cancels = h.recorder.batch(before);
    REQUIRE_EQ(cancels.size(), 4u);  // three Canceled, then MassCancelDone
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(cancels[i].type == EventType::Canceled);
        CHECK(cancels[i].canceled.reason == CancelReason::CancelOnDisconnect);
        CHECK_EQ(cancels[i].canceled.session, 1);
    }
    CHECK(cancels[3].type == EventType::MassCancelDone);
    CHECK_EQ(cancels[3].mass_cancel_done.canceled_count, 3u);
    CHECK(cancels[3].mass_cancel_done.reason == CancelReason::CancelOnDisconnect);
    CHECK(h.recorder.sessions_of_batch(before) == std::vector<SessionId>{1});
    CHECK_EQ(h.core.engine().live_order_count(), 1u);  // BOB's order stays

    h.core.unbind(1, 7);  // not bound: no-op
    CHECK_EQ(h.recorder.batch_count(), before + 1);
}

TEST_CASE(server_core_sessions, unbind_keeps_orders_without_cancel_on_disconnect) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(3, 7));
    REQUIRE_EQ(h.core.submit(limit_order(3, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    const std::size_t before = h.recorder.batch_count();
    h.core.unbind(3, 7);
    CHECK(!h.core.is_bound(3));
    CHECK_EQ(h.recorder.batch_count(), before);
    CHECK_EQ(h.core.engine().live_order_count(), 1u);
}

TEST_CASE(server_core_sessions, unbind_with_nothing_resting_still_reports_mass_cancel_done) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 7));
    h.core.unbind(1, 7);
    REQUIRE_EQ(h.recorder.batch_count(), 1u);
    const auto batch = h.recorder.batch(0);
    REQUIRE_EQ(batch.size(), 1u);
    CHECK(batch[0].type == EventType::MassCancelDone);
    CHECK_EQ(batch[0].mass_cancel_done.canceled_count, 0u);
    // Still dispatched to the (now unbound) session: its gateway decides what to do.
    CHECK(h.recorder.sessions_of_batch(0) == std::vector<SessionId>{1});
    CHECK_EQ(h.core.counters().commands_applied, 1u);
}

TEST_CASE(server_core_sessions, unbind_from_a_callback_runs_after_the_batch) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 7));
    REQUIRE(h.core.bind(2, 8));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'000, 100)), SubmitResult::Applied);
    // BOB's gateway decides to drop ALICE while it is handling the fill batch.
    h.sessions.unbind_when_dispatching_to = 2;
    h.sessions.unbind_target = 1;
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'500'000, 40)), SubmitResult::Applied);

    REQUIRE_EQ(h.recorder.batch_count(), 3u);
    // The fill batch reached both sessions and market data whole...
    CHECK(h.recorder.sessions_of_batch(1) == (std::vector<SessionId>{2, 1}));
    CHECK_EQ(count_events(h.recorder.batch(1), EventType::Fill), 1u);
    // ...and only then was ALICE's remainder canceled, at a later stamp.
    const auto cancels = h.recorder.batch(2);
    REQUIRE_EQ(cancels.size(), 2u);
    CHECK(cancels[0].type == EventType::Canceled);
    CHECK(cancels[0].canceled.reason == CancelReason::CancelOnDisconnect);
    CHECK_EQ(cancels[0].canceled.canceled_qty, 60u);
    CHECK(cancels[1].type == EventType::MassCancelDone);
    CHECK(cancels[0].ts > h.recorder.batch(1)[0].ts);
    CHECK_EQ(h.core.counters().commands_deferred, 1u);
    CHECK(!h.core.is_bound(1));
}

TEST_CASE(server_core_sessions, deferred_queue_overflow_is_reported_not_lost_silently) {
    ServerConfig config = parse_test_config();
    const std::size_t deferred_capacity = config.users.size() + kDeferredCommandSlack;
    RecordingHarness h(config);
    CancelOrder cancel_unknown{};
    cancel_unknown.session = 1;
    cancel_unknown.orig_cl_ord_id = 999;
    h.sessions.storm_when_dispatching_to = 1;
    h.sessions.storm_commands = deferred_capacity + 1;
    h.sessions.storm_command = Command{cancel_unknown};

    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE_EQ(h.sessions.storm_results.size(), deferred_capacity + 1);
    for (std::size_t i = 0; i < deferred_capacity; ++i) {
        CHECK_EQ(h.sessions.storm_results[i], SubmitResult::Deferred);
    }
    CHECK_EQ(h.sessions.storm_results.back(), SubmitResult::DeferredQueueFull);
    CHECK_EQ(h.core.counters().commands_deferred, deferred_capacity);
    CHECK_EQ(h.core.counters().deferred_queue_overflows, 1u);
    // Every deferred cancel was applied (and rejected) before submit returned.
    CHECK_EQ(h.recorder.batch_count(), 1 + deferred_capacity);
    CHECK(h.recorder.batch(1)[0].type == EventType::Rejected);
    CHECK_EQ(h.core.counters().commands_applied, 1 + deferred_capacity);
}

TEST_CASE(server_core_sessions, instrument_lookup_by_symbol_and_id) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.core.find_instrument(Symbol("AAPL")), kAapl);
    CHECK_EQ(h.core.find_instrument(Symbol("MSFT")), kMsft);
    CHECK_EQ(h.core.find_instrument(Symbol("NOPE")), 0);
    REQUIRE(h.core.instrument(kMsft) != nullptr);
    CHECK(h.core.instrument(kMsft)->symbol.view() == "MSFT");
    CHECK(h.core.instrument(9) == nullptr);
}
