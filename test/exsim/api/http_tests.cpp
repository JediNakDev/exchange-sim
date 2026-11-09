// HTTP/1.1 parsing limits and the socket server's handling of malformed,
// oversized, pipelined and over-capacity connections.

#include "exsim/api/http_message.hpp"
#include "exsim/api/http_server.hpp"
#include "test/framework/alloc_counter.hpp"
#include "test/framework/test.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace exsim::api;

namespace {

HttpParseResult parse_text(std::string_view text, HttpRequest& request, const HttpLimits& limits = {}) {
    return parse_http_request(text, limits, request);
}

// One client connection to 127.0.0.1:port with a receive timeout.
class TestClientConnection {
public:
    explicit TestClientConnection(std::uint16_t port) {
        socket_descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected_ = ::connect(socket_descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        timeval timeout{};
        timeout.tv_sec = 5;
        ::setsockopt(socket_descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const int enabled = 1;  // each send_text() leaves as its own segment, so fragments reach the server apart
        ::setsockopt(socket_descriptor_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    }
    ~TestClientConnection() { ::close(socket_descriptor_); }
    TestClientConnection(const TestClientConnection&) = delete;
    TestClientConnection& operator=(const TestClientConnection&) = delete;

    [[nodiscard]] bool connected() const noexcept { return connected_; }
    void send_text(std::string_view text) const {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const ssize_t written = ::send(socket_descriptor_, text.data() + sent, text.size() - sent, 0);
            if (written <= 0) return;
            sent += static_cast<std::size_t>(written);
        }
    }
    // Reads until the peer closes or the receive timeout expires.
    [[nodiscard]] std::string receive_until_closed() const {
        std::string received;
        char chunk[4096];
        for (;;) {
            const ssize_t count = ::recv(socket_descriptor_, chunk, sizeof(chunk), 0);
            if (count <= 0) return received;
            received.append(chunk, static_cast<std::size_t>(count));
        }
    }
    // Reads until `marker` has appeared `occurrences` times.
    [[nodiscard]] std::string receive_until(std::string_view marker, std::size_t occurrences) const {
        std::string received;
        char chunk[4096];
        const auto count_markers = [&] {
            std::size_t found = 0;
            for (std::size_t at = received.find(marker); at != std::string::npos; at = received.find(marker, at + 1)) ++found;
            return found;
        };
        while (count_markers() < occurrences) {
            const ssize_t count = ::recv(socket_descriptor_, chunk, sizeof(chunk), 0);
            if (count <= 0) break;
            received.append(chunk, static_cast<std::size_t>(count));
        }
        return received;
    }

private:
    int socket_descriptor_ = -1;
    bool connected_ = false;
};

HttpResponse echo_path_handler(const HttpRequest& request) {
    HttpResponse response;
    response.body = request.method + " " + request.path + " body=" + std::to_string(request.body.size());
    return response;
}

HttpResponse echo_body_handler(const HttpRequest& request) {
    HttpResponse response;
    response.body = "[" + request.method + " " + request.path + " body=" + request.body + "]";
    return response;
}

HttpResponse plain_rejection(int status, std::string_view code, std::string_view) {
    HttpResponse response;
    response.status = status;
    response.body = std::string(code);
    response.close_connection = true;
    return response;
}

std::size_t count_open_descriptors() {
    std::size_t open_descriptors = 0;
    for (int descriptor = 0; descriptor < 4096; ++descriptor)
        if (::fcntl(descriptor, F_GETFD) != -1) ++open_descriptors;
    return open_descriptors;
}

// Retries until the server answers `path` on a fresh connection; a closed
// connection frees its slot only after the server joins its thread.
bool eventually_answers(std::uint16_t port, const std::string& path) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        TestClientConnection client(port);
        client.send_text("GET " + path + " HTTP/1.1\r\nConnection: close\r\n\r\n");
        if (client.receive_until_closed().find("GET " + path + " body=0") != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool wait_for_active_connections(const HttpServer& server, std::size_t expected) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (server.active_connection_count() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST_CASE(api_http_parse, complete_request_with_query_and_body) {
    HttpRequest request;
    const auto result = parse_text(
        "POST /v1/runs%2Fx?state=queued,paused&masterSeed=7&name=a+b%21 HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n"
        "Idempotency-Key:  key-12345678 \r\n\r\nbodyNEXT",
        request);
    REQUIRE(result.status == HttpParseStatus::Complete);
    CHECK_EQ(request.method, std::string("POST"));
    CHECK_EQ(request.path, std::string("/v1/runs/x"));
    CHECK_EQ(request.body, std::string("body"));
    CHECK_EQ(result.consumed_bytes, std::string_view(
        "POST /v1/runs%2Fx?state=queued,paused&masterSeed=7&name=a+b%21 HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n"
        "Idempotency-Key:  key-12345678 \r\n\r\nbody").size());
    REQUIRE(request.header("idempotency-key") != nullptr);
    CHECK_EQ(*request.header("idempotency-key"), std::string("key-12345678"));
    REQUIRE_EQ(request.query_values("state").size(), 1u);
    CHECK_EQ(std::string(request.query_values("state").front()), std::string("queued,paused"));
    CHECK_EQ(std::string(request.query_values("name").front()), std::string("a b!"));
    CHECK(request.keep_alive);
}

TEST_CASE(api_http_parse, every_prefix_needs_more_data) {
    const std::string text = "GET /v1/service HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
    for (std::size_t length = 0; length < text.size(); ++length) {
        HttpRequest request;
        const auto result = parse_text(std::string_view(text).substr(0, length), request);
        CHECK(result.status == HttpParseStatus::NeedMoreData);
    }
    HttpRequest request;
    CHECK(parse_text(text, request).status == HttpParseStatus::Complete);
}

TEST_CASE(api_http_parse, malformed_requests_are_rejected) {
    const char* malformed[] = {
        "HELLO\r\n\r\n",
        "get /v1/service HTTP/1.1\r\n\r\n",
        "GET /v1/service HTTP/2.0\r\n\r\n",
        "GET v1/service HTTP/1.1\r\n\r\n",
        "GET /v1/%zz HTTP/1.1\r\n\r\n",
        "GET /v1/%00 HTTP/1.1\r\n\r\n",
        "GET /v1/service#frag HTTP/1.1\r\n\r\n",
        "GET /v1/service?x=%g1 HTTP/1.1\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nNoColon\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nBad Name: x\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nX: a\r\n folded\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "GET /v1/service HTTP/1.1\r\nX: a\x01b\r\n\r\n",
    };
    for (const char* text : malformed) {
        HttpRequest request;
        const auto result = parse_text(text, request);
        CHECK(result.status == HttpParseStatus::BadRequest);
        CHECK(!result.error_message.empty());
    }
    HttpRequest request;
    CHECK(parse_text("POST /v1/runs HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", request).status == HttpParseStatus::NotImplemented);
}

TEST_CASE(api_http_parse, limits_are_enforced_before_reading_everything) {
    HttpLimits limits;
    limits.max_request_line_bytes = 64;
    limits.max_header_section_bytes = 256;
    limits.max_header_count = 3;
    limits.max_body_bytes = 10;
    HttpRequest request;
    // A request line that never ends is rejected once it passes the limit.
    CHECK(parse_text("GET /" + std::string(100, 'a'), request, limits).status == HttpParseStatus::HeadersTooLarge);
    CHECK(parse_text("GET / HTTP/1.1\r\nX: " + std::string(300, 'a'), request, limits).status == HttpParseStatus::HeadersTooLarge);
    CHECK(parse_text("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n", request, limits).status == HttpParseStatus::HeadersTooLarge);
    // The body limit applies to the declared length, before the body arrives.
    CHECK(parse_text("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", request, limits).status == HttpParseStatus::BodyTooLarge);
    CHECK(parse_text("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n", request, limits).status != HttpParseStatus::Complete);
}

TEST_CASE(api_http_parse, connection_close_and_http_1_0) {
    HttpRequest request;
    REQUIRE(parse_text("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", request).status == HttpParseStatus::Complete);
    CHECK(!request.keep_alive);
    HttpRequest legacy_request;
    REQUIRE(parse_text("GET / HTTP/1.0\r\n\r\n", legacy_request).status == HttpParseStatus::Complete);
    CHECK(!legacy_request.keep_alive);
}

TEST_CASE(api_http_parse, response_serialization) {
    HttpResponse response;
    response.status = 404;
    response.body = "{}";
    response.set_header("Content-Type", "application/json");
    response.set_header("Content-Type", "text/plain");  // replaces
    const std::string text = serialize_http_response(response, true);
    CHECK_EQ(text.rfind("HTTP/1.1 404 Not Found\r\n", 0), 0u);
    CHECK(text.find("Content-Type: text/plain\r\n") != std::string::npos);
    CHECK(text.find("application/json") == std::string::npos);
    CHECK(text.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(text.find("\r\n\r\n{}") != std::string::npos);
    CHECK(serialize_http_response(response, false).find("Connection: close\r\n") != std::string::npos);
}

TEST_CASE(api_http_server, serves_pipelined_requests_and_rejects_garbage) {
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, echo_path_handler, plain_rejection);
    server.start();
    REQUIRE(server.port() != 0);
    {
        TestClientConnection client(server.port());
        REQUIRE(client.connected());
        client.send_text("GET /one HTTP/1.1\r\n\r\nPOST /two HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /three HTTP/1.1\r\nConnection: close\r\n\r\n");
        const std::string received = client.receive_until_closed();
        const std::size_t first = received.find("GET /one body=0");
        const std::size_t second = received.find("POST /two body=3");
        const std::size_t third = received.find("GET /three body=0");
        CHECK(first != std::string::npos);
        CHECK(second != std::string::npos && second > first);
        CHECK(third != std::string::npos && third > second);
    }
    {
        TestClientConnection client(server.port());
        client.send_text("\x16\x03\x01 not http at all\r\n\r\n");
        const std::string received = client.receive_until_closed();
        CHECK_EQ(received.rfind("HTTP/1.1 400", 0), 0u);
        CHECK(received.find("invalid_request") != std::string::npos);
    }
    {
        TestClientConnection client(server.port());
        client.send_text("POST /x HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n");
        CHECK_EQ(client.receive_until_closed().rfind("HTTP/1.1 413", 0), 0u);
    }
    {
        TestClientConnection client(server.port());
        client.send_text("GET /x HTTP/1.1\r\nX: " + std::string(20'000, 'a') + "\r\n\r\n");
        CHECK_EQ(client.receive_until_closed().rfind("HTTP/1.1 431", 0), 0u);
    }
    // The server still answers after all of the above.
    TestClientConnection client(server.port());
    client.send_text("GET /alive HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(client.receive_until_closed().find("GET /alive") != std::string::npos);
    server.stop();
}

TEST_CASE(api_http_server, idle_connection_times_out_and_capacity_is_bounded) {
    HttpServerOptions options;
    options.port = 0;
    options.max_connections = 1;
    options.idle_timeout = std::chrono::milliseconds(300);
    HttpServer server(options, echo_path_handler, plain_rejection);
    server.start();
    {
        TestClientConnection holder(server.port());
        holder.send_text("GET /partial HTTP/1.1\r\n");  // never completed
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestClientConnection rejected(server.port());
        CHECK_EQ(rejected.receive_until_closed().rfind("HTTP/1.1 503", 0), 0u);
        const auto started = std::chrono::steady_clock::now();
        const std::string received = holder.receive_until_closed();  // closed by the idle timeout
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(4));
        CHECK(received.find("/partial body") == std::string::npos);
    }
    // The slot is free again once the idle connection was reaped.
    bool answered = false;
    for (int attempt = 0; attempt < 20 && !answered; ++attempt) {
        TestClientConnection client(server.port());
        client.send_text("GET /after HTTP/1.1\r\nConnection: close\r\n\r\n");
        answered = client.receive_until_closed().find("GET /after") != std::string::npos;
        if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(answered);
    server.stop();
}

TEST_CASE(api_http_server, stop_closes_open_keep_alive_connections) {
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, echo_path_handler, plain_rejection);
    server.start();
    TestClientConnection client(server.port());
    client.send_text("GET /kept HTTP/1.1\r\n\r\n");
    CHECK(client.receive_until("GET /kept body=0", 1).find("GET /kept body=0") != std::string::npos);
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    CHECK(client.receive_until_closed().empty());
}

TEST_CASE(api_http_parse, head_parse_stops_before_the_body) {
    const std::string head = "POST /v1/runs HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
    for (std::size_t length = 0; length < head.size(); ++length) {
        HttpRequest request;
        CHECK(parse_http_request_head(std::string_view(head).substr(0, length), HttpLimits{}, request).status ==
              HttpParseStatus::NeedMoreData);
    }
    for (const std::string& buffer : {head, head + "ab", head + "abcdeGET"}) {
        HttpRequest request;
        const auto result = parse_http_request_head(buffer, HttpLimits{}, request);
        REQUIRE(result.status == HttpParseStatus::Complete);
        CHECK_EQ(result.consumed_bytes, head.size());
        CHECK_EQ(result.body_bytes, 5u);
        CHECK(request.body.empty());
        CHECK_EQ(request.path, std::string("/v1/runs"));
    }
    HttpRequest request;
    const auto full = parse_http_request(head + "abcdeGET", HttpLimits{}, request);
    REQUIRE(full.status == HttpParseStatus::Complete);
    CHECK_EQ(full.consumed_bytes, head.size() + 5);
    CHECK_EQ(request.body, std::string("abcde"));
}

TEST_CASE(api_http_parse, header_errors_take_precedence_over_body_size) {
    HttpLimits limits;
    limits.max_body_bytes = 10;
    const std::pair<const char*, HttpParseStatus> cases[] = {
        {"POST / HTTP/1.1\r\nContent-Length: 11\r\nTransfer-Encoding: chunked\r\n\r\n", HttpParseStatus::NotImplemented},
        {"POST / HTTP/1.1\r\nContent-Length: 11\r\nBad Name: x\r\n\r\n", HttpParseStatus::BadRequest},
        {"POST / HTTP/1.1\r\nContent-Length: 11\r\nContent-Length: 12\r\n\r\n", HttpParseStatus::BadRequest},
        {"POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", HttpParseStatus::BodyTooLarge},
    };
    for (const auto& [text, expected] : cases) {
        HttpRequest head_request;
        HttpRequest full_request;
        CHECK(parse_http_request_head(text, limits, head_request).status == expected);
        CHECK(parse_http_request(text, limits, full_request).status == expected);
    }
}

TEST_CASE(api_http_server, fragmented_bodies_keep_pipeline_boundaries) {
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, echo_body_handler, plain_rejection);
    server.start();
    const std::string pipeline =
        "POST /a HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789"
        "GET /b HTTP/1.1\r\n\r\n"
        "POST /c HTTP/1.1\r\nContent-Length: 3\r\n\r\nxyz"
        "POST /d HTTP/1.1\r\nContent-Length: 4\r\nConnection: close\r\n\r\nlast";
    {
        // Splits inside the body, at a request boundary and inside a header name.
        TestClientConnection client(server.port());
        std::size_t offset = 0;
        for (const std::size_t split : {std::size_t{43}, std::size_t{50}, std::size_t{70}, std::size_t{90}, std::size_t{130}, pipeline.size()}) {
            client.send_text(std::string_view(pipeline).substr(offset, split - offset));
            offset = split;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const std::string received = client.receive_until_closed();
        const std::size_t first = received.find("[POST /a body=0123456789]");
        const std::size_t second = received.find("[GET /b body=]");
        const std::size_t third = received.find("[POST /c body=xyz]");
        const std::size_t fourth = received.find("[POST /d body=last]");
        CHECK(first != std::string::npos);
        CHECK(second != std::string::npos && second > first);
        CHECK(third != std::string::npos && third > second);
        CHECK(fourth != std::string::npos && fourth > third);
    }
    {
        TestClientConnection client(server.port());
        for (const char byte : pipeline) client.send_text(std::string_view(&byte, 1));
        const std::string received = client.receive_until_closed();
        CHECK(received.find("[POST /a body=0123456789]") != std::string::npos);
        CHECK(received.find("[GET /b body=]") != std::string::npos);
        CHECK(received.find("[POST /c body=xyz]") != std::string::npos);
        CHECK(received.find("[POST /d body=last]") != std::string::npos);
    }
    server.stop();
}

TEST_CASE(api_http_server, oversized_and_malformed_heads_are_rejected_before_the_body) {
    HttpServerOptions options;
    options.port = 0;
    options.idle_timeout = std::chrono::milliseconds(5'000);
    options.limits.max_body_bytes = 1024;
    HttpServer server(options, echo_body_handler, plain_rejection);
    server.start();
    const auto started = std::chrono::steady_clock::now();
    {
        TestClientConnection client(server.port());
        client.send_text("POST /big HTTP/1.1\r\nContent-Le");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        client.send_text("ngth: 1025\r\n\r\n0123");  // most of the declared body never arrives
        CHECK_EQ(client.receive_until_closed().rfind("HTTP/1.1 413", 0), 0u);
    }
    {
        TestClientConnection client(server.port());
        client.send_text("POST /bad HTTP/1.1\r\nContent-Length: 5000\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        client.send_text("Transfer-Encoding: chunked\r\n\r\n");
        CHECK_EQ(client.receive_until_closed().rfind("HTTP/1.1 501", 0), 0u);
    }
    {
        // A served request followed by a malformed one on the same connection.
        TestClientConnection client(server.port());
        client.send_text("POST /ok HTTP/1.1\r\nContent-Length: 2\r\n\r\nhiGET /x HTTP/1.1\r\nBad Name: y\r\n\r\n");
        const std::string received = client.receive_until_closed();
        const std::size_t served = received.find("[POST /ok body=hi]");
        const std::size_t rejected = received.find("HTTP/1.1 400");
        CHECK(served != std::string::npos);
        CHECK(rejected != std::string::npos && rejected > served);
    }
    // Answered on arrival of the head, not after the 5 s idle timeout.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    server.stop();
}

// The connection thread is the only one that reads the request, so the
// handler of a first request opens an allocation scope on that thread and
// the handler of the next request on the same connection reads it: the count
// covers receiving and parsing the second request.
TEST_CASE(api_http_server, fragmented_body_parses_the_head_once) {
    if (!testfw::allocation_counting_available()) return;
    struct ConnectionThreadMeasurement {
        std::optional<testfw::AllocationScope> scope;
        std::atomic<std::size_t> allocations_while_reading{0};
    } measurement;
    const auto measuring_handler = [&measurement](const HttpRequest& request) {
        if (request.path == "/start") {
            measurement.scope.emplace();
        } else {
            measurement.allocations_while_reading = measurement.scope->count();
            measurement.scope.reset();
        }
        HttpResponse response;
        response.body = "done";
        return response;
    };
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, measuring_handler, plain_rejection);
    server.start();
    TestClientConnection client(server.port());
    const auto allocations_reading_upload = [&](std::size_t body_bytes) {
        client.send_text("GET /start HTTP/1.1\r\n\r\n");
        (void)client.receive_until("done", 1);
        // A header value longer than the small-string buffer, as browsers send.
        client.send_text("POST /upload HTTP/1.1\r\nContent-Type: application/octet-stream\r\n"
                         "User-Agent: exsim-http-tests fragmented body allocation regression\r\nContent-Length: " +
                         std::to_string(body_bytes) + "\r\n\r\n" + std::string(body_bytes, 'b'));
        (void)client.receive_until("done", 1);
        return measurement.allocations_while_reading.load();
    };
    const std::size_t small_upload_allocations = allocations_reading_upload(16);
    // 512 KiB arrives over at least 32 reads of the server's 16 KiB chunk.
    const std::size_t large_upload_allocations = allocations_reading_upload(512 * 1024);
    // Re-parsing the head on every read would add several allocations per
    // read; only receive-buffer doublings and the body copy remain.
    CHECK_LE(large_upload_allocations, small_upload_allocations + 16);
    server.stop();
}

TEST_CASE(api_http_server, connection_slots_are_bounded_and_reused) {
    HttpServerOptions options;
    options.port = 0;
    options.max_connections = 2;
    HttpServer server(options, echo_path_handler, plain_rejection);
    server.start();
    for (int round = 0; round < 4; ++round) {
        {
            TestClientConnection first(server.port());
            TestClientConnection second(server.port());
            first.send_text("GET /first HTTP/1.1\r\n\r\n");
            second.send_text("GET /second HTTP/1.1\r\n\r\n");
            CHECK(first.receive_until("GET /first body=0", 1).find("GET /first body=0") != std::string::npos);
            CHECK(second.receive_until("GET /second body=0", 1).find("GET /second body=0") != std::string::npos);
            CHECK_EQ(server.active_connection_count(), 2u);
            TestClientConnection over_capacity(server.port());
            CHECK_EQ(over_capacity.receive_until_closed().rfind("HTTP/1.1 503", 0), 0u);
        }
        CHECK(wait_for_active_connections(server, 0));
    }
    CHECK(eventually_answers(server.port(), "/after-rounds"));
    server.stop();
    CHECK_EQ(server.active_connection_count(), 0u);
}

TEST_CASE(api_http_server, restart_and_failed_start_leave_no_descriptors) {
    const std::size_t descriptors_before = count_open_descriptors();
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, echo_path_handler, plain_rejection);
    server.start();
    const std::uint16_t first_port = server.port();
    const std::size_t descriptors_running = count_open_descriptors();
    bool duplicate_start_threw = false;
    try {
        server.start();
    } catch (const std::runtime_error&) {
        duplicate_start_threw = true;
    }
    CHECK(duplicate_start_threw);
    CHECK_EQ(count_open_descriptors(), descriptors_running);

    HttpServerOptions taken_port_options;
    taken_port_options.port = first_port;
    HttpServer second_server(taken_port_options, echo_path_handler, plain_rejection);
    bool failed_start_threw = false;
    try {
        second_server.start();
    } catch (const std::runtime_error&) {
        failed_start_threw = true;
    }
    CHECK(failed_start_threw);
    CHECK_EQ(count_open_descriptors(), descriptors_running);
    second_server.stop();  // nothing to stop
    CHECK(eventually_answers(first_port, "/still-running"));

    server.stop();
    server.stop();
    CHECK_EQ(count_open_descriptors(), descriptors_before);
    server.start();
    CHECK(eventually_answers(server.port(), "/restarted"));
    server.stop();
    // The failed server can start once the port is free.
    second_server.start();
    CHECK(eventually_answers(first_port, "/second-server"));
    second_server.stop();
    CHECK_EQ(count_open_descriptors(), descriptors_before);
}

TEST_CASE(api_http_server, throwing_handler_or_responder_drops_one_connection_only) {
    const auto throwing_handler = [](const HttpRequest& request) -> HttpResponse {
        if (request.path == "/throw-non-standard") throw 7;
        return echo_path_handler(request);
    };
    HttpServerOptions options;
    options.port = 0;
    HttpServer server(options, throwing_handler, plain_rejection);
    server.start();
    {
        TestClientConnection client(server.port());
        client.send_text("GET /throw-non-standard HTTP/1.1\r\nConnection: close\r\n\r\n");
        const std::string received = client.receive_until_closed();
        CHECK_EQ(received.rfind("HTTP/1.1 500", 0), 0u);
        CHECK(received.find("internal_error") != std::string::npos);
    }
    server.stop();

    const auto throwing_responder = [](int, std::string_view, std::string_view) -> HttpResponse {
        throw std::runtime_error("responder failed");
    };
    HttpServer fragile_server(options, echo_path_handler, throwing_responder);
    fragile_server.start();
    {
        TestClientConnection client(fragile_server.port());
        client.send_text("not http\r\n\r\n");
        CHECK(client.receive_until_closed().empty());
    }
    CHECK(eventually_answers(fragile_server.port(), "/survived"));
    CHECK(wait_for_active_connections(fragile_server, 0));
    fragile_server.stop();
}
