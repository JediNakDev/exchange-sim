#ifndef EXSIM_BOOK_PRICE_LEVEL_HPP
#define EXSIM_BOOK_PRICE_LEVEL_HPP

#include "book/order.hpp"
#include <cstddef>

namespace exsim::book {

/**
 * PriceLevel (Limit) - represents a single limit price in the order book.
 * 
 * Structure:
 * - Compact aggregate fields (price/volume/count)
 * - Doubly linked list of orders (headOrder/tailOrder) for O(1) order operations
 * - Indexed by tick ladder in OrderBook for cache-friendly lookup
 * 
 * Performance:
 * - Add order to existing level: O(1)
 * - Add first order at new level: O(1) amortized with ladder expansion
 * - Cancel order: O(1)
 * - Execute order: O(1)
 * - GetVolumeAtLimit: O(1)
 */
class PriceLevel {
public:
    // Price and aggregate data
    Price price;
    Quantity total_volume;      // Total quantity at this price level

    // Doubly linked list of orders at this price
    Order* head_order;
    Order* tail_order;

    explicit PriceLevel(Price price_) noexcept
        : price(price_)
        , total_volume(0)
        , head_order(nullptr)
        , tail_order(nullptr)
        , order_count_(0)
    {}

    // Add order to tail of the order list - O(1)
    void add_order(Order* order) noexcept {
        order->parent_level = this;
        order->prev_order = tail_order;
        order->next_order = nullptr;
        
        if (tail_order) {
            tail_order->next_order = order;
        } else {
            head_order = order;
        }
        tail_order = order;
        
        total_volume += order->remaining_quantity;
        ++order_count_;
    }

    // Remove order from list - O(1)
    void remove_order(Order* order) noexcept {
        if (order->prev_order) {
            order->prev_order->next_order = order->next_order;
        } else {
            head_order = order->next_order;
        }
        
        if (order->next_order) {
            order->next_order->prev_order = order->prev_order;
        } else {
            tail_order = order->prev_order;
        }
        
        total_volume -= order->remaining_quantity;
        --order_count_;
        
        order->prev_order = nullptr;
        order->next_order = nullptr;
        order->parent_level = nullptr;
    }

    [[nodiscard]] Order* front() const noexcept {
        return head_order;
    }

    // Pop front order - O(1) for execute operations
    void pop_front() noexcept {
        if (head_order) {
            Order* old_head = head_order;
            total_volume -= old_head->remaining_quantity;
            --order_count_;
            
            head_order = old_head->next_order;
            if (head_order) {
                head_order->prev_order = nullptr;
            } else {
                tail_order = nullptr;
            }
            
            old_head->prev_order = nullptr;
            old_head->next_order = nullptr;
            old_head->parent_level = nullptr;
        }
    }

    [[nodiscard]] bool is_empty() const noexcept { 
        return head_order == nullptr; 
    }

    [[nodiscard]] size_t order_count() const noexcept { 
        return order_count_; 
    }

private:
    size_t order_count_;        // Number of orders at this level
};

}

#endif
