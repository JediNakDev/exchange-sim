#include "exsim/api/http_server.hpp"

#include <algorithm>
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

// False when the peer is gone, the write timeout expired or no byte moved.
bool send_all(int socket_descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = ::send(socket_descriptor, bytes.data(), bytes.size(), kSendFlags);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

HttpServer::HttpServer(HttpServerOptions options, HttpRequestHandler handler, HttpRejectionResponder rejection_responder)
    : options_(std::move(options)), handler_(std::move(handler)), rejection_responder_(std::move(rejection_responder)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    if (accept_thread_.joinable()) throw std::runtime_error("HTTP server is already running");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("invalid IPv4 bind address: " + options_.bind_address);
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    try {
        const int enabled = 1;
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const int bind_error = errno;
            throw std::runtime_error("bind " + options_.bind_address + ":" + std::to_string(options_.port) + ": " + std::strerror(bind_error));
        }
        if (::listen(listener, 128) != 0) {
            const int listen_error = errno;
            throw std::runtime_error(std::string("listen: ") + std::strerror(listen_error));
        }
        sockaddr_in bound{};
        socklen_t bound_length = sizeof(bound);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) bound_port_ = ntohs(bound.sin_port);
        connection_slots_ = std::vector<ConnectionSlot>(options_.max_connections);
        listen_descriptor_ = listener;
        stopping_ = false;
        accept_thread_ = std::thread([this] { accept_loop(); });
    } catch (...) {
        ::close(listener);
        listen_descriptor_ = -1;
        throw;
    }
}

void HttpServer::stop() {
    if (!accept_thread_.joinable()) return;
    stopping_ = true;
    accept_thread_.join();
    ::close(listen_descriptor_);
    listen_descriptor_ = -1;
    for (ConnectionSlot& slot : connection_slots_)
        if (slot.thread.joinable()) ::shutdown(slot.socket_descriptor, SHUT_RDWR);
    for (ConnectionSlot& slot : connection_slots_) {
        if (!slot.thread.joinable()) continue;
        slot.thread.join();
        ::close(slot.socket_descriptor);
        slot.socket_descriptor = -1;
    }
}

void HttpServer::join_finished_connections() {
    for (ConnectionSlot& slot : connection_slots_) {
        if (!slot.thread.joinable() || !slot.is_finished.load()) continue;
        slot.thread.join();
        ::close(slot.socket_descriptor);
        slot.socket_descriptor = -1;
    }
}

void HttpServer::accept_loop() {
    while (!stopping_.load()) {
        pollfd listener{listen_descriptor_, POLLIN, 0};
        const int ready = ::poll(&listener, 1, 100);
        join_finished_connections();
        if (ready <= 0) continue;
        const int client = ::accept(listen_descriptor_, nullptr, nullptr);
        if (client < 0) continue;
        configure_client_socket(client, options_);
        const auto free_slot = std::find_if(connection_slots_.begin(), connection_slots_.end(),
                                            [](const ConnectionSlot& slot) { return !slot.thread.joinable(); });
        if (free_slot != connection_slots_.end()) {
            ConnectionSlot& slot = *free_slot;
            slot.socket_descriptor = client;
            slot.is_finished = false;
            ++active_connections_;
            try {
                slot.thread = std::thread([this, &slot] {
                    try {
                        serve_connection(slot.socket_descriptor);
                    } catch (...) {
                        // Allocation failure or a throwing rejection responder drops this connection only.
                    }
                    // Send FIN now instead of when the accept thread next reaps the slot.
                    ::shutdown(slot.socket_descriptor, SHUT_WR);
                    slot.is_finished = true;
                    --active_connections_;
                });
                continue;
            } catch (...) {
                // No thread for this connection: answer it like a full server.
                --active_connections_;
                slot.socket_descriptor = -1;
            }
        }
        try {
            HttpResponse response = rejection_responder_(503, "service_unavailable", "too many concurrent connections; retry shortly");
            response.close_connection = true;
            (void)send_all(client, serialize_http_response(response, false));
        } catch (...) {
            // The connection is closed below either way.
        }
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }
}

void HttpServer::serve_connection(int socket_descriptor) {
    // Bytes from request_start on are not served yet. Serving a request only
    // advances request_start; consumed bytes are dropped once, right before
    // the next read, so pipelined requests cost no per-request memmove.
    std::string received_bytes;
    std::size_t request_start = 0;
    HttpRequest request;
    // Set once the current request's line and headers are parsed, so a body
    // arriving over many reads does not re-parse them.
    bool has_request_head = false;
    std::size_t request_head_bytes = 0;
    std::size_t request_body_bytes = 0;
    std::size_t requests_served = 0;
    char chunk[16 * 1024];
    while (!stopping_.load()) {
        const std::string_view pending = std::string_view(received_bytes).substr(request_start);
        if (!has_request_head) {
            const HttpParseResult head = parse_http_request_head(pending, options_.limits, request);
            if (head.status == HttpParseStatus::Complete) {
                has_request_head = true;
                request_head_bytes = head.consumed_bytes;
                request_body_bytes = head.body_bytes;
            } else if (head.status != HttpParseStatus::NeedMoreData) {
                HttpResponse response = rejection_responder_(status_for(head.status), "invalid_request", head.error_message);
                (void)send_all(socket_descriptor, serialize_http_response(response, false));
                return;
            }
        }
        if (has_request_head && pending.size() - request_head_bytes >= request_body_bytes) {
            request.body.assign(pending.substr(request_head_bytes, request_body_bytes));
            request_start += request_head_bytes + request_body_bytes;
            has_request_head = false;
            ++requests_served;
            HttpResponse response;
            try {
                response = handler_(request);
            } catch (const std::exception& error) {
                response = rejection_responder_(500, "internal_error", error.what());
            } catch (...) {
                response = rejection_responder_(500, "internal_error", "request handler failed");
            }
            const bool keep_alive = request.keep_alive && !response.close_connection &&
                                    requests_served < options_.max_requests_per_connection && !stopping_.load();
            if (!send_all(socket_descriptor, serialize_http_response(response, keep_alive)) || !keep_alive) return;
            request = HttpRequest{};
            continue;
        }

        received_bytes.erase(0, request_start);
        request_start = 0;
        const ssize_t received = ::recv(socket_descriptor, chunk, sizeof(chunk), 0);
        if (received > 0) {
            received_bytes.append(chunk, static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !received_bytes.empty()) {
            HttpResponse response = rejection_responder_(408, "invalid_request", "request incomplete before the idle timeout");
            (void)send_all(socket_descriptor, serialize_http_response(response, false));
        }
        return;  // peer closed, timeout or error
    }
}

}  // namespace exsim::api
