#ifndef EXSIM_SIM_BOTS_NOISE_TAKER_HPP
#define EXSIM_SIM_BOTS_NOISE_TAKER_HPP
#include "bot_state.hpp"
#include "exsim/sim/random.hpp"
#include <bit>

namespace exsim::sim::bots {
struct NoiseTakerConfig : BotConfig {
    std::uint32_t mean_interval_ms = 100;
    Quantity min_size = 1, max_size = 100;
    std::uint32_t cross_ticks = 2;
    std::uint32_t limit_ppm = 10000;
    std::uint32_t limit_lifetime_ms = 1000;
    Price fair_value_ticks = 0;
    std::size_t order_capacity = 128;
};
class NoiseTaker : public BotState {
public:
    explicit NoiseTaker(NoiseTakerConfig config = {})
        : BotState(config, config.order_capacity), settings_(config), rng_(config.seed) {}
    [[nodiscard]] Timestamp next_arrival() const noexcept { return next_arrival_; }
    template <BotContext Context> void on_timer(Timestamp now, Context& context) {
        const auto* instrument = prepare(context);
        if (!instrument) return;
        if (!enabled_) { cancel_all(now, context); scheduled_ = false; return; }
        for (std::size_t i = 0; i < orders_.capacity(); ++i) {
            auto& order = orders_.slot(i);
            if (order.cl_ord_id && now >= order.expires_at) cancel(order, now, context);
        }
        if (phase_ != Phase::Continuous) { scheduled_ = false; return; }
        if (!settings_.mean_interval_ms) return;
        if (!scheduled_) { next_arrival_ = deadline(now, interval()); scheduled_ = true; }
        // Preserve arrival schedule through sparse caller ticks, but stamp submissions with supplied now.
        while (next_arrival_ != kNever && now >= next_arrival_) {
            next_arrival_ = deadline(next_arrival_, interval());
            const auto low_size = (std::uint64_t{settings_.min_size} + instrument->lot_size - 1) / instrument->lot_size;
            const auto high_size = std::min(settings_.max_size, instrument->max_order_qty) / instrument->lot_size;
            if (!low_size || low_size > high_size) continue;
            const auto qty = static_cast<Quantity>(rng_.between(static_cast<std::int64_t>(low_size), high_size)) * instrument->lot_size;
            const auto side = rng_.below(2) ? Side::BUY : Side::SELL;
            const bool passive = rng_.chance_ppm(settings_.limit_ppm);
            const auto low = instrument->min_price / instrument->tick_size;
            const auto high = instrument->max_price / instrument->tick_size;
            const auto center = std::clamp(settings_.fair_value_ticks ? settings_.fair_value_ticks
                : instrument->reference_price / instrument->tick_size, low, high);
            // Deliberately use a configured reference, not a stale reconstructed BBO.
            // IOC limits reach a band edge, crossing all available opposite liquidity.
            Price ticks = side == Side::BUY ? high : low;
            if (passive) {
                const auto distance = std::max<std::uint32_t>(1, settings_.cross_ticks);
                if (side == Side::BUY) {
                    if (center == low) continue;
                    ticks = center - std::min<Price>(distance, center - low);
                } else {
                    if (center == high) continue;
                    ticks = center + std::min<Price>(distance, high - center);
                }
            }
            place(side, ticks * instrument->tick_size, qty, passive ? TimeInForce::Day : TimeInForce::IOC,
                  passive ? deadline(now, millis_to_nanos(settings_.limit_lifetime_ms)) : kNever, now, context, passive);
        }
    }
private:
    Timestamp interval() noexcept {
        // Integer Q32 inverse exponential CDF. Normalization and the atanh
        // series avoid platform libm rounding differences in arrival timestamps.
        constexpr std::uint64_t one = std::uint64_t{1} << 32;
        constexpr std::uint64_t ln_two = 2977044472;
        const auto random = std::max<std::uint64_t>(1, rng_.next());
        const auto zeros = std::countl_zero(random);
        const auto normalized = (random << zeros) >> 31; // [1, 2) in Q32
        const auto z = ((normalized - one) << 32) / (normalized + one);
        const auto square = (z * z) >> 32;
        auto power = z;
        std::uint64_t logarithm = 0;
        for (std::uint64_t divisor = 1; divisor <= 29; divisor += 2) {
            logarithm += power / divisor;
            power = (power * square) >> 32;
        }
        const auto negative_log = static_cast<std::uint64_t>(zeros + 1) * ln_two - 2 * logarithm;
        const auto mean = millis_to_nanos(settings_.mean_interval_ms);
        const auto fraction = negative_log & (one - 1);
        const auto nanos = mean * (negative_log >> 32) + (mean >> 32) * fraction +
                           (((mean & (one - 1)) * fraction) >> 32);
        return std::max<Timestamp>(1, nanos);
    }
    NoiseTakerConfig settings_;
    Rng rng_;
    Timestamp next_arrival_ = 0;
    bool scheduled_ = false;
};
bool apply_bot_option(NoiseTakerConfig&, std::string_view key, std::string_view value);
} // namespace exsim::sim::bots
#endif
