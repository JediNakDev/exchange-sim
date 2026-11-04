#ifndef EXSIM_RESEARCH_STRATEGIES_QUOTE_MANAGER_HPP
#define EXSIM_RESEARCH_STRATEGIES_QUOTE_MANAGER_HPP

// One resting order per side, steered toward a target price and size.
//
//   * no live order and a nonzero target       -> submit a DAY limit order;
//   * target size zero                         -> cancel;
//   * acknowledged, no request in flight, and the price is off by at least
//     requote_threshold_ticks or the leaves differ from the target size
//                                              -> replace (new total = cum + size);
//   * order not yet acknowledged or a request in flight -> wait.
// A replace whose total would exceed the instrument's max_order_qty is
// turned into a cancel; the next update submits a fresh order.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/strategy_context.hpp"

#include <cstdint>

namespace exsim::research::strategies {

struct QuoteTarget {
    Price price = 0;
    Quantity qty = 0;  // 0 = no quote on this side
};

class QuoteManager {
public:
    explicit QuoteManager(const QuotingParameters& parameters) noexcept : parameters_(parameters) {}

    void apply(StrategyContext& context, const QuoteTarget& bid, const QuoteTarget& ask) noexcept {
        steer(context, Side::BUY, bid, bid_order_);
        steer(context, Side::SELL, ask, ask_order_);
    }

    void cancel_all(StrategyContext& context) noexcept {
        steer(context, Side::BUY, QuoteTarget{}, bid_order_);
        steer(context, Side::SELL, QuoteTarget{}, ask_order_);
    }

    [[nodiscard]] ClOrdId bid_order() const noexcept { return bid_order_; }
    [[nodiscard]] ClOrdId ask_order() const noexcept { return ask_order_; }

private:
    void steer(StrategyContext& context, Side side, const QuoteTarget& target, ClOrdId& token) noexcept {
        const StrategyOrderView* order = context.find_order(token);
        if (order == nullptr) token = 0;
        if (target.qty == 0) {
            if (order != nullptr && order->pending == PendingRequest::None && !order->cancel_after_ack)
                (void)context.cancel_order(token);
            return;
        }
        if (order == nullptr) {
            token = context.submit_limit_order(side, target.price, target.qty, TimeInForce::Day, parameters_.post_only);
            return;
        }
        if (order->state == OrderViewState::PendingNew || order->pending != PendingRequest::None || order->cancel_after_ack)
            return;
        const Price tick = context.instrument().tick_size;
        const Price distance = order->price > target.price ? order->price - target.price : target.price - order->price;
        const bool price_moved = distance >= static_cast<Price>(parameters_.requote_threshold_ticks) * tick;
        if (!price_moved && order->leaves_qty == target.qty) return;
        const std::uint64_t new_total = std::uint64_t{order->cum_qty} + target.qty;
        if (new_total > context.instrument().max_order_qty) {
            (void)context.cancel_order(token);
            return;
        }
        (void)context.replace_order(token, price_moved ? target.price : order->price, static_cast<Quantity>(new_total));
    }

    QuotingParameters parameters_;
    ClOrdId bid_order_ = 0;
    ClOrdId ask_order_ = 0;
};

}  // namespace exsim::research::strategies

#endif
