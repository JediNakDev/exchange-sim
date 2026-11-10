#ifndef EXSIM_API_API_JSON_HPP
#define EXSIM_API_API_JSON_HPP

// Compact JSON output for the exchange management API.

#include "api/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::api {

// raw() embeds an already serialized runtime snapshot.
class ApiJsonWriter {
public:
    ApiJsonWriter& begin_object();
    ApiJsonWriter& end_object();
    ApiJsonWriter& begin_array();
    ApiJsonWriter& end_array();
    ApiJsonWriter& key(std::string_view name);
    ApiJsonWriter& string(std::string_view text);
    ApiJsonWriter& null();
    ApiJsonWriter& integer_number(std::int64_t value);  // small bounded JSON numbers (counts, limits)
    ApiJsonWriter& unsigned_text(std::uint64_t value);  // "123"
    ApiJsonWriter& raw(std::string_view serialized_json_value);

    [[nodiscard]] std::string take() noexcept { return std::move(text_); }

private:
    void before_value();
    std::string text_;
    std::vector<bool> first_in_scope_;
    bool after_key_ = false;
};

// Compact re-serialization of a parsed JSON value (members in their original
// order, number literals verbatim).
[[nodiscard]] std::string serialize_json_value(const JsonValue& value);

}  // namespace exsim::api

#endif
