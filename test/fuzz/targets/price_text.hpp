#ifndef EXSIM_FUZZ_TARGETS_PRICE_TEXT_HPP
#define EXSIM_FUZZ_TARGETS_PRICE_TEXT_HPP

// Decimal price text <-> fixed-point Price (4 implied decimals). Three
// implementations exist and each is checked against a model of its own
// documented grammar and range:
//
//   sim::parse_price      [+-]? digits with at most one '.', >= 1 digit,
//                         <= 4 decimals ("5." and ".5" allowed); full int64 range
//   server::parse_price   '-'? digits* ('.' digits{1,4})?, not empty;
//                         "rejects anything else, including overflow"
//   fix44::parse_price    '-'? digits+ ('.' digits{1,4})?; full int64 range
//
// The whole input is the text. The first eight bytes also give a Price whose
// formatted forms must parse back to the same value in every parser.

#include "fuzz_support.hpp"

#include <protocol/fix44.hpp>
#include <server/config.hpp>
#include <sim/price_text.hpp>
#include <optional>

namespace exsim::fuzz {

namespace price_text_fuzz {

enum class PriceGrammar { Simulation, Server, Fix };

// A 128-bit model magnitude keeps the model independent of the int64
// arithmetic it checks; __extension__ keeps -Wpedantic builds clean.
__extension__ typedef unsigned __int128 ModelMagnitude;

struct ModelPrice {
    bool negative = false;
    ModelMagnitude magnitude = 0;  // saturates far above the int64 range
};

inline std::optional<ModelPrice> model_parse_shape(std::string_view text, PriceGrammar grammar) {
    ModelPrice result;
    if (!text.empty() && (text.front() == '-' || (grammar == PriceGrammar::Simulation && text.front() == '+'))) {
        result.negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    const bool has_dot = dot != std::string_view::npos;
    auto all_digits = [](std::string_view digits) {
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') return false;
        }
        return true;
    };
    if (!all_digits(whole) || !all_digits(fraction) || fraction.size() > 4) return std::nullopt;
    switch (grammar) {
        case PriceGrammar::Simulation:
            if (whole.empty() && fraction.empty()) return std::nullopt;
            break;
        case PriceGrammar::Server:
            if ((whole.empty() && fraction.empty()) || (has_dot && fraction.empty())) return std::nullopt;
            break;
        case PriceGrammar::Fix:
            if (whole.empty() || (has_dot && fraction.empty())) return std::nullopt;
            break;
    }
    constexpr ModelMagnitude saturation = static_cast<ModelMagnitude>(1) << 100;
    for (const char digit : whole) {
        result.magnitude = result.magnitude * 10 + static_cast<unsigned>(digit - '0');
        if (result.magnitude > saturation) result.magnitude = saturation;
    }
    for (std::size_t index = 0; index < 4; ++index) {
        result.magnitude = result.magnitude * 10 + (index < fraction.size() ? static_cast<unsigned>(fraction[index] - '0') : 0);
        if (result.magnitude > saturation) result.magnitude = saturation;
    }
    return result;
}

inline std::optional<Price> model_parse(std::string_view text, PriceGrammar grammar) {
    const auto shape = model_parse_shape(text, grammar);
    if (!shape) return std::nullopt;
    constexpr auto int64_max = static_cast<ModelMagnitude>(INT64_MAX);
    if (shape->negative) {
        if (shape->magnitude == int64_max + 1) {
            return INT64_MIN;
        }
        if (shape->magnitude > int64_max) return std::nullopt;
        return -static_cast<Price>(shape->magnitude);
    }
    if (shape->magnitude > int64_max) return std::nullopt;
    return static_cast<Price>(shape->magnitude);
}

inline std::optional<Price> server_parse(std::string_view text) {
    Price value = 0;
    if (!server::parse_price(text, value)) return std::nullopt;
    return value;
}

inline std::optional<Price> fix_parse(std::string_view text) {
    Price value = 0;
    if (!protocol::fix44::parse_price(text, value)) return std::nullopt;
    return value;
}

inline std::string fix_format(Price price) {
    char buffer[32];
    const std::size_t length = protocol::fix44::format_price(price, buffer, sizeof buffer);
    EXSIM_FUZZ_REQUIRE(length != 0, "fix44 format_price fits 32 bytes");
    return {buffer, length};
}

// "100.25", "-0.0001", "100" style: integer part, optional 1-4 decimals without a trailing zero.
inline bool is_minimal_decimal(std::string_view text, std::size_t minimum_decimals) {
    if (!text.empty() && text.front() == '-') text.remove_prefix(1);
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    if (whole.empty() || (whole.size() > 1 && whole.front() == '0')) return false;
    if (dot == std::string_view::npos) return minimum_decimals == 0;
    const std::string_view fraction = text.substr(dot + 1);
    if (fraction.size() < std::max<std::size_t>(minimum_decimals, 1) || fraction.size() > 4) return false;
    return fraction.size() <= minimum_decimals || fraction.back() != '0';
}

}  // namespace price_text_fuzz

inline int fuzz_price_text(const std::uint8_t* data, std::size_t size) {
    using namespace price_text_fuzz;
    const std::string_view text = as_text(data, size);

    EXSIM_FUZZ_REQUIRE(sim::parse_price(text) == model_parse(text, PriceGrammar::Simulation),
                       "sim::parse_price follows its grammar and range");
    EXSIM_FUZZ_REQUIRE(fix_parse(text) == model_parse(text, PriceGrammar::Fix),
                       "fix44::parse_price follows its grammar and range");
    EXSIM_FUZZ_REQUIRE(server_parse(text) == model_parse(text, PriceGrammar::Server),
                       "server::parse_price follows its grammar and rejects overflow");

    FuzzByteReader reader(data, size);
    const Price price = reader.take_interesting_int64();

    const std::string simulation_text = sim::format_price(price);
    EXSIM_FUZZ_REQUIRE(is_minimal_decimal(simulation_text, 2), "sim format has 2-4 decimals, trailing zeros trimmed");
    EXSIM_FUZZ_REQUIRE(sim::parse_price(simulation_text) == price, "sim format round trip");
    EXSIM_FUZZ_REQUIRE(fix_parse(simulation_text) == price, "fix44 parses the sim format");

    const std::string fix_text = fix_format(price);
    EXSIM_FUZZ_REQUIRE(is_minimal_decimal(fix_text, 0), "fix44 format is minimal");
    EXSIM_FUZZ_REQUIRE(fix_parse(fix_text) == price, "fix44 format round trip");
    EXSIM_FUZZ_REQUIRE(sim::parse_price(fix_text) == price, "sim parses the fix44 format");

    const std::string server_text = server::format_price(price);
    EXSIM_FUZZ_REQUIRE(server_text == fix_text, "server and fix44 formats agree");
    EXSIM_FUZZ_REQUIRE(server_parse(server_text) == price, "server format round trip across the full int64 range");
    return 0;
}

}  // namespace exsim::fuzz

#endif
