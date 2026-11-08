#ifndef EXSIM_API_HTTP_MESSAGE_HPP
#define EXSIM_API_HTTP_MESSAGE_HPP

// HTTP/1.1 request parsing and response serialization for the research API.
//
// Control-plane code: standard containers are fine, but every input is
// bounded. parse_http_request() works on a byte buffer that may hold a
// partial request, a complete one, or a complete one followed by the start of
// the next (pipelining). It never reads past the limits below, and rejects
// anything it does not understand instead of guessing.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::api {

struct HttpLimits {
    std::size_t max_request_line_bytes = 8 * 1024;
    std::size_t max_header_section_bytes = 16 * 1024;
    std::size_t max_header_count = 64;
    std::size_t max_body_bytes = 1024 * 1024;
};

struct HttpHeader {
    std::string name;  // lower-cased
    std::string value;  // surrounding whitespace removed
};

struct HttpRequest {
    std::string method;
    std::string target;       // raw request target, e.g. /v1/runs?limit=5
    std::string path;         // percent-decoded path without the query
    std::vector<std::pair<std::string, std::string>> query;  // decoded, in order
    std::vector<HttpHeader> headers;
    std::string body;
    bool keep_alive = true;

    // First header with this (lower-case) name.
    [[nodiscard]] const std::string* header(std::string_view lower_case_name) const noexcept;
    // Every value of a query parameter, in order.
    [[nodiscard]] std::vector<std::string_view> query_values(std::string_view name) const;
};

enum class HttpParseStatus : std::uint8_t {
    Complete = 0,          // a request was parsed; `consumed_bytes` belong to it
    NeedMoreData = 1,      // the buffer holds a valid prefix of a request
    BadRequest = 2,        // malformed; respond 400 and close
    HeadersTooLarge = 3,   // respond 431 and close
    BodyTooLarge = 4,      // respond 413 and close
    NotImplemented = 5,    // e.g. Transfer-Encoding; respond 501 and close
};

struct HttpParseResult {
    HttpParseStatus status = HttpParseStatus::NeedMoreData;
    std::size_t consumed_bytes = 0;
    std::size_t body_bytes = 0;  // declared Content-Length of a Complete result
    std::string error_message;  // for the rejection response
};

[[nodiscard]] HttpParseResult parse_http_request(std::string_view buffer, const HttpLimits& limits, HttpRequest& request);

// Parses only the request line and header section, with the same checks and
// rejection order as parse_http_request(). On Complete, `consumed_bytes` ends
// after the blank line, `body_bytes` is the declared body length (already
// checked against max_body_bytes) and `request.body` is empty. A reader that
// receives the body in fragments calls this once per request instead of
// re-parsing the headers on every read.
[[nodiscard]] HttpParseResult parse_http_request_head(std::string_view buffer, const HttpLimits& limits, HttpRequest& request);

// Percent-decodes a path or query component ('+' is a space only in queries).
// Returns nullopt for a malformed escape or a decoded NUL byte.
[[nodiscard]] std::optional<std::string> percent_decode(std::string_view text, bool plus_is_space);

struct HttpResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool close_connection = false;

    HttpResponse& set_header(std::string name, std::string value);
};

[[nodiscard]] std::string_view http_reason_phrase(int status) noexcept;
// Status line, headers (plus Content-Length and Connection), blank line, body.
[[nodiscard]] std::string serialize_http_response(const HttpResponse& response, bool keep_alive);

}  // namespace exsim::api

#endif
