#ifndef EXSIM_NET_TCP_CONNECTION_HPP
#define EXSIM_NET_TCP_CONNECTION_HPP

// One TCP stream: the socket plus its fixed-capacity receive and send
// buffers. Used by every TcpServer slot and by TcpClient; the buffers are
// allocated once and reused across reconnects.

#include <cstddef>
#include <exsim/net/socket.hpp>
#include <exsim/net/stream_buffers.hpp>
#include <utility>

namespace exsim::net {

class TcpConnection {
   public:
    TcpConnection(std::size_t receive_buffer_capacity, std::size_t send_buffer_capacity)
        : receive_buffer_(receive_buffer_capacity), send_buffer_(send_buffer_capacity) {}

    // Takes ownership of a connected socket and empties both buffers.
    void attach(Socket&& socket) noexcept {
        socket_ = std::move(socket);
        receive_buffer_.clear();
        send_buffer_.clear();
    }

    // Closes the socket (if open) and empties both buffers.
    void close() noexcept {
        socket_.close();
        receive_buffer_.clear();
        send_buffer_.clear();
    }

    // Disconnect while retaining final inbound bytes for the framer.
    void disconnect() noexcept { socket_.close(); }

    bool is_open() const noexcept { return socket_.is_open(); }
    int fd() const noexcept { return socket_.fd(); }

    ReceiveBuffer& receive_buffer() noexcept { return receive_buffer_; }
    const ReceiveBuffer& receive_buffer() const noexcept { return receive_buffer_; }
    SendBuffer& send_buffer() noexcept { return send_buffer_; }
    const SendBuffer& send_buffer() const noexcept { return send_buffer_; }

    ReceiveResult receive() noexcept { return receive_buffer_.receive_from(socket_.fd()); }
    FlushResult flush() noexcept { return send_buffer_.flush_to(socket_.fd()); }

   private:
    Socket socket_;
    ReceiveBuffer receive_buffer_;
    SendBuffer send_buffer_;
};

}  // namespace exsim::net

#endif
