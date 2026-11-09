#include "test/exsim/gateway/fix/fix_test_support.hpp"
#include "server/exchange_core.hpp"
#include "test/framework/alloc_counter.hpp"

using namespace fix_test;

namespace {
struct CoreClock {
    exsim::Timestamp day = kSessionStartNs;
    exsim::Timestamp day_ns() const noexcept { return day; }
    std::uint64_t realtime_ns() const noexcept { return kMidnightRealtimeNs + day; }
};
struct MarketData {
    void on_events(std::span<const exsim::Event>) noexcept {}
    void flush() noexcept {}
};
struct Sessions {
    fix::FixUserDirectory<TestByteSink>* directory = nullptr;
    template <class Context>
    void on_events(exsim::SessionId session, std::span<const exsim::Event> batch, Context& ctx) noexcept {
        if (auto* user = directory->find(session)) user->on_events(batch, ctx);
    }
};
exsim::server::ServerConfig config() {
    exsim::server::ServerConfig result;
    result.engine.engine = {4, 64};
    result.users = default_users();
    exsim::InstrumentConfig instrument;
    instrument.symbol = exsim::Symbol("AAPL");
    instrument.initial_phase = exsim::Phase::Continuous;
    result.instruments.push_back(instrument);
    result.exchange.trading_day_end_ns = kSessionStartNs + 3600 * fix::kNanosecondsPerSecond;
    return result;
}
using Core = exsim::server::ExchangeCore<Sessions, MarketData, CoreClock>;
static_assert(fix::FixGatewayContext<Core>);
struct Harness {
    exsim::server::ServerConfig settings = config();
    fix::FixUserDirectory<TestByteSink> directory{settings.users, "EXSIM", small_limits()};
    Sessions sessions{&directory};
    MarketData market_data;
    CoreClock clock;
    Core core;
    TestByteSink sink;
    fix::FixConnection<Core, TestByteSink> connection{directory};
    std::uint64_t sequence = 1;

    explicit Harness(exsim::server::CoreOptions options = {})
        : core(settings, sessions, market_data, clock, options) {
        connection.start(sink, 123, core);
        send("A", "98=0|108=30|554=secret1|");
        sink.clear();
    }
    ~Harness() { connection.on_disconnected(core); }
    void send(std::string_view type, std::string_view fields) {
        auto frame = client_message(type, sequence++, fields);
        REQUIRE_EQ(connection.on_data({frame.data(), frame.size()}, core), frame.size());
    }
    std::vector<ParsedFixMessage> drain() {
        auto frames = split_frames(sink.bytes());
        sink.clear();
        return frames;
    }
    void admin(std::string_view text) {
        std::string reply;
        core.execute_line(text, reply);
        CHECK(reply.find("ERR") == std::string::npos);
    }
};
}

TEST_CASE(gateway_fix_core, admin_reject_drop_and_recovery) {
    Harness h;
    CHECK(h.core.is_bound(1));
    CHECK_EQ(h.core.connection(1), 123u);
    h.admin("fault user CLIENT1 reject 1");
    h.send("D", new_order_fields("REJECT", "1", 100, "150"));
    auto frames = h.drain();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].get(39), std::string("8"));
    CHECK_EQ(frames[0].get(58), std::string("injected"));
    CHECK_EQ(h.core.counters().commands_applied, 0u);
    h.admin("fault user CLIENT1 drop_out 1");
    h.send("D", new_order_fields("ORDER", "1", 100, "150"));
    CHECK(h.drain().empty());
    CHECK_EQ(h.core.counters().commands_applied, 1u);
    h.send("2", "7=3|16=3|");
    frames = h.drain();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].get(11), std::string("ORDER"));
    CHECK_EQ(frames[0].get(43), std::string("Y"));
    h.connection.on_disconnected(h.core);
    CHECK(!h.core.is_bound(1));
    CHECK_EQ(h.core.counters().commands_applied, 2u); // cancel-on-disconnect
}

// FIX QA R1: a connection's release unbinds only its own binding. If the
// session was released early and a newer login bound it, the old
// connection's disconnect handler must not unbind or cancel the newer one.
TEST_CASE(gateway_fix_core, stale_connection_release_keeps_the_newer_binding) {
    Harness h;
    h.core.unbind(1, 123);  // released early, cancel-on-disconnect applied
    const exsim::server::ConnectionId newer_connection = 456;
    REQUIRE(h.core.bind(1, newer_connection));
    exsim::NewOrder order;
    order.instrument = 1;
    order.session = 1;
    order.owner = h.settings.users[0].owner;
    order.cl_ord_id = 4242;
    order.side = exsim::Side::BUY;
    order.price = 1'500'000;
    order.qty = 100;
    REQUIRE(h.core.submit(exsim::Command{order}) == exsim::server::SubmitResult::Applied);
    REQUIRE_EQ(h.core.engine().live_order_count(), 1u);

    h.connection.on_disconnected(h.core);  // the old connection's handler runs late
    CHECK_EQ(h.core.connection(1), newer_connection);
    CHECK_EQ(h.core.engine().live_order_count(), 1u);
    h.core.unbind(1, newer_connection);
    CHECK_EQ(h.core.engine().live_order_count(), 0u);
}

TEST_CASE(gateway_fix_core, refused_submissions_receive_application_responses) {
    Harness h;
    h.send("D", new_order_fields("ORDER", "1", 100, "150"));
    h.drain();
    h.clock.day = h.settings.exchange.trading_day_end_ns;
    h.send("D", new_order_fields("LATE", "1", 100, "150"));
    h.send("F", "41=ORDER|11=CANCEL|55=AAPL|54=1|60=20261103-09:30:00.000|");
    h.send("G", "41=ORDER|11=REPLACE|55=AAPL|54=1|60=20261103-09:30:00.000|38=90|40=2|44=150|");
    h.send("q", "11=MASS|530=7|");
    const auto frames = h.drain();
    REQUIRE_EQ(frames.size(), 4u);
    CHECK_EQ(frames[0].get(39), std::string("8"));
    CHECK_EQ(frames[0].get(58), std::string("outside trading day"));
    CHECK_EQ(frames[1].get(434), std::string("1"));
    CHECK_EQ(frames[2].get(434), std::string("2"));
    CHECK_EQ(frames[3].get(531), std::string("0"));
    CHECK_EQ(h.core.counters().commands_applied, 1u);
    CHECK_EQ(h.core.counters().commands_outside_trading_day, 4u);
}

TEST_CASE(gateway_fix_core, undersized_core_output_is_reported) {
    Harness h({1});
    h.send("D", new_order_fields("ORDER", "1", 100, "150"));
    const auto frames = h.drain();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].get(39), std::string("8"));
    CHECK_EQ(frames[0].get(58), std::string("event buffer too small"));
}

TEST_CASE(gateway_fix_core, steady_state_allocates_nothing) {
    Harness h;
    const auto order = client_message("D", 2, new_order_fields("ORDER", "1", 100, "150"));
    const auto cancel = client_message("F", 3, "41=ORDER|11=CANCEL|55=AAPL|54=1|60=20261103-09:30:00.000|");
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        h.connection.on_data({order.data(), order.size()}, h.core);
        h.connection.on_data({cancel.data(), cancel.size()}, h.core);
        allocations = scope.count();
    }
    CHECK_EQ(allocations, 0u);
    CHECK_EQ(h.core.counters().commands_applied, 2u);
}
