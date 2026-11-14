#include "benchmark/utils/sharded_engine.hpp"
#include "benchmark/utils/cpu_pinner.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>

namespace bench {

namespace {

std::uint64_t current_thread_token() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

}  // namespace

ShardedEngine::ShardedEngine(std::size_t shard_count, std::size_t batch_size, bool pin_workers)
    : batch_size_(std::max<std::size_t>(1, batch_size))
    , pin_workers_(pin_workers) {
    if (shard_count == 0) {
        shard_count = 1;
    }

    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        shards_.emplace_back(std::make_unique<Shard>());
    }
    producer_owner_thread_ = std::make_unique<std::atomic<std::uint64_t>[]>(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        producer_owner_thread_[i].store(0, std::memory_order_relaxed);
    }

    for (std::size_t i = 0; i < shards_.size(); ++i) {
        shards_[i]->worker = std::thread([this, i] { worker_loop(i); });
    }
}

ShardedEngine::~ShardedEngine() {
    stop();
}

std::optional<ShardedEngine::OrderHandle> ShardedEngine::submit_add(
    SymbolId symbol,
    Price price,
    Quantity quantity,
    Side side) noexcept {
    const std::uint64_t client_order_id = next_client_order_id_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t shard_idx = route(symbol);
    const bool accepted = try_submit(
        shard_idx, Command{CommandType::Add, symbol, client_order_id, price, quantity, side});
    if (!accepted) {
        return std::nullopt;
    }
    return OrderHandle{symbol, client_order_id};
}

bool ShardedEngine::submit_cancel(OrderHandle handle) noexcept {
    return try_submit(
        route(handle.symbol),
        Command{CommandType::Cancel, handle.symbol, handle.client_order_id, 0, 0, Side::BUY});
}

bool ShardedEngine::submit_modify(OrderHandle handle, Quantity new_quantity) noexcept {
    return try_submit(
        route(handle.symbol),
        Command{CommandType::Modify, handle.symbol, handle.client_order_id, 0, new_quantity, Side::BUY});
}

void ShardedEngine::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }

    for (std::size_t i = 0; i < shards_.size(); ++i) {
        Command stop_cmd;
        stop_cmd.type = CommandType::Stop;
        while (!shards_[i]->queue.try_push(stop_cmd)) {
            std::this_thread::yield();
        }
    }

    for (auto& shard : shards_) {
        if (shard->worker.joinable()) {
            shard->worker.join();
        }
    }
}

void ShardedEngine::flush() noexcept {
    while (inflight_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

std::size_t ShardedEngine::route(SymbolId symbol) const noexcept {
    return static_cast<std::size_t>(symbol) % shards_.size();
}

bool ShardedEngine::try_submit(std::size_t shard_idx, const Command& cmd) noexcept {
    if (stopped_.load(std::memory_order_acquire)) {
        return false;
    }

    // Enforce SPSC queue ownership: one producer thread per shard queue.
    const std::uint64_t producer_token = current_thread_token();
    std::uint64_t owner = producer_owner_thread_[shard_idx].load(std::memory_order_acquire);
    if (owner == 0) {
        std::uint64_t expected = 0;
        producer_owner_thread_[shard_idx].compare_exchange_strong(
            expected, producer_token, std::memory_order_release, std::memory_order_relaxed);
        owner = producer_owner_thread_[shard_idx].load(std::memory_order_acquire);
    }
    if (owner != producer_token) {
        return false;
    }

    // Count before publishing so flush cannot miss a queued command.
    inflight_.fetch_add(1, std::memory_order_release);
    if (!shards_[shard_idx]->queue.try_push(cmd)) {
        inflight_.fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

void ShardedEngine::worker_loop(std::size_t shard_idx) {
    Shard& shard = *shards_[shard_idx];
    if (pin_workers_) {
        const std::size_t hardware_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        (void)CPUPinner::pin(static_cast<int>(shard_idx % hardware_threads));
    }

    std::vector<Command> batch;
    batch.reserve(batch_size_);

    while (shard.running.load(std::memory_order_acquire)) {
        batch.clear();

        Command cmd;
        while (batch.size() < batch_size_ && shard.queue.try_pop(cmd)) {
            batch.push_back(cmd);
            if (cmd.type == CommandType::Stop) {
                break;
            }
        }

        if (batch.empty()) {
            std::this_thread::yield();
            continue;
        }

        for (const Command& op : batch) {
            switch (op.type) {
                case CommandType::Add: {
                    const auto result = shard.book.add_order(op.price, op.quantity, op.side);
                    if (result.order_id != 0 && result.remaining_quantity > 0) {
                        shard.client_to_book_order[op.client_order_id] = result.order_id;
                    }
                    inflight_.fetch_sub(1, std::memory_order_release);
                    break;
                }
                case CommandType::Cancel: {
                    auto it = shard.client_to_book_order.find(op.client_order_id);
                    if (it != shard.client_to_book_order.end()) {
                        if (shard.book.cancel_order(it->second)) {
                            shard.client_to_book_order.erase(it);
                        }
                    }
                    inflight_.fetch_sub(1, std::memory_order_release);
                    break;
                }
                case CommandType::Modify: {
                    auto it = shard.client_to_book_order.find(op.client_order_id);
                    if (it != shard.client_to_book_order.end()) {
                        static_cast<void>(shard.book.modify_order(it->second, op.quantity));
                    }
                    inflight_.fetch_sub(1, std::memory_order_release);
                    break;
                }
                case CommandType::Stop:
                    shard.running.store(false, std::memory_order_release);
                    break;
            }
        }
    }
}

}  // namespace bench
