#ifndef EXSIM_SIM_PRICE_TEXT_HPP
#define EXSIM_SIM_PRICE_TEXT_HPP

// Fixed-point Price (4 implied decimals) to decimal text.
//
//   format_price(1002500) == "100.25"   (at least 2 decimals, trailing zeros trimmed)
//
// Test, tooling and scenario code only; not used on the hot path.

#include <types.hpp>

#include <cstdint>
#include <string>

namespace exsim::sim {

[[nodiscard]] inline std::string format_price(Price price) {
    const bool negative = price < 0;
    // Unsigned negation is well defined for every int64 value, including INT64_MIN.
    const std::uint64_t magnitude =
        negative ? std::uint64_t{0} - static_cast<std::uint64_t>(price) : static_cast<std::uint64_t>(price);
    const std::uint64_t scale = static_cast<std::uint64_t>(kPriceScale);
    std::string fraction = std::to_string(magnitude % scale);
    fraction.insert(0, 4 - fraction.size(), '0');
    while (fraction.size() > 2 && fraction.back() == '0') {
        fraction.pop_back();
    }
    return (negative ? "-" : "") + std::to_string(magnitude / scale) + "." + fraction;
}


}  // namespace exsim::sim

#endif
