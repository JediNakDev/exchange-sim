#ifndef LOB_ORDER_BOOK_HPP
#define LOB_ORDER_BOOK_HPP

#include "price_level.hpp"
#include "flat_hash_map.hpp"
#include "object_pool.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lob {

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
    struct AddResult {
        OrderId order_id;
        std::span<const Fill> fills;
        Quantity remaining_quantity;
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

    std::vector<Fill> fill_buffer_;

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
    template<Side S> void match_order_impl(Order* incoming);
    template<Side S> void add_order_to_book_impl(Order* order);
    template<Side S> void remove_order_from_book_impl(Order* order);
    void clear();

public:
    OrderBook();
    ~OrderBook();
    
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] AddResult add_order(Price price, Quantity quantity, Side side);
    [[nodiscard]] bool cancel_order(OrderId order_id);
    [[nodiscard]] bool modify_order(OrderId order_id, Quantity new_quantity);

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
