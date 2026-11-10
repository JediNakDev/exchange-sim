#ifndef EXSIM_EXCHANGE_ENGINE_SHARDED_ENGINE_HPP
#define EXSIM_EXCHANGE_ENGINE_SHARDED_ENGINE_HPP

#include <exchange/order_book.hpp>
#include <exchange/spsc_queue.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace exsim::exchange::engine {

using SymbolId = std::uint32_t;

class ShardedEngine {
public:
    struct OrderHandle {
        SymbolId symbol;
        std::uint64_t client_order_id;
    };

    explicit ShardedEngine(
        std::size_t shard_count,
        std::size_t batch_size = 256,
        bool pin_workers = true);
    ~ShardedEngine();

    ShardedEngine(const ShardedEngine&) = delete;
    ShardedEngine& operator=(const ShardedEngine&) = delete;
    ShardedEngine(ShardedEngine&&) = delete;
    ShardedEngine& operator=(ShardedEngine&&) = delete;

    [[nodiscard]] std::optional<OrderHandle> submit_add(
        SymbolId symbol,
        Price price,
        Quantity quantity,
        Side side) noexcept;
    [[nodiscard]] bool submit_cancel(OrderHandle handle) noexcept;
    [[nodiscard]] bool submit_modify(OrderHandle handle, Quantity new_quantity) noexcept;

    void flush() noexcept;
    void stop();

private:
    static constexpr std::size_t kQueueCapacity = 1u << 16;

    enum class CommandType : std::uint8_t { Add, Cancel, Modify, Stop };

    struct Command {
        CommandType type = CommandType::Add;
        SymbolId symbol = 0;
        std::uint64_t client_order_id = 0;
        Price price = 0;
        Quantity quantity = 0;
        Side side = Side::BUY;
    };

    struct alignas(128) Shard {
        SPSCQueue<Command, kQueueCapacity> queue;
        OrderBook book;
        std::unordered_map<std::uint64_t, OrderId> client_to_book_order;
        std::thread worker;
        std::atomic<bool> running{true};
    };

    std::size_t route(SymbolId symbol) const noexcept;
    bool try_submit(std::size_t shard_idx, const Command& cmd) noexcept;
    void worker_loop(std::size_t shard_idx);

    std::vector<std::unique_ptr<Shard>> shards_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> producer_owner_thread_;
    std::size_t batch_size_;
    bool pin_workers_;
    std::atomic<bool> stopped_{false};
    alignas(128) std::atomic<std::uint64_t> inflight_{0};
    alignas(128) std::atomic<std::uint64_t> next_client_order_id_{1};
};

}  // namespace exsim::exchange::engine

#endif
