#ifndef EXSIM_API_JSON_HPP
#define EXSIM_API_JSON_HPP

// Minimal JSON support for run artifacts (setup/teardown code, allocates).
//
// JsonWriter emits compact, deterministic JSON: keys in call order, no
// whitespace except an optional trailing newline, doubles in shortest
// round-trip form (non-finite doubles become null). JsonValue/parse_json is a
// strict reader for summary/manifest files (used by compare and tests).

#include "types.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::api {

class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& string(std::string_view text);
    JsonWriter& number(double value);  // null when not finite
    JsonWriter& boolean(bool value);
    JsonWriter& null();
    // Exact integers as decimal strings (JSON numbers lose precision past 2^53).
    JsonWriter& integer_text(std::uint64_t value);
    JsonWriter& signed_integer_text(std::int64_t value);
    // Fixed-point price units (1/10000 currency) as a decimal string, e.g. "100.25".
    JsonWriter& price_text(Price value);
    // Twice a price-unit amount, as an exact decimal string with up to 5 decimals.
    JsonWriter& doubled_price_text(std::int64_t value_times_two);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::string take() noexcept { return std::move(text_); }
    void clear() noexcept {
        text_.clear();
        first_in_scope_.clear();
        after_key_ = false;
    }

private:
    void before_value();
    std::string text_;
    std::vector<bool> first_in_scope_;
    bool after_key_ = false;
};

[[nodiscard]] std::string json_escape(std::string_view text);
[[nodiscard]] std::string format_doubled_price(std::int64_t value_times_two);
[[nodiscard]] std::string format_hex64(std::uint64_t value);

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct JsonMember;

struct JsonValue {
    enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;  // string value, or the literal text of a number
    std::vector<JsonValue> items;
    // A named struct, not std::pair: std::pair checks traits of JsonValue while
    // JsonValue is still incomplete, which libstdc++ with clang rejects.
    std::vector<JsonMember> members;

    [[nodiscard]] const JsonValue* find(std::string_view name) const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return kind == Kind::Null; }
};

struct JsonMember {
    std::string name;
    JsonValue value;
};

// Strict RFC 8259 parse of one value (surrounding whitespace allowed). Throws JsonError.
[[nodiscard]] JsonValue parse_json(std::string_view text);

}  // namespace exsim::api

#endif
