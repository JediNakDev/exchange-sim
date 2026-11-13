#ifndef EXSIM_TESTS_SERVER_CORE_TEST_SUPPORT_HPP
#define EXSIM_TESTS_SERVER_CORE_TEST_SUPPORT_HPP

// Fakes for the ExchangeCore policies: a settable clock, recording Sessions
// and MarketData (allocate freely, for behaviour tests) and counting ones
// (allocation-free, for the steady-state allocation test).

#include "server/config.hpp"
#include "server/core_admin.hpp"
#include "server/exchange_core.hpp"
#include "server/journal/canonical.hpp"
#include "test/framework/test.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace exsim::server::core_test {

inline constexpr Timestamp kSecond = 1'000'000'000;
inline constexpr Timestamp kMillisecond = 1'000'000;

// Two instruments and five users: ALICE and BOB on OUCH, CAROL on FIX
// (ALICE and CAROL share STP owner 1), and MM1 and NT1 on OUCH with high
// session ids. Engine limits are small so the EventBuffer is.
inline const char* const kBaseConfig = R"(
[engine]
max_instruments = 4
max_orders = 4096

[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 300
reference_price = 150
initial_phase = continuous

[instrument MSFT]
tick_size = 0.01
min_price = 0.01
max_price = 600
reference_price = 400
initial_phase = pre_open

[user ALICE]
password = alice
protocol = ouch42
session_id = 1
owner = 1
stp = cancel_resting
cancel_on_disconnect = true

[user BOB]
password = bob
session_id = 2
owner = 2

[user CAROL]
password = carol
protocol = fix44
session_id = 3
owner = 1
stp = cancel_aggressor
cancel_on_disconnect = false

[user MM1]
password = mm1
session_id = 100
owner = 100

[user NT1]
password = nt1
session_id = 101
owner = 101
)";



inline ServerConfig parse_test_config(const std::string& extra = "") {
    ServerConfig config;
    std::string error;
    const bool parsed = parse_config(std::string(kBaseConfig) + extra, config, error);
    if (!parsed) {
        testfw::report_failure(__FILE__, __LINE__, "config: " + error);
    }
    REQUIRE(parsed);
    return config;
}

inline std::string temp_journal_path(const std::string& name) {
    return "/tmp/exsim-core-" + std::to_string(::getpid()) + "-" + name + ".journal";
}

struct FakeClock {
    Timestamp day = kSecond;  // ns since midnight
    std::uint64_t real = 1'700'000'000ull * kSecond;
    [[nodiscard]] Timestamp day_ns() const noexcept { return day; }
    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return real; }
};

struct SessionCall {
    SessionId session = 0;
    std::size_t batch_number = 0;  // index of the market-data batch this belongs to
    std::vector<Event> events;     // the events the session was given
};

struct Recorder {
    std::vector<Event> events;            // every batch, in order, as market data saw it
    std::vector<std::size_t> batch_ends;  // events.size() after each batch
    std::vector<SessionCall> session_calls;
    std::size_t flushes = 0;
    std::size_t market_data_timer_calls = 0;
    std::vector<Timestamp> market_data_timer_times;
    std::size_t sessions_shutdown_calls = 0;
    std::size_t market_data_shutdown_calls = 0;

    [[nodiscard]] std::size_t batch_count() const noexcept { return batch_ends.size(); }
    [[nodiscard]] std::span<const Event> batch(std::size_t index) const {
        const std::size_t begin = index == 0 ? 0 : batch_ends[index - 1];
        return {events.data() + begin, batch_ends[index] - begin};
    }
    [[nodiscard]] std::vector<SessionId> sessions_of_batch(std::size_t index) const {
        std::vector<SessionId> out;
        for (const SessionCall& call : session_calls) {
            if (call.batch_number == index) out.push_back(call.session);
        }
        return out;
    }
};

struct RecordingSessions {
    Recorder& recorder;
    // When dispatching to `unbind_when_dispatching_to`, unbind `unbind_target` once.
    SessionId unbind_when_dispatching_to = 0;
    SessionId unbind_target = 0;
    // When dispatching to `storm_when_dispatching_to`, submit `storm_commands` copies of `storm_command` once.
    SessionId storm_when_dispatching_to = 0;
    std::size_t storm_commands = 0;
    Command storm_command{};
    std::vector<SubmitResult> storm_results{};

    template <typename Core>
    void on_events(SessionId session, const SessionEvents& events, Core& core) {
        recorder.session_calls.push_back({session, recorder.batch_count(), {events.begin(), events.end()}});
        if (session == unbind_when_dispatching_to && unbind_target != 0) {
            const SessionId target = unbind_target;
            unbind_target = 0;
            core.unbind(target, core.connection(target).value_or(0));
        }
        if (session == storm_when_dispatching_to && storm_commands != 0) {
            const std::size_t n = storm_commands;
            storm_commands = 0;
            for (std::size_t i = 0; i < n; ++i) storm_results.push_back(core.submit(storm_command));
        }
    }
    template <typename Core>
    void on_shutdown(Timestamp, Core&) {
        ++recorder.sessions_shutdown_calls;
    }
};

// Every optional hook: two-argument timer, sequence, packet faults with reset, shutdown.
struct RecordingMarketData {
    Recorder& recorder;
    md::PacketFaultInjector injector{1500, 8};
    std::size_t resets = 0;

    void on_events(std::span<const Event> batch) {
        recorder.events.insert(recorder.events.end(), batch.begin(), batch.end());
        recorder.batch_ends.push_back(recorder.events.size());
    }
    void flush() { ++recorder.flushes; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return recorder.batch_count() + 1; }
    md::PacketFaultInjector& packet_faults() noexcept { return injector; }
    void reset_packet_faults() {
        injector = md::PacketFaultInjector(1500, 8);
        ++resets;
    }
    template <typename Core>
    void on_timer(Timestamp now, const Core&) {
        ++recorder.market_data_timer_calls;
        recorder.market_data_timer_times.push_back(now);
    }
    void on_shutdown(Timestamp) { ++recorder.market_data_shutdown_calls; }
};

// Packet faults but no reset hook, one-argument timer, no sequence, no shutdown.
struct NoResetMarketData {
    Recorder& recorder;
    md::PacketFaultInjector injector{1500, 8};
    void on_events(std::span<const Event> batch) {
        recorder.events.insert(recorder.events.end(), batch.begin(), batch.end());
        recorder.batch_ends.push_back(recorder.events.size());
    }
    void flush() { ++recorder.flushes; }
    md::PacketFaultInjector& packet_faults() noexcept { return injector; }
    void on_timer(Timestamp now) {
        ++recorder.market_data_timer_calls;
        recorder.market_data_timer_times.push_back(now);
    }
};

// Only the required members.
struct MinimalMarketData {
    Recorder& recorder;
    void on_events(std::span<const Event> batch) {
        recorder.events.insert(recorder.events.end(), batch.begin(), batch.end());
        recorder.batch_ends.push_back(recorder.events.size());
    }
    void flush() { ++recorder.flushes; }
};

// One core with recording fakes, for behaviour tests.
template <typename MarketData>
struct HarnessWith {
    explicit HarnessWith(const ServerConfig& config, CoreOptions options = {})
        : server_config(config),
          sessions{recorder},
          market_data{recorder},
          core(server_config, sessions, market_data, clock, options) {}

    ServerConfig server_config;  // the core keeps a reference
    Recorder recorder;
    FakeClock clock;
    RecordingSessions sessions;
    MarketData market_data;
    ExchangeCore<RecordingSessions, MarketData, FakeClock> core;

    std::string admin(std::string_view line) {
        std::string reply;
        CoreAdmin(core).execute_line(line, reply);
        return reply;
    }
};

using RecordingHarness = HarnessWith<RecordingMarketData>;

// Allocation-free fakes: fold every event into a canonical hash.
struct CountingSessions {
    std::uint64_t calls = 0;
    std::uint64_t events_seen = 0;
    template <typename Core>
    void on_events(SessionId, const SessionEvents& events, Core&) noexcept {
        ++calls;
        events_seen += events.size();
    }
};

struct CountingMarketData {
    std::uint64_t batches = 0;
    std::uint64_t events = 0;
    std::uint64_t flushes = 0;
    std::uint64_t fills = 0;
    std::uint64_t hash = 14695981039346656037ull;
    void on_events(std::span<const Event> batch) noexcept {
        ++batches;
        for (const Event& event : batch) {
            ++events;
            fills += event.type == EventType::Fill ? 1 : 0;
            hash = server::journal::hash_event(event, hash);
        }
    }
    void flush() noexcept { ++flushes; }
};

inline NewOrder limit_order(SessionId session, ClOrdId token, InstrumentId instrument, Side side, Price price,
                            Quantity qty, OwnerId owner = 0, StpMode stp = StpMode::None) {
    NewOrder order{};
    order.session = session;
    order.cl_ord_id = token;
    order.instrument = instrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.owner = owner;
    order.stp = stp;
    return order;
}

inline bool ends_with_ok(const std::string& reply) {
    return reply.size() >= 3 && reply.compare(reply.size() - 3, 3, "OK\n") == 0;
}

inline bool is_error(const std::string& reply) { return reply.rfind("ERR ", 0) == 0 && reply.back() == '\n'; }

inline bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace exsim::server::core_test

#endif
