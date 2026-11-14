// Feed correctness: a real MatchingEngine driven by long seeded command
// streams, published as ITCH over MoldUDP64, received and rebuilt by
// ItchBookBuilder. The rebuilt book must equal engine.depth() and every
// level FIFO (engine.level_orders()) after all packets of each batch:
//   * clean feed: checked after every command, receiver fully caught up;
//   * with dropped, duplicated and reordered packets (and dropped
//     retransmission responses): checked at every batch boundary as the
//     receiver recovers through the RetransmissionServer;
//   * both again with max_payload at its minimum (one message per packet).

#include <test/exsim/md/feed_harness.hpp>
#include <test/exsim/support/command_gen.hpp>
#include <test/exsim/support/format.hpp>
#include <test/framework/test.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

struct FeedRunTotals {
    std::uint64_t commands = 0;
    std::uint64_t messages = 0;
    std::uint64_t packets_dropped = 0;
    std::uint64_t packets_duplicated = 0;
    std::uint64_t packets_reordered = 0;
    std::uint64_t requests = 0;
    std::uint64_t responses_dropped = 0;
    std::uint64_t boundaries_checked = 0;
    std::uint64_t imbalance_messages = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t message_type_counts[256] = {};

    void add(const FeedHarness& feed, std::uint64_t command_count) {
        commands += command_count;
        messages += feed.publisher.stats().messages_sequenced;
        packets_dropped += feed.feed_injector.stats().dropped;
        packets_duplicated += feed.feed_injector.stats().duplicated;
        packets_reordered += feed.feed_injector.stats().reordered;
        requests += feed.receiver.stats().requests_sent;
        responses_dropped += feed.response_injector.stats().dropped;
        boundaries_checked += feed.handler.boundaries_checked;
        imbalance_messages += feed.publisher.stats().imbalance_messages_sent;
        heartbeats += feed.publisher.stats().heartbeats_sent;
        for (std::size_t type = 0; type < 256; ++type) message_type_counts[type] += feed.handler.message_type_counts[type];
    }

    // Every ITCH message type the publisher emits appeared on the feed.
    void check_type_coverage() const {
        for (char type : std::string_view("SRHAEXDUCQI")) {
            if (message_type_counts[static_cast<unsigned char>(type)] == 0)
                FAIL(std::string("no '") + type + "' message in the run");
        }
    }

    void print(const char* label) const {
        std::printf("    %s: %llu commands, %llu messages, %llu boundaries checked, %llu dropped, %llu duplicated, "
                    "%llu reordered, %llu requests, %llu responses dropped, %llu NOII, %llu heartbeats\n",
                    label, static_cast<unsigned long long>(commands), static_cast<unsigned long long>(messages),
                    static_cast<unsigned long long>(boundaries_checked),
                    static_cast<unsigned long long>(packets_dropped),
                    static_cast<unsigned long long>(packets_duplicated),
                    static_cast<unsigned long long>(packets_reordered), static_cast<unsigned long long>(requests),
                    static_cast<unsigned long long>(responses_dropped),
                    static_cast<unsigned long long>(imbalance_messages), static_cast<unsigned long long>(heartbeats));
        std::printf("      by type:");
        for (char type : std::string_view("SRHAEXDUCQI"))
            std::printf(" %c=%llu", type,
                        static_cast<unsigned long long>(message_type_counts[static_cast<unsigned char>(type)]));
        std::printf("\n");
    }
};

FeedHarnessOptions faulty_options(std::uint64_t seed) {
    FeedHarnessOptions options;
    options.feed_faults.drop_ppm = 30'000;
    options.feed_faults.duplicate_ppm = 20'000;
    options.feed_faults.reorder_ppm = 30'000;
    options.feed_faults.reorder_delay = 3;
    options.feed_faults.seed = seed * 7919 + 1;
    options.response_drop_ppm = 100'000;
    options.response_fault_seed = seed * 104'729 + 3;
    return options;
}

// One seeded run. Command times advance 1 ms per command with a 1.5 s idle
// gap every 700 commands, so heartbeats and the NOII timer fire.
void run_feed(std::uint64_t seed, std::size_t command_count, bool tiny_engine, const FeedHarnessOptions& options,
              bool check_every_batch, FeedRunTotals& totals) {
    const server::journal::JournalSetup setup = testsupport::fuzz_setup(tiny_engine);
    FeedHarness feed(setup, options);
    testsupport::CommandGenerator generator(seed);
    Timestamp now = 1'000'000;
    feed.start(now);
    for (std::size_t index = 0; index < command_count; ++index) {
        now += 1'000'000;
        if (index % 700 == 699) now += 1'500'000'000;
        Command command = generator.next(feed.engine);
        set_command_time(command, now);
        feed.step(command, index, now);
        if (check_every_batch) {
            const std::string mismatch = feed.clean_mismatch();
            if (!mismatch.empty())
                FAIL("seed " + std::to_string(seed) + " command " + std::to_string(index) + " " +
                     testsupport::format_command(command) + ": " + mismatch);
        }
        if (!feed.handler.first_failure.empty())
            FAIL("seed " + std::to_string(seed) + ": " + feed.handler.first_failure);
    }
    feed.finish(now + 1'000'000);
    feed.require_consistent_end();
    totals.add(feed, command_count);
}

}  // namespace

TEST_CASE(md_feed, clean_feed_matches_engine_after_every_batch) {
    FeedRunTotals totals;
    for (std::uint64_t seed = 1; seed <= 12; ++seed)
        run_feed(seed, 20'000, seed % 3 == 0, FeedHarnessOptions{}, true, totals);
    CHECK_EQ(totals.commands, 240'000u);
    CHECK_EQ(totals.requests, 0u);
    CHECK_GT(totals.imbalance_messages, 0u);
    CHECK_GT(totals.heartbeats, 0u);
    totals.check_type_coverage();
    totals.print("clean");
}

TEST_CASE(md_feed, faulty_feed_recovers_through_retransmission) {
    FeedRunTotals totals;
    for (std::uint64_t seed = 101; seed <= 108; ++seed)
        run_feed(seed, 20'000, seed % 3 == 0, faulty_options(seed), false, totals);
    CHECK_EQ(totals.commands, 160'000u);
    CHECK_GT(totals.packets_dropped, 0u);
    CHECK_GT(totals.packets_duplicated, 0u);
    CHECK_GT(totals.packets_reordered, 0u);
    CHECK_GT(totals.responses_dropped, 0u);
    CHECK_GT(totals.requests, 0u);
    CHECK_GT(totals.boundaries_checked, 0u);
    totals.check_type_coverage();
    totals.print("faults");
}

TEST_CASE(md_feed, minimum_payload_clean_and_with_faults) {
    FeedRunTotals clean;
    FeedRunTotals faulty;
    for (std::uint64_t seed = 201; seed <= 204; ++seed) {
        FeedHarnessOptions options;
        options.max_payload = md::kMinimumMaxPayload;
        run_feed(seed, 10'000, seed % 2 == 0, options, true, clean);
        FeedHarnessOptions faulty_tiny = faulty_options(seed);
        faulty_tiny.max_payload = md::kMinimumMaxPayload;
        run_feed(seed + 1'000, 10'000, seed % 2 == 1, faulty_tiny, false, faulty);
    }
    CHECK_EQ(clean.commands, 40'000u);
    CHECK_EQ(faulty.commands, 40'000u);
    CHECK_GT(faulty.packets_dropped, 0u);
    CHECK_GT(faulty.requests, 0u);
    clean.print("minimum payload, clean");
    faulty.print("minimum payload, faults");
}
