#include "api/api_json.hpp"

#include <charconv>

namespace exsim::api {

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
    text_ += json_escape(name);
    text_ += ':';
    after_key_ = true;
    return *this;
}

ApiJsonWriter& ApiJsonWriter::string(std::string_view value) {
    before_value();
    text_ += json_escape(value);
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


ApiJsonWriter& ApiJsonWriter::raw(std::string_view serialized_json_value) {
    before_value();
    text_ += serialized_json_value;
    return *this;
}

// ---- Fixed point -----------------------------------------------------------

namespace {

void serialize_into(const JsonValue& value, std::string& out) {
    using Kind = JsonValue::Kind;
    switch (value.kind) {
        case Kind::Null: out += "null"; return;
        case Kind::Boolean: out += value.boolean ? "true" : "false"; return;
        case Kind::Number: out += value.text; return;
        case Kind::String: out += json_escape(value.text); return;
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
                out += json_escape(value.members[index].name);
                out += ':';
                serialize_into(value.members[index].value, out);
            }
            out += '}';
            return;
        }
    }
}

}  // namespace

std::string serialize_json_value(const JsonValue& value) {
    std::string out;
    serialize_into(value, out);
    return out;
}

}  // namespace exsim::api
