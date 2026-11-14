// Query API contracts, output preflight (InsufficientOutput leaves
// engine and output unchanged) and order slot reuse at full capacity.

#include <test/exsim/engine/engine_fixture.hpp>
#include <test/framework/alloc_counter.hpp>

#include <cstdio>
#include <limits>
#include <random>

using namespace engine_fixture;

TEST_CASE(engine_queries, depth_and_level_orders_in_priority_order_with_truncation) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 90, 5));
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 100, 6));
    fixture.apply(limit_order(kAliceSession, 3, Side::BUY, 80, 7));
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 1));
    fixture.apply(limit_order(kBobSession, 2, Side::SELL, 130, 2));
    fixture.apply(limit_order(kBobSession, 3, Side::SELL, 120, 3));

    LevelInfo levels[3];
    REQUIRE_EQ(fixture.engine.depth(1, Side::BUY, levels, 3), std::size_t{3});
    CHECK_EQ(levels[0].price, Price{100});
    CHECK_EQ(levels[0].qty, std::uint64_t{7});
    CHECK_EQ(levels[0].order_count, 2u);
    CHECK_EQ(levels[1].price, Price{90});
    CHECK_EQ(levels[2].price, Price{80});
    REQUIRE_EQ(fixture.engine.depth(1, Side::BUY, levels, 2), std::size_t{2});
    CHECK_EQ(levels[1].price, Price{90});
    CHECK_EQ(fixture.engine.depth(1, Side::BUY, levels, 0), std::size_t{0});
    REQUIRE_EQ(fixture.engine.depth(1, Side::SELL, levels, 3), std::size_t{2});
    CHECK_EQ(levels[0].price, Price{120});
    CHECK_EQ(levels[1].price, Price{130});
    CHECK_EQ(fixture.engine.depth(2, Side::BUY, levels, 3), std::size_t{0});
    CHECK_EQ(fixture.engine.depth(0, Side::BUY, levels, 3), std::size_t{0});

    OrderId ids[2] = {0, 0};
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, 100, ids, 2), std::size_t{2});
    CHECK_EQ(ids[0], OrderId{2});
    CHECK_EQ(ids[1], OrderId{4});
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, 100, ids, 1), std::size_t{1});
    CHECK_EQ(fixture.engine.level_orders(1, Side::SELL, 100, ids, 2), std::size_t{0});
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, 110, ids, 2), std::size_t{0});   // empty level
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, 95, ids, 2), std::size_t{0});    // off grid
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, 5000, ids, 2), std::size_t{0});  // out of band
    CHECK_EQ(fixture.engine.level_orders(1, Side::BUY, -10, ids, 2), std::size_t{0});
    CHECK_EQ(fixture.engine.level_orders(3, Side::BUY, 100, ids, 2), std::size_t{0});

    CHECK_EQ(*fixture.engine.best_bid(1), Price{100});
    CHECK_EQ(*fixture.engine.best_ask(1), Price{120});
    CHECK(!fixture.engine.best_bid(2).has_value());
    CHECK(!fixture.engine.best_ask(0).has_value());
    CHECK(!fixture.engine.last_trade_price(2).has_value());
}

TEST_CASE(engine_queries, find_order_and_for_each_order) {
    EngineFixture fixture(Phase::Continuous, 2);
    CHECK(all_live_orders(fixture.engine).empty());
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5, 2));   // 1
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 150, 5, 1));    // 2
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 160, 5, 2));  // 3
    fixture.apply(ReplaceOrder{4, kAliceSession, 1, 9, 90, 5, false});    // 1 becomes 4 with token 9

    std::vector<OrderId> visited;
    for (const OrderInfo& order : all_live_orders(fixture.engine)) visited.push_back(order.order_id);
    CHECK_EQ(visited, (std::vector<OrderId>{2, 3, 4}));

    const auto replaced = fixture.engine.find_order(OrderId{4});
    REQUIRE(replaced.has_value());
    CHECK_EQ(replaced->cl_ord_id, ClOrdId{9});
    CHECK_EQ(replaced->instrument, InstrumentId{2});
    CHECK_EQ(replaced->price, Price{90});
    CHECK(!fixture.engine.find_order(OrderId{1}).has_value());
    CHECK(!fixture.engine.find_order(OrderId{0}).has_value());
    CHECK(!fixture.engine.find_order(OrderId{999}).has_value());
    CHECK(!fixture.engine.find_order(kAliceSession, 1).has_value());
    CHECK(!fixture.engine.find_order(SessionId{0}, 9).has_value());
    CHECK(!fixture.engine.find_order(kAliceSession, 0).has_value());
    CHECK(!fixture.engine.find_order(kAliceSession, kMaxClOrdId + 1).has_value());
    CHECK_EQ(fixture.engine.find_order(kBobSession, 1)->order_id, OrderId{2});
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{3});
    check_book_invariants(fixture.engine);
}

namespace {

std::vector<OrderInfo> orders_at_level(const MatchingEngine& engine, InstrumentId id, Side side, Price price) {
    std::vector<OrderInfo> visited;
    engine.for_each_level_order(id, side, price, [&](const OrderInfo& order) { visited.push_back(order); });
    return visited;
}

}  // namespace

TEST_CASE(engine_queries, for_each_level_order_visits_one_level_in_fifo_order) {
    EngineFixture fixture(Phase::Continuous, 2);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5));     // 1
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 90, 4));        // 2, other level
    fixture.apply(limit_order(kBobSession, 2, Side::BUY, 100, 6));       // 3
    fixture.apply(limit_order(kAliceSession, 2, Side::BUY, 100, 7, 2));  // 4, other instrument
    fixture.apply(limit_order(kAliceSession, 3, Side::BUY, 100, 8));     // 5
    fixture.apply(limit_order(kBobSession, 3, Side::SELL, 100, 2));      // partially fills 1

    const std::vector<OrderInfo> level = orders_at_level(fixture.engine, 1, Side::BUY, 100);
    REQUIRE_EQ(level.size(), std::size_t{3});
    CHECK_EQ(level[0].order_id, OrderId{1});
    CHECK_EQ(level[0].leaves_qty, Quantity{3});
    CHECK_EQ(level[0].cum_qty, Quantity{2});
    CHECK_EQ(level[1].order_id, OrderId{3});
    CHECK_EQ(level[1].session, kBobSession);
    CHECK_EQ(level[1].leaves_qty, Quantity{6});
    CHECK_EQ(level[2].order_id, OrderId{5});
    CHECK_EQ(level[2].cl_ord_id, ClOrdId{3});
    for (const OrderInfo& order : level) {
        CHECK_EQ(order.instrument, InstrumentId{1});
        CHECK_EQ(order.price, Price{100});
        CHECK(order.side == Side::BUY);
    }

    // Same order as level_orders, and the same fields as find_order.
    OrderId ids[4] = {};
    REQUIRE_EQ(fixture.engine.level_orders(1, Side::BUY, 100, ids, 4), level.size());
    for (std::size_t i = 0; i < level.size(); ++i) {
        CHECK_EQ(level[i].order_id, ids[i]);
        CHECK_EQ(fixture.engine.find_order(ids[i])->leaves_qty, level[i].leaves_qty);
    }

    // Fully filling the head removes it; the queue behind keeps its order.
    fixture.apply(limit_order(kBobSession, 4, Side::SELL, 100, 3));
    const std::vector<OrderInfo> after_fill = orders_at_level(fixture.engine, 1, Side::BUY, 100);
    REQUIRE_EQ(after_fill.size(), std::size_t{2});
    CHECK_EQ(after_fill[0].order_id, OrderId{3});
    CHECK_EQ(after_fill[1].order_id, OrderId{5});

    const std::vector<OrderInfo> other_instrument = orders_at_level(fixture.engine, 2, Side::BUY, 100);
    REQUIRE_EQ(other_instrument.size(), std::size_t{1});
    CHECK_EQ(other_instrument[0].order_id, OrderId{4});
    check_book_invariants(fixture.engine);
}

TEST_CASE(engine_queries, for_each_level_order_visits_nothing_for_absent_or_invalid_levels) {
    EngineFixture fixture;
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5));
    fixture.apply(limit_order(kAliceSession, 2, Side::SELL, 120, 5));

    CHECK(orders_at_level(fixture.engine, 1, Side::SELL, 100).empty());   // other side
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, 110).empty());    // empty level
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, 95).empty());     // off grid
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, 5000).empty());   // above band
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, -10).empty());    // below band
    CHECK(orders_at_level(fixture.engine, 0, Side::BUY, 100).empty());    // no instrument 0
    CHECK(orders_at_level(fixture.engine, 2, Side::BUY, 100).empty());    // unknown instrument
    CHECK(orders_at_level(fixture.engine, 1, static_cast<Side>(2), 100).empty());
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, std::numeric_limits<Price>::max()).empty());
    CHECK(orders_at_level(fixture.engine, 1, Side::BUY, std::numeric_limits<Price>::min()).empty());
}

TEST_CASE(engine_queries, for_each_level_order_allocates_nothing) {
    EngineFixture fixture;
    for (ClOrdId token = 1; token <= 20; ++token) {
        fixture.apply(limit_order(kAliceSession, token, Side::SELL, 150, token));
    }
    Quantity visited_leaves = 0;
    std::size_t visited_orders = 0;
    std::size_t allocations = 0;
    {
        testfw::AllocationScope scope;
        fixture.engine.for_each_level_order(1, Side::SELL, 150, [&](const OrderInfo& order) {
            visited_leaves += order.leaves_qty;
            ++visited_orders;
        });
        fixture.engine.for_each_level_order(1, Side::SELL, 140, [&](const OrderInfo&) { ++visited_orders; });
        allocations = scope.count();
    }
    CHECK_EQ(visited_orders, std::size_t{20});
    CHECK_EQ(visited_leaves, Quantity{210});
    CHECK_EQ(allocations, std::size_t{0});
}

TEST_CASE(engine_buffer, insufficient_output_changes_neither_engine_nor_output) {
    EngineFixture fixture(Phase::Continuous, 2);
    fixture.apply(limit_order(kAliceSession, 1, Side::BUY, 100, 5));
    fixture.apply(limit_order(kBobSession, 1, Side::SELL, 120, 5));
    const Event marker{EventType::PhaseChanged, 0, 31337, {.phase_changed = {Phase::Closed, Phase::Halted}}};

    const Command commands[] = {
        Command{limit_order(kCarolSession, 1, Side::BUY, 120, 3)},
        Command{limit_order(kCarolSession, 1, Side::BUY, 7, 3)},  // would be a rejection
        Command{CancelOrder{1, kAliceSession, 1, 0, 0}},
        Command{CancelOrder{1, kAliceSession, 99, 0, 0}},         // would be a rejection
        Command{ReplaceOrder{1, kAliceSession, 1, 0, 120, 5, false}},
        Command{MassCancel{1, kBobSession, 0, 0, false}},
        Command{SetPhase{1, 0, Phase::Closed}},
        Command{SetPhase{1, 9, Phase::Closed}},                    // would be a rejection
    };
    for (const Command& command : commands) {
        const std::uint64_t state_before = book_state_digest(fixture.engine);
        EventBuffer full_output(1);
        REQUIRE(full_output.push(marker));
        CHECK_EQ(fixture.engine.handle(command, full_output), HandleResult::InsufficientOutput);
        REQUIRE_EQ(full_output.size(), std::size_t{1});
        CHECK_EQ(full_output[0].ts, Timestamp{31337});
        CHECK_EQ(full_output[0].phase_changed.new_phase, Phase::Halted);
        CHECK_EQ(book_state_digest(fixture.engine), state_before);
    }

    // After the refused attempts no id was consumed: the next order gets id 3
    // and the first fill is match id 1. Events are appended after a marker.
    EventBuffer roomy(required_event_capacity(fixture.engine_config));
    REQUIRE(roomy.push(marker));
    REQUIRE_EQ(fixture.engine.handle(commands[0], roomy), HandleResult::Applied);
    REQUIRE_EQ(roomy.size(), std::size_t{3});  // marker, Accepted, Fill (3 of the resting 5)
    CHECK_EQ(roomy[0].ts, Timestamp{31337});
    CHECK_EQ(roomy[1].accepted.order_id, OrderId{3});
    CHECK_EQ(roomy[2].fill.match_id, MatchId{1});
    CHECK_EQ(roomy[2].fill.qty, Quantity{3});
}

TEST_CASE(engine_buffer, startup_sized_buffer_covers_worst_case_commands) {
    constexpr std::size_t kMaxOrders = 16;
    const EngineConfig config{2, kMaxOrders};
    EngineFixture fixture(Phase::PreOpen, 2, config);
    // Fill the order pool with a fully crossed book on both instruments.
    ClOrdId token = 1;
    for (InstrumentId instrument = 1; instrument <= 2; ++instrument) {
        for (int i = 0; i < 4; ++i) {
            fixture.apply(limit_order(kAliceSession, token++, Side::BUY, 200 - 10 * i, 1, instrument));
            fixture.apply(limit_order(kBobSession, token++, Side::SELL, 10 + 10 * i, 1, instrument));
        }
    }
    REQUIRE_EQ(fixture.engine.live_order_count(), kMaxOrders);
    // Exactly the documented capacity: every command must apply.
    EventBuffer exact(required_event_capacity(config));
    REQUIRE_EQ(fixture.engine.handle(SetPhase{1, 0, Phase::Continuous}, exact), HandleResult::Applied);
    // Per instrument: 4 fills, AuctionSummary, PhaseChanged.
    CHECK_EQ(exact.size(), std::size_t{12});
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});

    // A sweep of a full book: Accepted + one fill per resting order + Rested.
    for (ClOrdId i = 0; i + 1 < kMaxOrders; ++i) {
        fixture.apply(limit_order(kBobSession, 100 + i, Side::SELL, static_cast<Price>(10 + 10 * (i % 19)), 1));
    }
    exact.clear();
    REQUIRE_EQ(fixture.engine.handle(limit_order(kAliceSession, 500, Side::BUY, 200, 100), exact),
               HandleResult::Applied);
    CHECK_EQ(exact.size(), std::size_t{kMaxOrders + 1});
    CHECK_EQ(exact[exact.size() - 1].rested.leaves_qty, Quantity{100 - (kMaxOrders - 1)});
    check_book_invariants(fixture.engine);
}

TEST_CASE(engine_capacity, slots_are_reused_under_churn_at_full_capacity) {
    constexpr std::size_t kMaxOrders = 4;
    EngineFixture fixture(Phase::Continuous, 1, EngineConfig{1, kMaxOrders});
    std::mt19937_64 random(20261103);
    std::vector<ClOrdId> live_tokens;
    ClOrdId next_token = 1;
    OrderId last_order_id = 0;
    for (int step = 0; step < 20000; ++step) {
        const bool add = live_tokens.empty() || random() % 3 != 0;
        if (add) {
            const Side side = random() % 2 == 0 ? Side::BUY : Side::SELL;
            // Bids 10..90, asks 110..190: never crossing, so every add rests.
            const Price price = side == Side::BUY ? static_cast<Price>(10 * (1 + random() % 9))
                                                  : static_cast<Price>(110 + 10 * (random() % 9));
            const ClOrdId token = next_token++;
            fixture.apply(limit_order(kAliceSession, token, side, price, 1 + random() % 9));
            if (live_tokens.size() == kMaxOrders) {
                REQUIRE_EQ(fixture.events[0].rejected.reason, RejectReason::CapacityExceeded);
                continue;
            }
            REQUIRE_EQ(fixture.events[1].type, EventType::Rested);
            CHECK_GT(fixture.events[0].accepted.order_id, last_order_id);
            last_order_id = fixture.events[0].accepted.order_id;
            live_tokens.push_back(token);
        } else {
            const std::size_t victim = random() % live_tokens.size();
            fixture.apply(CancelOrder{0, kAliceSession, live_tokens[victim], 0, 0});
            REQUIRE_EQ(fixture.only_event().type, EventType::Canceled);
            live_tokens.erase(live_tokens.begin() + static_cast<std::ptrdiff_t>(victim));
        }
        REQUIRE_EQ(fixture.engine.live_order_count(), live_tokens.size());
        if (step % 97 == 0) check_book_invariants(fixture.engine);
    }
    check_book_invariants(fixture.engine);
}

TEST_CASE(engine_capacity, single_slot_engine) {
    EngineFixture fixture(Phase::Continuous, 1, EngineConfig{1, 1});
    fixture.apply(limit_order(kAliceSession, 1, Side::SELL, 100, 5));
    fixture.apply(limit_order(kBobSession, 1, Side::BUY, 100, 5));
    CHECK_EQ(fixture.only_event().rejected.reason, RejectReason::CapacityExceeded);
    fixture.apply(ReplaceOrder{1, kAliceSession, 1, 2, 110, 6, false});  // priority loss reuses the slot
    REQUIRE_EQ(fixture.events.size(), std::size_t{2});
    CHECK_EQ(fixture.events[1].rested.order_id, OrderId{2});
    fixture.apply(CancelOrder{2, kAliceSession, 2, 0, 0});
    fixture.apply(with_tif(limit_order(kBobSession, 1, Side::BUY, 100, 5), TimeInForce::IOC));
    CHECK_EQ(fixture.events[1].canceled.reason, CancelReason::ImmediateOrCancel);
    CHECK_EQ(fixture.engine.live_order_count(), std::size_t{0});
}

TEST_CASE(engine_capacity, instruments_share_one_order_limit_and_id_lookup) {
    // Four instruments draw from one max_orders pool: the limit is global, a
    // slot freed on one instrument is usable on another, and lookup by
    // OrderId finds orders of every instrument.
    constexpr std::size_t kMaxOrders = 6;
    EngineFixture fixture(Phase::Continuous, 4, EngineConfig{4, kMaxOrders});
    for (ClOrdId token = 1; token <= kMaxOrders; ++token) {
        const auto instrument = static_cast<InstrumentId>(1 + (token - 1) % 4);
        fixture.apply(limit_order(kAliceSession, token, Side::BUY, 50, 1, instrument));
        REQUIRE_EQ(fixture.events[1].type, EventType::Rested);
    }
    for (InstrumentId instrument = 1; instrument <= 4; ++instrument) {
        fixture.apply(limit_order(kBobSession, instrument, Side::SELL, 150, 1, instrument));
        CHECK_EQ(fixture.only_event().rejected.reason, RejectReason::CapacityExceeded);
    }
    for (OrderId order_id = 1; order_id <= kMaxOrders; ++order_id) {
        const auto order = fixture.engine.find_order(order_id);
        REQUIRE(order.has_value());
        CHECK_EQ(order->instrument, static_cast<InstrumentId>(1 + (order_id - 1) % 4));
    }
    CHECK(!fixture.engine.find_order(OrderId{kMaxOrders + 1}).has_value());

    fixture.apply(CancelOrder{0, kAliceSession, 1, 0, 0});  // frees a slot on instrument 1
    fixture.apply(limit_order(kBobSession, 9, Side::SELL, 150, 1, 4));
    REQUIRE_EQ(fixture.events[1].type, EventType::Rested);
    CHECK_EQ(fixture.engine.find_order(fixture.events[1].rested.order_id)->instrument, InstrumentId{4});
    CHECK(!fixture.engine.find_order(OrderId{1}).has_value());
    check_book_invariants(fixture.engine);
}

TEST_CASE(engine_instruments, find_instrument_by_symbol_after_many_instruments) {
    MatchingEngine engine(EngineConfig{41, 8});  // room for one more, so only the symbol can reject
    char symbol[16] = {};
    for (int i = 0; i < 40; ++i) {
        std::snprintf(symbol, sizeof(symbol), "S%02d", i);
        REQUIRE_EQ(engine.add_instrument(small_instrument_config(symbol)), static_cast<InstrumentId>(i + 1));
    }
    CHECK_EQ(engine.add_instrument(small_instrument_config("S07")), InstrumentId{0});  // symbol taken
    for (int i = 0; i < 40; ++i) {
        std::snprintf(symbol, sizeof(symbol), "S%02d", i);
        CHECK_EQ(engine.find_instrument(Symbol(symbol)), static_cast<InstrumentId>(i + 1));
    }
    CHECK_EQ(engine.find_instrument(Symbol("S40")), InstrumentId{0});
    CHECK_EQ(engine.find_instrument(Symbol("")), InstrumentId{0});
}
