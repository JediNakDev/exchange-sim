#include <lob/order_book.hpp>
#include <lob/compiler.hpp>
#include <algorithm>

namespace lob {

namespace {

constexpr std::size_t kInitialOrderCapacity = 1u << 16;
constexpr std::size_t kInitialOrderPoolObjects = 1u << 16;
constexpr std::size_t kInitialLevelPoolObjects = 1u << 14;
constexpr std::size_t kInitialFillCapacity = 1024;

constexpr Price kDefaultMinPrice = -100000;
constexpr Price kDefaultMaxPrice = 100000;

inline std::size_t bit_word_index(std::size_t idx) noexcept { return idx >> 6; }
inline std::size_t bit_offset(std::size_t idx) noexcept { return idx & 63u; }

}  // namespace

OrderBook::OrderBook()
    : min_price_(0)
    , max_price_(-1)
    , ladder_initialized_(false)
    , highest_buy_(nullptr)
    , lowest_sell_(nullptr)
    , next_order_id_(1) {
    orders_.reserve(kInitialOrderCapacity);

    order_pool_.reserve(kInitialOrderPoolObjects);
    level_pool_.reserve(kInitialLevelPoolObjects);
    fill_buffer_.reserve(kInitialFillCapacity);

#ifdef LOB_DETERMINISTIC_POOL
    order_pool_.set_allow_growth(false);
    level_pool_.set_allow_growth(false);
#else
    orders_.set_allow_growth(true);
#endif

    initialize_ladders(kDefaultMinPrice, kDefaultMaxPrice);
}

OrderBook::~OrderBook() { clear(); }

void OrderBook::initialize_ladders(Price min_price, Price max_price) {
    if (min_price > max_price) {
        std::swap(min_price, max_price);
    }

    min_price_ = min_price;
    max_price_ = max_price;
    ladder_initialized_ = true;

    const std::size_t size = static_cast<std::size_t>(max_price_ - min_price_ + 1);
    bid_ladder_.assign(size, nullptr);
    ask_ladder_.assign(size, nullptr);

    const std::size_t words = (size + 63) / 64;
    bid_active_words_.assign(words, 0);
    ask_active_words_.assign(words, 0);
}

void OrderBook::ensure_price_range(Price price) {
    if (LOB_UNLIKELY(!ladder_initialized_)) {
        initialize_ladders(price, price);
        return;
    }
    if (LOB_LIKELY(price >= min_price_ && price <= max_price_)) {
        return;
    }

#ifdef LOB_DETERMINISTIC_POOL
    // Deterministic mode forbids ladder growth in hot path.
    return;
#else
    const Price new_min = std::min(min_price_, price);
    const Price new_max = std::max(max_price_, price);
    const std::size_t old_size = bid_ladder_.size();
    const std::size_t new_size = static_cast<std::size_t>(new_max - new_min + 1);
    const std::size_t shift = static_cast<std::size_t>(min_price_ - new_min);

    std::vector<PriceLevel*> new_bid(new_size, nullptr);
    std::vector<PriceLevel*> new_ask(new_size, nullptr);

    for (std::size_t i = 0; i < old_size; ++i) {
        new_bid[i + shift] = bid_ladder_[i];
        new_ask[i + shift] = ask_ladder_[i];
    }

    bid_ladder_.swap(new_bid);
    ask_ladder_.swap(new_ask);
    min_price_ = new_min;
    max_price_ = new_max;

    const std::size_t words = (new_size + 63) / 64;
    bid_active_words_.assign(words, 0);
    ask_active_words_.assign(words, 0);
    for (std::size_t i = 0; i < new_size; ++i) {
        if (bid_ladder_[i]) {
            set_active(bid_active_words_, i);
        }
        if (ask_ladder_[i]) {
            set_active(ask_active_words_, i);
        }
    }
#endif
}

std::size_t OrderBook::ladder_index(Price price) const noexcept {
    return static_cast<std::size_t>(price - min_price_);
}

void OrderBook::set_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept {
    words[bit_word_index(idx)] |= (std::uint64_t{1} << bit_offset(idx));
}

void OrderBook::clear_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept {
    words[bit_word_index(idx)] &= ~(std::uint64_t{1} << bit_offset(idx));
}

std::size_t OrderBook::word_count() const noexcept { return bid_active_words_.size(); }

std::optional<std::size_t> OrderBook::find_prev_active(
    const std::vector<std::uint64_t>& words,
    std::size_t from_idx) const noexcept {
    if (words.empty() || from_idx >= bid_ladder_.size()) {
        return std::nullopt;
    }

    std::size_t word = bit_word_index(from_idx);
    std::uint64_t bits = words[word];
    const unsigned offset = static_cast<unsigned>(bit_offset(from_idx));
    const std::uint64_t mask = (offset == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (offset + 1)) - 1);
    bits &= mask;

    while (true) {
        if (bits != 0) {
            const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(bits));
            return (word << 6) + msb;
        }
        if (word == 0) {
            return std::nullopt;
        }
        --word;
        bits = words[word];
    }
}

std::optional<std::size_t> OrderBook::find_next_active(
    const std::vector<std::uint64_t>& words,
    std::size_t from_idx) const noexcept {
    if (words.empty() || from_idx >= bid_ladder_.size()) {
        return std::nullopt;
    }

    std::size_t word = bit_word_index(from_idx);
    std::uint64_t bits = words[word];
    const unsigned offset = static_cast<unsigned>(bit_offset(from_idx));
    const std::uint64_t mask = ~((std::uint64_t{1} << offset) - 1);
    bits &= mask;

    while (word < words.size()) {
        if (bits != 0) {
            const unsigned lsb = static_cast<unsigned>(__builtin_ctzll(bits));
            const std::size_t idx = (word << 6) + lsb;
            if (idx < bid_ladder_.size()) {
                return idx;
            }
            return std::nullopt;
        }
        ++word;
        if (word < words.size()) {
            bits = words[word];
        }
    }
    return std::nullopt;
}

void OrderBook::refresh_best_levels() noexcept {
    highest_buy_ = nullptr;
    lowest_sell_ = nullptr;

    for (std::size_t w = word_count(); w > 0; --w) {
        const std::uint64_t bits = bid_active_words_[w - 1];
        if (bits == 0) {
            continue;
        }
        const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(bits));
        const std::size_t idx = ((w - 1) << 6) + msb;
        if (idx < bid_ladder_.size()) {
            highest_buy_ = bid_ladder_[idx];
        }
        break;
    }

    for (std::size_t w = 0; w < word_count(); ++w) {
        const std::uint64_t bits = ask_active_words_[w];
        if (bits == 0) {
            continue;
        }
        const unsigned lsb = static_cast<unsigned>(__builtin_ctzll(bits));
        const std::size_t idx = (w << 6) + lsb;
        if (idx < ask_ladder_.size()) {
            lowest_sell_ = ask_ladder_[idx];
        }
        break;
    }
}

template<Side S>
void OrderBook::add_order_to_book_impl(Order* order) {
    ensure_price_range(order->price);
    if (order->price < min_price_ || order->price > max_price_) {
        return;
    }

    const std::size_t idx = ladder_index(order->price);
    auto& ladder = (S == Side::BUY) ? bid_ladder_ : ask_ladder_;
    auto& active = (S == Side::BUY) ? bid_active_words_ : ask_active_words_;
    auto*& best  = (S == Side::BUY) ? highest_buy_ : lowest_sell_;

    PriceLevel*& level = ladder[idx];
    if (LOB_UNLIKELY(!level)) {
        level = level_pool_.create(order->price);
        if (LOB_UNLIKELY(!level)) {
            return;
        }
        set_active(active, idx);
        if constexpr (S == Side::BUY) {
            if (!best || level->price > best->price) best = level;
        } else {
            if (!best || level->price < best->price) best = level;
        }
    }
    level->add_order(order);
}

template<Side S>
void OrderBook::remove_order_from_book_impl(Order* order) {
    PriceLevel* level = order->parent_level;
    if (!level) {
        return;
    }

    level->remove_order(order);
    if (!level->is_empty()) {
        return;
    }

    const std::size_t idx = ladder_index(level->price);
    auto& ladder = (S == Side::BUY) ? bid_ladder_ : ask_ladder_;
    auto& active = (S == Side::BUY) ? bid_active_words_ : ask_active_words_;

    ladder[idx] = nullptr;
    clear_active(active, idx);

    if constexpr (S == Side::BUY) {
        if (level == highest_buy_) {
            if (idx == 0) {
                highest_buy_ = nullptr;
            } else {
                const auto prev = find_prev_active(bid_active_words_, idx - 1);
                highest_buy_ = prev ? bid_ladder_[*prev] : nullptr;
            }
        }
    } else {
        if (level == lowest_sell_) {
            const auto next = find_next_active(ask_active_words_, idx + 1);
            lowest_sell_ = next ? ask_ladder_[*next] : nullptr;
        }
    }
    level_pool_.destroy(level);
}

template<Side S>
void OrderBook::match_order_impl(Order* incoming) {
    auto& fills = fill_buffer_;

    while (!incoming->is_filled()) {
        PriceLevel*& best = (S == Side::BUY) ? lowest_sell_ : highest_buy_;
        if (!best) break;

        PriceLevel* contra_level = best;

        if constexpr (S == Side::BUY) {
            if (LOB_UNLIKELY(incoming->price < contra_level->price)) break;
        } else {
            if (LOB_UNLIKELY(incoming->price > contra_level->price)) break;
        }

        while (!incoming->is_filled() && !contra_level->is_empty()) {
            Order* resting = contra_level->front();
            const Quantity fill_qty = std::min(incoming->remaining_quantity, resting->remaining_quantity);

            if constexpr (S == Side::BUY) {
                fills.push_back(Fill{incoming->id, resting->id, contra_level->price, fill_qty});
            } else {
                fills.push_back(Fill{resting->id, incoming->id, contra_level->price, fill_qty});
            }
            incoming->fill(fill_qty);
            resting->fill(fill_qty);
            contra_level->update_quantity(-static_cast<int64_t>(fill_qty));

            if (LOB_LIKELY(resting->is_filled())) {
                const OrderId resting_id = resting->id;
                contra_level->pop_front();
                (void)orders_.erase(resting_id);
                order_pool_.destroy(resting);
            }
        }

        if (LOB_LIKELY(contra_level->is_empty())) {
            const std::size_t idx = ladder_index(contra_level->price);
            auto& ladder = (S == Side::BUY) ? ask_ladder_ : bid_ladder_;
            auto& active = (S == Side::BUY) ? ask_active_words_ : bid_active_words_;

            ladder[idx] = nullptr;
            clear_active(active, idx);
            level_pool_.destroy(contra_level);

            if constexpr (S == Side::BUY) {
                const auto next = find_next_active(ask_active_words_, idx + 1);
                lowest_sell_ = next ? ask_ladder_[*next] : nullptr;
            } else {
                if (idx == 0) {
                    highest_buy_ = nullptr;
                } else {
                    const auto prev = find_prev_active(bid_active_words_, idx - 1);
                    highest_buy_ = prev ? bid_ladder_[*prev] : nullptr;
                }
            }
        }
    }
}

// Explicit template instantiations
template void OrderBook::match_order_impl<Side::BUY>(Order*);
template void OrderBook::match_order_impl<Side::SELL>(Order*);
template void OrderBook::add_order_to_book_impl<Side::BUY>(Order*);
template void OrderBook::add_order_to_book_impl<Side::SELL>(Order*);
template void OrderBook::remove_order_from_book_impl<Side::BUY>(Order*);
template void OrderBook::remove_order_from_book_impl<Side::SELL>(Order*);

OrderBook::AddResult OrderBook::add_order(Price price, Quantity quantity, Side side) {
    fill_buffer_.clear();
    ensure_price_range(price);
    if (LOB_UNLIKELY(price < min_price_ || price > max_price_)) {
        return AddResult{0, {}, 0};
    }

    const OrderId order_id = next_order_id_++;
    Order* order_ptr = order_pool_.create(order_id, price, quantity, side);
    if (LOB_UNLIKELY(!order_ptr)) {
        return AddResult{0, {}, 0};
    }

    if (side == Side::BUY) {
        match_order_impl<Side::BUY>(order_ptr);
    } else {
        match_order_impl<Side::SELL>(order_ptr);
    }
    const Quantity remaining = order_ptr->remaining_quantity;
    if (!order_ptr->is_filled()) {
        if (LOB_UNLIKELY(!orders_.insert(order_ptr->id, order_ptr))) {
            // Id map full (deterministic mode): drop the remainder.
            order_pool_.destroy(order_ptr);
            return AddResult{order_id, fill_buffer_, 0};
        }
        if (side == Side::BUY) {
            add_order_to_book_impl<Side::BUY>(order_ptr);
        } else {
            add_order_to_book_impl<Side::SELL>(order_ptr);
        }
    } else {
        order_pool_.destroy(order_ptr);
    }

    return AddResult{order_id, fill_buffer_, remaining};
}

bool OrderBook::cancel_order(OrderId order_id) {
    Order** slot = orders_.find(order_id);
    if (LOB_UNLIKELY(!slot)) {
        return false;
    }

    Order* order = *slot;
    if (order->side == Side::BUY) {
        remove_order_from_book_impl<Side::BUY>(order);
    } else {
        remove_order_from_book_impl<Side::SELL>(order);
    }
    (void)orders_.erase(order_id);
    order_pool_.destroy(order);
    return true;
}

bool OrderBook::modify_order(OrderId order_id, Quantity new_quantity) {
    Order** slot = orders_.find(order_id);
    if (LOB_UNLIKELY(!slot)) {
        return false;
    }

    Order* order = *slot;
    const Quantity filled_qty = order->quantity - order->remaining_quantity;
    const Quantity new_remaining = (new_quantity > filled_qty) ? (new_quantity - filled_qty) : 0;

    PriceLevel* level = order->parent_level;
    if (level) {
        const int64_t qty_diff = static_cast<int64_t>(new_remaining) - static_cast<int64_t>(order->remaining_quantity);
        level->update_quantity(qty_diff);
    }

    order->quantity = new_quantity;
    order->remaining_quantity = new_remaining;
    return true;
}

std::optional<Price> OrderBook::get_best_bid() const {
    if (!highest_buy_) {
        return std::nullopt;
    }
    return highest_buy_->price;
}

std::optional<Price> OrderBook::get_best_ask() const {
    if (!lowest_sell_) {
        return std::nullopt;
    }
    return lowest_sell_->price;
}

std::optional<Price> OrderBook::get_spread() const {
    const auto bid = get_best_bid();
    const auto ask = get_best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::optional<Price> OrderBook::get_mid_price() const {
    const auto bid = get_best_bid();
    const auto ask = get_best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return (*bid + *ask) / 2;
}

Quantity OrderBook::get_bid_quantity_at_top() const {
    return highest_buy_ ? highest_buy_->total_volume : 0;
}

Quantity OrderBook::get_ask_quantity_at_top() const {
    return lowest_sell_ ? lowest_sell_->total_volume : 0;
}

size_t OrderBook::get_bid_levels() const noexcept {
    size_t count = 0;
    for (std::uint64_t w : bid_active_words_) {
        count += static_cast<size_t>(__builtin_popcountll(w));
    }
    return count;
}

size_t OrderBook::get_ask_levels() const noexcept {
    size_t count = 0;
    for (std::uint64_t w : ask_active_words_) {
        count += static_cast<size_t>(__builtin_popcountll(w));
    }
    return count;
}

OrderBook::BookSnapshot OrderBook::get_snapshot(size_t depth) const {
    BookSnapshot snapshot;

    if (depth == 0 || !ladder_initialized_) {
        return snapshot;
    }

    if (highest_buy_) {
        std::size_t idx = ladder_index(highest_buy_->price);
        while (true) {
            PriceLevel* level = bid_ladder_[idx];
            if (level) {
                snapshot.bids.push_back({level->price, level->total_volume, level->order_count()});
                if (snapshot.bids.size() >= depth) {
                    break;
                }
            }
            if (idx == 0) {
                break;
            }
            --idx;
        }
    }

    if (lowest_sell_) {
        for (std::size_t idx = ladder_index(lowest_sell_->price);
             idx < ask_ladder_.size() && snapshot.asks.size() < depth;
             ++idx) {
            PriceLevel* level = ask_ladder_[idx];
            if (level) {
                snapshot.asks.push_back({level->price, level->total_volume, level->order_count()});
            }
        }
    }

    return snapshot;
}

void OrderBook::clear() {
    orders_.for_each([this](std::uint64_t, Order* order) { order_pool_.destroy(order); });
    orders_.clear();

    for (PriceLevel*& level : bid_ladder_) {
        if (level) {
            level_pool_.destroy(level);
            level = nullptr;
        }
    }
    for (PriceLevel*& level : ask_ladder_) {
        if (level) {
            level_pool_.destroy(level);
            level = nullptr;
        }
    }
    std::fill(bid_active_words_.begin(), bid_active_words_.end(), 0);
    std::fill(ask_active_words_.begin(), ask_active_words_.end(), 0);

    highest_buy_ = nullptr;
    lowest_sell_ = nullptr;
}

}  // namespace lob
