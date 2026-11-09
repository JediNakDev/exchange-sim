#ifndef LOB_TOOLS_REFERENCE_BOOK_HPP
#define LOB_TOOLS_REFERENCE_BOOK_HPP

// Reference implementation of a price-time priority limit order book.
// Deliberately simple (std::map + std::list) so it's obviously correct.
// Used as the oracle when validating the production OrderBook against an
// ITCH 5.0 feed: both books are driven by the same event stream and their
// snapshots compared after every message.

#include <lob/types.hpp>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace lob::tools {

class ReferenceBook {
public:
    struct Order {
        OrderId  id;
        Price    price;
        Quantity quantity;
        Side     side;
    };
    struct Level {
        Price    price;
        Quantity total_qty;
        std::size_t order_count;
    };
    struct Snapshot {
        std::vector<Level> bids; // sorted high → low
        std::vector<Level> asks; // sorted low  → high
    };

    bool add(OrderId id, Side side, Price price, Quantity qty) {
        if (orders_.count(id)) return false;
        auto& ladder = (side == Side::BUY) ? bids_ : asks_;
        auto& queue  = ladder[price];
        queue.push_back({id, price, qty, side});
        auto it = std::prev(queue.end());
        orders_[id] = Loc{side, price, it};
        return true;
    }

    // ITCH 'E' / 'X': reduce remaining shares on a resting order.
    bool reduce(OrderId id, Quantity shares) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        auto& loc = it->second;
        auto& order = *loc.it;
        if (shares >= order.quantity) {
            return erase(id);
        }
        order.quantity -= shares;
        return true;
    }

    // ITCH 'D'
    bool erase(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        auto& loc = it->second;
        auto& ladder = (loc.side == Side::BUY) ? bids_ : asks_;
        auto lvl = ladder.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) ladder.erase(lvl);
        orders_.erase(it);
        return true;
    }

    // ITCH 'U': atomic delete-old + add-new with new ref/price/qty.
    bool replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty) {
        auto it = orders_.find(old_id);
        if (it == orders_.end()) return false;
        Side s = it->second.side;
        erase(old_id);
        return add(new_id, s, new_price, new_qty);
    }

    Snapshot top(std::size_t depth) const {
        Snapshot s;
        s.bids.reserve(depth);
        s.asks.reserve(depth);
        std::size_t n = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && n < depth; ++it, ++n) {
            s.bids.push_back(aggregate(it->first, it->second));
        }
        n = 0;
        for (auto it = asks_.begin(); it != asks_.end() && n < depth; ++it, ++n) {
            s.asks.push_back(aggregate(it->first, it->second));
        }
        return s;
    }

    std::size_t live_orders() const noexcept { return orders_.size(); }
    std::size_t bid_levels()  const noexcept { return bids_.size(); }
    std::size_t ask_levels()  const noexcept { return asks_.size(); }

private:
    struct Loc {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    static Level aggregate(Price p, const std::list<Order>& q) {
        Quantity total = 0;
        for (auto const& o : q) total += o.quantity;
        return {p, total, q.size()};
    }

    std::map<Price, std::list<Order>> bids_;
    std::map<Price, std::list<Order>> asks_;
    std::unordered_map<OrderId, Loc>  orders_;
};

}  // namespace lob::tools

#endif
