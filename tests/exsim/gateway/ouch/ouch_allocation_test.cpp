// The OUCH gateway hot path (server and client) performs no heap allocation
// after setup: thousands of orders, executions, replaces, cancels, rejects,
// heartbeats and a full replay inside testfw::AllocationScope.

#include "ouch_test_harness.hpp"
#include "../../../framework/alloc_counter.hpp"

using namespace exsim;
using namespace exsim::gateway::ouch;
using namespace exsim::gateway::ouch::testing;

namespace {

constexpr Price kHundredDollars = 1'000'000;
constexpr Price kTick = 100;

// Counts callbacks without allocating.
struct CountingHandler {
    std::uint64_t sequenced_messages = 0;
    std::uint64_t executions = 0;
    std::uint64_t rejects = 0;
    std::uint64_t server_heartbeats = 0;
    std::uint64_t last_sequence = 0;
    bool sequence_gap = false;

    void record(std::uint64_t sequence) noexcept {
        if (last_sequence != 0 && sequence != last_sequence + 1) sequence_gap = true;
        last_sequence = sequence;
        ++sequenced_messages;
    }
    void on_server_heartbeat() noexcept { ++server_heartbeats; }
    void on_system_event(const ouch42::SystemEvent&, std::uint64_t sequence) noexcept { record(sequence); }
    void on_order_accepted(const ouch42::OrderAccepted&, std::uint64_t sequence) noexcept { record(sequence); }
    void on_order_replaced(const ouch42::OrderReplaced&, std::uint64_t sequence) noexcept { record(sequence); }
    void on_order_canceled(const ouch42::OrderCanceled&, std::uint64_t sequence) noexcept { record(sequence); }
    void on_order_executed(const ouch42::OrderExecuted&, std::uint64_t sequence) noexcept {
        record(sequence);
        ++executions;
    }
    void on_order_rejected(const ouch42::OrderRejected&, std::uint64_t sequence) noexcept {
        record(sequence);
        ++rejects;
    }
};

// Writes a decimal token like "B12345" without std::string.
std::array<char, 14> numbered_token(char prefix, std::uint32_t number) noexcept {
    std::array<char, 14> token = protocol::wire::spaces<14>();
    char digits[12];
    std::size_t length = 0;
    do {
        digits[length++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0);
    token[0] = prefix;
    for (std::size_t index = 0; index < length; ++index) token[1 + index] = digits[length - 1 - index];
    return token;
}

ouch42::EnterOrder order_with_token(const std::array<char, 14>& token, char side, Quantity shares, Price price,
                                    std::uint32_t time_in_force) noexcept {
    ouch42::EnterOrder order;
    order.token = token;
    order.side = side;
    order.shares = shares;
    order.stock = Symbol("AAPL");
    order.price = price;
    order.time_in_force = time_in_force;
    order.firm = {'F', 'R', 'M', '1'};
    order.display = 'Y';
    order.capacity = 'A';
    order.ise = 'N';
    order.minimum_quantity = 0;
    order.cross_type = 'N';
    order.customer_type = ' ';
    return order;
}

}  // namespace

TEST_CASE(gateway_ouch_allocation, steady_state_conversation_and_replay_do_not_allocate) {
    TestExchangeConfig config = TestExchangeConfig::standard();
    config.limits.max_tokens = 32768;
    config.limits.store_messages = 131072;
    config.limits.store_bytes = std::size_t{16} << 20;
    TestExchange exchange(config);
    CountingHandler alice_handler;
    CountingHandler bob_handler;
    ClientLink<CountingHandler> alice(exchange, alice_handler, std::size_t{4} << 20);
    ClientLink<CountingHandler> bob(exchange, bob_handler, std::size_t{4} << 20);
    REQUIRE(alice.login("alice", "alicepw", 1));
    REQUIRE(bob.login("bob", "bobpw", 1));

    constexpr std::uint32_t kRounds = 2500;
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (std::uint32_t round = 1; round <= kRounds; ++round) {
            const Timestamp now = exchange.now();
            const Price bid_price = kHundredDollars - static_cast<Price>(round % 7) * kTick;
            // Resting bid, partially executed by an IOC sell, replaced, reduced, then cancelled.
            alice.client().enter_order(order_with_token(numbered_token('B', round), 'B', 100, bid_price, 99999), now);
            alice.pump();
            bob.client().enter_order(order_with_token(numbered_token('S', round), 'S', 30, bid_price, 0), now);
            bob.pump();
            ouch42::ReplaceOrder replace;
            replace.existing_token = numbered_token('B', round);
            replace.replacement_token = numbered_token('R', round);
            replace.shares = 90;
            replace.price = bid_price - kTick;
            replace.time_in_force = 99999;
            replace.display = 'Y';
            replace.ise = 'N';
            alice.client().replace_order(replace, now);
            ouch42::CancelOrder reduce;
            reduce.token = numbered_token('R', round);
            reduce.shares = 70;
            alice.client().cancel_order(reduce, now);
            reduce.shares = 0;
            alice.client().cancel_order(reduce, now);
            // A duplicate token (ignored) and an unknown symbol (rejected at the gateway).
            alice.client().enter_order(order_with_token(numbered_token('B', round), 'B', 1, bid_price, 99999), now);
            ouch42::EnterOrder unknown_symbol = order_with_token(numbered_token('Z', round), 'B', 1, bid_price, 99999);
            unknown_symbol.stock = Symbol("ZZZZ");
            alice.client().enter_order(unknown_symbol, now);
            alice.pump();
            // Every tenth round is followed by 1.5 s of silence: heartbeats on both sides.
            exchange.advance(round % 10 == 0 ? 1'500'000'000 : 1'000'000);
            alice.timers();
            bob.timers();
            alice.pump();
            bob.pump();
        }
        allocations = scope.count();
    }
    CHECK(alice.client().state() == OuchClientState::LoggedIn);
    CHECK(bob.client().state() == OuchClientState::LoggedIn);
    CHECK(!alice_handler.sequence_gap);
    CHECK(!bob_handler.sequence_gap);
    CHECK_GE(alice_handler.sequenced_messages, std::uint64_t{kRounds} * 6);  // A E U C C J per round
    CHECK_GE(alice_handler.executions, std::uint64_t{kRounds});
    CHECK_GE(alice_handler.rejects, std::uint64_t{kRounds});
    CHECK_GT(alice_handler.server_heartbeats, 0u);
    CHECK_EQ(exchange.engine().live_order_count(), 0u);
    if (testfw::allocation_counting_available()) CHECK_EQ(allocations, 0u);

    // Reconnect and replay the whole day. Building the connection object (the
    // test sink) is setup; the login and replay themselves must not allocate.
    const std::uint64_t messages_stored = exchange.user_state(2).store().last_seq();
    bob.drop_transport();
    bob.connect();
    CountingHandler replay_handler;
    OuchClient<MemoryByteSink, CountingHandler> replay_client(MemoryByteSink(std::size_t{1} << 16), replay_handler);
    {
        testfw::AllocationScope scope;
        REQUIRE(replay_client.login({"bob", "bobpw", ""}, 1, exchange.now()));
        bob.connection().on_data(replay_client.transport().bytes(), exchange);
        replay_client.transport().clear();
        while (bob.connection().sink().size() > 0) {
            replay_client.on_data(bob.connection().sink().bytes(), exchange.now());
            bob.connection().sink().clear();
            bob.connection().on_writable(exchange);
        }
        allocations = scope.count();
    }
    CHECK_EQ(replay_handler.sequenced_messages, messages_stored);
    CHECK(!replay_handler.sequence_gap);
    CHECK(!bob.connection().is_replaying());
    if (testfw::allocation_counting_available()) CHECK_EQ(allocations, 0u);
}
