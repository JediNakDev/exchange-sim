#ifndef EXSIM_ENGINE_ORDER_POOL_HPP
#define EXSIM_ENGINE_ORDER_POOL_HPP

// Pre-sized order storage for the vector-ladder matching engine.
//
// Orders live in one contiguous array and are addressed by 32-bit slot
// indices instead of pointers: links are half the size of a pointer, so a
// whole order (book links included) fits one 64-byte cache line. Slot 0 is
// never handed out, which lets 0 serve as the null link everywhere.

#include <exsim/types.hpp>
#include <lob/compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>

namespace exsim::engine {

using OrderSlot = std::uint32_t;
inline constexpr OrderSlot kNullSlot = 0;

// A live order. Fields touched by matching come first.
struct alignas(64) Order {
    OrderSlot next_in_level;  // FIFO successor at the same price; free-list link when unused
    OrderSlot prev_in_level;  // FIFO predecessor at the same price
    Quantity leaves_qty;
    Quantity cum_qty;
    Price price;
    OrderId order_id;
    ClOrdId cl_ord_id;
    OwnerId owner;
    Quantity qty;
    SessionId session;
    InstrumentId instrument;
    Side side;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
    OrderSlot next_by_order_id;  // engine-wide list of live orders in ascending OrderId
    OrderSlot prev_by_order_id;
};
static_assert(sizeof(Order) == 64, "an order must fill exactly one cache line");

class OrderPool {
public:
    // Allocates and pre-faults every slot up front; allocate() never touches the heap.
    explicit OrderPool(std::size_t max_live_orders)
        : orders_(new Order[checked_slot_count(max_live_orders)]()), capacity_(max_live_orders) {
        for (std::size_t slot = 1; slot < max_live_orders; ++slot) {
            orders_[slot].next_in_level = static_cast<OrderSlot>(slot + 1);
        }
        free_head_ = max_live_orders == 0 ? kNullSlot : OrderSlot{1};
    }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    // Caller guarantees a free slot exists (live count < capacity).
    [[nodiscard]] OrderSlot allocate() noexcept {
        const OrderSlot slot = free_head_;
        free_head_ = orders_[slot].next_in_level;
        return slot;
    }

    void release(OrderSlot slot) noexcept {
        orders_[slot].next_in_level = free_head_;
        free_head_ = slot;
    }

    [[nodiscard]] Order& operator[](OrderSlot slot) noexcept { return orders_[slot]; }
    [[nodiscard]] const Order& operator[](OrderSlot slot) const noexcept { return orders_[slot]; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    static std::size_t checked_slot_count(std::size_t count) {
        if (count >= std::numeric_limits<std::size_t>::max() / sizeof(Order))
            throw std::length_error("order pool storage");
        return count + 1;
    }
    std::unique_ptr<Order[]> orders_;
    std::size_t capacity_ = 0;
    OrderSlot free_head_ = kNullSlot;
};

}  // namespace exsim::engine

#endif
