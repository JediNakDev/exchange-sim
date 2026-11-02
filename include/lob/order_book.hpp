#ifndef LOB_ORDER_BOOK_HPP
#define LOB_ORDER_BOOK_HPP

#include "price_level.hpp"
#include "flat_hash_map.hpp"
#include "object_pool.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lob {

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
 * - Add order (existing level): O(1)
 * - Add order (new level): O(1) amortized
 * - Cancel order: O(1)
 * - Execute order: O(1)
 * - GetBestBid/Ask: O(1)
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
        // was reached, or the book had no capacity to rest the remainder.
        Quantity cancelled_quantity = 0;
        // True when matching stopped at max_fills_per_add_order while the
        // order still crossed resting liquidity.
        bool fill_limit_reached = false;
    };

private:
    // Order storage - keyed by order ID for O(1) lookup.
    FlatHashMap<Order*> orders_;

    // Tick-indexed ladders (cache-friendly contiguous structures).
    std::vector<PriceLevel*> bid_ladder_;
    std::vector<PriceLevel*> ask_ladder_;
    std::vector<std::uint64_t> bid_active_words_;
    std::vector<std::uint64_t> ask_active_words_;
    Price min_price_;
    Price max_price_;
    bool ladder_initialized_;
    
    // Cached best prices for O(1) access
    PriceLevel* highest_buy_;   // Best bid (max price in buy tree)
    PriceLevel* lowest_sell_;   // Best ask (min price in sell tree)

    ObjectPool<Order> order_pool_;
    ObjectPool<PriceLevel> level_pool_;

    // Reserved to max_fills_per_add_order_ at construction and never grown.
    std::vector<Fill> fill_buffer_;
    std::size_t max_fills_per_add_order_;

    OrderId next_order_id_;

    void refresh_best_levels() noexcept;
    void initialize_ladders(Price min_price, Price max_price);
    void ensure_price_range(Price price);
    [[nodiscard]] std::size_t ladder_index(Price price) const noexcept;
    void set_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept;
    void clear_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept;
    [[nodiscard]] std::size_t word_count() const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_prev_active(
        const std::vector<std::uint64_t>& words,
        std::size_t from_idx) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_next_active(
        const std::vector<std::uint64_t>& words,
        std::size_t from_idx) const noexcept;

    // Order book operations — templatized on Side to eliminate branches in inner loops
    // Returns true when matching stopped at the fill limit while `incoming`
    // still crossed the contra side.
    template<Side S> [[nodiscard]] bool match_order_impl(Order* incoming);
    // Returns false when the order could not be placed (no level capacity).
    template<Side S> [[nodiscard]] bool add_order_to_book_impl(Order* order);
    template<Side S> void remove_order_from_book_impl(Order* order);
    void clear();

public:
    OrderBook();
    explicit OrderBook(const OrderBookConfig& config);
    ~OrderBook();
    
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] AddResult add_order(Price price, Quantity quantity, Side side);
    [[nodiscard]] bool cancel_order(OrderId order_id);
    // Sets the order's total quantity. If that leaves nothing to execute
    // (new_quantity <= already filled) the order is removed from the book.
    [[nodiscard]] bool modify_order(OrderId order_id, Quantity new_quantity);

    // True while `order_id` is resting on the book (not filled or cancelled).
    [[nodiscard]] bool has_order(OrderId order_id) const noexcept { return orders_.contains(order_id); }
    [[nodiscard]] std::size_t max_fills_per_add_order() const noexcept { return max_fills_per_add_order_; }

    [[nodiscard]] std::optional<Price> get_best_bid() const;
    [[nodiscard]] std::optional<Price> get_best_ask() const;
    [[nodiscard]] std::optional<Price> get_spread() const;
    [[nodiscard]] std::optional<Price> get_mid_price() const;

    [[nodiscard]] Quantity get_bid_quantity_at_top() const;
    [[nodiscard]] Quantity get_ask_quantity_at_top() const;

    [[nodiscard]] size_t get_bid_levels() const noexcept;
    [[nodiscard]] size_t get_ask_levels() const noexcept;
    [[nodiscard]] size_t get_total_orders() const noexcept { return orders_.size(); }

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
