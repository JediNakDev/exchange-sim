// Independent QA review reproducer (review of e2cdd11). Expected to FAIL until
// fixed. See /tmp/exsim-review-proto/REPORT.md, finding F5.

#include "tests/exsim/server/core/core_test_support.hpp"

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {
constexpr InstrumentId kAapl = 1;
}

// ExchangeCore::unbind clears the binding and then does
// `(void)submit(Command{cancel_all})`. Called from inside a dispatch while the
// deferred ring is full, that submit returns DeferredQueueFull and the
// cancel-on-disconnect MassCancel is dropped: the session is unbound but its
// orders stay live, although the user has cancel_on_disconnect = true and
// EXCHANGE_CORE.md section 3 says unbind submits the MassCancel.
TEST_CASE(server_core_review, unbind_during_full_deferred_queue_keeps_cancel_on_disconnect) {
    ServerConfig config = parse_test_config();
    const std::size_t deferred_capacity = config.users.size() + kDeferredCommandSlack;
    RecordingHarness h(config);
    REQUIRE(h.core.bind(1, 7));
    REQUIRE(h.core.bind(2, 8));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::SELL, 1'500'000, 100)), SubmitResult::Applied);
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::SELL, 2'000'000, 10)), SubmitResult::Applied);

    // The fill batch is dispatched to BOB (buy leg) first, then ALICE.
    CancelOrder cancel_unknown{};
    cancel_unknown.session = 2;
    cancel_unknown.orig_cl_ord_id = 999;
    h.sessions.storm_when_dispatching_to = 2;
    h.sessions.storm_commands = deferred_capacity;
    h.sessions.storm_command = Command{cancel_unknown};
    h.sessions.unbind_when_dispatching_to = 1;
    h.sessions.unbind_target = 1;
    REQUIRE_EQ(h.core.submit(limit_order(2, 1, kAapl, Side::BUY, 1'500'000, 40)), SubmitResult::Applied);

    CHECK(!h.core.is_bound(1));
    // ALICE has cancel_on_disconnect: none of her orders may survive the unbind.
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
    CHECK_EQ(h.core.counters().deferred_queue_overflows, 0u);
}
