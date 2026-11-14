// Steady state of the exchange core allocates nothing on the C++ heap:
// submit and dispatch (including deferred commands from inside a callback),
// the timer with external client commands and journaling on, bind/unbind with
// cancel-on-disconnect, and every fault query a gateway or the loop makes.

#include "test/exsim/server/core/core_test_support.hpp"
#include "test/framework/alloc_counter.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <string>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;

// Allocation-free Sessions that, once armed, unbinds a session from inside
// the dispatch to `trigger_session` (exercising the deferred queue).
struct UnbindingSessions {
    std::uint64_t calls = 0;
    SessionId trigger_session = 0;
    SessionId unbind_target = 0;

    template <typename Core>
    void on_events(SessionId session, const SessionEvents&, Core& core) noexcept {
        ++calls;
        if (session == trigger_session && unbind_target != 0) {
            const SessionId target = unbind_target;
            unbind_target = 0;
            core.unbind(target, core.connection(target).value_or(0));
        }
    }
};

using CountingCore = ExchangeCore<UnbindingSessions, CountingMarketData, FakeClock>;

struct Workload {
    UnbindingSessions& sessions;
    FakeClock& clock;
    CountingCore& core;
    Timestamp now = kSecond;
    ClOrdId next_token = 1;
    std::size_t disconnects_taken = 0;

    // One trading "second": timers every millisecond, client flow and
    // connection churn in between.
    void run_one_round() {
        for (int step = 0; step < 1000; ++step) {
            now += kMillisecond;
            clock.day = now;
            core.timer(now);
            if (step % 10 == 0) client_flow();
            if (step == 400) fault_queries();
            if (step == 700) connection_churn();
        }
    }

    void client_flow() {
        const ClOrdId resting = next_token++;
        (void)core.submit(Command{limit_order(1, resting, kAapl, Side::BUY, 1'300'000, 100)});
        ReplaceOrder replace{};
        replace.session = 1;
        replace.orig_cl_ord_id = resting;
        replace.cl_ord_id = next_token++;
        replace.price = 1'300'100;
        replace.qty = 80;
        (void)core.submit(Command{replace});
        CancelOrder cancel{};
        cancel.session = 1;
        cancel.orig_cl_ord_id = replace.cl_ord_id;
        cancel.cl_ord_id = next_token++;
        (void)core.submit(Command{cancel});
        if (const auto best_ask = core.engine().best_ask(kAapl)) {
            (void)core.submit(Command{limit_order(2, next_token++, kAapl, Side::BUY, *best_ask, 10)});
        }
        (void)core.now();
        (void)core.realtime_ns();
    }

    void connection_churn() {
        (void)core.bind(1, 1001);
        (void)core.submit(Command{limit_order(1, next_token++, kAapl, Side::SELL, 2'900'000, 10)});
        // BOB's gateway drops ALICE while handling BOB's own fill batch.
        sessions.trigger_session = 2;
        sessions.unbind_target = 1;
        (void)core.submit(Command{limit_order(2, next_token++, kAapl, Side::SELL, 1'000'000, 5)});
        sessions.unbind_target = 0;
        core.unbind(1, 1001);  // no-op if the callback already did it
        (void)core.bind(3, 1003);
        core.unbind(3, 1003);  // CAROL: no cancel-on-disconnect
    }

    void fault_queries() {
        (void)core.consume_injected_reject(1);
        (void)core.consume_drop_out(2);
        (void)core.inbound_delay_ns(3);
        (void)core.user_faults(1);
        std::array<SessionId, 4> to_close{};
        if (core.has_disconnect_requests()) {
            const std::size_t count = core.take_disconnect_requests(to_close);
            for (std::size_t i = 0; i < count; ++i) core.unbind(to_close[i], core.connection(to_close[i]).value_or(0));
            disconnects_taken += count;
        }
        (void)core.flush_journal();
    }
};

}  // namespace

TEST_CASE(server_core_allocation, steady_state_allocates_nothing) {
    if (!testfw::allocation_counting_available()) return;  // ASan/TSan builds
    const std::string journal_path = temp_journal_path("allocation");
    const ServerConfig config =
        parse_test_config(std::string{} + "\n[journal]\npath = " + journal_path + "\nflush_interval_ms = 100\n");

    UnbindingSessions sessions;
    CountingMarketData market_data;
    FakeClock clock;
    {
        CountingCore core(config, sessions, market_data, clock);
        Workload workload{sessions, clock, core};
        workload.run_one_round();  // warm-up: client flow and first journal flush

        // Fault state as the admin console would leave it (admin allocates,
        // so it stays outside the measured scope).
        std::string reply;
        CoreAdmin admin(core);
        admin.execute_line("fault user ALICE reject 1", reply);
        admin.execute_line("fault user BOB drop_out 1", reply);
        admin.execute_line("fault user CAROL delay 5", reply);
        (void)core.bind(1, 1001);
        admin.execute_line("fault user ALICE disconnect", reply);

        std::size_t allocations = 0;
        {
            testfw::AllocationScope scope;
            for (int round = 0; round < 3; ++round) workload.run_one_round();
            allocations = scope.count();
        }
        CHECK_EQ(allocations, 0u);
        CHECK(market_data.fills > 0u);
        CHECK(core.counters().commands_deferred > 0u);
        CHECK(core.counters().journal_records > 3 * 128u);  // several full journal buffers written
        CHECK(core.journal_state() == JournalState::Healthy);
        CHECK_EQ(workload.disconnects_taken, 1u);
        CHECK(!core.has_disconnect_requests());
        CHECK_EQ(core.counters().output_capacity_failures, 0u);
    }
    std::remove(journal_path.c_str());
}
