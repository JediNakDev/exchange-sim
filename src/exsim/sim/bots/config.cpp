#include "exsim/sim/bots/market_maker.hpp"
#include "exsim/sim/bots/noise_taker.hpp"
#include <charconv>

namespace exsim::sim::bots {
namespace {
template <typename T> bool number(T& destination, std::string_view value) {
    if (value.empty()) return false;
    T parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    destination = parsed; return true;
}
bool boolean(bool& destination, std::string_view value) {
    if (value == "true" || value == "1") { destination = true; return true; }
    if (value == "false" || value == "0") { destination = false; return true; }
    return false;
}
bool common(BotConfig& config, std::string_view key, std::string_view value) {
    if (key == "name") { config.name.assign(value); return true; }
    if (key == "session_id") return number(config.session_id, value) && config.session_id != 0;
    if (key == "instrument") return number(config.instrument, value) && config.instrument != 0;
    if (key == "owner") return number(config.owner, value);
    if (key == "seed") return number(config.seed, value);
    if (key == "enabled") return boolean(config.enabled, value);
    if (key == "stp") {
        if (value == "none") config.stp = StpMode::None;
        else if (value == "cancel_resting") config.stp = StpMode::CancelResting;
        else if (value == "cancel_aggressor") config.stp = StpMode::CancelAggressor;
        else if (value == "cancel_both") config.stp = StpMode::CancelBoth;
        else return false;
        return true;
    }
    return false;
}
bool option(MarketMakerConfig& config, std::string_view key, std::string_view value) {
    if (key == "levels") return number(config.levels, value) && config.levels > 0 && config.levels <= MarketMaker::kMaxLevels;
    if (key == "size") return number(config.size, value) && config.size > 0;
    if (key == "spread_ticks") return number(config.spread_ticks, value) && config.spread_ticks > 0;
    if (key == "fair_value_ticks") return number(config.fair_value_ticks, value) && config.fair_value_ticks >= 0;
    if (key == "fair_value_step_ms") return number(config.fair_value_step_ms, value);
    if (key == "fair_value_step_ticks") return number(config.fair_value_step_ticks, value);
    if (key == "requote_ms") return number(config.requote_ms, value) && config.requote_ms > 0;
    if (key == "skew_ticks_per_unit") return number(config.skew_ticks_per_unit, value);
    if (key == "cancel_on_halt") return boolean(config.cancel_on_halt, value);
    if (key == "order_capacity") return number(config.order_capacity, value) && config.order_capacity > 0 && config.order_capacity <= 65536;
    return common(config, key, value);
}
bool option(NoiseTakerConfig& config, std::string_view key, std::string_view value) {
    if (key == "mean_interval_ms") return number(config.mean_interval_ms, value) && config.mean_interval_ms > 0;
    if (key == "min_size") return number(config.min_size, value) && config.min_size > 0;
    if (key == "max_size") return number(config.max_size, value) && config.max_size > 0;
    if (key == "cross_ticks") return number(config.cross_ticks, value);
    if (key == "limit_ppm") return number(config.limit_ppm, value) && config.limit_ppm <= 1000000;
    if (key == "limit_lifetime_ms") return number(config.limit_lifetime_ms, value);
    if (key == "fair_value_ticks") return number(config.fair_value_ticks, value) && config.fair_value_ticks >= 0;
    if (key == "order_capacity") return number(config.order_capacity, value) && config.order_capacity > 0 && config.order_capacity <= 65536;
    return common(config, key, value);
}
} // namespace
bool apply_bot_option(MarketMakerConfig& config, std::string_view key, std::string_view value) {
    auto candidate = config;
    if (!option(candidate, key, value)) return false;
    config = candidate; return true;
}
bool apply_bot_option(NoiseTakerConfig& config, std::string_view key, std::string_view value) {
    auto candidate = config;
    if (!option(candidate, key, value)) return false;
    config = candidate; return true;
}
} // namespace exsim::sim::bots
