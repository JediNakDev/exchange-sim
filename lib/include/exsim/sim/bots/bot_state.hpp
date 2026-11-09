#ifndef EXSIM_SIM_BOTS_BOT_STATE_HPP
#define EXSIM_SIM_BOTS_BOT_STATE_HPP
#include "bot_common.hpp"
#include "bot_context.hpp"
#include "own_order_table.hpp"
#include "exsim/event.hpp"
#include <algorithm>
#include <span>

namespace exsim::sim::bots {
inline constexpr Timestamp deadline(Timestamp now, Timestamp delay) noexcept {
    return delay >= kNever - now ? kNever : now + delay;
}

// Shared lifecycle state, owned by a strategy. No submissions from callbacks.
class BotState {
public:
    BotState(BotConfig config, std::size_t capacity) : config_(config), orders_(capacity), enabled_(config.enabled) {}
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; dirty_ = true; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] const BotConfig& config() const noexcept { return config_; }
    [[nodiscard]] const OwnOrderTable& orders() const noexcept { return orders_; }
    [[nodiscard]] const BotStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    void on_events(std::span<const Event> events) noexcept {
        for (const auto& event : events) {
            if (event.type == EventType::Rejected) {
                const auto& reject = event.rejected;
                if (reject.session != config_.session_id) continue;
                const auto id = reject.cmd == CommandType::NewOrder ? reject.cl_ord_id : reject.orig_cl_ord_id;
                if (auto* order = orders_.find(id)) {
                    if (reject.cmd == CommandType::NewOrder || reject.reason == RejectReason::UnknownOrder)
                        orders_.erase(*order);
                    else order->in_flight = InFlightRequest::None;
                    ++stats_.rejects_received;
                    dirty_ = true;
                }
                continue;
            }
            if (event.instrument != config_.instrument) continue;
            switch (event.type) {
                case EventType::PhaseChanged:
                    phase_ = event.phase_changed.new_phase; phase_known_ = true; dirty_ = true; break;
                case EventType::Accepted:
                    if (event.accepted.session == config_.session_id)
                        if (auto* order = orders_.find(event.accepted.cl_ord_id)) order->order_id = event.accepted.order_id;
                    break;
                case EventType::Rested:
                    if (event.rested.session == config_.session_id)
                        if (auto* order = orders_.find(event.rested.cl_ord_id)) {
                            order->price = event.rested.price; order->leaves_qty = event.rested.leaves_qty;
                            order->in_flight = InFlightRequest::None;
                        }
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == config_.session_id)
                        if (auto* order = orders_.find(event.replaced.orig_cl_ord_id)) {
                            order->order_id = event.replaced.new_order_id; order->price = event.replaced.price;
                            order->leaves_qty = event.replaced.leaves_qty; order->cum_qty = event.replaced.cum_qty;
                            order->in_flight = InFlightRequest::None;
                        }
                    break;
                case EventType::Canceled:
                    if (event.canceled.session == config_.session_id)
                        if (auto* order = orders_.find(event.canceled.cl_ord_id)) {
                            if (event.canceled.leaves_qty == 0) orders_.erase(*order);
                            else {
                                order->leaves_qty = event.canceled.leaves_qty;
                                order->cum_qty = event.canceled.cum_qty;
                                order->in_flight = InFlightRequest::None;
                            }
                            ++stats_.orders_canceled; dirty_ = true;
                        }
                    break;
                case EventType::Fill:
                    fill(event.fill.buy, event.fill.qty, true);
                    fill(event.fill.sell, event.fill.qty, false);
                    break;
                default: break;
            }
        }
    }
protected:
    template <BotContext Context> const InstrumentConfig* prepare(Context& context) noexcept {
        const auto* instrument = context.instrument(config_.instrument);
        if (!is_valid_bot_config(config_) || !instrument || !is_tradable_by_bots(*instrument)) return nullptr;
        if (!phase_known_) { phase_ = instrument->initial_phase; phase_known_ = true; }
        return instrument;
    }
    template <BotContext Context> void cancel(OwnOrder& order, Timestamp now, Context& context) {
        if (order.in_flight != InFlightRequest::None) return;
        const auto id = order.cl_ord_id;
        order.in_flight = InFlightRequest::Cancel;
        ++stats_.commands_submitted; ++stats_.cancels_submitted;
        context.submit(Command{CancelOrder{now, config_.session_id, id, 0}});
    }
    template <BotContext Context> void cancel_all(Timestamp now, Context& context) {
        for (std::size_t i = 0; i < orders_.capacity(); ++i) {
            auto& order = orders_.slot(i);
            if (order.cl_ord_id) cancel(order, now, context);
        }
    }
    template <BotContext Context> void place(Side side, Price price, Quantity qty, TimeInForce tif,
                                           Timestamp expires, Timestamp now, Context& context, bool post_only = false) {
        auto* order = orders_.allocate();
        if (!order) { ++stats_.capacity_skips; return; }
        order->side = side; order->price = price; order->leaves_qty = qty;
        order->expires_at = expires; order->in_flight = InFlightRequest::New;
        NewOrder command{};
        command.ts = now; command.instrument = config_.instrument; command.session = config_.session_id;
        command.owner = config_.owner; command.stp = config_.stp; command.cl_ord_id = order->cl_ord_id;
        command.post_only = post_only;
        command.side = side; command.price = price; command.qty = qty; command.tif = tif;
        ++stats_.commands_submitted; ++stats_.new_orders_submitted;
        context.submit(Command{command});
    }
    BotConfig config_;
    OwnOrderTable orders_;
    BotStats stats_{};
    bool enabled_;
    bool dirty_ = true;
    bool phase_known_ = false;
    Phase phase_ = Phase::Closed;
private:
    void fill(const FillLeg& leg, Quantity qty, bool buy) noexcept {
        if (leg.session != config_.session_id) return;
        // Saturate inventory for arbitrarily long synthetic runs.
        const auto delta = buy ? std::int64_t{qty} : -std::int64_t{qty};
        if (delta > 0 && inventory_ > std::numeric_limits<std::int64_t>::max() - delta)
            inventory_ = std::numeric_limits<std::int64_t>::max();
        else if (delta < 0 && inventory_ < std::numeric_limits<std::int64_t>::min() - delta)
            inventory_ = std::numeric_limits<std::int64_t>::min();
        else inventory_ += delta;
        if (auto* order = orders_.find(leg.cl_ord_id)) {
            order->leaves_qty = leg.leaves_qty; order->cum_qty = leg.cum_qty;
            if (!leg.leaves_qty) orders_.erase(*order);
        }
        ++stats_.fills_received; stats_.filled_qty += qty; dirty_ = true;
    }
    std::int64_t inventory_ = 0;
};
} // namespace exsim::sim::bots
#endif
