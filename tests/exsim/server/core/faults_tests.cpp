// User (gateway) fault state: set by `fault user ...`, consumed by the
// gateways and the server loop through allocation-free queries.

#include "tests/exsim/server/core/core_test_support.hpp"

#include <array>
#include <span>
#include <string>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {
constexpr InstrumentId kAapl = 1;
}  // namespace

TEST_CASE(server_core_faults, reject_counts_are_set_and_consumed_one_by_one) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("fault user ALICE reject 2"),
             std::string("user ALICE session=1 reject=2 delay_us=0 drop_out=0 disconnect=no\nOK\n"));
    CHECK(!h.core.consume_injected_reject(2));  // other sessions are unaffected
    CHECK(h.core.consume_injected_reject(1));
    CHECK(h.core.consume_injected_reject(1));
    CHECK(!h.core.consume_injected_reject(1));

    // A new command replaces the count, it does not add to it.
    CHECK(ends_with_ok(h.admin("fault user ALICE reject 5")));
    CHECK(ends_with_ok(h.admin("fault user ALICE reject 1")));
    REQUIRE(h.core.user_faults(1) != nullptr);
    CHECK_EQ(h.core.user_faults(1)->reject_next_new_orders, 1u);
    CHECK(is_error(h.admin("fault user ALICE reject 0")));  // counts start at 1; `off` clears
    CHECK_EQ(h.core.user_faults(1)->reject_next_new_orders, 1u);
    CHECK(ends_with_ok(h.admin("fault user ALICE off")));
    CHECK(!h.core.consume_injected_reject(1));
}

TEST_CASE(server_core_faults, injected_rejects_never_reach_the_engine) {
    RecordingHarness h(parse_test_config());
    CHECK(ends_with_ok(h.admin("fault user ALICE reject 1")));
    // What a gateway does per inbound new order.
    const bool rejected_by_gateway = h.core.consume_injected_reject(1);
    CHECK(rejected_by_gateway);
    CHECK_EQ(h.core.counters().commands_applied, 0u);
    CHECK_EQ(h.recorder.batch_count(), 0u);
}

TEST_CASE(server_core_faults, inbound_delay_in_microseconds) {
    RecordingHarness h(parse_test_config());
    CHECK_EQ(h.admin("fault user BOB delay 250"),
             std::string("user BOB session=2 reject=0 delay_us=250 drop_out=0 disconnect=no\nOK\n"));
    CHECK_EQ(h.core.inbound_delay_ns(2), 250'000u);
    CHECK_EQ(h.core.inbound_delay_ns(2), 250'000u);  // a setting, not consumed
    CHECK_EQ(h.core.inbound_delay_ns(1), 0u);
    CHECK(ends_with_ok(h.admin("fault user BOB delay 0")));
    CHECK_EQ(h.core.inbound_delay_ns(2), 0u);
}

TEST_CASE(server_core_faults, drop_out_counts_are_consumed_one_by_one) {
    RecordingHarness h(parse_test_config());
    CHECK(ends_with_ok(h.admin("fault user CAROL drop_out 2")));
    CHECK(h.core.consume_drop_out(3));
    CHECK(h.core.consume_drop_out(3));
    CHECK(!h.core.consume_drop_out(3));
    CHECK(!h.core.consume_drop_out(1));
}

TEST_CASE(server_core_faults, disconnect_requests_are_taken_once_in_config_order) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 11));
    REQUIRE(h.core.bind(2, 12));
    CHECK(!h.core.has_disconnect_requests());
    CHECK(ends_with_ok(h.admin("fault user BOB disconnect")));
    CHECK_EQ(h.admin("fault user ALICE disconnect"),
             std::string("user ALICE session=1 reject=0 delay_us=0 drop_out=0 disconnect=requested\nOK\n"));
    CHECK(ends_with_ok(h.admin("fault user ALICE disconnect")));  // repeated: still one request
    CHECK(h.core.has_disconnect_requests());

    std::array<SessionId, 1> one_slot{};
    CHECK_EQ(h.core.take_disconnect_requests(one_slot), 1u);
    CHECK_EQ(one_slot[0], 1);  // ALICE is configured before BOB
    CHECK(h.core.has_disconnect_requests());
    std::array<SessionId, 4> slots{};
    CHECK_EQ(h.core.take_disconnect_requests(slots), 1u);
    CHECK_EQ(slots[0], 2);
    CHECK(!h.core.has_disconnect_requests());
    CHECK_EQ(h.core.take_disconnect_requests(slots), 0u);
    // Taking a request does not unbind: the server loop closes and unbinds.
    CHECK(h.core.is_bound(1));
    CHECK(!h.core.user_faults(1)->disconnect_requested);
}

TEST_CASE(server_core_faults, the_loop_closing_a_requested_session_cancels_its_orders) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 11));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(ends_with_ok(h.admin("fault user ALICE disconnect")));
    std::array<SessionId, 8> to_close{};
    const std::size_t count = h.core.take_disconnect_requests(to_close);
    for (std::size_t i = 0; i < count; ++i) h.core.unbind(to_close[i]);
    CHECK(!h.core.is_bound(1));
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
}

TEST_CASE(server_core_faults, unbind_drops_a_pending_disconnect_request) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(3, 13));
    CHECK(ends_with_ok(h.admin("fault user CAROL disconnect")));
    h.core.unbind(3);  // the client left on its own first
    CHECK(!h.core.has_disconnect_requests());
    std::array<SessionId, 4> slots{};
    CHECK_EQ(h.core.take_disconnect_requests(slots), 0u);
}

TEST_CASE(server_core_faults, counts_survive_a_reconnect) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 11));
    CHECK(ends_with_ok(h.admin("fault user ALICE reject 1")));
    CHECK(ends_with_ok(h.admin("fault user ALICE drop_out 1")));
    h.core.unbind(1);
    REQUIRE(h.core.bind(1, 12));
    CHECK(h.core.consume_injected_reject(1));
    CHECK(h.core.consume_drop_out(1));
}

TEST_CASE(server_core_faults, off_clears_everything_for_that_user_only) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 11));
    CHECK(ends_with_ok(h.admin("fault user ALICE reject 3")));
    CHECK(ends_with_ok(h.admin("fault user ALICE delay 10")));
    CHECK(ends_with_ok(h.admin("fault user ALICE drop_out 3")));
    CHECK(ends_with_ok(h.admin("fault user ALICE disconnect")));
    CHECK(ends_with_ok(h.admin("fault user BOB reject 1")));
    CHECK_EQ(h.admin("fault user ALICE off"),
             std::string("user ALICE session=1 reject=0 delay_us=0 drop_out=0 disconnect=no\nOK\n"));
    CHECK(!h.core.user_faults(1)->any());
    CHECK(!h.core.has_disconnect_requests());
    CHECK(h.core.consume_injected_reject(2));
}

TEST_CASE(server_core_faults, queries_for_unknown_sessions_are_harmless) {
    RecordingHarness h(parse_test_config());
    CHECK(h.core.user_faults(55) == nullptr);
    CHECK(!h.core.consume_injected_reject(55));
    CHECK(!h.core.consume_drop_out(55));
    CHECK_EQ(h.core.inbound_delay_ns(55), 0u);
    CHECK(!h.core.consume_injected_reject(0));
}
