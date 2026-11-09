#include "api/api_json.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <system_error>

namespace exsim::api {

namespace {

std::uint64_t magnitude_of(std::int64_t value) noexcept {
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
}

constexpr std::int64_t kPowersOfTen[] = {1,
                                         10,
                                         100,
                                         1'000,
                                         10'000,
                                         100'000,
                                         1'000'000,
                                         10'000'000,
                                         100'000'000,
                                         1'000'000'000,
                                         10'000'000'000,
                                         100'000'000'000,
                                         1'000'000'000'000,
                                         10'000'000'000'000,
                                         100'000'000'000'000,
                                         1'000'000'000'000'000,
                                         10'000'000'000'000'000,
                                         100'000'000'000'000'000,
                                         1'000'000'000'000'000'000};

bool rescale(std::int64_t units, int from_digits, int to_digits, std::int64_t& out) noexcept {
    if (to_digits < from_digits || to_digits > 18) return false;
    return !__builtin_mul_overflow(units, kPowersOfTen[to_digits - from_digits], &out);
}

}  // namespace

// ---- ApiJsonWriter ------------------------------------------------------------

void ApiJsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_in_scope_.empty()) {
        if (!first_in_scope_.back()) text_ += ',';
        first_in_scope_.back() = false;
    }
}

ApiJsonWriter& ApiJsonWriter::begin_object() {
    before_value();
    text_ += '{';
    first_in_scope_.push_back(true);
    return *this;
}

ApiJsonWriter& ApiJsonWriter::end_object() {
    text_ += '}';
    first_in_scope_.pop_back();
    return *this;
}

ApiJsonWriter& ApiJsonWriter::begin_array() {
    before_value();
    text_ += '[';
    first_in_scope_.push_back(true);
    return *this;
}

ApiJsonWriter& ApiJsonWriter::end_array() {
    text_ += ']';
    first_in_scope_.pop_back();
    return *this;
}

ApiJsonWriter& ApiJsonWriter::key(std::string_view name) {
    before_value();
    text_ += research::json_escape(name);
    text_ += ':';
    after_key_ = true;
    return *this;
}

ApiJsonWriter& ApiJsonWriter::string(std::string_view value) {
    before_value();
    text_ += research::json_escape(value);
    return *this;
}

ApiJsonWriter& ApiJsonWriter::optional_string(const std::optional<std::string>& value) {
    return value ? string(*value) : null();
}

ApiJsonWriter& ApiJsonWriter::boolean(bool value) {
    before_value();
    text_ += value ? "true" : "false";
    return *this;
}

ApiJsonWriter& ApiJsonWriter::null() {
    before_value();
    text_ += "null";
    return *this;
}

ApiJsonWriter& ApiJsonWriter::integer_number(std::int64_t value) {
    before_value();
    text_ += std::to_string(value);
    return *this;
}

ApiJsonWriter& ApiJsonWriter::unsigned_text(std::uint64_t value) {
    before_value();
    text_ += '"';
    text_ += std::to_string(value);
    text_ += '"';
    return *this;
}

ApiJsonWriter& ApiJsonWriter::signed_text(std::int64_t value) {
    before_value();
    text_ += '"';
    text_ += std::to_string(value);
    text_ += '"';
    return *this;
}

ApiJsonWriter& ApiJsonWriter::raw(std::string_view serialized_json_value) {
    before_value();
    text_ += serialized_json_value;
    return *this;
}

// ---- Fixed point -----------------------------------------------------------

std::string format_scaled_integer(std::int64_t units, int fraction_digits) {
    const std::uint64_t magnitude = magnitude_of(units);
    std::string digits = std::to_string(magnitude);
    if (fraction_digits <= 0) return (units < 0 ? "-" : "") + digits;
    const auto width = static_cast<std::size_t>(fraction_digits);
    if (digits.size() <= width) digits.insert(0, width + 1 - digits.size(), '0');
    digits.insert(digits.size() - width, 1, '.');
    return (units < 0 ? "-" : "") + digits;
}

std::string format_price_units(std::int64_t price_units) { return format_scaled_integer(price_units, 4); }

std::string format_doubled_price_units(std::int64_t doubled_price_units) {
    // value / 2 price units == value * 5 units of 1e-5 currency. Doubled
    // amounts stay far below 2^62 / 5 under the runner's arithmetic bounds.
    const bool negative = doubled_price_units < 0;
    const std::uint64_t hundred_thousandths = magnitude_of(doubled_price_units) * 5;
    std::string digits = std::to_string(hundred_thousandths);
    if (digits.size() <= 5) digits.insert(0, 6 - digits.size(), '0');
    digits.insert(digits.size() - 5, 1, '.');
    return (negative && hundred_thousandths != 0 ? "-" : "") + digits;
}

std::string format_price_units_at_mid_scale(std::int64_t price_units) { return format_scaled_integer(price_units, 4) + "0"; }

std::string format_derived_decimal(double value) {
    if (!std::isfinite(value)) value = 0;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    std::string text(buffer);
    if (text == "-0.000000") text = "0.000000";
    return text;
}

bool parse_unsigned_integer_text(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty() || text.size() > 20) return false;
    if (text.size() > 1 && text.front() == '0') return false;
    for (const char character : text)
        if (character < '0' || character > '9') return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool parse_signed_integer_text(std::string_view text, std::int64_t& value) noexcept {
    const bool negative = !text.empty() && text.front() == '-';
    const std::string_view digits = negative ? text.substr(1) : text;
    std::uint64_t magnitude = 0;
    if (!parse_unsigned_integer_text(digits, magnitude)) return false;
    if (negative && magnitude == 0) return false;
    if (negative) {
        if (magnitude > std::uint64_t{1} << 63) return false;
        value = magnitude == std::uint64_t{1} << 63 ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(magnitude);
        return true;
    }
    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    value = static_cast<std::int64_t>(magnitude);
    return true;
}

std::optional<ScaledDecimal> parse_scaled_decimal(std::string_view text) noexcept {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view integer_part = text.substr(0, dot);
    const std::string_view fraction_part = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (integer_part.empty() || (integer_part.size() > 1 && integer_part.front() == '0')) return std::nullopt;
    if (dot != std::string_view::npos && fraction_part.empty()) return std::nullopt;
    if (integer_part.size() + fraction_part.size() > 18) return std::nullopt;
    std::int64_t units = 0;
    for (const std::string_view part : {integer_part, fraction_part}) {
        for (const char character : part) {
            if (character < '0' || character > '9') return std::nullopt;
            units = units * 10 + (character - '0');
        }
    }
    ScaledDecimal value;
    value.units = negative ? -units : units;
    value.fraction_digits = static_cast<int>(fraction_part.size());
    return value;
}

bool parse_exact_fixed_decimal(std::string_view text, int fraction_digits, bool allow_negative, std::int64_t& units) noexcept {
    if (!allow_negative && !text.empty() && text.front() == '-') return false;
    const auto parsed = parse_scaled_decimal(text);
    if (!parsed || parsed->fraction_digits != fraction_digits) return false;
    units = parsed->units;
    return true;
}

std::string format_scaled_decimal(const ScaledDecimal& value) { return format_scaled_integer(value.units, value.fraction_digits); }

std::optional<ScaledDecimal> subtract_scaled_decimals(const ScaledDecimal& left, const ScaledDecimal& right) noexcept {
    const int digits = std::max(left.fraction_digits, right.fraction_digits);
    std::int64_t left_units = 0;
    std::int64_t right_units = 0;
    if (!rescale(left.units, left.fraction_digits, digits, left_units) ||
        !rescale(right.units, right.fraction_digits, digits, right_units))
        return std::nullopt;
    ScaledDecimal result;
    result.fraction_digits = digits;
    if (__builtin_sub_overflow(left_units, right_units, &result.units)) return std::nullopt;
    return result;
}

int compare_scaled_decimals(const ScaledDecimal& left, const ScaledDecimal& right) noexcept {
    const int digits = std::max(left.fraction_digits, right.fraction_digits);
    std::int64_t left_units = 0;
    std::int64_t right_units = 0;
    if (rescale(left.units, left.fraction_digits, digits, left_units) &&
        rescale(right.units, right.fraction_digits, digits, right_units))
        return left_units < right_units ? -1 : (left_units > right_units ? 1 : 0);
    const double left_value = scaled_decimal_to_double(left);
    const double right_value = scaled_decimal_to_double(right);
    return left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
}

double scaled_decimal_to_double(const ScaledDecimal& value) noexcept {
    return static_cast<double>(value.units) / static_cast<double>(kPowersOfTen[std::min(value.fraction_digits, 18)]);
}

// ---- Misc ---------------------------------------------------------------------

std::int64_t current_unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string format_wall_clock_time(std::int64_t unix_milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(unix_milliseconds / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(unix_milliseconds % 1000));
    return buffer;
}

std::uint64_t fnv1a64_bytes(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char character : bytes) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

namespace {

void serialize_into(const research::JsonValue& value, std::string& out) {
    using Kind = research::JsonValue::Kind;
    switch (value.kind) {
        case Kind::Null: out += "null"; return;
        case Kind::Boolean: out += value.boolean ? "true" : "false"; return;
        case Kind::Number: out += value.text; return;
        case Kind::String: out += research::json_escape(value.text); return;
        case Kind::Array: {
            out += '[';
            for (std::size_t index = 0; index < value.items.size(); ++index) {
                if (index != 0) out += ',';
                serialize_into(value.items[index], out);
            }
            out += ']';
            return;
        }
        case Kind::Object: {
            out += '{';
            for (std::size_t index = 0; index < value.members.size(); ++index) {
                if (index != 0) out += ',';
                out += research::json_escape(value.members[index].name);
                out += ':';
                serialize_into(value.members[index].value, out);
            }
            out += '}';
            return;
        }
    }
}

}  // namespace

std::string serialize_json_value(const research::JsonValue& value) {
    std::string out;
    serialize_into(value, out);
    return out;
}

}  // namespace exsim::api
