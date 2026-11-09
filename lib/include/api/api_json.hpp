#ifndef EXSIM_API_API_JSON_HPP
#define EXSIM_API_API_JSON_HPP

// JSON output and exact decimal encoding for the exchange management API (HTTP API contract,
// "Units and number encoding").
//
// Exact values are integers in engine fixed point (price units of 1/10000
// currency, shares, nanoseconds) and are rendered as decimal strings with the
// scale the contract requires. Derived statistics are float64 rounded half to
// even to 6 fractional digits. Nothing here is on a simulation hot path.

#include "api/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::api {

// Compact JSON writer with the same call shape as JsonWriter, plus
// raw() to embed an already serialized value (a stored configuration).
class ApiJsonWriter {
public:
    ApiJsonWriter& begin_object();
    ApiJsonWriter& end_object();
    ApiJsonWriter& begin_array();
    ApiJsonWriter& end_array();
    ApiJsonWriter& key(std::string_view name);
    ApiJsonWriter& string(std::string_view text);
    ApiJsonWriter& optional_string(const std::optional<std::string>& text);
    ApiJsonWriter& boolean(bool value);
    ApiJsonWriter& null();
    ApiJsonWriter& integer_number(std::int64_t value);  // small bounded JSON numbers (counts, limits)
    ApiJsonWriter& unsigned_text(std::uint64_t value);  // "123"
    ApiJsonWriter& signed_text(std::int64_t value);     // "-123"
    ApiJsonWriter& raw(std::string_view serialized_json_value);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::string take() noexcept { return std::move(text_); }

private:
    void before_value();
    std::string text_;
    std::vector<bool> first_in_scope_;
    bool after_key_ = false;
};

// ---- Exact fixed-point rendering ------------------------------------------

// `units` with `fraction_digits` implied decimals: (12345, 4) -> "1.2345".
[[nodiscard]] std::string format_scaled_integer(std::int64_t units, int fraction_digits);
// Price units (1/10000 currency): exactly 4 fractional digits ("100.0100").
[[nodiscard]] std::string format_price_units(std::int64_t price_units);
// Twice a price-unit amount: exactly 5 fractional digits ("100.00500").
[[nodiscard]] std::string format_doubled_price_units(std::int64_t doubled_price_units);
// Price units padded to the 5-digit mid/markout scale ("100.00000").
[[nodiscard]] std::string format_price_units_at_mid_scale(std::int64_t price_units);
// float64 rounded half to even to exactly 6 fractional digits; never "-0.000000".
[[nodiscard]] std::string format_derived_decimal(double value);

// ---- Strict parsing of contract scalars -------------------------------------

// ^(0|[1-9][0-9]*)$ within uint64.
[[nodiscard]] bool parse_unsigned_integer_text(std::string_view text, std::uint64_t& value) noexcept;
// ^(0|-?[1-9][0-9]*)$ within int64.
[[nodiscard]] bool parse_signed_integer_text(std::string_view text, std::int64_t& value) noexcept;
// Decimal with exactly `fraction_digits` fractional digits (sign allowed when
// `allow_negative`), as integer units of that scale.
[[nodiscard]] bool parse_exact_fixed_decimal(std::string_view text, int fraction_digits, bool allow_negative,
                                             std::int64_t& units) noexcept;

// An exact decimal (ExactDecimal pattern) as an integer plus its scale.
struct ScaledDecimal {
    std::int64_t units = 0;
    int fraction_digits = 0;
};

// ^-?(0|[1-9][0-9]*)(\.[0-9]+)?$ with at most 18 significant digits.
[[nodiscard]] std::optional<ScaledDecimal> parse_scaled_decimal(std::string_view text) noexcept;
[[nodiscard]] std::string format_scaled_decimal(const ScaledDecimal& value);
// left - right at the larger scale; nullopt on overflow.
[[nodiscard]] std::optional<ScaledDecimal> subtract_scaled_decimals(const ScaledDecimal& left, const ScaledDecimal& right) noexcept;
// -1, 0, 1 (exact).
[[nodiscard]] int compare_scaled_decimals(const ScaledDecimal& left, const ScaledDecimal& right) noexcept;
[[nodiscard]] double scaled_decimal_to_double(const ScaledDecimal& value) noexcept;

// ---- Misc -------------------------------------------------------------------

// RFC 3339 UTC with millisecond precision, e.g. 2026-11-05T10:00:00.000Z.
[[nodiscard]] std::string format_wall_clock_time(std::int64_t unix_milliseconds);
[[nodiscard]] std::int64_t current_unix_milliseconds();

// Lower-case hex of an FNV-1a 64 hash of `bytes`.
[[nodiscard]] std::uint64_t fnv1a64_bytes(std::string_view bytes, std::uint64_t seed = 14695981039346656037ull) noexcept;

// Compact re-serialization of a parsed JSON value (members in their original
// order, number literals verbatim).
[[nodiscard]] std::string serialize_json_value(const JsonValue& value);

}  // namespace exsim::api

#endif
