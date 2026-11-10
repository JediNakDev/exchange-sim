#include "ipc/exchange_control.hpp"
#include "net/socket.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <poll.h>
#include <stdexcept>
namespace exsim::ipc {
namespace {
void transfer(int fd, char* bytes, std::size_t size, bool writing, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("exchange control timeout");
        pollfd event{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready = ::poll(&event, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) throw std::runtime_error("exchange control unavailable");
        const ssize_t count = writing ? net::send_some(fd, bytes, size) : net::receive_some(fd, bytes, size);
        if (count < 0 && net::is_would_block(errno)) continue;
        if (count <= 0) throw std::runtime_error("exchange control closed");
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}
constexpr std::size_t kMaxFrame = 1024 * 1024;
}
void send_control(int fd, std::string_view message, int timeout_ms) {
    if (message.size() > kMaxFrame) throw std::runtime_error("exchange control frame too large");
    auto size = htonl(static_cast<std::uint32_t>(message.size()));
    transfer(fd, reinterpret_cast<char*>(&size), sizeof size, true, timeout_ms);
    transfer(fd, const_cast<char*>(message.data()), message.size(), true, timeout_ms);
}
std::string receive_control(int fd, int timeout_ms) {
    std::uint32_t size = 0;
    transfer(fd, reinterpret_cast<char*>(&size), sizeof size, false, timeout_ms);
    size = ntohl(size);
    if (size > kMaxFrame) throw std::runtime_error("exchange control frame too large");
    std::string message(size, '\0');
    transfer(fd, message.data(), size, false, timeout_ms);
    return message;
}
}
