// Steady state allocates nothing: two FixClients trade through their
// FixConnections and a real MatchingEngine (orders, fills, replaces, cancels,
// mass cancels, heartbeats, TestRequests, gap recovery with PossDup resends),
// measured with testfw::AllocationScope.

#include "tests/exsim/gateway/fix/fix_test_support.hpp"
#include "tests/framework/alloc_counter.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

using namespace fix_test;

namespace {

constexpr exsim::Timestamp kSecond = fix::kNanosecondsPerSecond;

struct CountingHandler {
    std::uint64_t logons = 0;
    std::uint64_t execution_reports = 0;
    std::uint64_t possible_duplicates = 0;
    std::uint64_t cancel_rejects = 0;
    std::uint64_t mass_cancel_reports = 0;
    std::uint64_t session_rejects = 0;
    std::uint64_t logouts = 0;

    void on_logon(const fix::FixInboundMessageInfo&) { ++logons; }
    void on_execution_report(const fix44::ExecutionReport&, const fix::FixInboundMessageInfo& info) {
        ++execution_reports;
        possible_duplicates += info.possible_duplicate ? 1 : 0;
    }
    void on_order_cancel_reject(const fix44::OrderCancelReject&, const fix::FixInboundMessageInfo&) {
        ++cancel_rejects;
    }
    void on_order_mass_cancel_report(const fix44::OrderMassCancelReport&, const fix::FixInboundMessageInfo&) {
        ++mass_cancel_reports;
    }
    void on_session_reject(const fix44::Reject&, const fix::FixInboundMessageInfo&) { ++session_rejects; }
    void on_logout(std::string_view) { ++logouts; }
};

// "<prefix><number>" in a caller buffer, without allocating.
std::string_view token(char (&buffer)[24], char prefix, unsigned number) {
    buffer[0] = prefix;
    const auto result = std::to_chars(buffer + 1, buffer + sizeof buffer, number);
    return {buffer, static_cast<std::size_t>(result.ptr - buffer)};
}

}  // namespace

TEST_CASE(gateway_fix_allocation, steady_state_trading_and_recovery_do_not_allocate) {
    if (!testfw::allocation_counting_available()) {
        return;  // sanitizer builds keep their own allocator
    }
    FixTestExchange exchange;
    exchange.record_history = false;
    CountingHandler trader_handler;
    CountingHandler counterparty_handler;
    FixClientWire<CountingHandler> trader(exchange, trader_handler, client_settings("CLIENT1", "secret1", 5));
    FixClientWire<CountingHandler> counterparty(exchange, counterparty_handler, client_settings("CLIENT2", {}, 5));
    trader.connect(false);
    trader.pump();
    counterparty.connect(false);
    counterparty.pump();
    REQUIRE(trader.client().active());
    REQUIRE(counterparty.client().active());

    char buy_token[24];
    char sell_token[24];
    char replace_token[24];
    char cancel_token[24];
    char mass_cancel_token[24];
    const auto run_iteration = [&](unsigned iteration) {
        const fix::FixTime now = exchange.fix_time();
        const std::string_view buy = token(buy_token, 'B', iteration);
        const std::string_view replacement = token(replace_token, 'R', iteration);
        trader.client().send_new_order_single(make_new_order_single(buy, exsim::Side::BUY, 100, 150 * kDollar), now);
        trader.pump();
        counterparty.client().send_new_order_single(
            make_new_order_single(token(sell_token, 'S', iteration), exsim::Side::SELL, 60, 150 * kDollar), now);
        counterparty.pump();
        if (iteration % 7 == 0) {
            exchange.user(1).drop_out_transmissions(1);  // the trader recovers it with a ResendRequest
        }
        trader.client().send_order_cancel_replace_request(
            make_replace_request(buy, replacement, exsim::Side::BUY, 120, 150 * kDollar), now);
        trader.client().send_order_cancel_request(
            make_cancel_request(replacement, token(cancel_token, 'C', iteration), exsim::Side::BUY), now);
        if (iteration % 10 == 0) {
            trader.client().send_order_mass_cancel_request(
                make_mass_cancel_request(token(mass_cancel_token, 'M', iteration), "AAPL"), now);
        }
        trader.pump();
        if (iteration % 25 == 0) {
            // The trader is silent for 8 s: the exchange probes it with a TestRequest.
            exchange.advance(8 * kSecond);
            trader.connection().on_timer(exchange);
            counterparty.connection().on_timer(exchange);
            counterparty.client().on_timer(exchange.fix_time());
            counterparty.pump();
        }
        trader.tick(kSecond);
        counterparty.tick(0);
    };

    for (unsigned iteration = 1; iteration <= 30; ++iteration) {
        run_iteration(iteration);  // warm-up outside the measured scope
    }
    const std::uint64_t reports_before = trader_handler.execution_reports;
    const std::uint64_t test_requests_before = trader.connection().counters().test_requests_sent;
    const std::uint64_t resends_before = trader.client().counters().resend_requests_sent;
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        for (unsigned iteration = 31; iteration <= 330; ++iteration) {
            run_iteration(iteration);
        }
        allocations = scope.count();
    }
    CHECK_EQ(allocations, std::size_t{0});
    CHECK(!trader.wants_disconnect());
    CHECK(!counterparty.wants_disconnect());
    // The measured loop really did the work.
    CHECK_GE(trader_handler.execution_reports - reports_before, 300u * 4);
    CHECK_GT(trader_handler.possible_duplicates, 40u);
    CHECK_GT(trader_handler.mass_cancel_reports, 30u);
    CHECK_GT(trader.connection().counters().test_requests_sent, test_requests_before);
    CHECK_GT(trader.client().counters().resend_requests_sent, resends_before);
    CHECK_GE(trader.client().counters().heartbeats_sent, 10u);
    CHECK_EQ(trader_handler.session_rejects, 0u);
    CHECK_EQ(trader_handler.cancel_rejects, 0u);
}
