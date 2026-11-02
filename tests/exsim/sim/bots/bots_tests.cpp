#include "fake_exchange.hpp"
#include "tests/framework/test.hpp"
using namespace bot_tests;

TEST_CASE(sim_bots_maker, ladder_and_idle) {
    auto config = maker_config(); config.size = 107;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot);
    tick(bot, exchange, 0);
    CHECK_EQ(exchange.commands.size(), 6u); CHECK_EQ(bot.orders().size(), exchange.count(1));
    for (std::size_t i = 0; i < 6; ++i) {
        const auto& order = exchange.commands[i].new_order;
        CHECK_EQ(order.qty, 105u); CHECK_EQ(order.stp, config.stp);
        CHECK_EQ(order.price, i < 3 ? 990 - static_cast<Price>(i) * 10 : 1010 + static_cast<Price>(i - 3) * 10);
    }
    tick(bot, exchange, 1); CHECK_EQ(exchange.commands.size(), 6u);
    tick(bot, exchange, 100000000); CHECK_EQ(exchange.commands.size(), 6u);
}
TEST_CASE(sim_bots_maker, minimal_move) {
    auto config = maker_config(); config.fair_value_step_ms = 1; config.seed = 17;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot);
    tick(bot, exchange, 0); tick(bot, exchange, 1000000);
    CHECK_EQ(exchange.commands.size(), 8u); CHECK_EQ(bot.stats().replaces_submitted, 2u);
    CHECK_EQ(bot.stats().cancels_submitted, 0u); CHECK_EQ(bot.orders().size(), 6u);
}
TEST_CASE(sim_bots_maker, partial_fill_and_replenish_total) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    NewOrder take{}; take.ts = 1; take.instrument = 1; take.session = 2; take.cl_ord_id = 1;
    take.side = Side::SELL; take.qty = 40; take.price = 990; take.tif = TimeInForce::IOC;
    exchange.submit(Command{take}); CHECK_EQ(bot.inventory(), 40);
    tick(bot, exchange, 2);
    CHECK_EQ(exchange.commands.back().type, CommandType::Replace);
    CHECK_EQ(exchange.commands.back().replace.qty, 140u);
    CHECK_EQ(bot.orders().size(), exchange.count(1));
    take.cl_ord_id = 2; take.qty = 100; exchange.submit(Command{take}); tick(bot, exchange, 3);
    CHECK_EQ(bot.orders().size(), 6u); CHECK_EQ(bot.orders().size(), exchange.count(1));
}
TEST_CASE(sim_bots_maker, phases_and_stop_start) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot);
    exchange.config.initial_phase = Phase::PreOpen; exchange.phase = Phase::PreOpen;
    tick(bot, exchange, 0); CHECK_EQ(exchange.count(1), 6u);
    exchange.change_phase(Phase::Halted); tick(bot, exchange, 1); CHECK_EQ(exchange.count(1), 0u);
    exchange.change_phase(Phase::Continuous); tick(bot, exchange, 2); CHECK_EQ(exchange.count(1), 6u);
    bot.set_enabled(false); tick(bot, exchange, 3); CHECK_EQ(exchange.count(1), 0u);
    bot.set_enabled(true); tick(bot, exchange, 4); CHECK_EQ(exchange.count(1), 6u);
    exchange.change_phase(Phase::Closed); tick(bot, exchange, 5); CHECK_EQ(bot.orders().size(), 0u);
    exchange.change_phase(Phase::PreClose); tick(bot, exchange, 6); CHECK_EQ(exchange.count(1), 6u);
}
TEST_CASE(sim_bots_maker, stp_expiry_and_rejection) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    NewOrder take{}; take.instrument = 1; take.session = 2; take.owner = 1; take.cl_ord_id = 1;
    take.side = Side::BUY; take.qty = 5; take.price = 2000; take.tif = TimeInForce::IOC; take.stp = StpMode::CancelResting;
    exchange.submit(Command{take}); CHECK_EQ(bot.orders().size(), 3u); CHECK_EQ(bot.orders().size(), exchange.count(1));
    exchange.reject_next = true; tick(bot, exchange, 1); CHECK_EQ(bot.stats().rejects_received, 1u);
    CHECK_EQ(bot.orders().size(), exchange.count(1)); tick(bot, exchange, 2); CHECK_EQ(bot.orders().size(), 6u);
    exchange.change_phase(Phase::Closed); CHECK_EQ(bot.orders().size(), exchange.count(1));
}
TEST_CASE(sim_bots_maker, delayed_callbacks_and_large_move) {
    auto config = maker_config(); config.fair_value_step_ms = 1; config.fair_value_step_ticks = 10;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot); exchange.deferred = true;
    tick(bot, exchange, 0); tick(bot, exchange, 1); CHECK_EQ(exchange.commands.size(), 6u); exchange.flush();
    for (Timestamp i = 1; i <= 100; ++i) {
        tick(bot, exchange, i * 1000000); exchange.flush();
        CHECK_EQ(bot.orders().size(), exchange.count(1));
        Price bid = 0, ask = 3000;
        for (const auto& order : exchange.book) if (order.id && order.command.session == 1) {
            if (order.command.side == Side::BUY) bid = std::max(bid, order.command.price);
            else ask = std::min(ask, order.command.price);
        }
        CHECK_LT(bid, ask);
    }
}
TEST_CASE(sim_bots_maker, band_capacity_and_halt_retention) {
    auto config = maker_config(); config.levels = 64; config.order_capacity = 8; config.fair_value_ticks = 1; config.cancel_on_halt = false;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    CHECK_EQ(bot.orders().size(), 8u); CHECK_GT(bot.stats().capacity_skips, 0u);
    for (const auto& order : exchange.book) if (order.id) { CHECK_GE(order.command.price, 10); CHECK_LE(order.command.price, 2000); }
    exchange.change_phase(Phase::Halted); const auto count = exchange.commands.size(); tick(bot, exchange, 100000000);
    CHECK_EQ(exchange.commands.size(), count);
}
TEST_CASE(sim_bots_maker, replace_rejection_and_budget_rollover) {
    auto config = maker_config(); config.levels = 1; config.size = 1000;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    NewOrder take{}; take.instrument = 1; take.session = 2; take.cl_ord_id = 1;
    take.qty = 5; take.price = 990; take.side = Side::SELL; take.tif = TimeInForce::IOC;
    exchange.submit(Command{take}); tick(bot, exchange, 1); tick(bot, exchange, 2);
    CHECK_EQ(bot.orders().size(), 2u); CHECK_EQ(bot.orders().size(), exchange.count(1));
    CHECK_EQ(bot.stats().cancels_submitted, 1u);
}
TEST_CASE(sim_bots_noise, poisson_rate_and_determinism) {
    NoiseTakerConfig config; config.session_id = 1; config.instrument = 1; config.limit_ppm = 0; config.mean_interval_ms = 10;
    NoiseTaker first(config), second(config); config.seed = 2; NoiseTaker different(config);
    FakeExchange a, b, c; a.bind(first); b.bind(second); c.bind(different);
    for (Timestamp i = 0; i <= 100000; ++i) {
        tick(first, a, i * 1000000); tick(second, b, i * 1000000); tick(different, c, i * 1000000);
    }
    CHECK_GT(a.commands.size(), 9500u); CHECK_LT(a.commands.size(), 10500u); CHECK_EQ(a.commands.size(), b.commands.size());
    bool differs = a.commands.size() != c.commands.size();
    for (std::size_t i = 0; i < a.commands.size(); ++i) {
        const auto& x = a.commands[i].new_order; const auto& y = b.commands[i].new_order;
        CHECK_EQ(x.ts, y.ts); CHECK_EQ(x.side, y.side); CHECK_EQ(x.qty, y.qty); CHECK_EQ(x.price, y.price);
        if (i < c.commands.size()) differs |= x.ts != c.commands[i].new_order.ts || x.qty != c.commands[i].new_order.qty;
    }
    CHECK(differs); CHECK_EQ(first.orders().size(), 0u);
}
TEST_CASE(sim_bots_noise, passive_expiry_phases_and_stop) {
    NoiseTakerConfig config; config.session_id = 1; config.instrument = 1;
    config.mean_interval_ms = 1; config.limit_ppm = 1000000; config.limit_lifetime_ms = 2;
    NoiseTaker bot(config); FakeExchange exchange; exchange.bind(bot);
    tick(bot, exchange, 0);
    for (Timestamp i = 1; i < 30; ++i) { tick(bot, exchange, i * 1000000); CHECK_EQ(bot.orders().size(), exchange.count(1)); }
    CHECK_GT(bot.stats().cancels_submitted, 0u);
    exchange.change_phase(Phase::PreOpen); const auto count = bot.stats().new_orders_submitted;
    tick(bot, exchange, 100000000); CHECK_EQ(bot.stats().new_orders_submitted, count);
    bot.set_enabled(false); tick(bot, exchange, 101000000); CHECK_EQ(bot.orders().size(), 0u);
    bot.set_enabled(true); exchange.change_phase(Phase::Continuous); tick(bot, exchange, 102000000);
    CHECK_GT(bot.next_arrival(), 102000000u);
}
TEST_CASE(sim_bots_config, options_are_transactional) {
    auto config = maker_config(); CHECK(apply_bot_option(config, "levels", "5")); CHECK_EQ(config.levels, 5u);
    CHECK(!apply_bot_option(config, "levels", "65")); CHECK_EQ(config.levels, 5u);
    CHECK(!apply_bot_option(config, "seed", "-1")); CHECK(!apply_bot_option(config, "skew_ticks_per_unit", "nan"));
    CHECK(!apply_bot_option(config, "unknown", "1")); CHECK(apply_bot_option(config, "cancel_on_halt", "false"));
    NoiseTakerConfig noise; CHECK(apply_bot_option(noise, "limit_ppm", "1000000"));
    CHECK(!apply_bot_option(noise, "limit_ppm", "1000001")); CHECK_EQ(noise.limit_ppm, 1000000u);
}
TEST_CASE(sim_bots_registry, bounded_storage_and_fanout) {
    BotSet<2> bots; auto config = maker_config(); auto* maker = bots.add(config); REQUIRE(maker);
    CHECK(!bots.add(config)); NoiseTakerConfig noise; noise.session_id = 2; noise.instrument = 1;
    REQUIRE(bots.add(noise)); CHECK(!bots.add(noise)); CHECK_EQ(bots.size(), 2u);
    FakeExchange exchange; exchange.bind(bots); tick(bots, exchange, 0); CHECK_EQ(exchange.count(1), 6u);
    bots.set_enabled(false); tick(bots, exchange, 1); CHECK_EQ(exchange.count(1), 0u);
}
TEST_CASE(sim_bots_table, allocation_and_lookup) {
    OwnOrderTable orders(8);
    std::array<ClOrdId, 8> ids{};
    for (auto& id : ids) { auto* order = orders.allocate(); REQUIRE(order); id = order->cl_ord_id; }
    CHECK(!orders.allocate()); orders.erase(*orders.find(ids[2]));
    auto* next = orders.allocate(); REQUIRE(next); CHECK_GT(next->cl_ord_id, ids.back()); CHECK(!orders.find(ids[2]));
}
TEST_CASE(sim_bots_maker, rejected_replace_recovers) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    NewOrder take{}; take.instrument = 1; take.session = 2; take.cl_ord_id = 1;
    take.qty = 5; take.price = 990; take.side = Side::SELL; take.tif = TimeInForce::IOC;
    exchange.submit(Command{take}); exchange.reject_next = true; tick(bot, exchange, 1);
    CHECK_EQ(bot.stats().rejects_received, 1u);
    tick(bot, exchange, 2); CHECK_EQ(bot.stats().replaces_submitted, 2u);
    CHECK_EQ(bot.orders().size(), exchange.count(1));
    const auto* order = exchange.find(1, 1); REQUIRE(order); CHECK_EQ(order->leaves, 100u);
}
TEST_CASE(sim_bots_maker, same_price_decrease_keeps_priority) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    exchange.submit(Command{ReplaceOrder{0, 1, 1, 0, 990, 200}});
    const auto* before = exchange.find(1, 1); REQUIRE(before); const auto priority = before->id;
    tick(bot, exchange, 100000000);
    const auto* after = exchange.find(1, 1); REQUIRE(after);
    CHECK_EQ(after->id, priority); CHECK_EQ(after->leaves, 100u);
}
TEST_CASE(sim_bots_maker, large_jump_reuses_all_orders) {
    auto config = maker_config(); config.fair_value_step_ms = 1; config.fair_value_step_ticks = 10;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot);
    tick(bot, exchange, 0); tick(bot, exchange, 1000000);
    CHECK_EQ(bot.stats().replaces_submitted, 6u); CHECK_EQ(bot.stats().new_orders_submitted, 6u);
    CHECK_EQ(bot.stats().cancels_submitted, 0u); CHECK_EQ(bot.orders().size(), exchange.count(1));
}
TEST_CASE(sim_bots_maker, unknown_cancel_and_foreign_events) {
    MarketMaker bot(maker_config()); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    auto foreign = exchange.event(EventType::PhaseChanged); foreign.instrument = 2;
    foreign.phase_changed = {Phase::Continuous, Phase::Halted}; bot.on_events({&foreign, 1});
    CHECK_EQ(bot.phase(), Phase::Continuous);
    auto* missing = exchange.find(1, 1); REQUIRE(missing); *missing = {};
    bot.set_enabled(false); tick(bot, exchange, 1);
    CHECK_EQ(bot.orders().size(), 0u); CHECK_EQ(exchange.count(1), 0u);
}
TEST_CASE(sim_bots_maker, inventory_skews_quotes_down) {
    auto config = maker_config(); config.skew_ticks_per_unit = 2;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    NewOrder take{}; take.instrument = 1; take.session = 2; take.cl_ord_id = 1;
    take.qty = 100; take.price = 990; take.side = Side::SELL; take.tif = TimeInForce::IOC;
    exchange.submit(Command{take}); tick(bot, exchange, 1);
    Price bid = 0;
    for (const auto& order : exchange.book) if (order.id && order.command.session == 1 && order.command.side == Side::BUY)
        bid = std::max(bid, order.command.price);
    CHECK_EQ(bid, 970); CHECK_EQ(bot.inventory(), 100);
}
TEST_CASE(sim_bots_maker, delayed_rejected_move_never_self_trades) {
    auto config = maker_config(); config.fair_value_step_ms = 1; config.fair_value_step_ticks = 10; config.stp = StpMode::None;
    MarketMaker bot(config); FakeExchange exchange; exchange.bind(bot); tick(bot, exchange, 0);
    exchange.deferred = true;
    for (Timestamp i = 1; i < 50; ++i) {
        exchange.reject_next = true; tick(bot, exchange, i * 1000000); exchange.flush();
        tick(bot, exchange, i * 1000000 + 1); exchange.flush();
        CHECK_EQ(bot.orders().size(), exchange.count(1));
    }
    CHECK_EQ(bot.stats().filled_qty, 0u);
}
TEST_CASE(sim_bots_maker, seeded_command_stream) {
    auto config = maker_config(); config.fair_value_step_ms = 1;
    MarketMaker first(config), second(config); config.seed = 99; MarketMaker different(config);
    FakeExchange a, b, c; a.bind(first); b.bind(second); c.bind(different);
    for (Timestamp i = 0; i < 100; ++i) {
        tick(first, a, i * 1000000); tick(second, b, i * 1000000); tick(different, c, i * 1000000);
    }
    CHECK_EQ(a.commands.size(), b.commands.size()); bool differs = a.commands.size() != c.commands.size();
    for (std::size_t i = 0; i < a.commands.size(); ++i) {
        const auto& x = a.commands[i]; const auto& y = b.commands[i]; CHECK_EQ(x.type, y.type); CHECK_EQ(x.ts(), y.ts());
        if (x.type == CommandType::Replace) {
            CHECK_EQ(x.replace.price, y.replace.price); CHECK_EQ(x.replace.orig_cl_ord_id, y.replace.orig_cl_ord_id);
            if (i < c.commands.size() && c.commands[i].type == CommandType::Replace) differs |= x.replace.price != c.commands[i].replace.price;
        }
    }
    CHECK(differs);
}
