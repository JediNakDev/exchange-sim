#ifndef EXSIM_BOOK_ORDER_BOOK_HPP
#define EXSIM_BOOK_ORDER_BOOK_HPP

#include "book/active_bitmap.hpp"
#include "book/price_level.hpp"
#include "util/flat_hash_map.hpp"
#include "util/object_pool.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace exsim::book {

#ifdef EXSIM_BOOK_DETERMINISTIC_POOL
inline constexpr bool kOrderBookGrowsByDefault = false;
#else
inline constexpr bool kOrderBookGrowsByDefault = true;
#endif

// Setup-time sizing for one OrderBook. Everything is allocated in the
// constructor; after that the matching path never grows the fill buffer.
struct OrderBookConfig {
    std::size_t max_resting_orders = std::size_t{1} << 16;
    std::size_t max_price_levels = std::size_t{1} << 14;
    Price min_price = -100000;
    Price max_price = 100000;
    // Upper bound on fills one add_order call may produce. When an aggressive
    // order still crosses after this many fills, matching stops and the
    // unexecuted remainder is cancelled (it never rests, so the book cannot
    // become crossed). AddResult::fill_limit_reached reports it. Values
    // below 1 are treated as 1.
    std::size_t max_fills_per_add_order = 1024;
    // false: the limits above are hard. add_order never allocates, rejects
    // prices outside [min_price, max_price] and cancels a remainder that
    // finds no free resting-order or price-level slot.
    // true: the limits are initial sizes. Pools, the id map and the ladder
    // grow on demand (heap allocation inside add_order); a failed growth
    // throws and leaves the book unchanged. Replaying external data relies
    // on this; EXSIM_BOOK_DETERMINISTIC_POOL builds default to false.
    bool allow_growth = kOrderBookGrowsByDefault;
};

// Resting-order pool, price-level pool and OrderId map. A book built from a
// config owns its own; books built on one shared storage draw from a single
// capacity, so memory is bounded by the total order limit, not by limit x
// books. Order IDs must then be unique across those books.
struct OrderBookStorage {
    util::FlatHashMap<Order*> orders;
    util::ObjectPool<Order> order_pool;
    util::ObjectPool<PriceLevel> level_pool;

    OrderBookStorage(std::size_t max_resting_orders, std::size_t max_price_levels, bool allow_growth);
};

/**
 * OrderBook - cache-friendly ladder-based order book.
 * 
 * Structure:
 * - Tick-indexed ladders for buy and sell levels
 * - Open-addressing hash map for O(1) order lookup by order ID
 * - Cached pointers to best bid (highest_buy_) and best ask (lowest_sell_)
 * 
 * Performance:
 * - Add order, cancel order, execute order: O(1), plus an O(log64 P)
 *   occupancy search when the best level empties (P = ticks in the band)
 * - GetBestBid/Ask, level counts, side totals: O(1)
 * - GetVolumeAtLimit: O(1)
 */
class OrderBook {
public:
    // `fills` is a view into the book's reusable fill buffer: it stays valid
    // until the next mutating call on this book. Copy it if you need it
    // longer. (Returning a vector would allocate on every aggressive order.)
    // It holds every fill the call executed; it is never silently truncated.
    struct AddResult {
        OrderId order_id;
        std::span<const Fill> fills;
        // Quantity left resting on the book under `order_id`.
        Quantity remaining_quantity;
        // Unexecuted quantity discarded instead of resting: the fill limit
        // was reached, or a bounded book had no resting-order or price-level
        // slot left for the remainder.
        Quantity cancelled_quantity = 0;
        // True when matching stopped at max_fills_per_add_order while the
        // order still crossed resting liquidity.
        bool fill_limit_reached = false;
    };

private:
    // Pools and the OrderId map, owned or shared (see OrderBookStorage).
    std::shared_ptr<OrderBookStorage> storage_;
    util::FlatHashMap<Order*>& orders_;
    util::ObjectPool<Order>& order_pool_;
    util::ObjectPool<PriceLevel>& level_pool_;

    // Tick-indexed ladders with one occupancy bit per tick.
    std::vector<PriceLevel*> bid_ladder_;
    std::vector<PriceLevel*> ask_ladder_;
    ActiveBitmap bid_active_levels_;
    ActiveBitmap ask_active_levels_;
    Price min_price_;
    Price max_price_;

    // Cached best prices for O(1) access
    PriceLevel* highest_buy_;   // Best bid (max price in buy tree)
    PriceLevel* lowest_sell_;   // Best ask (min price in sell tree)

    // Orders resting on this book and their leaves per side (BUY, SELL).
    std::size_t order_count_ = 0;
    Quantity leaves_by_side_[2] = {0, 0};

    // Reserved to max_fills_per_add_order_ at construction and never grown.
    std::vector<Fill> fill_buffer_;
    std::size_t max_fills_per_add_order_;
    bool allow_growth_;

    OrderId next_order_id_;

    // False when `price` is outside the ladder and the ladder may not grow.
    [[nodiscard]] bool ensure_price_range(Price price);
    [[nodiscard]] std::size_t ladder_index(Price price) const noexcept;
    // True when `order` rests on this book rather than on another book that
    // shares the storage.
    [[nodiscard]] bool rests_here(const Order& order) const noexcept;
    // Returns the order with `id` if it rests on this book.
    [[nodiscard]] Order* find_order_here(OrderId id) const noexcept;

    // Order book operations, templated on Side to remove branches in inner loops
    // Returns true when matching stopped at the fill limit while `incoming`
    // still crossed the contra side.
    template<Side S> [[nodiscard]] bool match_order_impl(Order& incoming);
    // Returns false when the order could not be placed (no level capacity).
    template<Side S> [[nodiscard]] bool add_order_to_book_impl(Order* order);
    template<Side S> void remove_order_from_book_impl(Order* order);

public:
    OrderBook();
    explicit OrderBook(const OrderBookConfig& config);
    // Draws orders, levels and IDs from `storage`; the config's
    // max_resting_orders and max_price_levels are not used.
    OrderBook(const OrderBookConfig& config, std::shared_ptr<OrderBookStorage> storage);
    ~OrderBook() = default;
    
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Passive storage operations for exchange rules (auctions, STP, IOC/FOK)
    // implemented by the owning exchange. These never run implicit matching.
    // A bounded book allocates only at construction. Zero leaves are retained
    // until remove_order or cancel_order so the owner can emit its final
    // execution report.
    [[nodiscard]] Order* rest_order(OrderId id, Price price, Quantity quantity,
                                    Quantity leaves, Side side, std::uint32_t context_index);
    // Pointer operations on an order returned by rest_order: no ID lookup.
    // Executes `quantity` (at most the order's leaves) against `order`.
    void execute_order(Order& order, Quantity quantity) noexcept;
    // Sets total quantity and leaves in place, keeping FIFO position. Fails
    // without change when leaves would grow or exceed total.
    [[nodiscard]] bool amend_order(Order& order, Quantity total, Quantity leaves) noexcept;
    void remove_order(Order& order) noexcept;
    [[nodiscard]] const Order* find_order(OrderId id) const noexcept;
    [[nodiscard]] const PriceLevel* price_level(Side side, Price price) const noexcept;
    [[nodiscard]] std::optional<Price> next_price(Side side, Price from, bool ascending) const noexcept;
    [[nodiscard]] std::uint64_t active_word(Side side, std::size_t word) const noexcept;

    [[nodiscard]] AddResult add_order(Price price, Quantity quantity, Side side);
    [[nodiscard]] bool cancel_order(OrderId order_id);
    // Sets the order's total quantity. If that leaves nothing to execute
    // (new_quantity <= already filled) the order is removed from the book.
    [[nodiscard]] bool modify_order(OrderId order_id, Quantity new_quantity);

    // True while `order_id` is resting on the book (not filled or cancelled).
    [[nodiscard]] bool has_order(OrderId order_id) const noexcept { return find_order_here(order_id) != nullptr; }
    [[nodiscard]] std::size_t max_fills_per_add_order() const noexcept { return max_fills_per_add_order_; }

    [[nodiscard]] std::optional<Price> get_best_bid() const;
    [[nodiscard]] std::optional<Price> get_best_ask() const;
    [[nodiscard]] std::optional<Price> get_spread() const;
    [[nodiscard]] std::optional<Price> get_mid_price() const;

    [[nodiscard]] Quantity get_bid_quantity_at_top() const;
    [[nodiscard]] Quantity get_ask_quantity_at_top() const;

    [[nodiscard]] size_t get_bid_levels() const noexcept { return bid_active_levels_.count(); }
    [[nodiscard]] size_t get_ask_levels() const noexcept { return ask_active_levels_.count(); }
    [[nodiscard]] size_t get_total_orders() const noexcept { return order_count_; }
    // Sum of leaves over the resting orders of one side. Exact while that sum
    // fits Quantity (always for the engine's 32-bit quantities).
    [[nodiscard]] Quantity get_total_leaves(Side side) const noexcept {
        return leaves_by_side_[static_cast<std::size_t>(side)];
    }

    struct BookSnapshot {
        struct Level {
            Price price;
            Quantity quantity;
            size_t order_count;
        };
        std::vector<Level> bids;
        std::vector<Level> asks;
    };
    
    [[nodiscard]] BookSnapshot get_snapshot(size_t depth = 5) const;
};

}

#endif
