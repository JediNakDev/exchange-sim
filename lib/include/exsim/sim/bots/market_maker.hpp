#ifndef EXSIM_SIM_BOTS_MARKET_MAKER_HPP
#define EXSIM_SIM_BOTS_MARKET_MAKER_HPP
#include "bot_state.hpp"
#include "exsim/sim/random.hpp"
#include <array>

namespace exsim::sim::bots {
struct MarketMakerConfig : BotConfig {
    std::uint32_t levels = 3;
    Quantity size = 100;
    std::uint32_t spread_ticks = 1; // half spread
    Price fair_value_ticks = 0; // zero uses instrument reference
    std::uint32_t fair_value_step_ms = 1000;
    std::uint32_t fair_value_step_ticks = 1;
    std::uint32_t requote_ms = 100;
    std::uint32_t skew_ticks_per_unit = 0;
    bool cancel_on_halt = true;
    std::size_t order_capacity = 128;
};
class MarketMaker : public BotState {
public:
    static constexpr std::size_t kMaxLevels = 64;
    explicit MarketMaker(MarketMakerConfig config = {})
        : BotState(config, config.order_capacity), settings_(config), rng_(config.seed) {}
    [[nodiscard]] Price fair_value_ticks() const noexcept { return fair_ticks_; }
    template <BotContext Context> void on_timer(Timestamp now, Context& context) {
        const auto* instrument = prepare(context);
        if (!instrument) return;
        if (!initialized_) {
            fair_ticks_ = settings_.fair_value_ticks ? settings_.fair_value_ticks : instrument->reference_price / instrument->tick_size;
            fair_ticks_ = std::clamp(fair_ticks_, instrument->min_price / instrument->tick_size,
                                   instrument->max_price / instrument->tick_size);
            next_step_ = deadline(now, millis_to_nanos(settings_.fair_value_step_ms)); initialized_ = true;
        }
        if (!enabled_ || phase_ == Phase::Closed || (phase_ == Phase::Halted && settings_.cancel_on_halt)) {
            cancel_all(now, context); return;
        }
        if (!phase_accepts_limit_orders(phase_)) return;
        // Catch up each seeded step. Caller controls time; no wall clock or scheduling.
        if (settings_.fair_value_step_ms && now != kNever) {
            while (now >= next_step_ && next_step_ != kNever) {
                const auto low = instrument->min_price / instrument->tick_size;
                const auto high = instrument->max_price / instrument->tick_size;
                const auto step = std::min<Price>(settings_.fair_value_step_ticks, high - low);
                fair_ticks_ = rng_.below(2) ? fair_ticks_ + std::min(step, high - fair_ticks_)
                                                          : fair_ticks_ - std::min(step, fair_ticks_ - low);
                next_step_ = deadline(next_step_, millis_to_nanos(settings_.fair_value_step_ms)); dirty_ = true;
            }
        }
        if (!dirty_ && now < next_quote_) return;
        dirty_ = false; next_quote_ = deadline(now, millis_to_nanos(settings_.requote_ms));
        const auto qty = std::min(settings_.size, instrument->max_order_qty) / instrument->lot_size * instrument->lot_size;
        if (!qty) { cancel_all(now, context); return; }
        const auto low = instrument->min_price / instrument->tick_size;
        const auto high = instrument->max_price / instrument->tick_size;
        const auto position = inventory();
        const auto magnitude = position < 0 ? static_cast<std::uint64_t>(-(position + 1)) + 1
                                            : static_cast<std::uint64_t>(position);
        const auto coefficient = settings_.skew_ticks_per_unit;
        const auto room = static_cast<std::uint64_t>(high - low);
        std::uint64_t shift = 0;
        if (coefficient) {
            const auto units = magnitude / qty;
            if (units > room / coefficient) shift = room;
            else {
                shift = units * coefficient;
                shift += std::min(room - shift, (magnitude % qty) * coefficient / qty);
            }
        }
        const auto ticks = static_cast<Price>(shift);
        const auto center = position >= 0 ? fair_ticks_ - std::min(ticks, fair_ticks_ - low)
                                         : fair_ticks_ + std::min(ticks, high - fair_ticks_);
        struct Target { Side side; Price price; bool assigned = false; };
        std::array<Target, kMaxLevels * 2> targets{};
        std::size_t count = 0;
        const auto levels = std::min<std::size_t>(settings_.levels, kMaxLevels);
        for (const auto side : {Side::BUY, Side::SELL}) {
            for (std::size_t level = 0; level < levels; ++level) {
                const auto offset = std::max<std::uint64_t>(1, settings_.spread_ticks) + level;
                const auto side_room = side == Side::BUY ? center - low : high - center;
                if (offset > static_cast<std::uint64_t>(side_room)) continue;
                const auto target_ticks = side == Side::BUY ? center - static_cast<Price>(offset) : center + static_cast<Price>(offset);
                targets[count++] = Target{side, target_ticks * instrument->tick_size, false};
            }
        }
        // Retain every desired price, including pending orders. No duplicate targets.
        for (std::size_t i = 0; i < orders_.capacity(); ++i) {
            auto& order = orders_.slot(i);
            if (!order.cl_ord_id) continue;
            for (std::size_t t = 0; t < count; ++t) {
                auto& target = targets[t];
                if (!target.assigned && order.side == target.side && order.working_price() == target.price) {
                    target.assigned = true;
                    if (order.in_flight == InFlightRequest::None && order.leaves_qty != qty)
                        replace(order, target.price, qty, instrument->max_order_qty, now, context);
                    break;
                }
            }
        }
        // Move the unblocked side first, then retry the other side. A valid
        // uncrossed target ladder makes at least one direction safe to move.
        for (int pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < orders_.capacity(); ++i) {
                auto& order = orders_.slot(i);
                if (!order.cl_ord_id || order.in_flight != InFlightRequest::None) continue;
                bool wanted = false;
                for (std::size_t t = 0; t < count; ++t)
                    wanted |= order.side == targets[t].side && order.working_price() == targets[t].price;
                if (wanted) continue;
                std::size_t selected = count;
                bool missing = false;
                for (std::size_t t = 0; t < count; ++t) {
                    if (targets[t].assigned || targets[t].side != order.side) continue;
                    missing = true;
                    if (!would_cross(order.side, targets[t].price)) { selected = t; break; }
                }
                if (selected != count) {
                    targets[selected].assigned = true;
                    replace(order, targets[selected].price, qty, instrument->max_order_qty, now, context);
                } else if (!missing) cancel(order, now, context);
                else dirty_ = true;
            }
        }
        for (std::size_t t = 0; t < count; ++t) {
            const auto& target = targets[t];
            if (!target.assigned && !would_cross(target.side, target.price))
                place(target.side, target.price, qty, TimeInForce::Day, 0, now, context);
            else if (!target.assigned) dirty_ = true;
        }
    }
private:
    [[nodiscard]] bool would_cross(Side side, Price price) const noexcept {
        for (std::size_t i = 0; i < orders_.capacity(); ++i) {
            const auto& order = orders_.slot(i);
            if (!order.cl_ord_id || order.side == side) continue;
            if (side == Side::BUY ? price >= order.working_price() : price <= order.working_price()) return true;
            // Until acknowledgment a replace may have been rejected. Protect
            // both the confirmed and requested quote to avoid self crossing.
            if (order.in_flight == InFlightRequest::Replace &&
                (side == Side::BUY ? price >= order.price : price <= order.price)) return true;
        }
        return false;
    }
    template <BotContext Context> void replace(OwnOrder& order, Price price, Quantity qty, Quantity max_qty,
                                               Timestamp now, Context& context) {
        // Replace quantity is TOTAL. Cancel exhausted total-quantity budgets before replenishing.
        if (order.cum_qty > max_qty - qty) { cancel(order, now, context); dirty_ = true; return; }
        ReplaceOrder command{now, config_.session_id, order.cl_ord_id, 0, price, order.cum_qty + qty};
        order.requested_price = price; order.requested_leaves_qty = qty; order.in_flight = InFlightRequest::Replace;
        ++stats_.commands_submitted; ++stats_.replaces_submitted;
        context.submit(Command{command});
    }
    MarketMakerConfig settings_;
    Rng rng_;
    Price fair_ticks_ = 0;
    Timestamp next_step_ = 0, next_quote_ = 0;
    bool initialized_ = false;
};
bool apply_bot_option(MarketMakerConfig&, std::string_view key, std::string_view value);
} // namespace exsim::sim::bots
#endif
