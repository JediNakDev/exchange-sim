#ifndef EXSIM_NET_SOCKET_HPP
#define EXSIM_NET_SOCKET_HPP

// POSIX socket ownership and IPv4 setup helpers.
//
// Error convention for the whole exsim::net layer: a function that can fail
// returns an ErrorCode, which is 0 on success and otherwise the errno value of
// the failing system call. Setup constructors allocate storage; all I/O methods are noexcept.
//
// Every socket created here is non-blocking, close-on-exec and SIGPIPE-safe:
// SO_NOSIGPIPE is set where the platform has it (macOS) and every send passes
// MSG_NOSIGNAL where the platform has it (Linux), so writing to a peer that
// has gone away reports EPIPE instead of killing the process.

#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace exsim::net {

using ErrorCode = int;  // 0 = success, otherwise an errno value

// Owns one file descriptor and closes it on destruction. Move-only.
class Socket {
   public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket() { close(); }

    Socket(Socket&& other) noexcept : fd_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }

    // Gives up ownership without closing.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void close() noexcept;

   private:
    int fd_ = -1;
};

// IPv4 address and port, both in host byte order (127.0.0.1 == 0x7F000001).
struct Ipv4Endpoint {
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    friend constexpr bool operator==(const Ipv4Endpoint&, const Ipv4Endpoint&) = default;
};

inline constexpr std::uint32_t kIpv4AnyAddress = 0x00000000;       // INADDR_ANY
inline constexpr std::uint32_t kIpv4LoopbackAddress = 0x7F000001;  // 127.0.0.1

constexpr Ipv4Endpoint loopback_endpoint(std::uint16_t port) noexcept {
    return {kIpv4LoopbackAddress, port};
}

constexpr bool is_ipv4_multicast_address(std::uint32_t address) noexcept {
    return (address >> 28) == 0xE;
}

// Parses a dotted quad ("10.0.0.1"); no hostnames. Returns false on malformed input.
bool parse_ipv4_address(std::string_view text, std::uint32_t& address_out) noexcept;

// "255.255.255.255:65535" plus a terminating NUL.
inline constexpr std::size_t kIpv4EndpointTextCapacity = 22;

// Writes "a.b.c.d:port" and a terminating NUL into `out` (needs at least
// kIpv4EndpointTextCapacity bytes). Returns the text length, or 0 if `out` is too small.
std::size_t format_ipv4_endpoint(Ipv4Endpoint endpoint, std::span<char> out) noexcept;

// --- socket creation -------------------------------------------------------

// Creates a non-blocking, close-on-exec, SIGPIPE-safe IPv4 socket.
ErrorCode open_tcp_socket(Socket& socket_out) noexcept;
ErrorCode open_udp_socket(Socket& socket_out) noexcept;

// --- socket options ----------------------------------------------------------

ErrorCode set_non_blocking(int fd) noexcept;
ErrorCode set_tcp_no_delay(int fd, bool enabled) noexcept;
ErrorCode set_reuse_address(int fd, bool enabled) noexcept;
// SO_SNDBUF / SO_RCVBUF. The kernel may round the value (Linux doubles it).
// For TCP, set the receive size before connect/listen so the window scale accounts for it.
ErrorCode set_send_buffer_size(int fd, int bytes) noexcept;
ErrorCode set_receive_buffer_size(int fd, int bytes) noexcept;
ErrorCode get_send_buffer_size(int fd, int& bytes_out) noexcept;
ErrorCode get_receive_buffer_size(int fd, int& bytes_out) noexcept;
// SO_NOSIGPIPE where available (macOS/BSD); a successful no-op elsewhere.
ErrorCode set_no_sigpipe(int fd) noexcept;

// --- addressing and connection setup ---------------------------------------

ErrorCode bind_ipv4(int fd, Ipv4Endpoint local) noexcept;
ErrorCode listen_for_connections(int fd, int backlog) noexcept;

// Opens a TCP listener: SO_REUSEADDR, optional SO_SNDBUF/SO_RCVBUF (0 = OS
// default; accepted sockets inherit them), bind, listen. Bind port 0 to let
// the OS pick a free port, then read it back with local_endpoint().
ErrorCode open_tcp_listener(Ipv4Endpoint local, int backlog, int send_buffer_bytes, int receive_buffer_bytes,
                            Socket& listener_out) noexcept;

// Accepts one pending connection. Returns EAGAIN (== EWOULDBLOCK) when the
// backlog is empty. The accepted socket is non-blocking, close-on-exec and
// SIGPIPE-safe.
ErrorCode accept_connection(int listen_fd, Socket& accepted_out, Ipv4Endpoint& peer_out) noexcept;

// Starts a non-blocking connect. Returns 0 when the connection completed
// immediately, EINPROGRESS when it completes later (wait for the socket to
// become writable, then call connect_result()), otherwise the error.
ErrorCode start_connect(int fd, Ipv4Endpoint remote) noexcept;

// Outcome of an in-progress non-blocking connect: 0 = connected, EINPROGRESS
// = not finished yet, otherwise the connect error (e.g. ECONNREFUSED).
ErrorCode connect_result(int fd) noexcept;

// SO_ERROR: the socket's pending error (clears it), 0 if none.
ErrorCode take_socket_error(int fd) noexcept;

ErrorCode local_endpoint(int fd, Ipv4Endpoint& endpoint_out) noexcept;
ErrorCode peer_endpoint(int fd, Ipv4Endpoint& endpoint_out) noexcept;

// --- per-message I/O ---------------------------------------------------------

#if defined(MSG_NOSIGNAL)
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;  // SIGPIPE suppressed by SO_NOSIGPIPE at socket creation
#endif

// One send() call, retried on EINTR. Returns the number of bytes sent
// (possibly fewer than requested), or -1 with errno set (EAGAIN when the
// socket buffer is full). Never raises SIGPIPE.
inline ssize_t send_some(int fd, const char* data, std::size_t length) noexcept {
    for (;;) {
        const ssize_t sent = ::send(fd, data, length, kSendFlags);
        if (sent >= 0 || errno != EINTR) {
            return sent;
        }
    }
}

// One recv() call, retried on EINTR. Returns the number of bytes received,
// 0 on orderly shutdown by the peer, or -1 with errno set.
inline ssize_t receive_some(int fd, char* data, std::size_t capacity) noexcept {
    for (;;) {
        const ssize_t received = ::recv(fd, data, capacity, 0);
        if (received >= 0 || errno != EINTR) {
            return received;
        }
    }
}

inline bool is_would_block(ErrorCode error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

}  // namespace exsim::net

#endif
