#ifndef EXSIM_MATCHING_ENGINE_HPP
#define EXSIM_MATCHING_ENGINE_HPP

// Multi-instrument matching engine. Pure and single-threaded: no I/O, no
// clocks, no threads. Every input is a Command, every output is an Event
// appended to a caller-owned EventBuffer, so a journaled command stream
// replays to a bit-identical event stream.
//
// Hot-path guarantee: after construction, handle() performs no heap
// allocation (orders, price levels and id maps are all pre-sized from
// EngineConfig). When capacity is exhausted orders are rejected with
// RejectReason::CapacityExceeded rather than growing.
//
// Semantics are specified in docs/SPEC.md.

#include "command.hpp"
#include "event.hpp"
#include "instrument.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

namespace exsim {

struct EngineConfig {
    std::size_t max_instruments = 64;
    std::size_t max_orders = 1u << 20;  // live (resting) orders across all instruments
};

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
    std::uint64_t imbalance_qty = 0;
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
    // Returns the new InstrumentId (1, 2, ...) or 0 if the config is invalid
    // or max_instruments is reached.
    [[nodiscard]] InstrumentId add_instrument(const InstrumentConfig& config);

    // ---- Hot path -------------------------------------------------------
    void handle(const NewOrder& cmd, EventBuffer& out);
    void handle(const CancelOrder& cmd, EventBuffer& out);
    void handle(const ReplaceOrder& cmd, EventBuffer& out);
    void handle(const MassCancel& cmd, EventBuffer& out);
    void handle(const SetPhase& cmd, EventBuffer& out);
    void handle(const Command& cmd, EventBuffer& out);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Implementation hook for for_each_order (ascending OrderId).
    void visit_orders(void (*fn)(const OrderInfo&, void*), void* ctx) const;
};

template <typename F>
void MatchingEngine::for_each_order(F&& f) const {
    visit_orders(
        [](const OrderInfo& o, void* ctx) { (*static_cast<std::remove_reference_t<F>*>(ctx))(o); },
        const_cast<void*>(static_cast<const void*>(&f)));
}

}  // namespace exsim

#endif
