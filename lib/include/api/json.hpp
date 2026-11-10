#ifndef EXSIM_API_JSON_HPP
#define EXSIM_API_JSON_HPP

// Strict JSON parsing and string escaping for exchange management.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::api {

[[nodiscard]] std::string json_escape(std::string_view text);

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
};

struct JsonMember {
    std::string name;
    JsonValue value;
};

// Strict RFC 8259 parse of one value (surrounding whitespace allowed). Throws JsonError.
[[nodiscard]] JsonValue parse_json(std::string_view text);

}  // namespace exsim::api

#endif
