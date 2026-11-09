// Recovery through a running server whose per-connection send buffer is
// smaller than what must be sent again (server contract 2.4): an OUCH replay
// and a FIX ResendRequest pause while the buffer is full and resume as the
// socket drains, in sequence order, without closing the connection.

#include "test/exsim/server/e2e/e2e_support.hpp"

using namespace exsim;
using namespace exsim::e2e;

namespace {

constexpr std::size_t kConnectionSendBufferBytes = 4096;

}  // namespace

TEST_CASE(server_e2e_ouch_replay, replay_larger_than_the_connection_send_buffer_completes) {
    constexpr int kOrders = 300;  // 300 Order Accepted frames of 69 bytes: five send buffers
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0, /*bots_enabled=*/false, /*busy_poll=*/false,
                           kConnectionSendBufferBytes);
    REQUIRE(server.started());
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));

    // KEEP has no cancel-on-disconnect: its store holds System Event 'S' and
    // one Order Accepted per order.
    {
        OuchRecorder live_events;
        OuchSession keep(live_events);
        auto pump = [&] { keep.poll(); };
        REQUIRE(keep.connect_and_login(server.ouch(), {"KEEP", "keep", ""}, 0));
        REQUIRE(wait_until([&] { return live_events.logins_accepted == 1; }, pump));
        for (int order = 0; order < kOrders; order += 10) {
            for (int index = order; index < order + 10; ++index) {
                REQUIRE(keep.client().enter_order(
                    ouch_limit_order("R" + std::to_string(index), Side::BUY, 10, 100 * kDollar), net::monotonic_ns()));
            }
            keep.flush();
            REQUIRE(wait_until([&] { return live_events.accepted.size() == std::size_t(order + 10); }, pump));
        }
        keep.disconnect();
    }
    REQUIRE(wait_until(
        [&] { return admin.request("status").find("user KEEP session=5 protocol=ouch42 bound=no") != std::string::npos; },
        [] {}));

    OuchRecorder replay_events;
    OuchSession replay(replay_events);
    auto pump = [&] { replay.poll(); };
    REQUIRE(replay.connect_and_login(server.ouch(), {"KEEP", "keep", ""}, 1));
    REQUIRE(wait_until([&] { return replay_events.sequences.size() == std::size_t(1 + kOrders) || !replay.connected(); },
                       pump));
    CHECK(replay.connected());
    CHECK_EQ(replay_events.logins_accepted, 1);
    CHECK_EQ(replay_events.sequences.size(), std::size_t(1 + kOrders));
    CHECK_EQ(replay_events.accepted.size(), std::size_t(kOrders));
    for (std::size_t index = 0; index < replay_events.sequences.size(); ++index) {
        CHECK_EQ(replay_events.sequences[index], index + 1);  // in order, no gap, no repeat
    }

    // Live output continues after the replay on the same connection.
    REQUIRE(replay.client().enter_order(ouch_limit_order("AFTER", Side::BUY, 10, 100 * kDollar), net::monotonic_ns()));
    replay.flush();
    REQUIRE(wait_until([&] { return replay_events.has_accepted("AFTER"); }, pump));
    CHECK_EQ(replay_events.sequences.back(), std::uint64_t(2 + kOrders));
}

TEST_CASE(server_e2e_fix_resend, resend_larger_than_the_connection_send_buffer_completes_in_order) {
    constexpr int kOrders = 300;  // 300 accepts and 300 cancel-on-disconnect reports: about 40 send buffers
    ServerUnderTest server(make_e2e_config(/*with_bots=*/false), 0, /*bots_enabled=*/false, /*busy_poll=*/false,
                           kConnectionSendBufferBytes);
    REQUIRE(server.started());
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));

    FixRecorder carol_events;
    FixSession carol(carol_events, fix_settings("CAROL", "carol"));
    auto pump = [&] { carol.poll(); };
    REQUIRE(carol.connect_and_logon(server.fix(), false));
    REQUIRE(wait_until([&] { return carol_events.logons == 1; }, pump));
    for (int order = 0; order < kOrders; order += 10) {
        for (int index = order; index < order + 10; ++index) {
            REQUIRE(carol.client().send_new_order_single(
                fix_limit_order("R" + std::to_string(index), Side::BUY, 10, 100 * kDollar), client::fix_time_now()));
        }
        REQUIRE(carol.flush());
        REQUIRE(wait_until([&] { return carol_events.reports.size() == std::size_t(order + 10); }, pump));
    }
    // Disconnect cancels the 300 orders; their reports are stored for CAROL.
    carol.disconnect();
    REQUIRE(wait_until(
        [&] { return admin.request("status").find("user CAROL session=3 protocol=fix44 bound=no") != std::string::npos; },
        [] {}));

    // The client lost everything it received: the Logon response reveals the
    // gap and the client asks for the whole day again (ResendRequest 1..0).
    carol.client().session_day_state().set_expected_inbound_seq(1);
    carol_events.reports.clear();
    REQUIRE(carol.connect_and_logon(server.fix(), false));
    REQUIRE(wait_until([&] { return carol_events.logons == 2 || !carol.connected(); }, pump));
    // Sent while the resend is still paused: its report must follow the resend.
    REQUIRE(carol.client().send_new_order_single(fix_limit_order("LIVE", Side::BUY, 10, 100 * kDollar),
                                                 client::fix_time_now()));
    REQUIRE(carol.flush());
    REQUIRE(wait_until([&] { return carol_events.count("LIVE", '0') == 1 || !carol.connected(); }, pump));

    CHECK(carol.connected());
    CHECK_EQ(carol_events.logons, 2);
    CHECK_EQ(carol_events.reports.size(), std::size_t(2 * kOrders + 1));
    for (int index = 0; index < kOrders; ++index) {
        const std::string client_order_id = "R" + std::to_string(index);
        CHECK_EQ(carol_events.count(client_order_id, '0'), std::size_t{1});
        CHECK_EQ(carol_events.count(client_order_id, '4'), std::size_t{1});
    }
    for (std::size_t index = 1; index < carol_events.reports.size(); ++index) {
        CHECK(carol_events.reports[index].sequence > carol_events.reports[index - 1].sequence);
    }
    CHECK_EQ(carol_events.reports.back().client_order_id, std::string("LIVE"));
    CHECK(!carol.client().resend_request_outstanding());
}
