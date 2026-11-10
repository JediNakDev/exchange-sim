#include "api/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace exsim::api {

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[byte >> 4];
                    out += kHex[byte & 15];
                } else {
                    out += character;
                }
        }
    }
    out += '"';
    return out;
}

const JsonValue* JsonValue::find(std::string_view name) const noexcept {
    for (const auto& member : members)
        if (member.name == name) return &member.value;
    return nullptr;
}

namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse_document() {
        JsonValue value = parse_value(0);
        skip_whitespace();
        if (position_ != text_.size()) error("trailing characters");
        return value;
    }

private:
    [[noreturn]] void error(const std::string& message) const {
        throw JsonError("JSON offset " + std::to_string(position_) + ": " + message);
    }
    void skip_whitespace() noexcept {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r' || text_[position_] == '\t'))
            ++position_;
    }
    bool consume(char expected) noexcept {
        skip_whitespace();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    void expect_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) error("invalid literal");
        position_ += literal.size();
    }

    JsonValue parse_value(int depth) {
        if (depth > 64) error("nesting too deep");
        skip_whitespace();
        if (position_ >= text_.size()) error("unexpected end");
        JsonValue value;
        const char head = text_[position_];
        if (head == '{') {
            ++position_;
            value.kind = JsonValue::Kind::Object;
            if (consume('}')) return value;
            do {
                skip_whitespace();
                if (position_ >= text_.size() || text_[position_] != '"') error("expected key");
                std::string name = parse_string();
                if (!consume(':')) error("expected ':'");
                value.members.push_back(JsonMember{std::move(name), parse_value(depth + 1)});
            } while (consume(','));
            if (!consume('}')) error("expected '}'");
        } else if (head == '[') {
            ++position_;
            value.kind = JsonValue::Kind::Array;
            if (consume(']')) return value;
            do value.items.push_back(parse_value(depth + 1));
            while (consume(','));
            if (!consume(']')) error("expected ']'");
        } else if (head == '"') {
            value.kind = JsonValue::Kind::String;
            value.text = parse_string();
        } else if (head == 't') {
            expect_literal("true");
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = true;
        } else if (head == 'f') {
            expect_literal("false");
            value.kind = JsonValue::Kind::Boolean;
        } else if (head == 'n') {
            expect_literal("null");
        } else {
            value.kind = JsonValue::Kind::Number;
            const std::size_t start = position_;
            if (text_[position_] == '-') ++position_;
            const auto digits = [&] {
                const std::size_t first = position_;
                while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
                return position_ - first;
            };
            if (digits() == 0) error("invalid number");
            if (position_ < text_.size() && text_[position_] == '.') {
                ++position_;
                if (digits() == 0) error("invalid number");
            }
            if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
                ++position_;
                if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
                if (digits() == 0) error("invalid number");
            }
            value.text = std::string(text_.substr(start, position_ - start));
            value.number = std::strtod(value.text.c_str(), nullptr);
        }
        return value;
    }

    std::string parse_string() {
        ++position_;  // opening quote
        std::string out;
        while (true) {
            if (position_ >= text_.size()) error("unterminated string");
            const char character = text_[position_++];
            if (character == '"') return out;
            if (static_cast<unsigned char>(character) < 0x20) error("control character in string");
            if (character != '\\') {
                out += character;
                continue;
            }
            if (position_ >= text_.size()) error("unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (position_ + 4 > text_.size()) error("short unicode escape");
                    unsigned code = 0;
                    const auto result = std::from_chars(text_.data() + position_, text_.data() + position_ + 4, code, 16);
                    if (result.ptr != text_.data() + position_ + 4) error("invalid unicode escape");
                    position_ += 4;
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: error("invalid escape");
            }
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

}  // namespace

JsonValue parse_json(std::string_view text) { return JsonParser(text).parse_document(); }

}  // namespace exsim::api
