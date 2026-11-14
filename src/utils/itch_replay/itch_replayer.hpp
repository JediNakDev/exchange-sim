#ifndef EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP
#define EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP

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
#include <book/order_book.hpp>
#include <protocol/itch50.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace exsim::itch_replay {

using book::OrderBook;
using book::OrderId;
using book::Price;
using book::Quantity;
using book::Side;

// Wire size of the ITCH 5.0 messages that change the book (A, F, E, X, D,
// U), or 0 for any other type byte.
[[nodiscard]] inline std::size_t book_message_size(char type) noexcept {
    namespace itch50 = protocol::itch50;
    switch (type) {
    case itch50::AddOrder::type: return itch50::AddOrder::wire_size;
    case itch50::AddOrderMPID::type: return itch50::AddOrderMPID::wire_size;
    case itch50::OrderExecuted::type: return itch50::OrderExecuted::wire_size;
    case itch50::OrderCancel::type: return itch50::OrderCancel::wire_size;
    case itch50::OrderDelete::type: return itch50::OrderDelete::wire_size;
    case itch50::OrderReplace::type: return itch50::OrderReplace::wire_size;
    default: return 0;
    }
}

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

    // Decodes one ITCH 5.0 message (exactly book_message_size(type) bytes)
    // and applies it. Returns false for a message that does not decode.
    bool apply(std::span<const char> message) {
        namespace itch50 = protocol::itch50;
        switch (message[0]) {
        case itch50::AddOrder::type: {
            itch50::AddOrder m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            do_add(m.order_reference, m.side, m.price, m.shares);
            ++stats_.adds;
            return true;
        }
        case itch50::AddOrderMPID::type: {
            itch50::AddOrderMPID m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            do_add(m.order_reference, m.side, m.price, m.shares);
            ++stats_.adds;
            return true;
        }
        case itch50::OrderExecuted::type: {
            itch50::OrderExecuted m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            reduce(m.order_reference, m.shares);
            ++stats_.executes;
            return true;
        }
        case itch50::OrderCancel::type: {
            itch50::OrderCancel m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            reduce(m.order_reference, m.shares);
            ++stats_.cancels;
            return true;
        }
        case itch50::OrderDelete::type: {
            itch50::OrderDelete m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            do_delete(m.order_reference);
            ++stats_.deletes;
            return true;
        }
        case itch50::OrderReplace::type: {
            itch50::OrderReplace m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            ++stats_.replaces;
            auto it = live_.find(m.original_reference);
            if (it == live_.end()) { ++stats_.unknown_ref; return true; }
            Side side = it->second.side;
            do_delete(m.original_reference);
            do_add(m.new_reference, side, m.price, m.shares);
            return true;
        }
        default:
            return false;
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

}  // namespace exsim::itch_replay

#endif
