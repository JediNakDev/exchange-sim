#include <lob/engine/sharded_engine.hpp>
#include <lob/engine/thread_pinning.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <thread>

namespace lob::engine {

namespace {

std::uint64_t current_thread_token() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

// FlatHashMap reserves key 0, and SymbolId 0 is a valid symbol.
std::uint64_t symbol_key(SymbolId symbol) noexcept { return std::uint64_t{symbol} + 1; }

OrderBookConfig without_growth(const OrderBookConfig& book_config) {
    OrderBookConfig bounded = book_config;
    bounded.allow_growth = false;
    return bounded;
}

ShardedEngineConfig legacy_config(std::size_t shard_count, std::size_t batch_size, bool pin_workers) {
    ShardedEngineConfig config;
    config.shard_count = shard_count;
    config.batch_size = batch_size;
    config.pin_workers = pin_workers;
    return config;
}

}  // namespace

ShardedEngine::InstrumentBook::InstrumentBook(const OrderBookConfig& book_config)
    : book(without_growth(book_config))
    , client_to_book_order(std::max<std::size_t>(1, book_config.max_resting_orders))
    , book_to_client_order(std::max<std::size_t>(1, book_config.max_resting_orders)) {
    // Growth stays disabled: a full map rejects instead of allocating.
}

ShardedEngine::Shard::Shard(const ShardedEngineConfig& config)
    : symbol_to_book_slot(std::max<std::size_t>(1, config.max_symbols_per_shard)) {
    const std::size_t book_count = std::max<std::size_t>(1, config.max_symbols_per_shard);
    instrument_books.reserve(book_count);
    for (std::size_t i = 0; i < book_count; ++i) {
        instrument_books.emplace_back(std::make_unique<InstrumentBook>(config.book_config));
    }
}

ShardedEngine::ShardedEngine(std::size_t shard_count, std::size_t batch_size, bool pin_workers)
    : ShardedEngine(legacy_config(shard_count, batch_size, pin_workers)) {}

ShardedEngine::ShardedEngine(const ShardedEngineConfig& config)
    : batch_size_(std::max<std::size_t>(1, config.batch_size))
    , pin_workers_(config.pin_workers)
    , book_min_price_(std::min(config.book_config.min_price, config.book_config.max_price))
    , book_max_price_(std::max(config.book_config.min_price, config.book_config.max_price)) {
    const std::size_t shard_count = std::max<std::size_t>(1, config.shard_count);

    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        shards_.emplace_back(std::make_unique<Shard>(config));
    }
    producer_owner_thread_ = std::make_unique<std::atomic<std::uint64_t>[]>(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        producer_owner_thread_[i].store(0, std::memory_order_relaxed);
    }

    try {
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            shards_[i]->worker = std::thread([this, i] { worker_loop(i); });
        }
    } catch (...) {
        // Destroying a joinable std::thread terminates, so exit and join the
        // workers already started. No command can be queued yet.
        stop();
        throw;
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
    if (price < book_min_price_ || price > book_max_price_) {
        return std::nullopt;  // the bounded book could never hold it
    }
    const std::size_t shard_idx = route(symbol);
    if (!claim_producer(shard_idx)) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> book_slot = find_or_bind_book_slot(*shards_[shard_idx], symbol);
    if (!book_slot) {
        return std::nullopt;  // shard symbol capacity exhausted
    }

    const std::uint64_t client_order_id = next_client_order_id_.fetch_add(1, std::memory_order_relaxed);
    Command cmd;
    cmd.type = CommandType::Add;
    cmd.side = side;
    cmd.book_slot = *book_slot;
    cmd.client_order_id = client_order_id;
    cmd.price = price;
    cmd.quantity = quantity;
    if (!push_command(shard_idx, cmd)) {
        return std::nullopt;
    }
    return OrderHandle{symbol, client_order_id};
}

bool ShardedEngine::submit_cancel(OrderHandle handle) noexcept {
    const std::size_t shard_idx = route(handle.symbol);
    if (!claim_producer(shard_idx)) {
        return false;
    }
    const std::optional<std::uint32_t> book_slot = find_book_slot(*shards_[shard_idx], handle.symbol);
    if (!book_slot) {
        return false;
    }
    Command cmd;
    cmd.type = CommandType::Cancel;
    cmd.book_slot = *book_slot;
    cmd.client_order_id = handle.client_order_id;
    return push_command(shard_idx, cmd);
}

bool ShardedEngine::submit_modify(OrderHandle handle, Quantity new_quantity) noexcept {
    const std::size_t shard_idx = route(handle.symbol);
    if (!claim_producer(shard_idx)) {
        return false;
    }
    const std::optional<std::uint32_t> book_slot = find_book_slot(*shards_[shard_idx], handle.symbol);
    if (!book_slot) {
        return false;
    }
    Command cmd;
    cmd.type = CommandType::Modify;
    cmd.book_slot = *book_slot;
    cmd.client_order_id = handle.client_order_id;
    cmd.quantity = new_quantity;
    return push_command(shard_idx, cmd);
}

void ShardedEngine::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }

    // push_command counts a submission before it re-checks stopped_ (both
    // seq_cst), so each submission either sees the stop and backs out or is
    // counted here and processed before the workers are told to exit. Only
    // producers ever push, so the SPSC queues keep a single writer.
    while (inflight_.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    for (auto& shard : shards_) {
        shard->stop_requested.store(true, std::memory_order_release);
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

const OrderBook* ShardedEngine::find_book(SymbolId symbol) const noexcept {
    const Shard& shard = *shards_[route(symbol)];
    const std::optional<std::uint32_t> book_slot = find_book_slot(shard, symbol);
    if (!book_slot) {
        return nullptr;
    }
    return &shard.instrument_books[*book_slot]->book;
}

std::size_t ShardedEngine::client_order_mapping_count() const noexcept {
    std::size_t count = 0;
    for (const auto& shard : shards_) {
        for (const auto& instrument : shard->instrument_books) {
            count += instrument->client_to_book_order.size();
        }
    }
    return count;
}

std::size_t ShardedEngine::book_order_mapping_count() const noexcept {
    std::size_t count = 0;
    for (const auto& shard : shards_) {
        for (const auto& instrument : shard->instrument_books) {
            count += instrument->book_to_client_order.size();
        }
    }
    return count;
}

std::uint64_t ShardedEngine::capacity_rejection_count() const noexcept {
    std::uint64_t count = 0;
    for (const auto& shard : shards_) {
        count += shard->capacity_rejections;
    }
    return count;
}

std::uint64_t ShardedEngine::fill_limit_cancellation_count() const noexcept {
    std::uint64_t count = 0;
    for (const auto& shard : shards_) {
        count += shard->fill_limit_cancellations;
    }
    return count;
}

std::size_t ShardedEngine::route(SymbolId symbol) const noexcept {
    return static_cast<std::size_t>(symbol) % shards_.size();
}

bool ShardedEngine::claim_producer(std::size_t shard_idx) noexcept {
    if (stopped_.load(std::memory_order_acquire)) {
        return false;
    }

    // Enforce SPSC queue ownership: one producer thread per shard queue. The
    // owner is also the only thread that touches the shard's symbol binding.
    const std::uint64_t producer_token = current_thread_token();
    std::uint64_t owner = producer_owner_thread_[shard_idx].load(std::memory_order_acquire);
    if (owner == 0) {
        std::uint64_t expected = 0;
        producer_owner_thread_[shard_idx].compare_exchange_strong(
            expected, producer_token, std::memory_order_acq_rel, std::memory_order_acquire);
        owner = producer_owner_thread_[shard_idx].load(std::memory_order_acquire);
    }
    return owner == producer_token;
}

std::optional<std::uint32_t> ShardedEngine::find_book_slot(const Shard& shard, SymbolId symbol) const noexcept {
    if (const std::uint32_t* slot = shard.symbol_to_book_slot.find(symbol_key(symbol))) {
        return *slot;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ShardedEngine::find_or_bind_book_slot(Shard& shard, SymbolId symbol) noexcept {
    if (const std::optional<std::uint32_t> slot = find_book_slot(shard, symbol)) {
        return slot;
    }
    if (shard.bound_symbol_count >= shard.instrument_books.size()) {
        return std::nullopt;
    }
    const std::uint32_t slot = shard.bound_symbol_count;
    // Growth is disabled and the map holds at least instrument_books.size()
    // keys, so this cannot allocate or fail.
    if (!shard.symbol_to_book_slot.insert(symbol_key(symbol), slot)) {
        return std::nullopt;
    }
    ++shard.bound_symbol_count;
    return slot;
}

bool ShardedEngine::push_command(std::size_t shard_idx, const Command& cmd) noexcept {
    // Count before publishing so a fast worker can never decrement first,
    // and before re-checking stopped_ so stop() waits for this command.
    inflight_.fetch_add(1, std::memory_order_seq_cst);
    if (stopped_.load(std::memory_order_seq_cst) || !shards_[shard_idx]->queue.try_push(cmd)) {
        inflight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void ShardedEngine::erase_mapping(
    InstrumentBook& instrument,
    std::uint64_t client_order_id,
    OrderId book_order_id) noexcept {
    (void)instrument.client_to_book_order.erase(client_order_id);
    (void)instrument.book_to_client_order.erase(book_order_id);
}

void ShardedEngine::apply_add(Shard& shard, const Command& op) {
    InstrumentBook& instrument = *shard.instrument_books[op.book_slot];
    const OrderBook::AddResult result = instrument.book.add_order(op.price, op.quantity, op.side);

    // Every executed fill is reported (the fill buffer is bounded by stopping
    // the match, never by dropping fills), so each fully executed resting
    // order is seen here and its mapping reclaimed. Matching consumes resting
    // orders front to back, so only the last fill can leave one resting.
    for (std::size_t i = 0; i < result.fills.size(); ++i) {
        const Fill& fill = result.fills[i];
        const OrderId resting_order_id = (op.side == Side::BUY) ? fill.sell_order_id : fill.buy_order_id;
        const bool is_last_fill = i + 1 == result.fills.size();
        if (is_last_fill && instrument.book.has_order(resting_order_id)) {
            continue;  // partially filled, still resting
        }
        if (const std::uint64_t* client_order_id = instrument.book_to_client_order.find(resting_order_id)) {
            erase_mapping(instrument, *client_order_id, resting_order_id);
        }
    }
    if (result.fill_limit_reached) {
        ++shard.fill_limit_cancellations;
    } else if (result.cancelled_quantity > 0) {
        ++shard.capacity_rejections;  // the book itself had no room to rest it
    }

    if (result.order_id == 0 || result.remaining_quantity == 0) {
        return;
    }
    if (!instrument.client_to_book_order.insert(op.client_order_id, result.order_id)) {
        (void)instrument.book.cancel_order(result.order_id);
        ++shard.capacity_rejections;
        return;
    }
    if (!instrument.book_to_client_order.insert(result.order_id, op.client_order_id)) {
        (void)instrument.client_to_book_order.erase(op.client_order_id);
        (void)instrument.book.cancel_order(result.order_id);
        ++shard.capacity_rejections;
    }
}

void ShardedEngine::apply_cancel(Shard& shard, const Command& op) {
    InstrumentBook& instrument = *shard.instrument_books[op.book_slot];
    const OrderId* book_order_id = instrument.client_to_book_order.find(op.client_order_id);
    if (!book_order_id) {
        return;
    }
    const OrderId resting_order_id = *book_order_id;
    (void)instrument.book.cancel_order(resting_order_id);
    erase_mapping(instrument, op.client_order_id, resting_order_id);
}

void ShardedEngine::apply_modify(Shard& shard, const Command& op) {
    InstrumentBook& instrument = *shard.instrument_books[op.book_slot];
    const OrderId* book_order_id = instrument.client_to_book_order.find(op.client_order_id);
    if (!book_order_id) {
        return;
    }
    const OrderId resting_order_id = *book_order_id;
    (void)instrument.book.modify_order(resting_order_id, op.quantity);
    if (!instrument.book.has_order(resting_order_id)) {
        // Modified down to nothing left to execute: the book removed it.
        erase_mapping(instrument, op.client_order_id, resting_order_id);
    }
}

void ShardedEngine::worker_loop(std::size_t shard_idx) {
    Shard& shard = *shards_[shard_idx];
    if (pin_workers_) {
        const std::size_t core = shard_idx % hardware_threads();
        (void)pin_current_thread_to_core(core);
    }

    for (;;) {
        std::size_t applied = 0;
        Command op;
        while (applied < batch_size_ && shard.queue.try_pop(op)) {
            switch (op.type) {
                case CommandType::Add:
                    apply_add(shard, op);
                    break;
                case CommandType::Cancel:
                    apply_cancel(shard, op);
                    break;
                case CommandType::Modify:
                    apply_modify(shard, op);
                    break;
            }
            ++applied;
        }
        if (applied != 0) {
            // One update of the shared counter per batch, not per command.
            inflight_.fetch_sub(applied, std::memory_order_release);
            continue;
        }
        // stop() sets this only once every accepted command was applied.
        if (shard.stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        std::this_thread::yield();
    }
}

}  // namespace lob::engine
