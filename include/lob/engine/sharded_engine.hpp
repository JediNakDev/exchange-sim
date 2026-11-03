#ifndef LOB_ENGINE_SHARDED_ENGINE_HPP
#define LOB_ENGINE_SHARDED_ENGINE_HPP

#include "../flat_hash_map.hpp"
#include "../order_book.hpp"
#include "spsc_queue.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace lob::engine {

using SymbolId = std::uint32_t;

// Setup-time sizing. Every shard pre-creates `max_symbols_per_shard` books,
// each sized by `book_config`, so the worker never allocates a book or a
// client-id mapping after construction.
struct ShardedEngineConfig {
    std::size_t shard_count = 1;
    std::size_t batch_size = 256;
    bool pin_workers = true;
    // Distinct symbols one shard can hold. The first order for a new symbol
    // binds it to a free book; once all are bound, adds for further symbols
    // routed to that shard are rejected (submit_add returns nullopt).
    std::size_t max_symbols_per_shard = 8;
    // Also bounds the client-id mappings of each book: a resting remainder
    // that finds no free mapping slot is cancelled and counted in
    // capacity_rejection_count().
    OrderBookConfig book_config{
        .max_resting_orders = std::size_t{1} << 13,
        .max_price_levels = std::size_t{1} << 12,
        .min_price = 0,
        .max_price = (Price{1} << 16) - 1,
        .max_fills_per_add_order = 1024,
    };
};

class ShardedEngine {
public:
    struct OrderHandle {
        SymbolId symbol;
        std::uint64_t client_order_id;
    };

    explicit ShardedEngine(const ShardedEngineConfig& config);
    explicit ShardedEngine(
        std::size_t shard_count,
        std::size_t batch_size = 256,
        bool pin_workers = true);
    ~ShardedEngine();

    ShardedEngine(const ShardedEngine&) = delete;
    ShardedEngine& operator=(const ShardedEngine&) = delete;
    ShardedEngine(ShardedEngine&&) = delete;
    ShardedEngine& operator=(ShardedEngine&&) = delete;

    // Each shard accepts submissions from one producer thread only (the
    // first thread that submits to it); other threads are refused.
    [[nodiscard]] std::optional<OrderHandle> submit_add(
        SymbolId symbol,
        Price price,
        Quantity quantity,
        Side side) noexcept;
    [[nodiscard]] bool submit_cancel(OrderHandle handle) noexcept;
    [[nodiscard]] bool submit_modify(OrderHandle handle, Quantity new_quantity) noexcept;

    void flush() noexcept;
    void stop();

    // Quiescent inspection. Call only from the producer thread after flush()
    // (or after stop()), with no submission in flight; workers own this
    // state while commands are being processed.
    [[nodiscard]] const OrderBook* find_book(SymbolId symbol) const noexcept;
    // Live client-id -> book-order mappings over all shards and symbols.
    [[nodiscard]] std::size_t client_order_mapping_count() const noexcept;
    // Live book-order -> client-id reverse mappings (always equal to the
    // forward count; exposed so tests can prove both are reclaimed).
    [[nodiscard]] std::size_t book_order_mapping_count() const noexcept;
    // Resting remainders cancelled for capacity: the book's mapping maps or
    // the book itself (id map or price levels) had no room.
    [[nodiscard]] std::uint64_t capacity_rejection_count() const noexcept;
    // Aggressive orders whose remainder was cancelled at the fill limit.
    [[nodiscard]] std::uint64_t fill_limit_cancellation_count() const noexcept;

private:
    static constexpr std::size_t kQueueCapacity = 1u << 16;

    enum class CommandType : std::uint8_t { Add, Cancel, Modify, Stop };

    struct Command {
        CommandType type = CommandType::Add;
        Side side = Side::BUY;
        std::uint32_t book_slot = 0;
        std::uint64_t client_order_id = 0;
        Price price = 0;
        Quantity quantity = 0;
    };

    // One instrument's book plus the mappings that make its orders
    // addressable by engine-wide client ids. Book order ids restart at 1 in
    // every book, so the reverse map must be per book.
    struct InstrumentBook {
        explicit InstrumentBook(const OrderBookConfig& book_config);

        OrderBook book;
        FlatHashMap<OrderId> client_to_book_order;
        FlatHashMap<std::uint64_t> book_to_client_order;
    };

    struct alignas(128) Shard {
        explicit Shard(const ShardedEngineConfig& config);

        SPSCQueue<Command, kQueueCapacity> queue;
        // Created in the constructor; never resized afterwards.
        std::vector<std::unique_ptr<InstrumentBook>> instrument_books;
        // Producer-thread state: symbol (+1, since key 0 is reserved) ->
        // index into instrument_books.
        FlatHashMap<std::uint32_t> symbol_to_book_slot;
        std::uint32_t bound_symbol_count = 0;
        // Worker-thread counters, read only when quiescent.
        std::uint64_t capacity_rejections = 0;
        std::uint64_t fill_limit_cancellations = 0;
        std::thread worker;
        std::atomic<bool> running{true};
    };

    std::size_t route(SymbolId symbol) const noexcept;
    bool claim_producer(std::size_t shard_idx) noexcept;
    std::optional<std::uint32_t> find_book_slot(const Shard& shard, SymbolId symbol) const noexcept;
    std::optional<std::uint32_t> find_or_bind_book_slot(Shard& shard, SymbolId symbol) noexcept;
    bool push_command(std::size_t shard_idx, const Command& cmd) noexcept;
    void worker_loop(std::size_t shard_idx);
    static void apply_add(Shard& shard, const Command& op);
    static void apply_cancel(Shard& shard, const Command& op);
    static void apply_modify(Shard& shard, const Command& op);
    static void erase_mapping(InstrumentBook& instrument, std::uint64_t client_order_id, OrderId book_order_id) noexcept;

    std::vector<std::unique_ptr<Shard>> shards_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> producer_owner_thread_;
    std::size_t batch_size_;
    bool pin_workers_;
    std::atomic<bool> stopped_{false};
    alignas(128) std::atomic<std::uint64_t> inflight_{0};
    alignas(128) std::atomic<std::uint64_t> next_client_order_id_{1};
};

}  // namespace lob::engine

#endif
