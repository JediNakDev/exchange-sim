#ifndef EXSIM_SIM_BOTS_BOT_COMMON_HPP
#define EXSIM_SIM_BOTS_BOT_COMMON_HPP

// Pieces shared by every simulated-liquidity bot: identity/config, counters,
// time and phase helpers. Plain structs, no virtual base.

#include "exsim/instrument.hpp"
#include "exsim/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace exsim::sim::bots {

inline constexpr Timestamp kNever = std::numeric_limits<Timestamp>::max();
inline constexpr Timestamp kNanosPerMilli = 1'000'000;

[[nodiscard]] constexpr Timestamp millis_to_nanos(std::uint32_t millis) noexcept {
    return Timestamp{millis} * kNanosPerMilli;
}

// Bot name from the `[bot NAME]` config section, stored inline so a bot
// holds no std::string. Longer names are truncated.
struct BotName {
    static constexpr std::size_t kCapacity = 32;

    std::array<char, kCapacity> chars{};
    std::uint8_t length = 0;

    BotName() = default;
    explicit BotName(std::string_view text) noexcept { assign(text); }

    void assign(std::string_view text) noexcept {
        length = static_cast<std::uint8_t>(text.size() < kCapacity ? text.size() : kCapacity);
        for (std::size_t i = 0; i < length; ++i) {
            chars[i] = text[i];
        }
    }

    [[nodiscard]] std::string_view view() const noexcept { return {chars.data(), length}; }
};

// Identity shared by all bot kinds. One bot per session: the bot allocates
// the session's ClOrdIds itself, so the session must not be shared with
// another bot or with a wire client.
struct BotConfig {
    BotName name{};
    SessionId session_id = 0;                    // the bot user's session_id (non-zero)
    OwnerId owner = 0;                           // STP group of the bot user
    StpMode stp = StpMode::CancelResting;        // STP mode put on every order
    InstrumentId instrument = 0;                 // the one instrument the bot trades (non-zero)
    std::uint64_t seed = 1;                      // Rng seed: same seed, same timer calls => same commands
    bool enabled = true;                         // starts active (admin `bots on|off` overrides)
};

[[nodiscard]] constexpr bool is_valid_bot_config(const BotConfig& config) noexcept {
    return config.session_id != 0 && config.instrument != 0;
}

// Instrument fields the bots rely on to produce valid prices and sizes.
[[nodiscard]] constexpr bool is_tradable_by_bots(const InstrumentConfig& instrument) noexcept {
    return instrument.tick_size > 0 && instrument.lot_size > 0 && instrument.max_order_qty >= instrument.lot_size &&
           instrument.min_price > 0 && instrument.min_price <= instrument.max_price &&
           instrument.min_price % instrument.tick_size == 0 && instrument.max_price % instrument.tick_size == 0;
}

// Phases in which a limit DAY order is accepted (SPEC §3).
[[nodiscard]] constexpr bool phase_accepts_limit_orders(Phase phase) noexcept {
    return phase == Phase::PreOpen || phase == Phase::Continuous || phase == Phase::PreClose;
}

// Rounds `price` down to the tick grid and clamps it into the band.
[[nodiscard]] constexpr Price clamp_to_band(Price price, const InstrumentConfig& instrument) noexcept {
    price -= price % instrument.tick_size;
    if (price < instrument.min_price) {
        return instrument.min_price;
    }
    if (price > instrument.max_price) {
        return instrument.max_price;
    }
    return price;
}

struct BotStats {
    std::uint64_t commands_submitted = 0;
    std::uint64_t new_orders_submitted = 0;
    std::uint64_t replaces_submitted = 0;
    std::uint64_t cancels_submitted = 0;
    std::uint64_t mass_cancels_submitted = 0;
    std::uint64_t rejects_received = 0;
    std::uint64_t fills_received = 0;        // own Fill legs (two for a self trade)
    std::uint64_t filled_qty = 0;
    std::uint64_t orders_canceled = 0;       // Canceled events for our orders (any reason)
    std::uint64_t capacity_skips = 0;        // orders not sent because the own-order table was full
};

}  // namespace exsim::sim::bots

#endif
