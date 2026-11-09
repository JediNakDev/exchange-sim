// Minimal use of the lob::OrderBook core: rest two asks, sweep them with an
// aggressive bid, and print the resulting fills and top of book.

#include <exchange/order_book.hpp>

#include <cstdio>

int main() {
    lob::OrderBook book;

    (void)book.add_order(10100, 50, lob::Side::SELL);
    (void)book.add_order(10200, 50, lob::Side::SELL);

    const auto result = book.add_order(10200, 80, lob::Side::BUY);
    for (const lob::Fill& f : result.fills) {
        std::printf("fill buy=%llu sell=%llu px=%lld qty=%llu\n",
                    static_cast<unsigned long long>(f.buy_order_id),
                    static_cast<unsigned long long>(f.sell_order_id),
                    static_cast<long long>(f.price),
                    static_cast<unsigned long long>(f.quantity));
    }

    const auto ask = book.get_best_ask();
    std::printf("best ask=%lld qty=%llu\n",
                static_cast<long long>(ask ? *ask : 0),
                static_cast<unsigned long long>(book.get_ask_quantity_at_top()));
    return 0;
}
