#ifndef EXSIM_SIM_PRICE_TEXT_HPP
#define EXSIM_SIM_PRICE_TEXT_HPP

// Decimal text <-> fixed-point Price (4 implied decimals).
//
//   format_price(1002500) == "100.25"   (at least 2 decimals, trailing zeros trimmed)
//   parse_price("100.25") == 1002500    (at most 4 decimals, optional leading '-')
//
// Test, tooling and scenario code only; not used on the hot path.

#include <exsim/types.hpp>

#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <string_view>

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

[[nodiscard]] inline std::optional<Price> parse_price(std::string_view text) {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t whole = 0;
    std::uint64_t fraction = 0;
    int fraction_digits = -1;  // -1 until the decimal point is seen
    bool any_digit = false;
    for (const char ch : text) {
        if (ch == '.') {
            if (fraction_digits >= 0) {
                return std::nullopt;
            }
            fraction_digits = 0;
            continue;
        }
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        any_digit = true;
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (fraction_digits >= 0) {
            if (++fraction_digits > 4) {
                return std::nullopt;
            }
            fraction = fraction * 10 + digit;
        } else {
            if (whole > (std::uint64_t{1} << 59) / 10) {
                return std::nullopt;  // overflow guard (|price| < ~5.7e13 dollars)
            }
            whole = whole * 10 + digit;
        }
    }
    if (!any_digit) {
        return std::nullopt;
    }
    for (int i = fraction_digits < 0 ? 0 : fraction_digits; i < 4; ++i) {
        fraction *= 10;
    }
    const std::uint64_t limit = negative ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << 63)-1;
    if (whole > (limit - fraction) / static_cast<std::uint64_t>(kPriceScale)) return std::nullopt;
    const std::uint64_t magnitude = whole * static_cast<std::uint64_t>(kPriceScale) + fraction;
    if (negative && magnitude == (std::uint64_t{1} << 63)) return std::numeric_limits<Price>::min();
    const Price value = static_cast<Price>(magnitude);
    return negative ? -value : value;
}

}  // namespace exsim::sim

#endif
