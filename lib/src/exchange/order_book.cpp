#include <exchange/order_book.hpp>
#include <exchange/compiler.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace exsim::exchange {

namespace {

inline std::size_t bit_word_index(std::size_t idx) noexcept { return idx >> 6; }
inline std::size_t bit_offset(std::size_t idx) noexcept { return idx & 63u; }
inline std::size_t bit_word_count(std::size_t ladder_size) noexcept { return (ladder_size + 63) / 64; }

inline void set_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept {
    words[bit_word_index(idx)] |= (std::uint64_t{1} << bit_offset(idx));
}

inline void clear_active(std::vector<std::uint64_t>& words, std::size_t idx) noexcept {
    words[bit_word_index(idx)] &= ~(std::uint64_t{1} << bit_offset(idx));
}

// Number of ticks in [min_price, max_price] (min_price <= max_price). The
// unsigned subtraction is exact, so extreme bands cannot overflow into a
// short ladder.
std::size_t ladder_size_for(Price min_price, Price max_price) {
    const std::uint64_t span = static_cast<std::uint64_t>(max_price) - static_cast<std::uint64_t>(min_price);
    if (span >= std::numeric_limits<std::size_t>::max() / sizeof(PriceLevel*)) {
        throw std::length_error("OrderBook price band too wide");
    }
    return static_cast<std::size_t>(span) + 1;
}

}  // namespace

OrderBook::OrderBook() : OrderBook(OrderBookConfig{}) {}

OrderBook::OrderBook(const OrderBookConfig& config)
    : min_price_(std::min(config.min_price, config.max_price))
    , max_price_(std::max(config.min_price, config.max_price))
    , highest_buy_(nullptr)
    , lowest_sell_(nullptr)
    , max_fills_per_add_order_(std::max<std::size_t>(1, config.max_fills_per_add_order))
    , allow_growth_(config.allow_growth)
    , next_order_id_(1) {
    const std::size_t ladder_size = ladder_size_for(min_price_, max_price_);
    bid_ladder_.assign(ladder_size, nullptr);
    ask_ladder_.assign(ladder_size, nullptr);
    bid_active_words_.assign(bit_word_count(ladder_size), 0);
    ask_active_words_.assign(bit_word_count(ladder_size), 0);

    const auto max_resting_orders = std::max<std::size_t>(1, config.max_resting_orders);
    orders_.reserve(max_resting_orders);
    order_pool_.reserve(max_resting_orders);
    level_pool_.reserve(std::max<std::size_t>(1, config.max_price_levels));
    fill_buffer_.reserve(max_fills_per_add_order_);

    orders_.set_allow_growth(allow_growth_);
    order_pool_.set_allow_growth(allow_growth_);
    level_pool_.set_allow_growth(allow_growth_);
}

bool OrderBook::ensure_price_range(Price price) {
    if (EXSIM_EXCHANGE_LIKELY(price >= min_price_ && price <= max_price_)) {
        return true;
    }
    if (!allow_growth_) {
        return false;
    }

    const Price new_min = std::min(min_price_, price);
    const Price new_max = std::max(max_price_, price);
    const std::size_t new_size = ladder_size_for(new_min, new_max);
    const std::size_t shift = static_cast<std::size_t>(
        static_cast<std::uint64_t>(min_price_) - static_cast<std::uint64_t>(new_min));

    // Build every replacement before touching the book, so a failed
    // allocation leaves it unchanged.
    std::vector<PriceLevel*> new_bid_ladder(new_size, nullptr);
    std::vector<PriceLevel*> new_ask_ladder(new_size, nullptr);
    std::vector<std::uint64_t> new_bid_words(bit_word_count(new_size), 0);
    std::vector<std::uint64_t> new_ask_words(bit_word_count(new_size), 0);
    for (std::size_t i = 0; i < bid_ladder_.size(); ++i) {
        if (PriceLevel* level = bid_ladder_[i]) {
            new_bid_ladder[i + shift] = level;
            set_active(new_bid_words, i + shift);
        }
        if (PriceLevel* level = ask_ladder_[i]) {
            new_ask_ladder[i + shift] = level;
            set_active(new_ask_words, i + shift);
        }
    }

    bid_ladder_.swap(new_bid_ladder);
    ask_ladder_.swap(new_ask_ladder);
    bid_active_words_.swap(new_bid_words);
    ask_active_words_.swap(new_ask_words);
    min_price_ = new_min;
    max_price_ = new_max;
    return true;
}

std::size_t OrderBook::ladder_index(Price price) const noexcept {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(min_price_));
}

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

template<Side S>
bool OrderBook::add_order_to_book_impl(Order* order) {
    // add_order already validated the price against the ladder range.

    const std::size_t idx = ladder_index(order->price);
    auto& ladder = (S == Side::BUY) ? bid_ladder_ : ask_ladder_;
    auto& active = (S == Side::BUY) ? bid_active_words_ : ask_active_words_;
    auto*& best  = (S == Side::BUY) ? highest_buy_ : lowest_sell_;

    PriceLevel*& level = ladder[idx];
    if (EXSIM_EXCHANGE_UNLIKELY(!level)) {
        level = level_pool_.create(order->price);
        if (EXSIM_EXCHANGE_UNLIKELY(!level)) {
            return false;
        }
        set_active(active, idx);
        if constexpr (S == Side::BUY) {
            if (!best || level->price > best->price) best = level;
        } else {
            if (!best || level->price < best->price) best = level;
        }
    }
    if (level->total_volume > std::numeric_limits<Quantity>::max() - order->remaining_quantity) return false;
    level->add_order(order);
    return true;
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
bool OrderBook::match_order_impl(Order& incoming) {
    auto& fills = fill_buffer_;

    while (!incoming.is_filled()) {
        PriceLevel*& best = (S == Side::BUY) ? lowest_sell_ : highest_buy_;
        if (!best) break;

        PriceLevel* contra_level = best;

        if constexpr (S == Side::BUY) {
            if (EXSIM_EXCHANGE_UNLIKELY(incoming.price < contra_level->price)) break;
        } else {
            if (EXSIM_EXCHANGE_UNLIKELY(incoming.price > contra_level->price)) break;
        }

        while (!incoming.is_filled() && !contra_level->is_empty()) {
            if (EXSIM_EXCHANGE_UNLIKELY(fills.size() >= max_fills_per_add_order_)) {
                // Still crossing, but the bounded fill buffer is full. Stop
                // here: every executed fill is reported and the caller
                // cancels the remainder. The current level is non-empty, so
                // no ladder cleanup is pending.
                return true;
            }
            Order* resting = contra_level->front();
            const Quantity fill_qty = std::min(incoming.remaining_quantity, resting->remaining_quantity);

            if constexpr (S == Side::BUY) {
                fills.push_back(Fill{incoming.id, resting->id, contra_level->price, fill_qty});
            } else {
                fills.push_back(Fill{resting->id, incoming.id, contra_level->price, fill_qty});
            }
            incoming.fill(fill_qty);
            resting->fill(fill_qty);
            contra_level->total_volume -= fill_qty;

            if (EXSIM_EXCHANGE_LIKELY(resting->is_filled())) {
                const OrderId resting_id = resting->id;
                contra_level->pop_front();
                (void)orders_.erase(resting_id);
                order_pool_.destroy(resting);
            }
        }

        if (EXSIM_EXCHANGE_LIKELY(contra_level->is_empty())) {
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
    return false;
}

// Explicit template instantiations
template bool OrderBook::match_order_impl<Side::BUY>(Order&);
template bool OrderBook::match_order_impl<Side::SELL>(Order&);
template bool OrderBook::add_order_to_book_impl<Side::BUY>(Order*);
template bool OrderBook::add_order_to_book_impl<Side::SELL>(Order*);
template void OrderBook::remove_order_from_book_impl<Side::BUY>(Order*);
template void OrderBook::remove_order_from_book_impl<Side::SELL>(Order*);

OrderBook::AddResult OrderBook::add_order(Price price, Quantity quantity, Side side) {
    fill_buffer_.clear();
    if (EXSIM_EXCHANGE_UNLIKELY(next_order_id_ == 0 || !ensure_price_range(price))) {
        return AddResult{0, {}, 0, 0, false};
    }
    if (allow_growth_) {
        // All growth happens before matching, so a failed allocation leaves
        // the book unchanged and resting a remainder cannot allocate.
        order_pool_.ensure_free_node();
        level_pool_.ensure_free_node();
        orders_.reserve(orders_.size() + 1);
    }

    // The aggressor matches from the stack and takes a pool slot only if it
    // rests, so a full bounded book still accepts orders that trade.
    Order incoming(next_order_id_++, price, quantity, side);
    const OrderId order_id = incoming.id;
    const bool fill_limit_reached = (side == Side::BUY)
        ? match_order_impl<Side::BUY>(incoming)
        : match_order_impl<Side::SELL>(incoming);
    const Quantity remaining = incoming.remaining_quantity;

    if (remaining == 0) {
        return AddResult{order_id, fill_buffer_, 0, 0, false};
    }
    if (EXSIM_EXCHANGE_UNLIKELY(fill_limit_reached)) {
        // The remainder still crosses; resting it would cross the book.
        return AddResult{order_id, fill_buffer_, 0, remaining, true};
    }
    Order* order_ptr = order_pool_.create(incoming);
    if (EXSIM_EXCHANGE_UNLIKELY(!order_ptr)) {
        return AddResult{order_id, fill_buffer_, 0, remaining, false};
    }
    if (EXSIM_EXCHANGE_UNLIKELY(!orders_.insert(order_id, order_ptr))) {
        order_pool_.destroy(order_ptr);
        return AddResult{order_id, fill_buffer_, 0, remaining, false};
    }
    const bool placed = (side == Side::BUY)
        ? add_order_to_book_impl<Side::BUY>(order_ptr)
        : add_order_to_book_impl<Side::SELL>(order_ptr);
    if (EXSIM_EXCHANGE_UNLIKELY(!placed)) {
        // No price-level slot: drop the remainder instead of keeping an id
        // that is not on any level.
        (void)orders_.erase(order_id);
        order_pool_.destroy(order_ptr);
        return AddResult{order_id, fill_buffer_, 0, remaining, false};
    }

    return AddResult{order_id, fill_buffer_, remaining, 0, false};
}

Order* OrderBook::rest_order(OrderId id, Price price, Quantity quantity,
                             Quantity leaves, Side side, std::uint32_t context_index) {
    if (id == 0 || leaves == 0 || leaves > quantity || orders_.contains(id) || !ensure_price_range(price)) return nullptr;
    if (allow_growth_) {
        order_pool_.ensure_free_node();
        level_pool_.ensure_free_node();
        orders_.reserve(orders_.size() + 1);
    }
    Order* order = order_pool_.create(id,price,quantity,side);
    if (!order) return nullptr;
    order->remaining_quantity = leaves;
    order->context_index = context_index;
    if (!orders_.insert(id,order)) { order_pool_.destroy(order); return nullptr; }
    const bool placed = side == Side::BUY ? add_order_to_book_impl<Side::BUY>(order) : add_order_to_book_impl<Side::SELL>(order);
    if (!placed) { (void)orders_.erase(id); order_pool_.destroy(order); return nullptr; }
    // Keep add_order's automatically generated identifiers disjoint when mixed.
    if (next_order_id_ != 0 && id >= next_order_id_) next_order_id_ = id + 1;
    return order;
}

bool OrderBook::set_leaves(OrderId id, Quantity leaves) noexcept {
    Order** slot = orders_.find(id);
    if (!slot) return false;
    Order& order = **slot;
    if (leaves > order.remaining_quantity) return false;
    order.parent_level->total_volume -= order.remaining_quantity - leaves;
    order.remaining_quantity = leaves;
    return true;
}

bool OrderBook::amend_order(OrderId id, Quantity total, Quantity leaves) noexcept {
    if (leaves > total || !set_leaves(id,leaves)) return false;
    (*orders_.find(id))->quantity = total;
    return true;
}

const Order* OrderBook::find_order(OrderId id) const noexcept {
    auto slot = orders_.find(id);
    return slot ? *slot : nullptr;
}
const PriceLevel* OrderBook::price_level(Side side, Price price) const noexcept {
    if (price < min_price_ || price > max_price_) return nullptr;
    return (side == Side::BUY ? bid_ladder_ : ask_ladder_)[ladder_index(price)];
}
std::optional<Price> OrderBook::next_price(Side side, Price from, bool ascending) const noexcept {
    if (ascending && from > max_price_) return std::nullopt;
    if (!ascending && from < min_price_) return std::nullopt;
    from = std::clamp(from,min_price_,max_price_);
    const auto& words = side == Side::BUY ? bid_active_words_ : ask_active_words_;
    const auto index = ascending ? find_next_active(words,ladder_index(from)) : find_prev_active(words,ladder_index(from));
    return index ? std::optional<Price>(min_price_ + static_cast<Price>(*index)) : std::nullopt;
}
std::uint64_t OrderBook::active_word(Side side, std::size_t word) const noexcept {
    const auto& words = side == Side::BUY ? bid_active_words_ : ask_active_words_;
    return word < words.size() ? words[word] : 0;
}

bool OrderBook::cancel_order(OrderId order_id) {
    Order** slot = orders_.find(order_id);
    if (EXSIM_EXCHANGE_UNLIKELY(!slot)) {
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
    if (EXSIM_EXCHANGE_UNLIKELY(!slot)) {
        return false;
    }

    Order* order = *slot;
    const Quantity filled_qty = order->quantity - order->remaining_quantity;
    const Quantity new_remaining = (new_quantity > filled_qty) ? (new_quantity - filled_qty) : 0;
    if (new_remaining == 0) {
        // Nothing left to execute: a zero-quantity order must not rest.
        return cancel_order(order_id);
    }

    PriceLevel* level = order->parent_level;
    const Quantity other_volume = level->total_volume - order->remaining_quantity;
    if (new_remaining > std::numeric_limits<Quantity>::max() - other_volume) return false;
    level->total_volume = other_volume + new_remaining;

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
    // (bid + ask) / 2 truncated toward zero, without forming bid + ask, which
    // overflows for books near either end of the Price range.
    const Price remainder_sum = *bid % 2 + *ask % 2;
    const Price half_sum = *bid / 2 + *ask / 2 + remainder_sum / 2;
    const Price leftover_half_tick = remainder_sum % 2;
    if (leftover_half_tick > 0 && half_sum < 0) {
        return half_sum + 1;
    }
    if (leftover_half_tick < 0 && half_sum > 0) {
        return half_sum - 1;
    }
    return half_sum;
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
    if (depth == 0) {
        return snapshot;
    }

    // Walk occupied levels through the bitmaps instead of every tick.
    if (highest_buy_) {
        std::optional<std::size_t> idx = ladder_index(highest_buy_->price);
        while (idx && snapshot.bids.size() < depth) {
            const PriceLevel* level = bid_ladder_[*idx];
            snapshot.bids.push_back({level->price, level->total_volume, level->order_count()});
            idx = (*idx == 0) ? std::nullopt : find_prev_active(bid_active_words_, *idx - 1);
        }
    }
    if (lowest_sell_) {
        std::optional<std::size_t> idx = ladder_index(lowest_sell_->price);
        while (idx && snapshot.asks.size() < depth) {
            const PriceLevel* level = ask_ladder_[*idx];
            snapshot.asks.push_back({level->price, level->total_volume, level->order_count()});
            idx = find_next_active(ask_active_words_, *idx + 1);
        }
    }

    return snapshot;
}

}  // namespace exsim::exchange
