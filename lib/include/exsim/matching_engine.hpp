#ifndef EXSIM_MATCHING_ENGINE_HPP
#define EXSIM_MATCHING_ENGINE_HPP

// Multi-instrument matching engine. Pure and single-threaded: no I/O, no
// clocks, no threads. Every input is a Command, every output is an Event
// appended to a caller-owned EventBuffer, so a journaled command stream
// replays to identical ordered active field values (never compare padding).
//
// Hot-path guarantee: after construction, handle() performs no heap
// allocation (orders, price levels and id maps are all pre-sized from
// EngineConfig). When capacity is exhausted orders are rejected with
// RejectReason::CapacityExceeded rather than growing.
//
// Setup cost: the constructor and add_instrument allocate and zero-fill all
// of their storage up front. If it does not fit they throw std::bad_alloc or
// std::length_error rather than returning, so size max_orders and price bands
// for physical memory, including the original LOB pools and per-instrument reservations.
//
// Semantics are specified in matching engine contract.

#include "command.hpp"
#include "event.hpp"
#include "instrument.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace exsim {

struct EngineConfig {
    std::size_t max_instruments = 64;
    std::size_t max_orders = 1u << 20;  // live (resting) orders across all instruments
};

// Conservative bound for ANY command, including all-instrument phase changes.
// Setup-time validation also bounds aggregate quantities and count fields.
[[nodiscard]] inline std::size_t required_event_capacity(const EngineConfig& c) {
    if (c.max_instruments == 0 || c.max_instruments > std::numeric_limits<InstrumentId>::max() ||
        c.max_orders == 0 || c.max_orders > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("EngineConfig bounds");
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (c.max_orders > (limit - 4) / 2 ||
        c.max_instruments > (limit - 4 - 2 * c.max_orders) / 2)
        throw std::length_error("event capacity");
    const auto count = 2 * c.max_orders + 2 * c.max_instruments + 4;
    if (count > limit / sizeof(Event)) throw std::length_error("event storage");
    return count;
}

enum class HandleResult { Applied, InsufficientOutput };

struct LevelInfo {
    Price price;
    std::uint64_t qty;
    std::uint32_t order_count;
};

struct OrderInfo {
    OrderId order_id;
    ClOrdId cl_ord_id;
    InstrumentId instrument;
    SessionId session;
    OwnerId owner;
    Side side;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
    Price price;
    Quantity qty;
    Quantity leaves_qty;
    Quantity cum_qty;
};

// Indicative auction state during a call phase (feeds ITCH NOII 'I').
struct AuctionIndicative {
    bool crossed = false;           // false => no executable volume
    Price price = 0;                // indicative uncross price
    std::uint64_t paired_qty = 0;   // V(p*)
    std::uint64_t imbalance_qty = 0; // at price if crossed; total side difference otherwise
    Aggressor imbalance_side = Aggressor::None;  // Buy/Sell surplus, None if balanced
};

class MatchingEngine {
public:
    explicit MatchingEngine(const EngineConfig& config = {});
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    // Setup-time only (allocates the instrument's price ladders).
    // Returns the new InstrumentId (1, 2, ...) or 0 if the config is invalid,
    // the band exceeds the engine's tick limit, the symbol is taken or
    // max_instruments is reached. Throws if the ladders cannot be allocated.
    [[nodiscard]] InstrumentId add_instrument(const InstrumentConfig& config);

    // Before ANY mutation (including counters), require remaining output >=
    // required_event_capacity(config). Otherwise return InsufficientOutput with
    // engine AND output unchanged. Applied includes semantic rejection events.
    // Implementations may use a tighter proven bound from a read-only preflight.
    // ---- Hot path -------------------------------------------------------
    [[nodiscard]] HandleResult handle(const NewOrder& cmd, EventBuffer& out);
    [[nodiscard]] HandleResult handle(const CancelOrder& cmd, EventBuffer& out);
    [[nodiscard]] HandleResult handle(const ReplaceOrder& cmd, EventBuffer& out);
    [[nodiscard]] HandleResult handle(const MassCancel& cmd, EventBuffer& out);
    [[nodiscard]] HandleResult handle(const SetPhase& cmd, EventBuffer& out);
    [[nodiscard]] HandleResult handle(const Command& cmd, EventBuffer& out);

    // ---- Queries (not latency critical, never allocate) ------------------
    [[nodiscard]] std::size_t instrument_count() const noexcept;
    [[nodiscard]] const InstrumentConfig* instrument(InstrumentId id) const noexcept;
    [[nodiscard]] InstrumentId find_instrument(const Symbol& symbol) const noexcept;  // 0 if unknown
    [[nodiscard]] Phase phase(InstrumentId id) const noexcept;
    [[nodiscard]] std::optional<Price> best_bid(InstrumentId id) const noexcept;
    [[nodiscard]] std::optional<Price> best_ask(InstrumentId id) const noexcept;
    [[nodiscard]] std::optional<Price> last_trade_price(InstrumentId id) const noexcept;

    // Writes up to `max_levels` levels of `side` in priority order (best
    // first) into `out`; returns the number written.
    std::size_t depth(InstrumentId id, Side side, LevelInfo* out, std::size_t max_levels) const noexcept;

    // Writes the OrderIds resting at (side, price) in FIFO order; returns
    // the number written (at most `max_orders`).
    std::size_t level_orders(InstrumentId id, Side side, Price price,
                             OrderId* out, std::size_t max_orders) const noexcept;

    [[nodiscard]] std::optional<OrderInfo> find_order(SessionId session, ClOrdId cl_ord_id) const noexcept;
    [[nodiscard]] std::optional<OrderInfo> find_order(OrderId order_id) const noexcept;
    [[nodiscard]] std::size_t live_order_count() const noexcept;

    [[nodiscard]] AuctionIndicative auction_indicative(InstrumentId id) const noexcept;

    // Visits every live order of every instrument in ascending OrderId order.
    // Allocation-free; intended for snapshots, journaling and tests.
    template <typename F>
    void for_each_order(F&& f) const;

    // Visits the orders resting at (side, price) of instrument `id` in FIFO
    // order. Visits none for an unknown instrument, an invalid side, a price
    // off the instrument's grid or band, or an empty level. Allocation-free;
    // `f` must not modify the engine.
    template <typename F>
    void for_each_level_order(InstrumentId id, Side side, Price price, F&& f) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Implementation hook for for_each_order (ascending OrderId).
    void visit_orders(void (*fn)(const OrderInfo&, void*), void* ctx) const;
    // Implementation hook for for_each_level_order (FIFO).
    void visit_level_orders(InstrumentId id, Side side, Price price,
                            void (*fn)(const OrderInfo&, void*), void* ctx) const;
};

template <typename F>
void MatchingEngine::for_each_order(F&& f) const {
    auto callback = [&](const OrderInfo& o) { f(o); };
    visit_orders(
        [](const OrderInfo& o, void* ctx) { (*static_cast<decltype(callback)*>(ctx))(o); },
        &callback);
}

template <typename F>
void MatchingEngine::for_each_level_order(InstrumentId id, Side side, Price price, F&& f) const {
    auto callback = [&](const OrderInfo& o) { f(o); };
    visit_level_orders(
        id, side, price,
        [](const OrderInfo& o, void* ctx) { (*static_cast<decltype(callback)*>(ctx))(o); },
        &callback);
}

}  // namespace exsim

#endif
