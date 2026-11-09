#ifndef EXSIM_FUZZ_TARGETS_BOT_OPTIONS_HPP
#define EXSIM_FUZZ_TARGETS_BOT_OPTIONS_HPP

// Bot configuration options (sim::bots::apply_bot_option).
//
// Input: the first byte picks the bot kind (market maker or noise taker);
// the rest is `key=value` lines applied in order to one configuration.
//
//  * Transactional: a rejected option leaves every field unchanged.
//  * An accepted option keeps every documented range (levels 1..64, sizes
//    and intervals positive, ppm <= 1000000, order_capacity 1..65536, ids
//    non-zero, fair value not negative) and applying it again changes nothing.
//  * The final configuration, rendered as one option per field, rebuilds the
//    identical configuration from the defaults.

#include "fuzz_support.hpp"

#include <sim/bots/market_maker.hpp>
#include <sim/bots/noise_taker.hpp>
#include <string>
#include <utility>
#include <vector>

namespace exsim::fuzz {

namespace bot_options_fuzz {

using sim::bots::BotConfig;
using sim::bots::MarketMakerConfig;
using sim::bots::NoiseTakerConfig;

using RenderedOptions = std::vector<std::pair<std::string, std::string>>;

inline std::string_view stp_option_name(StpMode mode) {
    switch (mode) {
        case StpMode::None: return "none";
        case StpMode::CancelResting: return "cancel_resting";
        case StpMode::CancelAggressor: return "cancel_aggressor";
        case StpMode::CancelBoth: return "cancel_both";
    }
    return "invalid";
}

inline void render_common(const BotConfig& config, RenderedOptions& options) {
    options.emplace_back("name", std::string(config.name.view()));
    if (config.session_id != 0) options.emplace_back("session_id", std::to_string(config.session_id));
    if (config.instrument != 0) options.emplace_back("instrument", std::to_string(config.instrument));
    options.emplace_back("owner", std::to_string(config.owner));
    options.emplace_back("seed", std::to_string(config.seed));
    options.emplace_back("enabled", config.enabled ? "true" : "false");
    options.emplace_back("stp", std::string(stp_option_name(config.stp)));
}

inline RenderedOptions render_options(const MarketMakerConfig& config) {
    RenderedOptions options;
    render_common(config, options);
    options.emplace_back("levels", std::to_string(config.levels));
    options.emplace_back("size", std::to_string(config.size));
    options.emplace_back("spread_ticks", std::to_string(config.spread_ticks));
    options.emplace_back("fair_value_ticks", std::to_string(config.fair_value_ticks));
    options.emplace_back("fair_value_step_ms", std::to_string(config.fair_value_step_ms));
    options.emplace_back("fair_value_step_ticks", std::to_string(config.fair_value_step_ticks));
    options.emplace_back("requote_ms", std::to_string(config.requote_ms));
    options.emplace_back("skew_ticks_per_unit", std::to_string(config.skew_ticks_per_unit));
    options.emplace_back("cancel_on_halt", config.cancel_on_halt ? "true" : "false");
    options.emplace_back("order_capacity", std::to_string(config.order_capacity));
    return options;
}

inline RenderedOptions render_options(const NoiseTakerConfig& config) {
    RenderedOptions options;
    render_common(config, options);
    options.emplace_back("mean_interval_ms", std::to_string(config.mean_interval_ms));
    options.emplace_back("min_size", std::to_string(config.min_size));
    options.emplace_back("max_size", std::to_string(config.max_size));
    options.emplace_back("cross_ticks", std::to_string(config.cross_ticks));
    options.emplace_back("limit_ppm", std::to_string(config.limit_ppm));
    options.emplace_back("limit_lifetime_ms", std::to_string(config.limit_lifetime_ms));
    options.emplace_back("fair_value_ticks", std::to_string(config.fair_value_ticks));
    options.emplace_back("order_capacity", std::to_string(config.order_capacity));
    return options;
}

inline void check_common_ranges(const BotConfig& config, const BotConfig& defaults) {
    EXSIM_FUZZ_REQUIRE(config.name.length <= sim::bots::BotName::kCapacity, "bot names are truncated to capacity");
    EXSIM_FUZZ_REQUIRE(config.session_id != 0 || defaults.session_id == 0, "session_id is never set to zero");
    EXSIM_FUZZ_REQUIRE(config.instrument != 0 || defaults.instrument == 0, "instrument is never set to zero");
    EXSIM_FUZZ_REQUIRE(static_cast<unsigned>(config.stp) <= static_cast<unsigned>(StpMode::CancelBoth),
                       "stp is a known mode");
}

inline void check_ranges(const MarketMakerConfig& config) {
    check_common_ranges(config, MarketMakerConfig{});
    EXSIM_FUZZ_REQUIRE(config.levels >= 1 && config.levels <= sim::bots::MarketMaker::kMaxLevels, "levels 1..64");
    EXSIM_FUZZ_REQUIRE(config.size > 0 && config.spread_ticks > 0 && config.requote_ms > 0,
                       "size, spread and requote interval are positive");
    EXSIM_FUZZ_REQUIRE(config.fair_value_ticks >= 0, "fair value is not negative");
    EXSIM_FUZZ_REQUIRE(config.order_capacity >= 1 && config.order_capacity <= 65536, "order_capacity 1..65536");
}

inline void check_ranges(const NoiseTakerConfig& config) {
    check_common_ranges(config, NoiseTakerConfig{});
    EXSIM_FUZZ_REQUIRE(config.mean_interval_ms > 0 && config.min_size > 0 && config.max_size > 0,
                       "interval and sizes are positive");
    EXSIM_FUZZ_REQUIRE(config.limit_ppm <= 1'000'000, "limit_ppm is ppm");
    EXSIM_FUZZ_REQUIRE(config.fair_value_ticks >= 0, "fair value is not negative");
    EXSIM_FUZZ_REQUIRE(config.order_capacity >= 1 && config.order_capacity <= 65536, "order_capacity 1..65536");
}

template <typename Config>
void apply_option_lines(std::string_view lines) {
    Config config{};
    while (!lines.empty()) {
        const std::size_t newline = lines.find('\n');
        const std::string_view line = lines.substr(0, newline);
        lines = newline == std::string_view::npos ? std::string_view{} : lines.substr(newline + 1);
        const std::size_t equals = line.find('=');
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = equals == std::string_view::npos ? std::string_view{} : line.substr(equals + 1);

        const RenderedOptions before = render_options(config);
        if (!sim::bots::apply_bot_option(config, key, value)) {
            EXSIM_FUZZ_REQUIRE(render_options(config) == before, "a rejected option changes nothing");
            continue;
        }
        check_ranges(config);
        const RenderedOptions after = render_options(config);
        EXSIM_FUZZ_REQUIRE(sim::bots::apply_bot_option(config, key, value), "an accepted option is accepted again");
        EXSIM_FUZZ_REQUIRE(render_options(config) == after, "applying an option twice is idempotent");
    }

    Config rebuilt{};
    for (const auto& [key, value] : render_options(config)) {
        EXSIM_FUZZ_REQUIRE(sim::bots::apply_bot_option(rebuilt, key, value), "a rendered option is accepted: " + key);
    }
    EXSIM_FUZZ_REQUIRE(render_options(rebuilt) == render_options(config), "rendered options rebuild the configuration");
}

}  // namespace bot_options_fuzz

inline int fuzz_bot_options(const std::uint8_t* data, std::size_t size) {
    using namespace bot_options_fuzz;
    if (size == 0) return 0;
    const std::string_view lines = as_text(data + 1, size - 1);
    if ((data[0] & 1) == 0) {
        apply_option_lines<MarketMakerConfig>(lines);
    } else {
        apply_option_lines<NoiseTakerConfig>(lines);
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
