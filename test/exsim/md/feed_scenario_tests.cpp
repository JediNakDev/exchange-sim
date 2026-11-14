// Targeted end-to-end scenarios through publisher -> receiver -> builder:
// the rebuilt book must equal the engine (levels and FIFO) after every
// command, plus scenario-specific feed state (trading state, crosses,
// imbalances, FIFO after replaces, recovery of a mass cancel split across
// packets).

#include <test/exsim/md/feed_harness.hpp>
#include <test/framework/test.hpp>

#include <string>
#include <vector>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

struct Scenario {
    FeedHarness feed;
    Timestamp now = 1'000'000;
    std::size_t command_index = 0;

    explicit Scenario(std::initializer_list<InstrumentConfig> instruments, FeedHarnessOptions options = {})
        : feed(setup_for(instruments), options) {
        feed.start(now);
    }

    static server::journal::JournalSetup setup_for(std::initializer_list<InstrumentConfig> instruments) {
        server::journal::JournalSetup setup{EngineConfig{4, 256}, {}};
        for (const auto& instrument : instruments) setup.instruments.push_back(instrument);
        return setup;
    }

    // Applies the command and requires builder == engine afterwards.
    void run(Command command) {
        now += 1'000'000;
        set_command_time(command, now);
        feed.step(command, command_index++, now);
        const std::string mismatch = feed.clean_mismatch();
        if (!mismatch.empty()) FAIL("after command " + std::to_string(command_index - 1) + ": " + mismatch);
    }

    void order(InstrumentId instrument, ClOrdId token, Side side, Price price, Quantity qty) {
        run(limit_order(0, instrument, 1, token, side, price, qty));
    }

    std::vector<OrderId> builder_fifo(InstrumentId instrument, Side side, Price price) const {
        std::vector<OrderId> ids(64);
        ids.resize(feed.builder.level_orders(instrument, side, price, ids.data(), ids.size()));
        return ids;
    }

    void finish() {
        feed.finish(now + 1'000'000);
        feed.require_consistent_end();
    }
};

}  // namespace

TEST_CASE(md_feed_scenario, opening_auction_uncrosses_the_rebuilt_book) {
    Scenario scenario({test_instrument("AAA", Phase::PreOpen)});
    MatchingEngine& engine = scenario.feed.engine;
    md::ItchBookBuilder& builder = scenario.feed.builder;
    CHECK_EQ(builder.trading_state(1), 'Q');
    scenario.order(1, 1, Side::BUY, 1'000'100, 100);
    scenario.order(1, 2, Side::BUY, 1'000'000, 50);
    scenario.order(1, 3, Side::SELL, 999'900, 80);
    scenario.order(1, 4, Side::SELL, 1'000'100, 40);
    // The call-phase book is crossed on the feed as in the engine.
    REQUIRE(builder.best_bid(1).has_value());
    REQUIRE(builder.best_ask(1).has_value());
    CHECK_GT(*builder.best_bid(1), *builder.best_ask(1));

    // NOII after 20 ms of command time: the engine's indicative uncross.
    scenario.now += 20'000'000;
    REQUIRE(scenario.feed.publisher.poll(scenario.now, engine) == md::PublishStatus::Ok);
    scenario.feed.pump(scenario.now);
    const auto imbalance = builder.last_imbalance(1);
    REQUIRE(imbalance.has_value());
    const AuctionIndicative indicative = engine.auction_indicative(1);
    CHECK(indicative.crossed);
    CHECK_EQ(imbalance->paired_shares, indicative.paired_qty);
    CHECK_EQ(imbalance->near_price, indicative.price);
    CHECK_EQ(imbalance->cross_type, 'O');

    scenario.run(SetPhase{0, 1, Phase::Continuous});
    CHECK_EQ(builder.trading_state(1), 'T');
    const auto cross = builder.last_cross(1);
    REQUIRE(cross.has_value());
    CHECK_EQ(cross->cross_type, 'O');
    CHECK_EQ(cross->shares, 100u);  // maximum volume at 1,000,100
    CHECK_EQ(cross->shares, indicative.paired_qty);
    CHECK_EQ(cross->price, indicative.price);
    CHECK(!(builder.best_bid(1) && builder.best_ask(1) && *builder.best_bid(1) >= *builder.best_ask(1)));
    // Q carries a fresh match number after the constituent C executions.
    CHECK_GT(scenario.feed.handler.message_type_counts[static_cast<unsigned char>('C')], 0u);
    scenario.finish();
}

TEST_CASE(md_feed_scenario, halt_keeps_crossed_book_then_uncrosses_with_cross_type_h) {
    Scenario scenario({test_instrument("AAA", Phase::PreOpen)});
    md::ItchBookBuilder& builder = scenario.feed.builder;
    scenario.order(1, 1, Side::BUY, 1'000'200, 30);
    scenario.order(1, 2, Side::SELL, 1'000'000, 20);
    scenario.run(SetPhase{0, 1, Phase::Halted});
    CHECK_EQ(builder.trading_state(1), 'H');
    CHECK_GT(*builder.best_bid(1), *builder.best_ask(1));  // halted, still crossed
    scenario.run(limit_order(0, 1, 1, 3, Side::BUY, 1'000'000, 5));  // rejected while halted: no output
    scenario.run(CancelOrder{0, 1, 1, 9, 25});                     // cancels are allowed: X 5
    scenario.run(SetPhase{0, 1, Phase::Continuous});
    CHECK_EQ(builder.trading_state(1), 'T');
    REQUIRE(builder.last_cross(1).has_value());
    CHECK_EQ(builder.last_cross(1)->cross_type, 'H');
    CHECK_EQ(builder.last_cross(1)->shares, 20u);
    CHECK_EQ(builder.order_count(), 1u);  // 5 left of order 1
    scenario.finish();
}

TEST_CASE(md_feed_scenario, replaces_keep_or_lose_priority_on_the_feed) {
    Scenario scenario({test_instrument("AAA")});
    md::ItchBookBuilder& builder = scenario.feed.builder;
    scenario.order(1, 1, Side::BUY, 1'000'000, 100);  // order 1
    scenario.order(1, 2, Side::BUY, 1'000'000, 100);  // order 2
    scenario.order(1, 3, Side::BUY, 1'000'000, 100);  // order 3
    // Reduction keeps priority: X, position unchanged.
    scenario.run(ReplaceOrder{0, 1, 1, 11, 1'000'000, 60, false});
    CHECK(scenario.builder_fifo(1, Side::BUY, 1'000'000) == std::vector<OrderId>({1, 2, 3}));
    // Increase loses priority: U to a new reference at the tail.
    scenario.run(ReplaceOrder{0, 1, 11, 12, 1'000'000, 150, false});
    const std::vector<OrderId> after_increase = scenario.builder_fifo(1, Side::BUY, 1'000'000);
    REQUIRE_EQ(after_increase.size(), 3u);
    CHECK_EQ(after_increase[0], 2u);
    CHECK_EQ(after_increase[1], 3u);
    CHECK(!builder.find_order(1).has_value());
    // Partial execution, then a replace to a quantity at or below the
    // executed amount removes the order (D).
    scenario.order(1, 4, Side::SELL, 1'000'000, 40);  // executes 40 of order 2
    scenario.run(ReplaceOrder{0, 1, 2, 13, 1'000'000, 30, false});
    CHECK(!builder.find_order(2).has_value());
    // A price change that crosses executes the replacement immediately (D, E, A).
    scenario.order(1, 5, Side::SELL, 1'000'200, 20);
    scenario.run(ReplaceOrder{0, 1, 3, 14, 1'000'200, 100, false});
    CHECK(!builder.best_ask(1).has_value());
    CHECK_EQ(*builder.best_bid(1), 1'000'200);
    scenario.finish();
}

TEST_CASE(md_feed_scenario, mass_cancel_split_across_packets_survives_drops) {
    FeedHarnessOptions options;
    options.max_payload = md::kMinimumMaxPayload;  // two 19-byte D messages per packet
    Scenario scenario({test_instrument("AAA"), test_instrument("BBB")}, options);
    for (ClOrdId token = 1; token <= 6; ++token) {
        scenario.order(1, token, token % 2 == 0 ? Side::BUY : Side::SELL, token % 2 == 0 ? 999'000 : 1'001'000,
                       10 * token);
        scenario.order(2, 100 + token, Side::BUY, 999'000 + 100 * static_cast<Price>(token), 5);
    }
    CHECK_EQ(scenario.feed.builder.order_count(), 12u);
    const std::uint64_t packets_before = scenario.feed.publisher.stats().packets_sent;
    scenario.feed.feed_injector.drop_next(3);
    scenario.run(MassCancel{0, 1, 0, 500, false});
    CHECK_EQ(scenario.feed.publisher.stats().packets_sent - packets_before, 6u);
    CHECK_EQ(scenario.feed.feed_injector.stats().dropped, 3u);
    CHECK_GE(scenario.feed.receiver.stats().requests_sent, 1u);
    CHECK_EQ(scenario.feed.builder.order_count(), 0u);
    scenario.finish();
}

TEST_CASE(md_feed_scenario, closing_auction_and_close_expire_day_orders) {
    Scenario scenario({test_instrument("AAA")});
    md::ItchBookBuilder& builder = scenario.feed.builder;
    scenario.order(1, 1, Side::BUY, 1'000'000, 10);
    scenario.run(SetPhase{0, 1, Phase::PreClose});
    CHECK_EQ(builder.trading_state(1), 'Q');
    scenario.order(1, 2, Side::SELL, 999'000, 4);
    scenario.run(limit_order(0, 1, 1, 3, Side::SELL, 1'002'000, 7, TimeInForce::GTC));
    scenario.run(SetPhase{0, 1, Phase::Closed});
    CHECK_EQ(builder.trading_state(1), 'H');
    REQUIRE(builder.last_cross(1).has_value());
    CHECK_EQ(builder.last_cross(1)->cross_type, 'C');
    CHECK_EQ(builder.last_cross(1)->shares, 4u);
    scenario.finish();
}

TEST_CASE(md_feed_scenario, late_dropped_tail_is_recovered_by_heartbeat) {
    Scenario scenario({test_instrument("AAA")});
    scenario.order(1, 1, Side::BUY, 1'000'000, 10);
    // Drop the only packet of the next batch: no later data reveals the gap.
    scenario.feed.feed_injector.drop_next(1);
    scenario.now += 1'000'000;
    Command command = limit_order(scenario.now, 1, 1, 2, Side::BUY, 1'000'000, 10);
    scenario.feed.step(command, 99, scenario.now);
    CHECK_EQ(scenario.feed.receiver.next_sequence() + 1, scenario.feed.publisher.next_sequence());
    CHECK(!scenario.feed.receiver.gap_open());
    // One second of silence: the heartbeat announces the sequence and the
    // receiver recovers the add through retransmission.
    scenario.now += 1'000'000'000;
    REQUIRE(scenario.feed.publisher.poll(scenario.now, scenario.feed.engine) == md::PublishStatus::Ok);
    scenario.feed.pump(scenario.now);
    CHECK_EQ(scenario.feed.publisher.stats().heartbeats_sent, 1u);
    CHECK_EQ(scenario.feed.clean_mismatch(), std::string());
    scenario.finish();
}
