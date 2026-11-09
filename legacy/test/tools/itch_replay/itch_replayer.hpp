#ifndef LOB_TOOLS_ITCH_REPLAYER_HPP
#define LOB_TOOLS_ITCH_REPLAYER_HPP

// Drives the production OrderBook from an ITCH 5.0 message stream.
//
// ITCH messages represent post-matching state published by the exchange:
// 'A'/'F' adds are always non-marketable (anything marketable would have
// been executed by NASDAQ before being broadcast). 'E'/'X' reduce the
// resting size; 'D' deletes the order; 'U' replaces atomically. We
// maintain an exchange-ref → (internal_id, side, qty) map so the public
// OrderBook API (which assigns its own ids) can be driven from external
// refs.

#include "reference_book.hpp"
#include <lob/order_book.hpp>
#include <lob/protocol/itch.hpp>
#include <cstdint>
#include <unordered_map>

namespace lob::tools {

struct ReplayStats {
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t unknown_ref = 0;
    std::uint64_t dropped_cross = 0;
    std::uint64_t total() const noexcept {
        return adds + executes + cancels + deletes + replaces;
    }
};

class ItchReplayer {
public:
    explicit ItchReplayer(OrderBook& book, ReferenceBook* validate = nullptr)
        : book_(book), reference_(validate) {}

    void apply(const itch::Message& m) {
        switch (m.type) {
        case itch::MessageType::AddOrder:
        case itch::MessageType::AddOrderMPID:
            do_add(m.add_order.order_ref, m.add_order.side,
                   m.add_order.price, m.add_order.shares);
            ++stats_.adds;
            break;
        case itch::MessageType::OrderExecuted:
            reduce(m.order_executed.order_ref, m.order_executed.executed_shares);
            ++stats_.executes;
            break;
        case itch::MessageType::OrderCancel:
            reduce(m.order_cancel.order_ref, m.order_cancel.cancelled_shares);
            ++stats_.cancels;
            break;
        case itch::MessageType::OrderDelete:
            do_delete(m.order_delete.order_ref);
            ++stats_.deletes;
            break;
        case itch::MessageType::OrderReplace: {
            ++stats_.replaces;
            auto it = live_.find(m.order_replace.original_order_ref);
            if (it == live_.end()) { ++stats_.unknown_ref; break; }
            Side side = it->second.side;
            do_delete(m.order_replace.original_order_ref);
            do_add(m.order_replace.new_order_ref, side,
                   m.order_replace.price, m.order_replace.shares);
            break;
        }
        }
    }

    ReplayStats const& stats() const noexcept { return stats_; }

private:
    struct Live { OrderId internal; Side side; Quantity qty; };

    void do_add(std::uint64_t ref, Side side, Price price, Quantity qty) {
        auto res = book_.add_order(price, qty, side);
        if (!res.fills.empty()) ++stats_.dropped_cross;
        // ITCH adds should not cross; if they do, the remaining is what
        // survived. We track only the surviving portion.
        if (res.remaining_quantity > 0) {
            live_[ref] = {res.order_id, side, res.remaining_quantity};
            if (reference_) reference_->add(ref, side, price, qty);
        }
    }

    void reduce(std::uint64_t ref, Quantity shares) {
        auto it = live_.find(ref);
        if (it == live_.end()) { ++stats_.unknown_ref; return; }
        if (shares >= it->second.qty) {
            (void)book_.cancel_order(it->second.internal);
            live_.erase(it);
            if (reference_) reference_->erase(ref);
        } else {
            it->second.qty -= shares;
            (void)book_.modify_order(it->second.internal, it->second.qty);
            if (reference_) reference_->reduce(ref, shares);
        }
    }

    void do_delete(std::uint64_t ref) {
        auto it = live_.find(ref);
        if (it == live_.end()) { ++stats_.unknown_ref; return; }
        (void)book_.cancel_order(it->second.internal);
        live_.erase(it);
        if (reference_) reference_->erase(ref);
    }

    OrderBook&     book_;
    ReferenceBook* reference_;
    std::unordered_map<std::uint64_t, Live> live_;
    ReplayStats stats_;
};

}  // namespace lob::tools

#endif
