// Allocation guard: after a warm-up that exercises every message path once,
// a long stretch of OUCH and FIX order entry with bots trading and market
// data flowing must not allocate on the server thread.

#include <functional>

#include "tests/exsim/server/e2e/e2e_support.hpp"

using namespace exsim;
using namespace exsim::e2e;

namespace {

constexpr int kOrdersPerBatch = 50;
constexpr int kOuchReplacesPerBatch = 10;  // the first orders of each batch are replaced
constexpr int kFixOrdersPerBatch = 10;

struct SteadyStateTraffic {
    OuchSession& alice;
    OuchRecorder& alice_events;
    FixSession& carol;
    FixRecorder& carol_events;
    std::function<void()> pump;
    std::size_t round_trips = 0;
    std::string failed_step{};

    bool fail(std::string step) {
        failed_step = std::move(step);
        return false;
    }

    bool send_fix(const fix44::NewOrderSingle& order) {
        return carol.client().send_new_order_single(order, client::fix_time_now());
    }

    // One batch: OUCH enter, replace part, cancel all; FIX enter, an OUCH sell
    // that trades with CAROL's first bid, FIX cancel all.
    bool run_batch(int batch) {
        const std::string prefix = std::to_string(batch) + "_";
        const std::size_t accepted_before = alice_events.accepted.size();
        for (int order = 0; order < kOrdersPerBatch; ++order) {
            if (!alice.client().enter_order(ouch_limit_order("A" + prefix + std::to_string(order), Side::BUY, 10,
                                                             (100 + order % 5) * kDollar),
                                            net::monotonic_ns())) {
                return fail("ouch enter send");
            }
        }
        alice.flush();
        if (!wait_until([&] { return alice_events.accepted.size() == accepted_before + kOrdersPerBatch; }, pump)) {
            return fail("ouch accepted");
        }
        round_trips += kOrdersPerBatch;

        const std::size_t replaced_before = alice_events.replaced.size();
        for (int order = 0; order < kOuchReplacesPerBatch; ++order) {
            if (!alice.client().replace_order(ouch_replace("A" + prefix + std::to_string(order),
                                                           "R" + prefix + std::to_string(order), 8, 99 * kDollar),
                                              net::monotonic_ns())) {
                return fail("ouch replace send");
            }
        }
        alice.flush();
        if (!wait_until([&] { return alice_events.replaced.size() == replaced_before + kOuchReplacesPerBatch; },
                        pump)) {
            return fail("ouch replaced");
        }
        round_trips += kOuchReplacesPerBatch;

        const std::size_t canceled_before = alice_events.canceled.size();
        for (int order = 0; order < kOrdersPerBatch; ++order) {
            const char* const live_prefix = order < kOuchReplacesPerBatch ? "R" : "A";
            if (!alice.client().cancel_order(ouch_cancel(live_prefix + prefix + std::to_string(order)),
                                             net::monotonic_ns())) {
                return fail("ouch cancel send");
            }
        }
        alice.flush();
        if (!wait_until([&] { return alice_events.canceled.size() == canceled_before + kOrdersPerBatch; }, pump)) {
            return fail("ouch canceled");
        }
        round_trips += kOrdersPerBatch;

        const std::size_t fix_reports_before = carol_events.reports.size();
        for (int order = 0; order < kFixOrdersPerBatch; ++order) {
            if (!send_fix(fix_limit_order("F" + prefix + std::to_string(order), Side::BUY, 10, 390 * kDollar, "MSFT"))) {
                return fail("fix enter send");
            }
        }
        carol.flush();
        if (!wait_until([&] { return carol_events.reports.size() == fix_reports_before + kFixOrdersPerBatch; },
                        pump)) {
            return fail("fix new reports");
        }
        round_trips += kFixOrdersPerBatch;

        // ALICE sells 4 into CAROL's first MSFT bid (MSFT has no bots).
        const std::string sell_token = "S" + prefix;
        const std::size_t executed_before = alice_events.executed.size();
        if (!alice.client().enter_order(ouch_limit_order(sell_token, Side::SELL, 4, 390 * kDollar, "MSFT"),
                                        net::monotonic_ns())) {
            return fail("ouch sell send");
        }
        alice.flush();
        // Both sides of the trade: ALICE's execution and CAROL's fill report.
        if (!wait_until(
                [&] {
                    return alice_events.executed.size() == executed_before + 1 &&
                           carol_events.reports.size() == fix_reports_before + kFixOrdersPerBatch + 1;
                },
                pump)) {
            return fail("trade reports");
        }
        round_trips += 1;

        const std::size_t fix_reports_before_cancel = carol_events.reports.size();
        for (int order = 0; order < kFixOrdersPerBatch; ++order) {
            if (!carol.client().send_order_cancel_request(
                    fix_cancel("F" + prefix + std::to_string(order), "C" + prefix + std::to_string(order), Side::BUY,
                               "MSFT"),
                    client::fix_time_now())) {
                return fail("fix cancel send");
            }
        }
        carol.flush();
        if (!wait_until([&] { return carol_events.reports.size() == fix_reports_before_cancel + kFixOrdersPerBatch; },
                        pump)) {
            {
            std::string seen = "fix cancel reports:";
            for (std::size_t index = fix_reports_before; index < carol_events.reports.size(); ++index) {
                seen += " " + carol_events.reports[index].client_order_id + "/" +
                        carol_events.reports[index].execution_type;
            }
            return fail(seen + " cancel_rejects=" + std::to_string(carol_events.cancel_rejects) +
                        " session_rejects=" + std::to_string(carol_events.session_rejects));
        }
        }
        round_trips += kFixOrdersPerBatch;
        return true;
    }
};

}  // namespace

TEST_CASE(server_e2e_allocation, steady_state_order_entry_market_data_and_bots_do_not_allocate) {
    BookFeedHandler feed_handler;
    Feed feed(feed_handler, feed_receiver_config());
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    ServerUnderTest server(make_e2e_config(/*with_bots=*/true), feed.feed_endpoint().port);
    REQUIRE(server.started());
    feed.set_retransmission_server(server.retransmission());
    OuchRecorder alice_events;
    OuchSession alice(alice_events);
    FixRecorder carol_events;
    FixSession carol(carol_events, fix_settings("CAROL", "carol"));
    SteadyStateTraffic traffic{alice, alice_events, carol, carol_events, [&] {
                                   alice.poll();
                                   carol.poll();
                                   feed.poll();
                               }};
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(reply_ok(admin.request("phase MSFT continuous")));
    REQUIRE(alice.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(carol.connect_and_logon(server.fix(), false));
    REQUIRE(wait_until([&] { return alice_events.logins_accepted == 1 && carol_events.logons == 1; }, traffic.pump));

    // Warm-up: every path once (plus a few batches so lazily sized state settles).
    for (int batch = 0; batch < 4; ++batch) REQUIRE_EQ(traffic.run_batch(batch) ? std::string() : traffic.failed_step, std::string());
    // F0_1 is already canceled: the replace takes the cancel reject path.
    REQUIRE(carol.client().send_order_cancel_replace_request(fix_replace("F0_1", "W1", Side::BUY, 5, 380 * kDollar, "MSFT"),
                                                             client::fix_time_now()));
    carol.flush();
    REQUIRE(wait_until([&] { return carol_events.cancel_rejects >= 1; }, traffic.pump));
    REQUIRE(wait_until([&] { return feed_handler.messages > 0; }, traffic.pump));

    const std::size_t round_trips_before = traffic.round_trips;
    const std::uint64_t feed_messages_before = feed_handler.messages;
    const std::uint64_t loop_iterations_before = server.loop_iterations();
    server.begin_allocation_count();
    int batch = 4;
    while (traffic.round_trips - round_trips_before < 10'000) REQUIRE_EQ(traffic.run_batch(batch++) ? std::string() : traffic.failed_step, std::string());
    // Some idle time too: timers, heartbeats, bots.
    wait_until([] { return false; }, traffic.pump, 100);
    const std::size_t allocations = server.end_allocation_count();

    const std::size_t counted_round_trips = traffic.round_trips - round_trips_before;
    CHECK_GE(counted_round_trips, 10'000u);
    CHECK_GT(feed_handler.messages - feed_messages_before, std::uint64_t(counted_round_trips));
    CHECK_GT(server.loop_iterations() - loop_iterations_before, 100u);
    CHECK_EQ(alice_events.rejected.size(), 0u);
    CHECK_EQ(carol_events.session_rejects, 0);
    if (testfw::allocation_counting_available()) CHECK_EQ(allocations, 0u);

    // The feed (with retransmission for anything lost) still rebuilds the book.
    server.stop();
    REQUIRE(wait_until([&] { return feed.end_of_session_reached(); }, traffic.pump));
    std::string difference;
    CHECK(builder_matches_engine(feed_handler.builder, server.runtime_after_stop().core().engine(), 1, difference));
    CHECK_EQ(difference, std::string());
}
