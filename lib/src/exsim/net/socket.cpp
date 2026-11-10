#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <charconv>
#include <net/socket.hpp>
#include <utility>

namespace exsim::net {
namespace {
ErrorCode system_call_error(int return_code) noexcept {
    return return_code < 0 ? errno : 0;
}
sockaddr_in socket_address_from_endpoint(Ipv4Endpoint endpoint_value) noexcept {
    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_addr.s_addr = htonl(endpoint_value.address);
    socket_address.sin_port = htons(endpoint_value.port);
    return socket_address;
}
Ipv4Endpoint endpoint_from_socket_address(const sockaddr_in& socket_address) noexcept {
    return {ntohl(socket_address.sin_addr.s_addr), ntohs(socket_address.sin_port)};
}
ErrorCode set_socket_option(int fd, int level, int name, int value) noexcept {
    return system_call_error(setsockopt(fd, level, name, &value, sizeof(value)));
}
ErrorCode configure_socket(int fd) noexcept {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return errno;
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return errno;
    const auto error = set_non_blocking(fd);
    return error ? error : set_no_sigpipe(fd);
}
ErrorCode open_socket(int type, Socket& output) noexcept {
    Socket next(::socket(AF_INET, type, 0));
    if (!next.is_open()) return errno;
    const auto error = configure_socket(next.fd());
    if (error) return error;
    output = std::move(next);
    return 0;
}
}  // namespace
void Socket::close() noexcept {
    if (fd_ >= 0) {
        const int fd = release();
        ::close(fd);
    }
}
bool parse_ipv4_address(std::string_view text, std::uint32_t& output) noexcept {
    std::uint32_t value = 0;
    for (int part = 0; part < 4; ++part) {
        const auto end = text.find('.');
        const auto word = text.substr(0, end);
        if (word.empty()) return false;
        unsigned octet = 0;
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), octet);
        if (parsed.ec != std::errc{} || parsed.ptr != word.data() + word.size() || octet > 255) return false;
        value = (value << 8) | octet;
        if (part == 3) {
            if (end != text.npos) return false;
        } else {
            if (end == text.npos) return false;
            text.remove_prefix(end + 1);
        }
    }
    output = value;
    return true;
}
std::size_t format_ipv4_endpoint(Ipv4Endpoint endpoint_value, std::span<char> output) noexcept {
    if (output.size() < kIpv4EndpointTextCapacity) return 0;
    char* cursor = output.data();
    for (int shift = 24; shift >= 0; shift -= 8) {
        cursor = std::to_chars(cursor, output.data() + output.size(), (endpoint_value.address >> shift) & 255).ptr;
        *cursor++ = shift ? '.' : ':';
    }
    cursor = std::to_chars(cursor, output.data() + output.size(), endpoint_value.port).ptr;
    *cursor = '\0';
    return static_cast<std::size_t>(cursor - output.data());
}
ErrorCode open_tcp_socket(Socket& output) noexcept {
    return open_socket(SOCK_STREAM, output);
}
ErrorCode open_udp_socket(Socket& output) noexcept {
    return open_socket(SOCK_DGRAM, output);
}
ErrorCode set_non_blocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? errno : system_call_error(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}
ErrorCode set_tcp_no_delay(int fd, bool enabled) noexcept {
    return set_socket_option(fd, IPPROTO_TCP, TCP_NODELAY, enabled);
}
ErrorCode set_reuse_address(int fd, bool enabled) noexcept {
    return set_socket_option(fd, SOL_SOCKET, SO_REUSEADDR, enabled);
}
ErrorCode set_send_buffer_size(int fd, int bytes) noexcept {
    return set_socket_option(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}
ErrorCode set_receive_buffer_size(int fd, int bytes) noexcept {
    return set_socket_option(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}
ErrorCode get_send_buffer_size(int fd, int& bytes) noexcept {
    socklen_t length = sizeof(bytes);
    return system_call_error(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, &length));
}
ErrorCode get_receive_buffer_size(int fd, int& bytes) noexcept {
    socklen_t length = sizeof(bytes);
    return system_call_error(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, &length));
}
ErrorCode set_no_sigpipe(int fd) noexcept {
#ifdef SO_NOSIGPIPE
    return set_socket_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#else
    (void)fd;
    return 0;
#endif
}
ErrorCode bind_ipv4(int fd, Ipv4Endpoint endpoint_value) noexcept {
    const auto socket_address = socket_address_from_endpoint(endpoint_value);
    return system_call_error(bind(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address)));
}
ErrorCode listen_for_connections(int fd, int backlog) noexcept {
    return system_call_error(listen(fd, backlog));
}
ErrorCode open_tcp_listener(Ipv4Endpoint endpoint_value, int backlog, int send_buffer_bytes, int receive_buffer_bytes,
                            Socket& output) noexcept {
    Socket next;
    int error = open_tcp_socket(next);
    if (error) return error;
    if ((error = set_reuse_address(next.fd(), true))) return error;
    if (send_buffer_bytes && (error = set_send_buffer_size(next.fd(), send_buffer_bytes))) return error;
    if (receive_buffer_bytes && (error = set_receive_buffer_size(next.fd(), receive_buffer_bytes))) return error;
    if ((error = bind_ipv4(next.fd(), endpoint_value)) || (error = listen_for_connections(next.fd(), backlog)))
        return error;
    output = std::move(next);
    return 0;
}
ErrorCode accept_connection(int fd, Socket& output, Ipv4Endpoint& peer) noexcept {
    sockaddr_in socket_address{};
    socklen_t length = sizeof(socket_address);
    int accepted;
    do {
        accepted = accept(fd, reinterpret_cast<sockaddr*>(&socket_address), &length);
    } while (accepted < 0 && errno == EINTR);
    if (accepted < 0) return errno;
    Socket next(accepted);
    const int error = configure_socket(accepted);
    if (error) return error;
    peer = endpoint_from_socket_address(socket_address);
    output = std::move(next);
    return 0;
}
ErrorCode start_connect(int fd, Ipv4Endpoint endpoint_value) noexcept {
    const auto socket_address = socket_address_from_endpoint(endpoint_value);
    return system_call_error(connect(fd, reinterpret_cast<const sockaddr*>(&socket_address), sizeof(socket_address)));
}
ErrorCode take_socket_error(int fd) noexcept {
    int error = 0;
    socklen_t length = sizeof(error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ? errno : error;
}
ErrorCode peer_endpoint(int fd, Ipv4Endpoint& output) noexcept {
    sockaddr_in socket_address{};
    socklen_t length = sizeof(socket_address);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&socket_address), &length) < 0) return errno;
    output = endpoint_from_socket_address(socket_address);
    return 0;
}
ErrorCode local_endpoint(int fd, Ipv4Endpoint& output) noexcept {
    sockaddr_in socket_address{};
    socklen_t length = sizeof(socket_address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&socket_address), &length) < 0) return errno;
    output = endpoint_from_socket_address(socket_address);
    return 0;
}
ErrorCode connect_result(int fd) noexcept {
    const int error = take_socket_error(fd);
    if (error) return error;
    Ipv4Endpoint peer{};
    const int status = peer_endpoint(fd, peer);
    return status == ENOTCONN ? EINPROGRESS : status;
}
}  // namespace exsim::net
