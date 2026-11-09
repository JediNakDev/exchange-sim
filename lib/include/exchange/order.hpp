#ifndef LOB_ORDER_HPP
#define LOB_ORDER_HPP

#include "types.hpp"

namespace lob {

class PriceLevel;

// One cache line per resting order: a fill touches a single line.
struct alignas(64) Order {
    OrderId id;
    Price price;
    Quantity quantity;
    Quantity remaining_quantity;
    Order* prev_order;
    Order* next_order;
    PriceLevel* parent_level;
    Side side;
    // Optional owner metadata slot; the book never interprets it.
    std::uint32_t context_index = 0;

    Order(OrderId id_, Price price_, Quantity quantity_, Side side_) noexcept
        : id(id_)
        , price(price_)
        , quantity(quantity_)
        , remaining_quantity(quantity_)
        , prev_order(nullptr)
        , next_order(nullptr)
        , parent_level(nullptr)
        , side(side_)
    {}

    [[nodiscard]] bool is_filled() const noexcept { 
        return remaining_quantity == 0; 
    }

    void fill(Quantity qty) noexcept {
        remaining_quantity = (qty >= remaining_quantity) ? 0 : remaining_quantity - qty;
    }
};

static_assert(sizeof(Order) == 64, "Order must stay exactly one cache line");

}

#endif
