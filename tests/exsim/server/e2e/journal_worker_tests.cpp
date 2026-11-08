#include "exsim/server/runtime/journal_worker.hpp"
#include "tests/exsim/server/e2e/e2e_support.hpp"
#include "tests/exsim/server/core/core_test_support.hpp"

#include <chrono>
#include <memory>
#include <thread>

using namespace exsim;
using namespace exsim::e2e;

namespace {

struct PersistenceControl {
    std::atomic<bool> block{true};
    std::atomic<bool> entered{false};
    std::atomic<bool> fail{false};
    std::atomic<bool> destroyed{false};
};

// A controllable I/O boundary: the same worker code used in production
// must keep its producer responsive while a write cannot make progress.
struct StalledWriter {
    static inline std::shared_ptr<PersistenceControl> next_control;
    std::shared_ptr<PersistenceControl> control = next_control;
    StalledWriter(const std::string&, const sim::JournalSetup&) {}
    ~StalledWriter() { control->destroyed.store(true, std::memory_order_release); }
    bool append(const Command&) noexcept { return true; }
    bool flush() noexcept {
        control->entered.store(true, std::memory_order_release);
        while (control->block.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return !control->fail.load(std::memory_order_acquire);
    }
};

// Always release a stalled worker even if a REQUIRE aborts a test body.
struct ReleaseWriter {
    std::shared_ptr<PersistenceControl> control;
    ~ReleaseWriter() { control->block.store(false, std::memory_order_release); }
};

}  // namespace

TEST_CASE(server_journal_worker, ordered_commands_and_footer_are_readable_after_drain) {
    const auto config = make_e2e_config(false);
    TemporaryJournalPath journal;
    server::runtime::JournalWorker<> writer(journal.path, {config.engine.engine, config.instruments});
    Command command{MassCancel{}};
    for (std::uint64_t index = 0; index < 300; ++index) {
        command.mass_cancel.ts = index;
        REQUIRE(writer.append(command));
    }
    REQUIRE(writer.flush());
    REQUIRE(wait_until([&] { return writer.drained(); }, [] {}));
    REQUIRE(writer.healthy());
    sim::JournalReader reader(journal.path);
    for (std::uint64_t index = 0; index < 300; ++index) {
        REQUIRE(reader.next(command));
        CHECK_EQ(command.mass_cancel.ts, index);
        CHECK(command.type == CommandType::MassCancel);
    }
    CHECK(!reader.next(command));
}

TEST_CASE(server_journal_worker, blocked_io_has_bounded_handoff_and_latches_overflow) {
    const auto control = std::make_shared<PersistenceControl>();
    StalledWriter::next_control = control;
    ReleaseWriter release{control};
    server::runtime::JournalWorker<StalledWriter, 4> writer("unused", {});
    REQUIRE(writer.flush());
    REQUIRE(wait_until([&] { return control->entered.load(std::memory_order_acquire); }, [] {}));
    // The consumer is stuck in flush, leaving exactly three usable slots.
    for (int index = 0; index < 3; ++index) REQUIRE(writer.append({}));
    CHECK(!writer.append({}));
    CHECK(!writer.healthy());
    CHECK(!writer.drained());
    CHECK(!writer.flush());
    control->block.store(false, std::memory_order_release);
}

TEST_CASE(server_journal_worker, io_failure_is_visible_without_another_append) {
    const auto control = std::make_shared<PersistenceControl>();
    StalledWriter::next_control = control;
    control->block.store(false, std::memory_order_release);
    control->fail.store(true, std::memory_order_release);
    server::runtime::JournalWorker<StalledWriter, 4> writer("unused", {});
    REQUIRE(writer.flush());
    REQUIRE(wait_until([&] { return !writer.healthy(); }, [] {}));
    CHECK(!writer.drained());
    CHECK(!writer.append({}));
}

TEST_CASE(server_journal_worker, stalled_writer_does_not_make_destruction_wait_forever) {
    const auto control = std::make_shared<PersistenceControl>();
    StalledWriter::next_control = control;
    ReleaseWriter release{control};
    auto writer = std::make_unique<server::runtime::JournalWorker<StalledWriter, 4>>("unused", sim::JournalSetup{});
    REQUIRE(writer->flush());
    REQUIRE(wait_until([&] { return control->entered.load(std::memory_order_acquire); }, [] {}));
    const auto started = std::chrono::steady_clock::now();
    writer.reset();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(!control->destroyed.load(std::memory_order_acquire));
    control->block.store(false, std::memory_order_release);
    REQUIRE(wait_until([&] { return control->destroyed.load(std::memory_order_acquire); }, [] {}));
}

TEST_CASE(server_journal_worker, core_applies_failure_policy_when_a_worker_fails_without_more_commands) {
    using namespace server::core_test;
    for (const auto policy : {server::JournalFailurePolicy::Shutdown, server::JournalFailurePolicy::Continue}) {
        const auto control = std::make_shared<PersistenceControl>();
        StalledWriter::next_control = control;
        control->block.store(false, std::memory_order_release);
        control->fail.store(true, std::memory_order_release);
        auto config = make_e2e_config(false, "unused");
        config.journal_failure_policy = policy;
        config.journal_flush_interval_ms = 0;  // error detection must not rely on a periodic flush
        FakeClock clock;
        clock.day = kTenAm;
        CountingSessions sessions;
        CountingMarketData market_data;
        server::ExchangeCore<CountingSessions, CountingMarketData, FakeClock, server::runtime::JournalWorker<StalledWriter, 4>>
            core(config, sessions, market_data, clock);
        REQUIRE(core.flush_journal());
        REQUIRE(wait_until([&] { return control->destroyed.load(std::memory_order_acquire); }, [] {}));
        core.timer(clock.day);
        CHECK(core.journal_state() == server::JournalState::Failed);
        CHECK_EQ(core.counters().journal_failures, 1u);
        CHECK_EQ(core.shutdown_requested(), policy == server::JournalFailurePolicy::Shutdown);
        core.timer(clock.day);
        CHECK_EQ(core.counters().journal_failures, 1u);
    }
}

TEST_CASE(server_journal_worker, core_reports_overflow_after_applying_the_current_command) {
    using namespace server::core_test;
    for (const auto policy : {server::JournalFailurePolicy::Shutdown, server::JournalFailurePolicy::Continue}) {
        const auto control = std::make_shared<PersistenceControl>();
        StalledWriter::next_control = control;
        ReleaseWriter release{control};
        auto config = make_e2e_config(false, "unused");
        config.journal_failure_policy = policy;
        FakeClock clock;
        clock.day = kTenAm;
        CountingSessions sessions;
        CountingMarketData market_data;
        server::ExchangeCore<CountingSessions, CountingMarketData, FakeClock, server::runtime::JournalWorker<StalledWriter, 4>>
            core(config, sessions, market_data, clock);
        REQUIRE(core.flush_journal());
        REQUIRE(wait_until([&] { return control->entered.load(std::memory_order_acquire); }, [] {}));
        for (ClOrdId token = 1; token <= 4; ++token)
            REQUIRE(core.submit(limit_order(1, token, 1, Side::BUY, 140 * kDollar, 10)) == server::SubmitResult::Applied);
        CHECK(core.journal_state() == server::JournalState::Failed);
        CHECK_EQ(core.counters().journal_failures, 1u);
        CHECK_EQ(core.counters().journal_records, 3u);
        CHECK_EQ(core.shutdown_requested(), policy == server::JournalFailurePolicy::Shutdown);
        if (policy == server::JournalFailurePolicy::Continue) CHECK_EQ(core.engine().live_order_count(), 4u);
        control->block.store(false, std::memory_order_release);
    }
}
