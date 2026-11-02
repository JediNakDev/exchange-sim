// Active maker and taker timer/event paths must not allocate on the C++ heap.
// Counting uses the shared per-thread counter in tests/framework/alloc_counter.cpp,
// which reports nothing under ASan/TSan, so the test is skipped there.
#include "fake_exchange.hpp"
#include "tests/framework/alloc_counter.hpp"
#include "tests/framework/test.hpp"

TEST_CASE(sim_bots_allocations, active_hot_paths_allocate_zero) {
    if (!testfw::allocation_counting_available()) return;
    using namespace bot_tests;
    BotSet<2> bots;
    auto config = maker_config(); config.fair_value_step_ms = 1;
    auto* maker = bots.add(config); REQUIRE(maker);
    NoiseTakerConfig noise; noise.session_id = 2; noise.instrument = 1; noise.mean_interval_ms = 1; noise.limit_ppm = 100000;
    REQUIRE(bots.add(noise));
    FakeExchange exchange; exchange.bind(bots);
    std::size_t allocations_while_trading = 0;
    {
        testfw::AllocationScope scope;
        for (Timestamp i = 0; i < 1000; ++i) tick(bots, exchange, i * 1000000);
        exchange.change_phase(Phase::Halted); tick(bots, exchange, 1000000000);
        exchange.change_phase(Phase::Continuous); tick(bots, exchange, 1001000000);
        bots.set_enabled(false); tick(bots, exchange, 1002000000);
        allocations_while_trading = scope.count();
    }
    CHECK_EQ(allocations_while_trading, 0u);
    CHECK_GT(maker->stats().fills_received, 0u); CHECK_EQ(maker->orders().size(), 0u);
}
