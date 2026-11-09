#ifndef EXSIM_RESEARCH_STRATEGIES_PRICE_GRID_HPP
#define EXSIM_RESEARCH_STRATEGIES_PRICE_GRID_HPP

// Tick-grid arithmetic shared by the baseline strategies. Prices are
// positive fixed-point values; "times two" values are bid + ask sums, so a
// mid between two ticks stays exact.

#include "exsim/instrument.hpp"
#include "exsim/types.hpp"

#include <algorithm>
#include <cstdint>

namespace exsim::research::strategies {

// Largest grid price <= value_times_two / 2.
[[nodiscard]] constexpr Price floor_half_to_tick(std::int64_t value_times_two, Price tick) noexcept {
    const std::int64_t step = 2 * tick;
    const std::int64_t quotient = value_times_two >= 0 ? value_times_two / step : -((-value_times_two + step - 1) / step);
    return quotient * tick;
}

// Smallest grid price >= value_times_two / 2.
[[nodiscard]] constexpr Price ceil_half_to_tick(std::int64_t value_times_two, Price tick) noexcept {
    return -floor_half_to_tick(-value_times_two, tick);
}

[[nodiscard]] constexpr Price clamp_to_band(Price price, const InstrumentConfig& instrument) noexcept {
    return std::clamp(price, instrument.min_price, instrument.max_price);
}

// Largest lot multiple <= qty (and <= limit when limit >= 0).
[[nodiscard]] constexpr Quantity round_down_to_lot(std::int64_t qty, Quantity lot) noexcept {
    if (qty <= 0 || lot == 0) return 0;
    const std::int64_t lots = qty / static_cast<std::int64_t>(lot);
    const std::int64_t rounded = lots * static_cast<std::int64_t>(lot);
    return rounded > static_cast<std::int64_t>(UINT32_MAX) ? (UINT32_MAX / lot) * lot : static_cast<Quantity>(rounded);
}

}  // namespace exsim::research::strategies

#endif
