#ifndef EXSIM_API_HTTP_SERVER_HPP
#define EXSIM_API_HTTP_SERVER_HPP

// Small blocking HTTP/1.1 server for the local exchange management API.
//
// One accept thread and one thread per connection, with a hard cap on
// concurrent connections (extra connections get 503 and are closed), socket
// read/write timeouts, bounded request sizes (HttpLimits) and a bounded
// number of requests per keep-alive connection. It binds loopback by default:
// v1 has no remote access control.

#include "api/http_message.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace exsim::api {

struct HttpServerOptions {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 7600;  // 0 = ephemeral (read it back with port())
    std::size_t max_connections = 32;
    std::size_t max_requests_per_connection = 1000;
    std::chrono::milliseconds idle_timeout{10'000};   // waiting for (the rest of) a request
    std::chrono::milliseconds write_timeout{10'000};
    HttpLimits limits;
};

using HttpRequestHandler = std::function<HttpResponse(const HttpRequest&)>;
// Builds the response for a request the server rejected before the handler
// (malformed, too large, over capacity): (HTTP status, error code, message).
using HttpRejectionResponder = std::function<HttpResponse(int, std::string_view, std::string_view)>;

class HttpServer {
public:
    HttpServer(HttpServerOptions options, HttpRequestHandler handler, HttpRejectionResponder rejection_responder);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds and listens, then starts accepting. Throws std::runtime_error,
    // also when already running; a failed start leaves no descriptor or
    // thread behind. start() after stop() binds again.
    void start();
    // Stops accepting, closes every connection and joins all threads. Does
    // nothing when not running. start() and stop() are called by the owner
    // thread only.
    void stop();
    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
    [[nodiscard]] std::size_t active_connection_count() const noexcept { return active_connections_.load(); }

private:
    // One per allowed concurrent connection, allocated by start(). Only the
    // accept thread (and stop(), after joining it) claims, joins and closes a
    // slot, so the slot array needs no lock. The connection thread never
    // closes its descriptor, so stop() cannot shut down a reused one.
    struct ConnectionSlot {
        int socket_descriptor = -1;
        std::thread thread;  // joinable while the slot is in use
        std::atomic<bool> is_finished{false};
    };

    void accept_loop();
    void join_finished_connections();
    void serve_connection(int socket_descriptor);

    HttpServerOptions options_;
    HttpRequestHandler handler_;
    HttpRejectionResponder rejection_responder_;
    int listen_descriptor_ = -1;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> active_connections_{0};
    std::thread accept_thread_;
    std::vector<ConnectionSlot> connection_slots_;
};

}  // namespace exsim::api

#endif
