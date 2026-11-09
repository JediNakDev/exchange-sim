#ifndef LOB_TOOLS_DIFF_HPP
#define LOB_TOOLS_DIFF_HPP

// Snapshot-level diff between the production OrderBook and ReferenceBook.
// Returns true iff the top-`depth` of both books agree on price, total
// quantity, and order count at every level.

#include "reference_book.hpp"
#include <lob/order_book.hpp>
#include <sstream>
#include <string>

namespace lob::tools {

struct DiffResult {
    bool        ok = true;
    std::string detail;
};

inline DiffResult diff_books(const OrderBook& engine,
                             const ReferenceBook& ref,
                             std::size_t depth = 10) {
    auto e = engine.get_snapshot(depth);
    auto r = ref.top(depth);
    std::ostringstream err;

    auto cmp = [&](const char* side, auto const& engine_side, auto const& ref_side) {
        if (engine_side.size() != ref_side.size()) {
            err << side << " level count: engine=" << engine_side.size()
                << " ref=" << ref_side.size() << "; ";
            return false;
        }
        for (std::size_t i = 0; i < engine_side.size(); ++i) {
            if (engine_side[i].price       != ref_side[i].price ||
                engine_side[i].quantity    != ref_side[i].total_qty ||
                engine_side[i].order_count != ref_side[i].order_count) {
                err << side << "[" << i << "] mismatch: "
                    << "engine(p=" << engine_side[i].price
                    << " q=" << engine_side[i].quantity
                    << " n=" << engine_side[i].order_count << ") "
                    << "ref(p=" << ref_side[i].price
                    << " q=" << ref_side[i].total_qty
                    << " n=" << ref_side[i].order_count << "); ";
                return false;
            }
        }
        return true;
    };

    bool ok_b = cmp("bid", e.bids, r.bids);
    bool ok_a = cmp("ask", e.asks, r.asks);
    DiffResult out;
    out.ok = ok_b && ok_a;
    if (!out.ok) out.detail = err.str();
    return out;
}

}  // namespace lob::tools

#endif
