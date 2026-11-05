#include "exsim/api/http_message.hpp"

#include <charconv>
#include <system_error>

namespace exsim::api {

namespace {

constexpr std::string_view kLineEnd = "\r\n";
constexpr std::string_view kHeaderSectionEnd = "\r\n\r\n";

bool is_token_character(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= '0' && byte <= '9') return true;
    if ((byte | 0x20) >= 'a' && (byte | 0x20) <= 'z') return true;
    switch (character) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~': return true;
        default: return false;
    }
}

bool is_header_value_byte(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
}

bool is_target_byte(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return byte > 0x20 && byte < 0x7f;
}

std::string to_lower_ascii(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return lowered;
}

std::string_view trim_whitespace(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool equals_ignoring_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool connection_header_has_token(std::string_view value, std::string_view token) noexcept {
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        const std::string_view item = trim_whitespace(value.substr(0, comma));
        if (equals_ignoring_case(item, token)) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

int hex_digit_value(char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

HttpParseResult rejection(HttpParseStatus status, std::string message) {
    HttpParseResult result;
    result.status = status;
    result.error_message = std::move(message);
    return result;
}

}  // namespace

const std::string* HttpRequest::header(std::string_view lower_case_name) const noexcept {
    for (const HttpHeader& entry : headers)
        if (entry.name == lower_case_name) return &entry.value;
    return nullptr;
}

std::vector<std::string_view> HttpRequest::query_values(std::string_view name) const {
    std::vector<std::string_view> values;
    for (const auto& [key, value] : query)
        if (key == name) values.emplace_back(value);
    return values;
}

std::optional<std::string> percent_decode(std::string_view text, bool plus_is_space) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '%') {
            if (index + 2 >= text.size()) return std::nullopt;
            const int high = hex_digit_value(text[index + 1]);
            const int low = hex_digit_value(text[index + 2]);
            if (high < 0 || low < 0) return std::nullopt;
            const char byte = static_cast<char>(high * 16 + low);
            if (byte == '\0') return std::nullopt;
            decoded.push_back(byte);
            index += 2;
        } else if (character == '+' && plus_is_space) {
            decoded.push_back(' ');
        } else {
            decoded.push_back(character);
        }
    }
    return decoded;
}

HttpParseResult parse_http_request(std::string_view buffer, const HttpLimits& limits, HttpRequest& request) {
    const std::size_t request_line_end = buffer.find(kLineEnd);
    if (request_line_end == std::string_view::npos) {
        if (buffer.size() > limits.max_request_line_bytes) return rejection(HttpParseStatus::HeadersTooLarge, "request line too long");
        return {};
    }
    if (request_line_end > limits.max_request_line_bytes) return rejection(HttpParseStatus::HeadersTooLarge, "request line too long");

    const std::size_t header_section_end = buffer.find(kHeaderSectionEnd);
    if (header_section_end == std::string_view::npos) {
        if (buffer.size() > limits.max_header_section_bytes) return rejection(HttpParseStatus::HeadersTooLarge, "header section too large");
        return {};
    }
    if (header_section_end + kHeaderSectionEnd.size() > limits.max_header_section_bytes)
        return rejection(HttpParseStatus::HeadersTooLarge, "header section too large");

    request = HttpRequest{};

    // Request line: METHOD SP request-target SP HTTP/1.x
    const std::string_view request_line = buffer.substr(0, request_line_end);
    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0) return rejection(HttpParseStatus::BadRequest, "malformed request line");
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos || second_space == first_space + 1)
        return rejection(HttpParseStatus::BadRequest, "malformed request line");
    const std::string_view method = request_line.substr(0, first_space);
    const std::string_view target = request_line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = request_line.substr(second_space + 1);
    for (const char character : method)
        if (character < 'A' || character > 'Z') return rejection(HttpParseStatus::BadRequest, "malformed method");
    bool http_1_0 = false;
    if (version == "HTTP/1.0") http_1_0 = true;
    else if (version != "HTTP/1.1") return rejection(HttpParseStatus::BadRequest, "unsupported HTTP version");
    if (target.empty() || target.front() != '/') return rejection(HttpParseStatus::BadRequest, "request target must be an absolute path");
    for (const char character : target)
        if (!is_target_byte(character)) return rejection(HttpParseStatus::BadRequest, "invalid byte in request target");

    request.method = std::string(method);
    request.target = std::string(target);
    const std::size_t question_mark = target.find('?');
    const std::string_view raw_path = target.substr(0, question_mark);
    if (raw_path.find('#') != std::string_view::npos) return rejection(HttpParseStatus::BadRequest, "fragment in request target");
    auto decoded_path = percent_decode(raw_path, false);
    if (!decoded_path) return rejection(HttpParseStatus::BadRequest, "malformed percent escape in path");
    request.path = std::move(*decoded_path);
    if (question_mark != std::string_view::npos) {
        std::string_view raw_query = target.substr(question_mark + 1);
        while (!raw_query.empty()) {
            const std::size_t ampersand = raw_query.find('&');
            const std::string_view pair = raw_query.substr(0, ampersand);
            if (!pair.empty()) {
                const std::size_t equals = pair.find('=');
                auto key = percent_decode(pair.substr(0, equals), true);
                auto value = percent_decode(equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1), true);
                if (!key || !value) return rejection(HttpParseStatus::BadRequest, "malformed percent escape in query");
                request.query.emplace_back(std::move(*key), std::move(*value));
            }
            if (ampersand == std::string_view::npos) break;
            raw_query.remove_prefix(ampersand + 1);
        }
    }

    // Header fields.
    std::optional<std::uint64_t> content_length;
    bool connection_close = false;
    bool connection_keep_alive = false;
    std::size_t line_start = request_line_end + kLineEnd.size();
    while (line_start < header_section_end + kLineEnd.size()) {
        const std::size_t line_end = buffer.find(kLineEnd, line_start);
        const std::string_view line = buffer.substr(line_start, line_end - line_start);
        line_start = line_end + kLineEnd.size();
        if (line.empty()) break;
        if (line.front() == ' ' || line.front() == '\t') return rejection(HttpParseStatus::BadRequest, "obsolete header line folding");
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return rejection(HttpParseStatus::BadRequest, "malformed header field");
        const std::string_view name = line.substr(0, colon);
        for (const char character : name)
            if (!is_token_character(character)) return rejection(HttpParseStatus::BadRequest, "malformed header name");
        const std::string_view value = trim_whitespace(line.substr(colon + 1));
        for (const char character : value)
            if (!is_header_value_byte(character)) return rejection(HttpParseStatus::BadRequest, "control character in header value");
        if (request.headers.size() == limits.max_header_count) return rejection(HttpParseStatus::HeadersTooLarge, "too many header fields");
        HttpHeader header{to_lower_ascii(name), std::string(value)};
        if (header.name == "transfer-encoding") return rejection(HttpParseStatus::NotImplemented, "Transfer-Encoding is not supported; send Content-Length");
        if (header.name == "content-length") {
            std::uint64_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || result.ec != std::errc() || result.ptr != value.data() + value.size() || value.front() == '+')
                return rejection(HttpParseStatus::BadRequest, "malformed Content-Length");
            if (content_length && *content_length != parsed) return rejection(HttpParseStatus::BadRequest, "conflicting Content-Length headers");
            content_length = parsed;
        }
        if (header.name == "connection") {
            connection_close |= connection_header_has_token(value, "close");
            connection_keep_alive |= connection_header_has_token(value, "keep-alive");
        }
        request.headers.push_back(std::move(header));
    }

    const std::uint64_t body_length = content_length.value_or(0);
    if (body_length > limits.max_body_bytes) return rejection(HttpParseStatus::BodyTooLarge, "request body too large");
    const std::size_t body_start = header_section_end + kHeaderSectionEnd.size();
    if (buffer.size() - body_start < body_length) return {};
    request.body = std::string(buffer.substr(body_start, static_cast<std::size_t>(body_length)));
    request.keep_alive = http_1_0 ? connection_keep_alive && !connection_close : !connection_close;

    HttpParseResult result;
    result.status = HttpParseStatus::Complete;
    result.consumed_bytes = body_start + static_cast<std::size_t>(body_length);
    return result;
}

HttpResponse& HttpResponse::set_header(std::string name, std::string value) {
    for (auto& [existing_name, existing_value] : headers) {
        if (equals_ignoring_case(existing_name, name)) {
            existing_value = std::move(value);
            return *this;
        }
    }
    headers.emplace_back(std::move(name), std::move(value));
    return *this;
}

std::string_view http_reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

std::string serialize_http_response(const HttpResponse& response, bool keep_alive) {
    std::string text;
    text.reserve(response.body.size() + 256);
    text += "HTTP/1.1 ";
    text += std::to_string(response.status);
    text += ' ';
    text += http_reason_phrase(response.status);
    text += kLineEnd;
    for (const auto& [name, value] : response.headers) {
        text += name;
        text += ": ";
        text += value;
        text += kLineEnd;
    }
    text += "Content-Length: ";
    text += std::to_string(response.body.size());
    text += kLineEnd;
    text += keep_alive && !response.close_connection ? "Connection: keep-alive" : "Connection: close";
    text += kLineEnd;
    text += kLineEnd;
    text += response.body;
    return text;
}

}  // namespace exsim::api
