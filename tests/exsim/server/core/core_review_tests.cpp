// Independent QA review reproducers (review of e2cdd11), updated to assert the
// fixed behaviour. See /tmp/exsim-review-proto/REPORT.md, findings F4 and F5,
// and the FIX QA review finding R1 (connection-aware unbind).

#include "tests/exsim/server/core/core_test_support.hpp"

#include "exsim/md/publisher.hpp"

#include <span>
#include <string>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {
constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

struct DiscardingPacketSink {
    void operator()(std::span<const char>) const noexcept {}
};

// The real ITCH publisher as MarketData: on_events and on_timer return its
// md::PublishStatus, and state()/stop_reason() are exposed.
struct PublisherMarketData {
    explicit PublisherMarketData(Recorder&) : publisher(md::PublisherConfig{}, DiscardingPacketSink{}) {}

    md::PublishStatus on_events(std::span<const Event> batch) { return publisher.on_events(batch); }
    void flush() { publisher.flush(); }
    md::PublishStatus on_timer(Timestamp now) { return publisher.poll_heartbeat(now); }
    [[nodiscard]] md::PublisherState state() const noexcept { return publisher.state(); }
    [[nodiscard]] md::PublishStatus stop_reason() const noexcept { return publisher.stop_reason(); }

    md::MarketDataPublisher<DiscardingPacketSink> publisher;
};

static_assert(MarketDataPublisherState<PublisherMarketData>);

// Tries to bind session 2 from inside the first dispatch.
struct BindingSessions {
    bool attempted = false;
    bool bind_result = true;
    template <typename Core>
    void on_events(SessionId, std::span<const Event>, Core& core) {
        if (attempted) return;
        attempted = true;
        bind_result = core.bind(2, 99);
    }
};
}  // namespace

// F4 (fixed). ExchangeCore::unbind used to do `(void)submit(cancel_all)`:
// inside a dispatch with the deferred ring full, the cancel-on-disconnect
// MassCancel was dropped. The ring now reserves one slot per user for it, so
// a storm that fills every ordinary slot cannot push it out.
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

    // One more ordinary command than the ordinary slots is still refused.
    h.sessions.storm_when_dispatching_to = 2;
    h.sessions.storm_commands = deferred_capacity + 1;
    h.sessions.storm_results.clear();
    REQUIRE_EQ(h.core.submit(limit_order(2, 2, kAapl, Side::BUY, 1'000'000, 1)), SubmitResult::Applied);
    CHECK_EQ(h.sessions.storm_results.back(), SubmitResult::DeferredQueueFull);
    CHECK_EQ(h.core.counters().deferred_queue_overflows, 1u);
}

// F4 invariant: logins come from inbound data, never from a dispatch
// callback, so at most one unbind MassCancel per user can be queued.
TEST_CASE(server_core_review, bind_is_refused_during_dispatch) {
    Recorder recorder;
    FakeClock clock;
    BindingSessions sessions;
    MinimalMarketData market_data{recorder};
    const ServerConfig config = parse_test_config();
    ExchangeCore<BindingSessions, MinimalMarketData, FakeClock> core(config, sessions, market_data, clock);
    REQUIRE_EQ(core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(sessions.attempted);
    CHECK(!sessions.bind_result);
    CHECK(!core.is_bound(2));
    CHECK(core.bind(2, 99));  // outside dispatch it works
}

// R1 (FIX QA review). unbind is connection-aware: a release from an older
// connection after a newer login bound the session is a no-op, so it neither
// unbinds the newer connection nor cancels its orders.
TEST_CASE(server_core_review, stale_unbind_keeps_the_newer_binding_and_its_orders) {
    RecordingHarness h(parse_test_config());
    REQUIRE(h.core.bind(1, 11));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.core.unbind(1, 11);  // connection 11 closed: cancel-on-disconnect
    REQUIRE_EQ(h.core.engine().live_order_count(), 0u);

    REQUIRE(h.core.bind(1, 12));  // the user logs in again
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    const std::size_t batches_before = h.recorder.batch_count();

    h.core.unbind(1, 11);  // connection 11's disconnect handler runs late
    CHECK(h.core.is_bound(1));
    CHECK(h.core.connection(1) == std::optional<ConnectionId>(12));
    CHECK_EQ(h.core.bound_connection_count(), 1u);
    CHECK_EQ(h.core.engine().live_order_count(), 1u);
    CHECK_EQ(h.recorder.batch_count(), batches_before);  // no MassCancel

    h.core.unbind(1, 12);
    CHECK(!h.core.is_bound(1));
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
}

// F5 (fixed). The core observes the publisher's md::PublishStatus: failed
// batches are counted, the last failure is kept, and `status` shows the
// publisher state.
TEST_CASE(server_core_review, market_data_publisher_failures_are_visible) {
    HarnessWith<PublisherMarketData> h(parse_test_config());
    md::MarketDataPublisher<DiscardingPacketSink>& publisher = h.market_data.publisher;
    REQUIRE(publisher.add_instrument(kAapl, *h.core.instrument(kAapl)) == md::PublishStatus::Ok);  // MSFT left out
    REQUIRE(publisher.start_session(h.clock.day) == md::PublishStatus::Ok);

    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.counters().market_data_failed_batches, 0u);
    std::string reply = h.admin("status");
    CHECK(contains(reply, "md state=live stop_reason=none failed_batches=0 last_failure=none\n"));

    // An event for an instrument the publisher does not know stops the feed.
    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kMsft, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(publisher.state() == md::PublisherState::Stopped);
    CHECK_EQ(h.core.counters().market_data_failed_batches, 1u);
    CHECK(h.core.counters().market_data_last_failure == md::PublishStatus::UnknownInstrument);

    // Matching goes on; every later batch is counted as not published.
    REQUIRE_EQ(h.core.submit(limit_order(1, 3, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.counters().market_data_failed_batches, 2u);
    CHECK(h.core.counters().market_data_last_failure == md::PublishStatus::NotAllowedInState);
    reply = h.admin("status");
    CHECK(contains(reply, "md state=stopped stop_reason=unknown_instrument failed_batches=2 "
                          "last_failure=not_allowed_in_state\n"));

    // Timer results are observed too, without counting as failed batches.
    h.clock.day += kSecond;
    h.core.timer(h.clock.day);
    CHECK_EQ(h.core.counters().market_data_failed_batches, 2u);
    CHECK(h.core.counters().market_data_last_failure == md::PublishStatus::NotAllowedInState);
}

TEST_CASE(server_core_review, market_data_state_unavailable_without_hooks) {
    HarnessWith<MinimalMarketData> h(parse_test_config());
    const std::string reply = h.admin("status");
    CHECK(contains(reply, "md state=unavailable failed_batches=0 last_failure=none\n"));
}
