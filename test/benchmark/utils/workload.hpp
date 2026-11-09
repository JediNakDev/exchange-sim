#pragma once

#include "constants.hpp"
#include <lob/order_book.hpp>

#include <algorithm>
#include <random>
#include <vector>

namespace bench {

class RandomWorkload {
public:
    struct OrderData {
        lob::Price price;
        uint64_t quantity;
        lob::Side side;
    };

    explicit RandomWorkload(size_t count, uint64_t seed = 42)
        : rng_(seed),
          price_dist_(BASE_PRICE - PRICE_LEVELS * TICK_SIZE,
                      BASE_PRICE + PRICE_LEVELS * TICK_SIZE),
          qty_dist_(1, 1000),
          side_dist_(0, 1) {
        orders_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            orders_.push_back({price_dist_(rng_), qty_dist_(rng_),
                               side_dist_(rng_) == 0 ? lob::Side::BUY : lob::Side::SELL});
        }

        cancel_indices_.reserve(count);
        modify_quantities_.reserve(count);

        std::uniform_int_distribution<size_t> idx_dist(0, count - 1);
        std::uniform_int_distribution<uint64_t> mod_qty_dist(1, 500);

        for (size_t i = 0; i < count; ++i) {
            cancel_indices_.push_back(idx_dist(rng_));
            modify_quantities_.push_back(mod_qty_dist(rng_));
        }

        std::shuffle(orders_.begin(), orders_.end(), rng_);
    }

    const OrderData& get(size_t index) const noexcept {
        return orders_[index % orders_.size()];
    }

    size_t cancel_index(size_t i) const noexcept {
        return cancel_indices_[i % cancel_indices_.size()];
    }

    uint64_t modify_quantity(size_t i) const noexcept {
        return modify_quantities_[i % modify_quantities_.size()];
    }

private:
    std::mt19937_64 rng_;
    std::uniform_int_distribution<lob::Price> price_dist_;
    std::uniform_int_distribution<uint64_t> qty_dist_;
    std::uniform_int_distribution<int> side_dist_;
    std::vector<OrderData> orders_;
    std::vector<size_t> cancel_indices_;
    std::vector<uint64_t> modify_quantities_;
};

inline const RandomWorkload& workload() {
    static RandomWorkload instance(1'000'000);
    return instance;
}

class PrePopulatedBook {
public:
    PrePopulatedBook(int levels = PRICE_LEVELS, int orders_per_level = ORDERS_PER_LEVEL) {
        std::mt19937_64 rng(12345);
        std::uniform_int_distribution<uint64_t> qty_dist(100, 10000);

        for (int i = 1; i <= levels; ++i) {
            lob::Price bid_price = BASE_PRICE - i * TICK_SIZE;
            lob::Price ask_price = BASE_PRICE + i * TICK_SIZE;
            for (int j = 0; j < orders_per_level; ++j) {
                ids_.push_back(book_.add_order(bid_price, qty_dist(rng), lob::Side::BUY).order_id);
                ids_.push_back(book_.add_order(ask_price, qty_dist(rng), lob::Side::SELL).order_id);
            }
        }
    }

    lob::OrderBook& book() { return book_; }
    const std::vector<lob::OrderId>& ids() const { return ids_; }

private:
    lob::OrderBook book_;
    std::vector<lob::OrderId> ids_;
};

} 
