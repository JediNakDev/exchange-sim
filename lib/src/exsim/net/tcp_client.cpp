#include <net/tcp_client.hpp>
namespace exsim::net {
ErrorCode TcpClient::connect(Ipv4Endpoint remote) noexcept {
    close();
    Socket socket;
    int error = open_tcp_socket(socket);
    if (error) return error;
    if ((error = set_tcp_no_delay(socket.fd(), true))) return error;
    error = start_connect(socket.fd(), remote);
    if (error && error != EINPROGRESS) return error;
    connection_.attach(std::move(socket));
    connecting_ = error == EINPROGRESS;
    connected_ = !connecting_;
    return error;
}
ErrorCode TcpClient::complete_connect() noexcept {
    if (connected_) return 0;
    if (!connecting_) return ENOTCONN;
    const int error = connect_result(fd());
    if (error == EINPROGRESS) return error;
    if (error) {
        close();
        return error;
    }
    connecting_ = false;
    connected_ = true;
    return 0;
}
ReceiveResult TcpClient::receive() noexcept {
    if (!connected_) return {ReceiveStatus::Error, ENOTCONN, 0};
    const auto received = connection_.receive();
    // Keep final received bytes available until the caller consumes them.
    if (received.status == ReceiveStatus::PeerClosed || received.status == ReceiveStatus::Error ||
        received.status == ReceiveStatus::BufferFull) {
        connection_.disconnect();
        connected_ = false;
    }
    return received;
}
FlushResult TcpClient::flush() noexcept {
    if (!connected_) return {FlushStatus::Error, ENOTCONN, 0};
    if (connection_.send_buffer().overflowed()) {
        close();
        return {FlushStatus::Error, ENOBUFS, 0};
    }
    const auto flushed = connection_.flush();
    if (flushed.status == FlushStatus::Error) close();
    return flushed;
}
}  // namespace exsim::net
