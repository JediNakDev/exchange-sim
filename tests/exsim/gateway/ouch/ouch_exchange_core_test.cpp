// The OUCH gateway on the real server::ExchangeCore: the core is the gateway
// Context and an OuchUserStateDirectory is the core's Sessions policy, as in
// the exsim server. Covers what the in-memory TestExchange cannot: the
// core's SubmitResult, its fault state driven by admin commands, unbind with
// cancel-on-disconnect, the shutdown hook, and bot and FIX users.

#include "ouch_test_harness.hpp"
#include "../../../framework/alloc_counter.hpp"
#include "exsim/server/config.hpp"
#include "exsim/server/exchange_core.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Timestamp kNineAm = 32'400 * kOneSecond;
constexpr Timestamp kTenAm = 36'000 * kOneSecond;
constexpr Timestamp kFourThirtyPm = 59'400 * kOneSecond;
constexpr Price kHundredFiftyDollars = 1'500'000;

// Trading day 09:30 to 16:00. ALICE and BOB trade over OUCH, CAROL is a FIX
// user and MM1 belongs to a (disabled) market-making bot.
const char* const kCoreConfig = R"(
[exchange]
soup_session = EXSIM
heartbeat_interval_ms = 1000
trading_day_start = 09:30
trading_day_end = 16:00

[engine]
max_instruments = 4
max_orders = 4096

[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 300
reference_price = 150
initial_phase = continuous

[user ALICE]
password = alicepw
protocol = ouch42
session_id = 1
owner = 1
cancel_on_disconnect = true

[user BOB]
password = bobpw
protocol = ouch42
session_id = 2
owner = 2
cancel_on_disconnect = false

[user CAROL]
password = carolpw
protocol = fix44
session_id = 3
owner = 3

[user MM1]
password = mm1
session_id = 100
owner = 100

[bot maker]
type = market_maker
user = MM1
instrument = AAPL
seed = 7
levels = 2
size = 100
spread_ticks = 1
requote_ms = 50
fair_value_step_ms = 200
)";

struct SettableClock {
    Timestamp day = kTenAm;
    [[nodiscard]] Timestamp day_ns() const noexcept { return day; }
    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return 1'700'000'000ull * kOneSecond + day; }
};

struct DiscardingMarketData {
    void on_events(std::span<const Event>) noexcept {}
    void flush() noexcept {}
};

using Core = server::ExchangeCore<TestUserStateDirectory, DiscardingMarketData, SettableClock>;

static_assert(OuchServerContext<Core>);
static_assert(server::CoreSessions<TestUserStateDirectory, Core>);
static_assert(server::SessionsShutdown<TestUserStateDirectory, Core>);

server::ServerConfig parse_core_config() {
    server::ServerConfig config;
    std::string error;
    const bool parsed = server::parse_config(kCoreConfig, config, error);
    if (!parsed) testfw::report_failure(__FILE__, __LINE__, "config: " + error);
    REQUIRE(parsed);
    return config;
}

OuchUserStateLimits small_limits() {
    OuchUserStateLimits limits;
    limits.max_tokens = 8192;
    limits.store_messages = 16384;
    limits.store_bytes = std::size_t{2} << 20;
    limits.store_reserve_messages = 0;
    return limits;
}

// The server's objects, in construction order.
struct CoreExchange {
    CoreExchange()
        : config(parse_core_config()),
          user_states(config, small_limits()),
          session_settings(OuchSessionSettings::from(config.exchange)),
          core(config, user_states, market_data, clock) {
        core.set_bots_enabled(false);
    }

    std::string admin(std::string_view line) {
        std::string reply;
        core.execute_line(line, reply);
        return reply;
    }

    server::ServerConfig config;
    TestUserStateDirectory user_states;
    OuchSessionSettings session_settings;
    DiscardingMarketData market_data;
    SettableClock clock;
    Core core;
    server::ConnectionId next_connection_id = 1;
};

// One TCP connection's server side, driven with the core as Context.
class CoreSession {
public:
    explicit CoreSession(CoreExchange& exchange)
        : exchange_(exchange),
          connection_(std::make_unique<TestConnection>(MemoryByteSink(), exchange.user_states,
                                                       exchange.next_connection_id++, exchange.session_settings,
                                                       exchange.core.now())) {}
    ~CoreSession() { disconnect(); }

    void send(std::string_view bytes) {
        connection_->on_data(std::span<const char>(bytes.data(), bytes.size()), exchange_.core);
    }
    template <class Message>
    void send_message(const Message& message) {
        send(unsequenced_frame(message));
    }
    bool login(std::string_view username, std::string_view password, std::uint64_t sequence = 0) {
        send(login_frame(username, password, "", sequence));
        return connection_->state() == OuchConnectionState::LoggedIn;
    }
    std::vector<std::string> take_frames() {
        std::vector<std::string> frames = split_frames(connection_->sink().view());
        connection_->sink().clear();
        return frames;
    }
    std::string take_types() { return sequenced_types(take_frames()); }
    void disconnect() {
        if (connection_ != nullptr) {
            connection_->on_disconnect(exchange_.core);
            connection_.reset();
        }
    }
    TestConnection& connection() { return *connection_; }

private:
    CoreExchange& exchange_;
    std::unique_ptr<TestConnection> connection_;
};

bool reply_ends_with_ok(const std::string& reply) {
    return reply.size() >= 3 && reply.compare(reply.size() - 3, 3, "OK\n") == 0;
}

}  // namespace

TEST_CASE(gateway_ouch_core, directory_holds_only_wire_ouch_users) {
    CoreExchange exchange;
    CHECK_EQ(exchange.user_states.size(), 2u);
    CHECK(exchange.user_states.find(1) != nullptr);
    CHECK(exchange.user_states.find(2) != nullptr);
    CHECK(exchange.user_states.find(3) == nullptr);    // FIX user
    CHECK(exchange.user_states.find(100) == nullptr);  // driven by a bot
    CHECK(exchange.user_states.find(4000) == nullptr);

    for (const auto& [username, password] : {std::pair{"CAROL", "carolpw"}, std::pair{"MM1", "mm1"}}) {
        CoreSession session(exchange);
        CHECK(!session.login(username, password));
        const std::vector<std::string> frames = session.take_frames();
        REQUIRE_EQ(frames.size(), 1u);
        CHECK(frames[0] == Golden().byte('A').soup('J'));  // Login Rejected, not authorized
        CHECK(session.connection().close_reason() == OuchCloseReason::LoginRejected);
    }
    CoreSession alice(exchange);
    CHECK(!alice.login("ALICE", "wrong"));
    CHECK(exchange.core.connection(1) == std::nullopt);
}

TEST_CASE(gateway_ouch_core, two_ouch_users_trade_through_the_core) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    REQUIRE(alice.login("ALICE", "alicepw"));
    CHECK(exchange.core.connection(1).has_value());
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0] == Golden().alpha("EXSIM", 10).numeric(1, 20).soup('A'));
    CHECK(frames[1] == Golden().byte('S').u64(kTenAm).byte('S').soup('S'));

    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredFiftyDollars));
    CHECK_EQ(alice.take_types(), "A");

    CoreSession bob(exchange);
    REQUIRE(bob.login("BOB", "bobpw"));
    bob.take_frames();
    bob.send_message(enter_order("ASK1", 'S', 40, "AAPL", kHundredFiftyDollars));
    frames = bob.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "AE");
    const auto bob_execution = decode_ouch<ouch42::OrderExecuted>(frames[1]);
    CHECK_EQ(bob_execution.liquidity, 'R');
    CHECK_EQ(bob_execution.shares, 40u);

    frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "E");
    const auto alice_execution = decode_ouch<ouch42::OrderExecuted>(frames[0]);
    CHECK_EQ(alice_execution.liquidity, 'A');
    CHECK_EQ(alice_execution.price, kHundredFiftyDollars);
    CHECK_EQ(alice_execution.match_number, bob_execution.match_number);

    alice.send_message(cancel_order("BID1", 0));
    frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "C");
    CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[0]).shares, 60u);
    CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[0]).reason, 'U');
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
}

TEST_CASE(gateway_ouch_core, admin_faults_reject_and_drop_then_cancel_on_disconnect) {
    CoreExchange exchange;
    std::uint64_t alice_messages = 0;
    {
        CoreSession alice(exchange);
        REQUIRE(alice.login("ALICE", "alicepw"));
        alice.take_frames();

        CHECK(reply_ends_with_ok(exchange.admin("fault user ALICE reject 1")));
        alice.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredFiftyDollars));
        std::vector<std::string> frames = alice.take_frames();
        REQUIRE_EQ(sequenced_types(frames), "J");
        CHECK_EQ(decode_ouch<ouch42::OrderRejected>(frames[0]).reason, 'O');
        CHECK_EQ(exchange.core.engine().live_order_count(), 0u);  // the engine never saw it
        alice.send_message(enter_order("BID2", 'B', 10, "AAPL", kHundredFiftyDollars));
        CHECK_EQ(alice.take_types(), "A");  // one count consumed

        CHECK(reply_ends_with_ok(exchange.admin("fault user ALICE drop_out 1")));
        alice.send_message(enter_order("BID3", 'B', 10, "AAPL", kHundredFiftyDollars));
        CHECK(alice.take_frames().empty());  // stored as message 4, not transmitted
        // The link is cut after the last dropped message (implicit SoupBinTCP numbering).
        CHECK(alice.connection().close_reason() == OuchCloseReason::FaultDropOut);
        CHECK_EQ(exchange.user_states.find(1)->statistics().outbound_messages_dropped_by_fault, 1u);
        // on_data released the closed connection: the core unbound ALICE and
        // cancel-on-disconnect cancelled BID2 and BID3.
        CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    }
    CHECK(exchange.core.connection(1) == std::nullopt);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    alice_messages = exchange.user_states.find(1)->store().last_seq();
    CHECK_EQ(alice_messages, 6u);  // S J A A C C

    // The client received 3 messages, so it asks for 4: the dropped one first.
    CoreSession recovered(exchange);
    REQUIRE(recovered.login("ALICE", "alicepw", 4));
    const std::vector<std::string> frames = recovered.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "ACC");
    CHECK_EQ(token_text(decode_ouch<ouch42::OrderAccepted>(frames[1]).token), "BID3");
    CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[2]).reason, 'U');
}

// FIX QA R1: a connection's release unbinds only its own binding. The old
// server loop unbound a requested disconnect itself; a newer login then bound
// the session and the old connection's disconnect handler must not undo it.
TEST_CASE(gateway_ouch_core, stale_connection_release_keeps_the_newer_binding) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    REQUIRE(alice.login("ALICE", "alicepw"));
    const server::ConnectionId old_connection = alice.connection().connection_id();
    exchange.core.unbind(1, old_connection);  // released early, cancel-on-disconnect applied
    const server::ConnectionId newer_connection = 9999;
    REQUIRE(exchange.core.bind(1, newer_connection));
    NewOrder order;
    order.instrument = 1;
    order.session = 1;
    order.owner = 1;
    order.cl_ord_id = 4242;
    order.side = Side::BUY;
    order.price = kHundredFiftyDollars;
    order.qty = 10;
    const auto next_sequence = exchange.user_states.find(1)->store().next_seq();
    REQUIRE(exchange.core.submit(Command{order}) == server::SubmitResult::Applied);
    // This core-originated order has no interned OUCH token. Its wire field
    // remains space-padded without copying from an empty view's null pointer.
    const auto stored = exchange.user_states.find(1)->store().get(next_sequence);
    REQUIRE(!stored.bytes.empty());
    const auto accepted = decode_ouch<ouch42::OrderAccepted>(std::string(stored.bytes.begin(), stored.bytes.end()));
    CHECK_EQ(std::string(accepted.token.begin(), accepted.token.end()), std::string(accepted.token.size(), ' '));
    REQUIRE_EQ(exchange.core.engine().live_order_count(), 1u);

    alice.disconnect();  // the old connection's handler runs late
    CHECK(exchange.core.connection(1) == std::optional<server::ConnectionId>(newer_connection));
    CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
    exchange.core.unbind(1, newer_connection);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
}

TEST_CASE(gateway_ouch_core, orders_outside_the_trading_day_are_rejected_exchange_closed) {
    CoreExchange exchange;
    exchange.clock.day = kNineAm;  // before 09:30
    CoreSession bob(exchange);
    REQUIRE(bob.login("BOB", "bobpw"));
    bob.take_frames();
    bob.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredFiftyDollars));
    std::vector<std::string> frames = bob.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "J");
    CHECK_EQ(decode_ouch<ouch42::OrderRejected>(frames[0]).reason, 'C');

    exchange.clock.day = kTenAm;
    bob.send_message(enter_order("BID1", 'B', 10, "AAPL", kHundredFiftyDollars));
    CHECK(bob.take_frames().empty());  // the token was used today: ignored
    bob.send_message(enter_order("BID2", 'B', 10, "AAPL", kHundredFiftyDollars));
    CHECK_EQ(bob.take_types(), "A");

    exchange.clock.day = kFourThirtyPm;  // after 16:00, before the core timer ends the day
    bob.send_message(replace_order("BID2", "BID3", 20, kHundredFiftyDollars));
    frames = bob.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "J");
    CHECK_EQ(token_text(decode_ouch<ouch42::OrderRejected>(frames[0]).token), "BID3");
    CHECK_EQ(decode_ouch<ouch42::OrderRejected>(frames[0]).reason, 'C');
    bob.send_message(cancel_order("BID2", 0));
    CHECK(bob.take_frames().empty());  // OUCH has no cancel reject
    CHECK_EQ(exchange.core.engine().live_order_count(), 1u);
}

TEST_CASE(gateway_ouch_core, shutdown_expires_day_orders_then_sends_end_of_day) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    REQUIRE(alice.login("ALICE", "alicepw"));
    alice.send_message(enter_order("BID1", 'B', 100, "AAPL", kHundredFiftyDollars));
    alice.take_frames();

    exchange.core.shutdown();
    CHECK(exchange.core.shutdown_requested());
    std::vector<std::string> frames = alice.take_frames();
    REQUIRE_EQ(sequenced_types(frames), "CS");
    CHECK_EQ(decode_ouch<ouch42::OrderCanceled>(frames[0]).reason, 'E');
    CHECK_EQ(decode_ouch<ouch42::SystemEvent>(frames[1]).event, 'E');
    // BOB never logged in: his End of Day event is stored for a later login.
    CHECK_EQ(exchange.user_states.find(2)->store().last_seq(), 1u);

    alice.connection().end_session(exchange.core);
    frames = alice.take_frames();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0] == control_frame<'Z'>());
    CHECK(alice.connection().close_requested());
}

TEST_CASE(gateway_ouch_core, steady_state_through_the_core_does_not_allocate) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    CoreSession bob(exchange);
    REQUIRE(alice.login("ALICE", "alicepw"));
    REQUIRE(bob.login("BOB", "bobpw"));
    alice.take_frames();
    bob.take_frames();

    // Frames are built before measuring: the gateway only sees bytes.
    constexpr std::size_t kRounds = 500;
    std::vector<std::string> alice_frames;
    std::vector<std::string> bob_frames;
    for (std::size_t round = 0; round < kRounds; ++round) {
        const std::string bid = "B" + std::to_string(round);
        alice_frames.push_back(unsequenced_frame(enter_order(bid, 'B', 100, "AAPL", kHundredFiftyDollars)));
        alice_frames.push_back(unsequenced_frame(cancel_order(bid, 0)));
        bob_frames.push_back(unsequenced_frame(enter_order("S" + std::to_string(round), 'S', 30, "AAPL",
                                                           kHundredFiftyDollars, kOuchTimeInForceImmediateOrCancel)));
    }
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (std::size_t round = 0; round < kRounds; ++round) {
            alice.send(alice_frames[2 * round]);
            bob.send(bob_frames[round]);
            alice.send(alice_frames[2 * round + 1]);
            alice.connection().sink().clear();
            bob.connection().sink().clear();
        }
        allocations = scope.count();
    }
    CHECK_EQ(exchange.user_states.find(1)->store().last_seq(), 1 + 3 * kRounds);  // S, then A E C per round
    CHECK_EQ(exchange.user_states.find(2)->store().last_seq(), 1 + 2 * kRounds);  // S, then A E per round
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    if (testfw::allocation_counting_available()) CHECK_EQ(allocations, 0u);
}
