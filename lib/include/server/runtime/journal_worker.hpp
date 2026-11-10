#ifndef EXSIM_SERVER_RUNTIME_JOURNAL_WORKER_HPP
#define EXSIM_SERVER_RUNTIME_JOURNAL_WORKER_HPP

#include "sim/journal.hpp"
#include "exchange/spsc_queue.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace exsim::server::runtime {

// The reactor only enqueues commands and flush requests. The writer policy
// owns all file operations after startup. Accepted entries are never dropped
// silently: overflow or an I/O error latches failure for the exchange core.
// A stalled writer retains its own state if shutdown must abandon the wait.
template <class Writer = sim::JournalWriter, std::size_t Capacity = 16385>
class JournalWorker {
public:
    JournalWorker(const std::string& path, const sim::JournalSetup& setup)
        : state_(std::make_shared<State>(path, setup)), worker_([state = state_] { persist(*state); }) {}

    ~JournalWorker() {
        state_->stopping.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (!state_->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (state_->done.load(std::memory_order_acquire)) worker_.join();
        else {
            abort();
            // The worker captures the state; it never accesses the runtime.
            // Process exit can reclaim a writer stuck in the kernel.
            worker_.detach();
        }
    }

    JournalWorker(const JournalWorker&) = delete;
    JournalWorker& operator=(const JournalWorker&) = delete;

    [[nodiscard]] bool append(const Command& command) noexcept {
        if (!enqueue({command, false})) return false;
        last_operation_flush_ = false;
        return true;
    }
    // Acceptance of a flush request, not a durability acknowledgement.
    [[nodiscard]] bool flush() noexcept {
        if (!healthy()) return false;
        if (last_operation_flush_) return true;
        if (!enqueue({{}, true})) return false;
        last_operation_flush_ = true;
        return true;
    }
    [[nodiscard]] bool healthy() const noexcept { return !state_->failed.load(std::memory_order_acquire); }
    [[nodiscard]] bool drained() const noexcept {
        return state_->flushed_operations.load(std::memory_order_acquire) == submitted_operations_;
    }
    void abort() noexcept {
        state_->failed.store(true, std::memory_order_release);
        state_->stopping.store(true, std::memory_order_release);
    }

private:
    struct Entry {
        Command command;
        bool flush;
    };
    struct State {
        State(const std::string& path, const sim::JournalSetup& setup) : writer(std::in_place, path, setup) {}
        std::optional<Writer> writer;  // closed by the persistence thread
        exsim::exchange::engine::SPSCQueue<Entry, Capacity> queue;
        std::atomic<bool> stopping{false};
        std::atomic<bool> failed{false};
        std::atomic<bool> done{false};
        std::atomic<std::uint64_t> flushed_operations{0};
    };

    bool enqueue(const Entry& entry) noexcept {
        if (!healthy() || state_->stopping.load(std::memory_order_acquire)) return false;
        if (!state_->queue.try_push(entry)) {
            abort();
            return false;
        }
        ++submitted_operations_;
        return true;
    }

    static void persist(State& state) noexcept {
        std::uint64_t processed = 0;
        bool io_ok = true;
        for (;;) {
            Entry entry{};
            if (state.queue.try_pop(entry)) {
                ++processed;
                io_ok = entry.flush ? state.writer->flush() : state.writer->append(entry.command);
                if (!io_ok) break;
                if (entry.flush) state.flushed_operations.store(processed, std::memory_order_release);
            } else if (state.stopping.load(std::memory_order_acquire)) {
                // Recheck after observing stopping so an entry published
                // before it cannot be missed by the final drain.
                if (!state.queue.empty()) continue;
                io_ok = state.writer->flush();
                if (io_ok) state.flushed_operations.store(processed, std::memory_order_release);
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!io_ok) state.failed.store(true, std::memory_order_release);
        state.writer.reset();  // close can block too; keep it off the reactor
        state.done.store(true, std::memory_order_release);
    }

    std::shared_ptr<State> state_;
    std::uint64_t submitted_operations_ = 0;  // reactor-owned
    bool last_operation_flush_ = false;
    std::thread worker_;
};

}  // namespace exsim::server::runtime

#endif
