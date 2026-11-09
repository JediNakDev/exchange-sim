#include <net/stream_buffers.hpp>
namespace exsim::net {
ReceiveResult ReceiveBuffer::receive_from(int fd) noexcept {
    const auto tail = writable_tail();
    if (tail.empty()) return {ReceiveStatus::BufferFull, 0, 0};
    const auto received = receive_some(fd, tail.data(), tail.size());
    if (received > 0) {
        commit(static_cast<std::size_t>(received));
        return {ReceiveStatus::Received, 0, static_cast<std::size_t>(received)};
    }
    if (received == 0) return {ReceiveStatus::PeerClosed, 0, 0};
    const int error = errno;
    return {is_would_block(error) ? ReceiveStatus::WouldBlock : ReceiveStatus::Error, error, 0};
}
FlushResult SendBuffer::flush_to(int fd) noexcept {
    if (!has_pending()) return {FlushStatus::Complete, 0, 0};
    const auto bytes = pending();
    const auto sent = send_some(fd, bytes.data(), bytes.size());
    if (sent < 0) {
        const int error = errno;
        return {is_would_block(error) ? FlushStatus::Blocked : FlushStatus::Error, error, 0};
    }
    if (sent == 0) return {FlushStatus::Error, EPIPE, 0};
    read_offset_ += static_cast<std::size_t>(sent);
    if (read_offset_ == write_offset_) {
        read_offset_ = 0;
        write_offset_ = 0;
    }
    return {has_pending() ? FlushStatus::Blocked : FlushStatus::Complete, 0, static_cast<std::size_t>(sent)};
}
}  // namespace exsim::net
