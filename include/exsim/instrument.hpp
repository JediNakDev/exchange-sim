#ifndef EXSIM_INSTRUMENT_HPP
#define EXSIM_INSTRUMENT_HPP

#include "types.hpp"
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace exsim {

// 8-byte, right-space-padded ASCII symbol (ITCH / OUCH "Stock" field).
struct Symbol {
    std::array<char, 8> chars{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

    Symbol() = default;
    explicit Symbol(std::string_view s) noexcept {
        for (std::size_t i = 0; i < chars.size() && i < s.size(); ++i) {
            chars[i] = s[i];
        }
    }

    // Symbol without the trailing padding.
    [[nodiscard]] std::string_view view() const noexcept {
        std::size_t n = chars.size();
        while (n > 0 && chars[n - 1] == ' ') {
            --n;
        }
        return {chars.data(), n};
    }

    friend bool operator==(const Symbol& a, const Symbol& b) noexcept { return a.chars == b.chars; }
    friend bool operator!=(const Symbol& a, const Symbol& b) noexcept { return !(a == b); }
};

// Static, setup-time configuration of one tradable instrument.
// The [min_price, max_price] band is also the size of the price ladder:
// (max_price - min_price) / tick_size + 1 levels per side.
struct InstrumentConfig {
    Symbol symbol;
    Price tick_size = 100;              // $0.01
    Quantity lot_size = 1;
    Quantity max_order_qty = 1'000'000;
    Price min_price = 100;              // inclusive, multiple of tick_size
    Price max_price = 10'000'000;       // inclusive, multiple of tick_size
    Price reference_price = 1'000'000;  // auction tie-break until first trade
    Phase initial_phase = Phase::Continuous;
};

// Layout-independent validation; engines also check their allocation byte sizes.
[[nodiscard]] inline bool valid_instrument_config(const InstrumentConfig& c) noexcept {
    if (c.tick_size <= 0 || c.lot_size == 0 || c.max_order_qty < c.lot_size ||
        c.min_price <= 0 || c.max_price < c.min_price ||
        c.min_price % c.tick_size != 0 || c.max_price % c.tick_size != 0 ||
        c.reference_price < c.min_price || c.reference_price > c.max_price ||
        c.reference_price % c.tick_size != 0 ||
        static_cast<unsigned>(c.initial_phase) > static_cast<unsigned>(Phase::PreClose)) return false;
    const auto levels = static_cast<std::uint64_t>((c.max_price - c.min_price) / c.tick_size) + 1;
    return levels <= std::numeric_limits<std::size_t>::max();
}

}  // namespace exsim

#endif
