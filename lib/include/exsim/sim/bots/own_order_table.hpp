#ifndef EXSIM_SIM_BOTS_OWN_ORDER_TABLE_HPP
#define EXSIM_SIM_BOTS_OWN_ORDER_TABLE_HPP

// Fixed-capacity table of one bot's own orders, keyed by the bot's ClOrdId.
//
// Bots bypass the wire gateways and put engine ClOrdIds straight into their
// commands, so they choose the ids themselves. The table relies on that: an
// order lives in slot `cl_ord_id % capacity`, and allocate() hands out the
// next increasing id whose slot is free. Lookup is one compare, erase just
// clears the slot (nothing moves, so iterating by slot index stays valid),
// and ids are never reused, which keeps them unique among live orders. Ids
// run 1, 2, 3, ... with an occasional gap when a long-lived order still
// occupies the natural slot of the next id.

#include "exsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace exsim::sim::bots {

// The command currently in flight for an order (sent, its events not yet seen).
enum class InFlightRequest : std::uint8_t { None = 0, New = 1, Replace = 2, Cancel = 3 };

struct OwnOrder {
    ClOrdId cl_ord_id = 0;               // 0 = free slot
    OrderId order_id = 0;                // 0 until Accepted
    Timestamp expires_at = 0;            // when the bot cancels it itself; 0 = never
    Price price = 0;                     // engine-confirmed price (the requested one while the New is in flight)
    Price requested_price = 0;           // price of the in-flight Replace
    Quantity leaves_qty = 0;             // engine-confirmed leaves (the requested qty while the New is in flight)
    Quantity requested_leaves_qty = 0;   // leaves the in-flight Replace asks for
    Quantity cum_qty = 0;
    Side side = Side::BUY;
    InFlightRequest in_flight = InFlightRequest::None;

    // What the engine holds right now. submit() is synchronous, so an
    // in-flight Replace has been processed by the engine, but may have been
    // rejected. Callers must also protect the confirmed price until acknowledgment.
    [[nodiscard]] Price working_price() const noexcept {
        return in_flight == InFlightRequest::Replace ? requested_price : price;
    }
    [[nodiscard]] Quantity working_leaves_qty() const noexcept {
        return in_flight == InFlightRequest::Replace ? requested_leaves_qty : leaves_qty;
    }
};

class OwnOrderTable {
public:
    // Capacity is rounded up to a power of two, bounded to [8, 65536].
    explicit OwnOrderTable(std::size_t min_capacity) : capacity_(round_up_capacity(min_capacity)) {
        slots_ = std::make_unique<OwnOrder[]>(capacity_);
    }

    // Claims a slot for a new order and gives it the next free ClOrdId.
    // nullptr when every slot is taken (the caller skips the order).
    [[nodiscard]] OwnOrder* allocate() noexcept {
        if (size_ == capacity_) {
            return nullptr;
        }
        // At least one slot is free and consecutive ids visit every slot, so
        // this loop ends within `capacity_` iterations.
        while (next_cl_ord_id_ <= kMaxClOrdId) {
            const ClOrdId cl_ord_id = next_cl_ord_id_++;
            OwnOrder& candidate = slots_[slot_index(cl_ord_id)];
            if (candidate.cl_ord_id == 0) {
                candidate = OwnOrder{};
                candidate.cl_ord_id = cl_ord_id;
                ++size_;
                return &candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] OwnOrder* find(ClOrdId cl_ord_id) noexcept {
        OwnOrder& candidate = slots_[slot_index(cl_ord_id)];
        return cl_ord_id != 0 && candidate.cl_ord_id == cl_ord_id ? &candidate : nullptr;
    }
    [[nodiscard]] const OwnOrder* find(ClOrdId cl_ord_id) const noexcept {
        const OwnOrder& candidate = slots_[slot_index(cl_ord_id)];
        return cl_ord_id != 0 && candidate.cl_ord_id == cl_ord_id ? &candidate : nullptr;
    }

    void erase(OwnOrder& order) noexcept {
        order = OwnOrder{};
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] ClOrdId next_cl_ord_id() const noexcept { return next_cl_ord_id_; }

    // Slot access for iteration in slot order; a free slot has cl_ord_id == 0.
    [[nodiscard]] OwnOrder& slot(std::size_t index) noexcept { return slots_[index]; }
    [[nodiscard]] const OwnOrder& slot(std::size_t index) const noexcept { return slots_[index]; }

    // Calls visitor(const OwnOrder&) for every live entry, in slot order.
    template <typename Visitor>
    void for_each(Visitor&& visitor) const {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (slots_[i].cl_ord_id != 0) {
                visitor(slots_[i]);
            }
        }
    }

private:
    static std::size_t round_up_capacity(std::size_t min_capacity) noexcept {
        // Bound setup memory and prevent power-of-two overflow for malformed direct configs.
        if (min_capacity > 65536) min_capacity = 65536;
        std::size_t capacity = 8;
        while (capacity < min_capacity) {
            capacity <<= 1;
        }
        return capacity;
    }

    [[nodiscard]] std::size_t slot_index(ClOrdId cl_ord_id) const noexcept {
        return static_cast<std::size_t>(cl_ord_id) & (capacity_ - 1);
    }

    std::size_t capacity_;
    std::unique_ptr<OwnOrder[]> slots_;
    std::size_t size_ = 0;
    ClOrdId next_cl_ord_id_ = 1;
};

}  // namespace exsim::sim::bots

#endif
