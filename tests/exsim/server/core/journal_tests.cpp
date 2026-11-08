// The canonical journal written by ExchangeCore: replaying it into a fresh
// engine reproduces every dispatched event, and write failures follow
// `[journal] on_failure`.

#include "tests/exsim/server/core/core_test_support.hpp"
#include "exsim/sim/journal.hpp"

#include <csignal>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <vector>

using namespace exsim;
using namespace exsim::server;
using namespace exsim::server::core_test;

namespace {

constexpr InstrumentId kAapl = 1;
constexpr InstrumentId kMsft = 2;

std::string journal_section(const std::string& path, const std::string& extra_keys = "") {
    return "\n[journal]\npath = " + path + "\n" + extra_keys;
}

// Removes the journal file when the test ends, whatever happened.
struct TempFile {
    std::string path;
    ~TempFile() { std::remove(path.c_str()); }
};

std::size_t file_size(const std::string& path) {
    struct stat info {};
    REQUIRE_EQ(::stat(path.c_str(), &info), 0);
    return static_cast<std::size_t>(info.st_size);
}

// While alive, any write that would grow a regular file past `limit_bytes`
// fails with EFBIG (SIGXFSZ is ignored). Nothing else may write to files
// meanwhile, test output included, so checks happen after it is destroyed.
class FileSizeLimit {
public:
    explicit FileSizeLimit(std::size_t limit_bytes) {
        std::cout.flush();
        std::fflush(nullptr);
        previous_handler_ = std::signal(SIGXFSZ, SIG_IGN);
        installed_ = ::getrlimit(RLIMIT_FSIZE, &previous_limit_) == 0;
        if (installed_) {
            rlimit limited = previous_limit_;
            limited.rlim_cur = static_cast<rlim_t>(limit_bytes);
            installed_ = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
        }
    }
    ~FileSizeLimit() {
        if (installed_) ::setrlimit(RLIMIT_FSIZE, &previous_limit_);
        std::signal(SIGXFSZ, previous_handler_);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    [[nodiscard]] bool installed() const noexcept { return installed_; }

private:
    rlimit previous_limit_{};
    void (*previous_handler_)(int) = SIG_DFL;
    bool installed_ = false;
};

struct Replay {
    std::vector<Command> commands;
    std::vector<Event> events;
};

Replay replay_journal(const std::string& path) {
    sim::JournalReader reader(path);
    MatchingEngine engine(reader.setup().engine);
    for (const InstrumentConfig& instrument : reader.setup().instruments) {
        REQUIRE(engine.add_instrument(instrument) != 0);
    }
    EventBuffer output(required_event_capacity(reader.setup().engine));
    Replay replay;
    Command command;
    while (reader.next(command)) {
        replay.commands.push_back(command);
        output.clear();
        REQUIRE(engine.handle(command, output) == HandleResult::Applied);
        replay.events.insert(replay.events.end(), output.begin(), output.end());
    }
    return replay;
}

bool same_canonical_events(const std::vector<Event>& left, const std::vector<Event>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (sim::encode(left[i]) != sim::encode(right[i])) return false;
    }
    return true;
}

SessionId command_session(const Command& command) {
    switch (command.type) {
        case CommandType::NewOrder: return command.new_order.session;
        case CommandType::Cancel: return command.cancel.session;
        case CommandType::Replace: return command.replace.session;
        case CommandType::MassCancel: return command.mass_cancel.session;
        case CommandType::SetPhase: return 0;
    }
    return 0;
}

bool journal_is_readable(const std::string& path) {
    try {
        sim::JournalReader reader(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

TEST_CASE(server_core_journal, replay_reproduces_every_dispatched_event) {
    const TempFile journal{temp_journal_path("replay")};
    RecordingHarness h(parse_test_config(std::string(kTwoBots) + journal_section(journal.path)));
    REQUIRE(h.core.journal_state() == JournalState::Healthy);
    REQUIRE(h.core.bind(1, 11));
    REQUIRE(h.core.bind(2, 12));

    // Clients, bots, admin and a disconnect, interleaved on one clock.
    for (Timestamp now = kSecond; now < 2 * kSecond; now += kMillisecond) {
        h.clock.day = now;
        h.core.timer(now);
        if (now == kSecond + 300 * kMillisecond) {
            REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'400'000, 100)), SubmitResult::Applied);
            REQUIRE_EQ(h.core.submit(limit_order(1, 2, kMsft, Side::BUY, 4'000'000, 30)), SubmitResult::Applied);
            REQUIRE_EQ(h.core.submit(limit_order(2, 1, kMsft, Side::SELL, 3'990'000, 20)), SubmitResult::Applied);
        }
        if (now == kSecond + 500 * kMillisecond) {
            std::string reply;
            h.core.execute_line("phase MSFT continuous", reply);
            REQUIRE(ends_with_ok(reply));
        }
        if (now == kSecond + 700 * kMillisecond) {
            const auto best_ask = h.core.engine().best_ask(kAapl);
            REQUIRE(best_ask.has_value());
            REQUIRE_EQ(h.core.submit(limit_order(2, 2, kAapl, Side::BUY, *best_ask, 10)), SubmitResult::Applied);
            h.core.unbind(1, 11);  // cancel-on-disconnect
        }
    }
    h.core.shutdown();  // SetPhase Closed, then a journal flush
    REQUIRE(h.core.journal_state() == JournalState::Healthy);

    const Replay replay = replay_journal(journal.path);
    CHECK_EQ(replay.commands.size(), h.core.counters().journal_records);
    CHECK_EQ(replay.commands.size(), h.core.counters().commands_applied);
    CHECK_EQ(replay.events.size(), h.recorder.events.size());
    CHECK(same_canonical_events(replay.events, h.recorder.events));

    bool saw_maker = false, saw_taker = false, saw_cancel_on_disconnect = false;
    for (const Command& command : replay.commands) {
        saw_maker = saw_maker || command_session(command) == 100;
        saw_taker = saw_taker || command_session(command) == 101;
        saw_cancel_on_disconnect = saw_cancel_on_disconnect || (command.type == CommandType::MassCancel &&
                                                                command.mass_cancel.cancel_on_disconnect);
    }
    CHECK(saw_maker);
    CHECK(saw_taker);
    CHECK(saw_cancel_on_disconnect);
    CHECK(replay.commands.back().type == CommandType::SetPhase);
}

TEST_CASE(server_core_journal, refused_commands_are_not_journaled) {
    const TempFile journal{temp_journal_path("refused")};
    RecordingHarness h(parse_test_config("[exchange]\ntrading_day_start = 00:00:02\n" +
                                         journal_section(journal.path)));
    h.clock.day = kSecond;
    CHECK_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::OutsideTradingDay);
    h.clock.day = 3 * kSecond;
    CHECK_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    REQUIRE(h.core.flush_journal());
    const Replay replay = replay_journal(journal.path);
    REQUIRE_EQ(replay.commands.size(), 1u);
    CHECK_EQ(replay.commands[0].new_order.cl_ord_id, 2u);
    CHECK_EQ(replay.commands[0].new_order.ts, 3 * kSecond);  // the stamp is journaled
}

TEST_CASE(server_core_journal, timer_flushes_periodically_so_the_file_is_readable_while_running) {
    const TempFile journal{temp_journal_path("periodic")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 500\n")));
    h.clock.day = 100 * kMillisecond;
    h.core.timer(h.clock.day);  // less than one interval since midnight: no flush yet
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK(!journal_is_readable(journal.path));  // records still buffered, no footer

    h.clock.day = 600 * kMillisecond;
    h.core.timer(h.clock.day);
    REQUIRE(journal_is_readable(journal.path));
    CHECK_EQ(replay_journal(journal.path).commands.size(), 1u);

    REQUIRE_EQ(h.core.submit(limit_order(1, 2, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.clock.day = 900 * kMillisecond;
    h.core.timer(h.clock.day);  // within the interval: the file still ends at the last flush
    CHECK_EQ(replay_journal(journal.path).commands.size(), 1u);
    h.clock.day = 1100 * kMillisecond;
    h.core.timer(h.clock.day);
    CHECK_EQ(replay_journal(journal.path).commands.size(), 2u);
}

TEST_CASE(server_core_journal, zero_flush_interval_never_flushes_from_the_timer) {
    const TempFile journal{temp_journal_path("no-periodic")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 0\n")));
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    h.core.timer(10 * kSecond);
    CHECK(!journal_is_readable(journal.path));
    REQUIRE(h.core.flush_journal());
    CHECK(journal_is_readable(journal.path));
}

TEST_CASE(server_core_journal, failed_flush_with_shutdown_policy_closes_the_exchange) {
    const TempFile journal{temp_journal_path("fail-shutdown")};
    RecordingHarness h(parse_test_config(journal_section(journal.path)));
    for (ClOrdId token = 1; token <= 3; ++token) {
        REQUIRE_EQ(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    }
    bool flushed = true;
    bool limit_installed = false;
    {
        const FileSizeLimit limit(file_size(journal.path));
        limit_installed = limit.installed();
        flushed = h.core.flush_journal();
    }
    REQUIRE(limit_installed);
    CHECK(!flushed);
    CHECK(h.core.journal_state() == JournalState::Failed);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK_EQ(h.core.counters().journal_records, 3u);
    // Shutdown ran: every instrument closed through submit (not journaled).
    CHECK(h.core.shutdown_requested());
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
    CHECK_EQ(h.core.counters().commands_applied, 4u);
    CHECK_EQ(h.recorder.sessions_shutdown_calls, 1u);
    CHECK(!h.core.flush_journal());  // failure is latched
    std::string reply;
    h.core.execute_line("status", reply);
    CHECK(contains(reply, "journal state=failed path=" + journal.path + " records=3 failures=1 on_failure=shutdown\n"));
}

TEST_CASE(server_core_journal, failed_append_with_shutdown_policy_finishes_the_batch_first) {
    const TempFile journal{temp_journal_path("fail-append")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "flush_interval_ms = 0\n")));
    std::vector<SubmitResult> results;
    std::vector<bool> shut_down_after;
    std::size_t batches_before_failure = 0;
    {
        const FileSizeLimit limit(file_size(journal.path));
        // 128 records fit the writer's buffer; the 129th append writes it out.
        for (ClOrdId token = 1; token <= 130; ++token) {
            if (token == 129) batches_before_failure = h.recorder.batch_count();
            results.push_back(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)));
            shut_down_after.push_back(h.core.shutdown_requested());
        }
    }
    REQUIRE_EQ(results.size(), 130u);
    CHECK(!shut_down_after[127]);
    CHECK(shut_down_after[128]);
    CHECK_EQ(results[128], SubmitResult::Applied);  // applied and dispatched before the shutdown
    CHECK(h.recorder.batch(batches_before_failure)[0].type == EventType::Accepted);
    CHECK(h.recorder.batch(batches_before_failure + 1)[0].type == EventType::Canceled);  // EndOfDay expiry
    CHECK_EQ(h.core.counters().journal_records, 128u);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK(h.core.engine().phase(kAapl) == Phase::Closed);
}

TEST_CASE(server_core_journal, failed_append_with_continue_policy_keeps_trading) {
    const TempFile journal{temp_journal_path("fail-continue")};
    RecordingHarness h(parse_test_config(journal_section(journal.path, "on_failure = continue\n")));
    std::vector<SubmitResult> results;
    {
        const FileSizeLimit limit(file_size(journal.path));
        for (ClOrdId token = 1; token <= 140; ++token) {
            results.push_back(h.core.submit(limit_order(1, token, kAapl, Side::BUY, 1'000'000, 10)));
        }
        h.core.timer(5 * kSecond);  // periodic flush is skipped once failed
    }
    for (const SubmitResult result : results) CHECK_EQ(result, SubmitResult::Applied);
    CHECK(!h.core.shutdown_requested());
    CHECK(h.core.journal_state() == JournalState::Failed);
    CHECK_EQ(h.core.counters().journal_records, 128u);
    CHECK_EQ(h.core.counters().journal_failures, 1u);
    CHECK_EQ(h.core.engine().live_order_count(), 140u);
    CHECK(!h.core.flush_journal());
    CHECK(!journal_is_readable(journal.path));
}

TEST_CASE(server_core_journal, disabled_journal_reports_disabled) {
    RecordingHarness h(parse_test_config());
    CHECK(h.core.journal_state() == JournalState::Disabled);
    CHECK(!h.core.flush_journal());
    REQUIRE_EQ(h.core.submit(limit_order(1, 1, kAapl, Side::BUY, 1'000'000, 10)), SubmitResult::Applied);
    CHECK_EQ(h.core.counters().journal_records, 0u);
    CHECK(!h.core.shutdown_requested());
}
