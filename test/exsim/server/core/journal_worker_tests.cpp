// runtime::JournalWorker: shutdown drains every accepted entry and waits for
// the writer thread without polling; a stalled writer is abandoned.

#include "server/runtime/journal_worker.hpp"
#include "test/framework/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace exsim;

namespace {

std::atomic<std::uint64_t> appended_commands{0};
std::atomic<std::uint64_t> flush_requests{0};
std::atomic<bool> stall_flush{false};
std::atomic<bool> writer_inside_flush{false};

struct CountingJournalWriter {
    CountingJournalWriter(const std::string&, const sim::JournalSetup&) {}
    bool append(const Command&) noexcept {
        appended_commands.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool flush() noexcept {
        writer_inside_flush.store(true, std::memory_order_release);
        while (stall_flush.load(std::memory_order_acquire)) std::this_thread::yield();
        flush_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

using TestJournalWorker = server::runtime::JournalWorker<CountingJournalWriter, 1024>;

}  // namespace

TEST_CASE(journal_worker, destructor_drains_accepted_entries_and_joins) {
    appended_commands = 0;
    flush_requests = 0;
    stall_flush = false;
    const auto started = std::chrono::steady_clock::now();
    {
        TestJournalWorker worker("unused", sim::JournalSetup{});
        for (int index = 0; index < 1000; ++index) REQUIRE(worker.append(Command{}));
        REQUIRE(worker.flush());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_EQ(appended_commands.load(), 1000u);
    CHECK_EQ(flush_requests.load(), 2u);  // the requested flush and the final one
    CHECK(elapsed < std::chrono::milliseconds(100));
}

TEST_CASE(journal_worker, ring_holds_capacity_minus_one_entries_then_fails) {
    flush_requests = 0;
    writer_inside_flush = false;
    stall_flush = true;
    {
        TestJournalWorker worker("unused", sim::JournalSetup{});
        REQUIRE(worker.flush());
        while (!writer_inside_flush.load(std::memory_order_acquire)) std::this_thread::yield();
        std::size_t accepted = 0;
        while (worker.append(Command{})) ++accepted;
        CHECK_EQ(accepted, 1023u);
        CHECK(!worker.healthy());
        stall_flush = false;
    }
}

TEST_CASE(journal_worker, stalled_writer_is_abandoned_after_the_shutdown_wait) {
    flush_requests = 0;
    writer_inside_flush = false;
    stall_flush = true;
    const auto started = std::chrono::steady_clock::now();
    {
        TestJournalWorker worker("unused", sim::JournalSetup{});
        REQUIRE(worker.flush());
        while (!writer_inside_flush.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::milliseconds(1000));
    stall_flush = false;  // the detached writer finishes on the state it owns
    while (flush_requests.load() < 2) std::this_thread::yield();
}
