// The FIXP/SBE gateway on the real server::ExchangeCore: the core is the
// gateway Context and a FixpSbeUserStateDirectory is the core's Sessions
// policy, as in the exsim server (SERVER.md 1 and 2). Covers the core's
// SubmitResult, admin-driven fault injection, cancel-on-disconnect through
// core.unbind, and the steady-state order path without allocation.

#include "fixp_sbe_test_support.hpp"
#include <tests/framework/alloc_counter.hpp>

#include "exsim/server/config.hpp"
#include "exsim/server/exchange_core.hpp"

using namespace fixp_sbe_test;

namespace {

constexpr exsim::Timestamp kOneSecondNs = 1'000'000'000ull;
constexpr exsim::Timestamp kTenAmNs = 36'000 * kOneSecondNs;
constexpr exsim::Timestamp kFivePmNs = 61'200 * kOneSecondNs;
constexpr std::int64_t kHundredFiftyDollars = 1'500'000;

// ALICE and BOB are configured as OUCH users by the parser (it has no FIXP/SBE
// protocol yet) and switched to the proposed protocol value after parsing.
// CAROL stays OUCH, MM1 belongs to a disabled bot.
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
protocol = ouch42
session_id = 3
owner = 3

[user MM1]
password = mm1
protocol = ouch42
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
    exsim::Timestamp day = kTenAmNs;
    [[nodiscard]] exsim::Timestamp day_ns() const noexcept { return day; }
    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return EngineTestContext::kRealtimeOriginUnixNs + day; }
};

struct DiscardingMarketData {
    void on_events(std::span<const exsim::Event>) noexcept {}
    void flush() noexcept {}
};

using Core = exsim::server::ExchangeCore<UserStateDirectory, DiscardingMarketData, SettableClock>;

static_assert(fixp::FixpSbeServerContext<Core>);
static_assert(exsim::server::CoreSessions<UserStateDirectory, Core>);

exsim::server::ServerConfig parse_core_config() {
    exsim::server::ServerConfig config;
    std::string error;
    const bool parsed = exsim::server::parse_config(kCoreConfig, config, error);
    if (!parsed) testfw::report_failure(__FILE__, __LINE__, "config: " + error);
    REQUIRE(parsed);
    for (exsim::server::UserConfig& user : config.users) {
        if (user.username == "ALICE" || user.username == "BOB" || user.username == "MM1") {
            user.protocol = exsim::server::Protocol::FixpSbe;
        }
    }
    return config;
}

// The server's objects, in construction order.
struct CoreExchange {
    CoreExchange()
        : config(parse_core_config()),
          user_states(config, EngineTestContext::small_user_state_settings()),
          session_settings(fixp::FixpSbeSessionSettings::from(config.exchange)),
          core(config, user_states, market_data, clock) {
        core.set_bots_enabled(false);
    }

    std::string admin(std::string_view line) {
        std::string reply;
        core.execute_line(line, reply);
        return reply;
    }

    exsim::server::ServerConfig config;
    UserStateDirectory user_states;
    fixp::FixpSbeSessionSettings session_settings;
    DiscardingMarketData market_data;
    SettableClock clock;
    Core core;
    exsim::server::ConnectionId next_connection_id = 1;
};

// One TCP connection's server side, driven with raw client frames and the
// core as Context.
class CoreSession {
public:
    explicit CoreSession(CoreExchange& exchange)
        : exchange_(exchange),
          connection_(std::make_unique<Connection>(MemorySink(&output_), exchange.user_states,
                                                   exchange.next_connection_id++, exchange.session_settings,
                                                   exchange.core.now())) {}
    ~CoreSession() { disconnect(); }

    CoreSession(const CoreSession&) = delete;
    CoreSession& operator=(const CoreSession&) = delete;

    template <class Message>
    void send(const Message& message) {
        const std::vector<char> frame = encode_frame(message);
        send_bytes(frame);
    }
    void send_bytes(std::span<const char> bytes) { connection_->on_data(bytes, exchange_.core); }

    bool log_in(const sbe::SessionUuid& session_uuid, std::string_view credentials) {
        sbe::Negotiate negotiate;
        negotiate.session_id = session_uuid;
        negotiate.timestamp_ns = exchange_.core.realtime_ns();
        negotiate.credentials = credentials;
        send(negotiate);
        sbe::Establish establish;
        establish.session_id = session_uuid;
        establish.timestamp_ns = exchange_.core.realtime_ns();
        establish.keepalive_interval_ms = 1000;
        send(establish);
        if (connection_->state() != fixp::FixpSbeConnectionState::Established) return false;
        sbe::Sequence sequence;
        sequence.next_seq_no = 1;
        send(sequence);
        output_.clear();
        return true;
    }

    [[nodiscard]] std::vector<ParsedFrame> take_output() { return take_frames(output_); }
    [[nodiscard]] MemoryBuffer& output() noexcept { return output_; }
    [[nodiscard]] Connection& connection() { return *connection_; }

    void disconnect() {
        if (connection_ != nullptr) {
            connection_->on_disconnect(exchange_.core);
            connection_.reset();
        }
    }

private:
    CoreExchange& exchange_;
    MemoryBuffer output_;
    std::unique_ptr<Connection> connection_;
};

}  // namespace

TEST_CASE(fixp_sbe_core, directory_holds_only_wire_fixp_users) {
    CoreExchange exchange;
    CHECK_EQ(exchange.user_states.size(), 2u);
    CHECK(exchange.user_states.find(1) != nullptr);
    CHECK(exchange.user_states.find(2) != nullptr);
    CHECK(exchange.user_states.find(3) == nullptr);    // OUCH user
    CHECK(exchange.user_states.find(100) == nullptr);  // driven by a bot
    CHECK(exchange.user_states.find(60000) == nullptr);

    CoreSession carol(exchange);
    CHECK(!carol.log_in(kFirstSessionUuid, "CAROL:carolpw"));
    const std::vector<ParsedFrame> frames = carol.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK(frames[0].as<sbe::NegotiationReject>().code == sbe::NegotiationRejectCode::Credentials);
}

TEST_CASE(fixp_sbe_core, two_users_trade_through_the_core) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    REQUIRE(alice.log_in(kFirstSessionUuid, "ALICE:alicepw"));
    CHECK(exchange.core.connection(1).has_value());
    alice.send(limit_order(1, sbe::SideCode::Buy, kHundredFiftyDollars, 100));
    std::vector<ParsedFrame> frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    const sbe::ExecutionReportNew accepted = frames[0].as<sbe::ExecutionReportNew>();
    CHECK_EQ(accepted.cl_ord_id, 1u);
    CHECK_EQ(accepted.transact_time_ns, exchange.core.realtime_ns());

    CoreSession bob(exchange);
    REQUIRE(bob.log_in(kSecondSessionUuid, "BOB:bobpw"));
    bob.send(limit_order(7, sbe::SideCode::Sell, kHundredFiftyDollars, 40));
    frames = bob.take_output();
    REQUIRE_EQ(frames.size(), 2u);
    CHECK(frames[0].is(sbe::TemplateId::ExecutionReportNew));
    const sbe::ExecutionReportTrade bob_trade = frames[1].as<sbe::ExecutionReportTrade>();
    CHECK(bob_trade.last_liquidity_ind == sbe::LastLiquidityIndicator::RemovedLiquidity);
    CHECK(bob_trade.ord_status == sbe::OrdStatusCode::Filled);
    CHECK_EQ(bob_trade.last_qty, 40u);

    frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    const sbe::ExecutionReportTrade alice_trade = frames[0].as<sbe::ExecutionReportTrade>();
    CHECK(alice_trade.last_liquidity_ind == sbe::LastLiquidityIndicator::AddedLiquidity);
    CHECK(alice_trade.ord_status == sbe::OrdStatusCode::PartiallyFilled);
    CHECK_EQ(alice_trade.leaves_qty, 60u);
    CHECK_EQ(alice_trade.trd_match_id, bob_trade.trd_match_id);

    alice.send(cancel_request(2, 1));
    frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    const sbe::ExecutionReportCancel canceled = frames[0].as<sbe::ExecutionReportCancel>();
    CHECK_EQ(canceled.canceled_qty, 60u);
    CHECK(canceled.ord_status == sbe::OrdStatusCode::Canceled);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
}

TEST_CASE(fixp_sbe_core, admin_faults_then_cancel_on_disconnect_and_recovery) {
    CoreExchange exchange;
    fixp::FixpSbeUserState<MemorySink>& alice_state = *exchange.user_states.find(1);
    {
        CoreSession alice(exchange);
        REQUIRE(alice.log_in(kFirstSessionUuid, "ALICE:alicepw"));
        CHECK(exchange.admin("fault user ALICE reject 1").ends_with("OK\n"));
        alice.send(limit_order(1, sbe::SideCode::Buy, kHundredFiftyDollars, 10));
        std::vector<ParsedFrame> frames = alice.take_output();
        REQUIRE_EQ(frames.size(), 1u);
        CHECK(frames[0].as<sbe::ExecutionReportReject>().exsim_reject_reason == sbe::ExsimRejectReasonCode::InjectedReject);
        CHECK_EQ(exchange.core.engine().live_order_count(), 0u);  // the engine never saw it

        // drop_out: stored but not sent; the next real-time message is
        // preceded by Sequence so the client sees the gap at once.
        CHECK(exchange.admin("fault user ALICE drop_out 1").ends_with("OK\n"));
        alice.send(limit_order(2, sbe::SideCode::Buy, kHundredFiftyDollars, 10));
        CHECK(alice.output().empty());
        alice.send(limit_order(3, sbe::SideCode::Buy, kHundredFiftyDollars, 10));
        frames = alice.take_output();
        REQUIRE_EQ(frames.size(), 2u);
        CHECK_EQ(frames[0].as<sbe::Sequence>().next_seq_no, 3u);
        CHECK(frames[1].is(sbe::TemplateId::ExecutionReportNew));
    }
    // The disconnect unbound ALICE: cancel-on-disconnect canceled both live
    // orders and reported them (stored only, no connection).
    CHECK(!exchange.core.connection(1).has_value());
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    CHECK_EQ(alice_state.last_server_sequence_number(), 6u);  // reject, 2 x New, 2 x Cancel, mass cancel report

    CoreSession alice(exchange);
    sbe::Establish establish;
    establish.session_id = kFirstSessionUuid;
    establish.timestamp_ns = exchange.core.realtime_ns();
    establish.keepalive_interval_ms = 1000;
    establish.credentials = "ALICE:alicepw";
    alice.send(establish);
    std::vector<ParsedFrame> frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].as<sbe::EstablishmentAck>().next_seq_no, 7u);
    sbe::RetransmitRequest request;
    request.session_id = kFirstSessionUuid;
    request.timestamp_ns = exchange.core.realtime_ns();
    request.from_seq_no = 4;
    request.count = 3;
    alice.send(request);
    frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 5u);
    CHECK(frames[0].is(sbe::TemplateId::Retransmission));
    CHECK(frames[1].as<sbe::ExecutionReportCancel>().cancel_reason == sbe::ExsimCancelReasonCode::CancelOnDisconnect);
    CHECK(frames[2].is(sbe::TemplateId::ExecutionReportCancel));
    const sbe::OrderMassCancelReport report = frames[3].as<sbe::OrderMassCancelReport>();
    CHECK_EQ(report.total_affected_orders, 2u);
    CHECK_EQ(report.cl_ord_id, 0u);
    CHECK_EQ(frames[4].as<sbe::Sequence>().next_seq_no, 7u);
}

TEST_CASE(fixp_sbe_core, outside_trading_day_is_answered_market_closed) {
    CoreExchange exchange;
    CoreSession alice(exchange);
    REQUIRE(alice.log_in(kFirstSessionUuid, "ALICE:alicepw"));
    exchange.clock.day = kFivePmNs;
    alice.send(limit_order(1, sbe::SideCode::Buy, kHundredFiftyDollars, 10));
    const std::vector<ParsedFrame> frames = alice.take_output();
    REQUIRE_EQ(frames.size(), 1u);
    const sbe::ExecutionReportReject reject = frames[0].as<sbe::ExecutionReportReject>();
    CHECK(reject.exsim_reject_reason == sbe::ExsimRejectReasonCode::MarketClosed);
    CHECK(reject.ord_rej_reason == sbe::OrdRejReasonCode::ExchangeClosed);
}

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
// Steady-state order path through the core: decode, gateway checks, engine,
// dispatch, encode, store, write. Partial fills, cancels and keepalives.
TEST_CASE(fixp_sbe_core, steady_state_order_path_allocates_nothing) {
    if (!testfw::allocation_counting_available()) return;
    CoreExchange exchange;
    CoreSession alice(exchange);
    CoreSession bob(exchange);
    REQUIRE(alice.log_in(kFirstSessionUuid, "ALICE:alicepw"));
    REQUIRE(bob.log_in(kSecondSessionUuid, "BOB:bobpw"));

    // Frames are encoded before the measured scope; ClOrdIds must stay unique
    // for the day, so each iteration uses fresh ones written in place.
    std::vector<char> buy = encode_frame(limit_order(1, sbe::SideCode::Buy, kHundredFiftyDollars, 100));
    std::vector<char> sell = encode_frame(limit_order(1, sbe::SideCode::Sell, kHundredFiftyDollars, 60));
    std::vector<char> cancel = encode_frame(cancel_request(1, 1));
    constexpr std::size_t kClOrdIdOffset = sbe::kFramePrefixSize;
    constexpr std::size_t kOrigClOrdIdOffset = sbe::kFramePrefixSize + 8;
    const auto run_iteration = [&](std::uint64_t iteration) {
        const std::uint64_t buy_id = 1 + iteration * 2;
        sbe::little_endian::store64(buy.data() + kClOrdIdOffset, buy_id);
        sbe::little_endian::store64(sell.data() + kClOrdIdOffset, 1 + iteration);
        sbe::little_endian::store64(cancel.data() + kClOrdIdOffset, buy_id + 1);
        sbe::little_endian::store64(cancel.data() + kOrigClOrdIdOffset, buy_id);
        alice.send_bytes(buy);
        bob.send_bytes(sell);
        alice.send_bytes(cancel);
        exchange.clock.day += 1'000'000;
        alice.connection().on_timer(exchange.core);
        bob.connection().on_timer(exchange.core);
        alice.output().clear();
        bob.output().clear();
    };
    for (std::uint64_t iteration = 0; iteration < 16; ++iteration) run_iteration(iteration);  // warm up

    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (std::uint64_t iteration = 16; iteration < 1016; ++iteration) run_iteration(iteration);
        allocations = scope.count();
    }
    CHECK_EQ(allocations, 0u);
    CHECK(alice.connection().state() == fixp::FixpSbeConnectionState::Established);
    CHECK(bob.connection().state() == fixp::FixpSbeConnectionState::Established);
    CHECK_EQ(exchange.core.engine().live_order_count(), 0u);
    // Per iteration: ALICE New, Trade, Cancel; BOB New, Trade.
    CHECK_EQ(exchange.user_states.find(1)->last_server_sequence_number(), 3u * 1016u);
    CHECK_EQ(exchange.user_states.find(2)->last_server_sequence_number(), 2u * 1016u);
}
#endif
#endif
