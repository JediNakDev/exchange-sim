#include "exsim/api/http_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace exsim::api {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

timeval to_timeval(std::chrono::milliseconds duration) {
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(duration.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((duration.count() % 1000) * 1000);
    return value;
}

void configure_client_socket(int socket_descriptor, const HttpServerOptions& options) {
    const timeval receive_timeout = to_timeval(options.idle_timeout);
    const timeval send_timeout = to_timeval(options.write_timeout);
    (void)setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    (void)setsockopt(socket_descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    const int enabled = 1;
    (void)setsockopt(socket_descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#ifdef SO_NOSIGPIPE
    (void)setsockopt(socket_descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

int status_for(HttpParseStatus status) noexcept {
    switch (status) {
        case HttpParseStatus::HeadersTooLarge: return 431;
        case HttpParseStatus::BodyTooLarge: return 413;
        case HttpParseStatus::NotImplemented: return 501;
        default: return 400;
    }
}

}  // namespace

HttpServer::HttpServer(HttpServerOptions options, HttpRequestHandler handler, HttpRejectionResponder rejection_responder)
    : options_(std::move(options)), handler_(std::move(handler)), rejection_responder_(std::move(rejection_responder)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("invalid IPv4 bind address: " + options_.bind_address);
    listen_descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_descriptor_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    const int enabled = 1;
    (void)setsockopt(listen_descriptor_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::bind(listen_descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = std::strerror(errno);
        ::close(listen_descriptor_);
        listen_descriptor_ = -1;
        throw std::runtime_error("bind " + options_.bind_address + ":" + std::to_string(options_.port) + ": " + message);
    }
    if (::listen(listen_descriptor_, 128) != 0) {
        const std::string message = std::strerror(errno);
        ::close(listen_descriptor_);
        listen_descriptor_ = -1;
        throw std::runtime_error("listen: " + message);
    }
    sockaddr_in bound{};
    socklen_t bound_length = sizeof(bound);
    if (::getsockname(listen_descriptor_, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) bound_port_ = ntohs(bound.sin_port);
    stopping_ = false;
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void HttpServer::stop() {
    if (stopping_.exchange(true)) return;
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_descriptor_ >= 0) {
        ::close(listen_descriptor_);
        listen_descriptor_ = -1;
    }
    std::lock_guard lock(connections_mutex_);
    for (Connection& connection : connections_) ::shutdown(connection.socket_descriptor, SHUT_RDWR);
    for (Connection& connection : connections_) {
        if (connection.thread.joinable()) connection.thread.join();
        ::close(connection.socket_descriptor);
    }
    connections_.clear();
}

void HttpServer::reap_finished_connections() {
    std::lock_guard lock(connections_mutex_);
    for (auto iterator = connections_.begin(); iterator != connections_.end();) {
        if (iterator->finished.load()) {
            if (iterator->thread.joinable()) iterator->thread.join();
            ::close(iterator->socket_descriptor);
            iterator = connections_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void HttpServer::accept_loop() {
    while (!stopping_.load()) {
        pollfd listener{listen_descriptor_, POLLIN, 0};
        const int ready = ::poll(&listener, 1, 100);
        reap_finished_connections();
        if (ready <= 0) continue;
        const int client = ::accept(listen_descriptor_, nullptr, nullptr);
        if (client < 0) continue;
        configure_client_socket(client, options_);
        if (active_connections_.load() >= options_.max_connections) {
            HttpResponse response = rejection_responder_(503, "service_unavailable", "too many concurrent connections; retry shortly");
            response.close_connection = true;
            (void)send_all(client, serialize_http_response(response, false));
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
            continue;
        }
        ++active_connections_;
        std::lock_guard lock(connections_mutex_);
        Connection& connection = connections_.emplace_back();
        connection.socket_descriptor = client;
        connection.thread = std::thread([this, &connection] {
            serve_connection(connection);
            --active_connections_;
            connection.finished = true;
        });
    }
}

bool HttpServer::send_all(int socket_descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(socket_descriptor, bytes.data(), bytes.size(), kSendFlags);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

void HttpServer::serve_connection(Connection& connection) {
    const int socket_descriptor = connection.socket_descriptor;
    std::string buffer;
    std::size_t requests_served = 0;
    char chunk[16 * 1024];
    while (!stopping_.load()) {
        HttpRequest request;
        const HttpParseResult parsed = parse_http_request(buffer, options_.limits, request);
        if (parsed.status == HttpParseStatus::NeedMoreData) {
            const ssize_t received = ::recv(socket_descriptor, chunk, sizeof(chunk), 0);
            if (received > 0) {
                buffer.append(chunk, static_cast<std::size_t>(received));
                continue;
            }
            if (received < 0 && errno == EINTR) continue;
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !buffer.empty()) {
                HttpResponse response = rejection_responder_(408, "invalid_request", "request incomplete before the idle timeout");
                (void)send_all(socket_descriptor, serialize_http_response(response, false));
            }
            return;  // peer closed, timeout or error
        }
        if (parsed.status != HttpParseStatus::Complete) {
            const int status = status_for(parsed.status);
            HttpResponse response = rejection_responder_(status, "invalid_request", parsed.error_message);
            (void)send_all(socket_descriptor, serialize_http_response(response, false));
            return;
        }
        buffer.erase(0, parsed.consumed_bytes);
        ++requests_served;
        HttpResponse response;
        try {
            response = handler_(request);
        } catch (const std::exception& error) {
            response = rejection_responder_(500, "internal_error", error.what());
        }
        const bool keep_alive = request.keep_alive && !response.close_connection && requests_served < options_.max_requests_per_connection &&
                                !stopping_.load();
        if (!send_all(socket_descriptor, serialize_http_response(response, keep_alive)) || !keep_alive) return;
    }
}

}  // namespace exsim::api
