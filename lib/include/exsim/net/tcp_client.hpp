#ifndef EXSIM_NET_TCP_CLIENT_HPP
#define EXSIM_NET_TCP_CLIENT_HPP
#include <exsim/net/tcp_connection.hpp>
namespace exsim::net {
// Caller registers fd() for write readiness during connect and while output
// is pending, and read readiness when connected. Storage survives reconnects.
class TcpClient {
   public:
    TcpClient(std::size_t receive_capacity = 65536, std::size_t send_capacity = 1048576)
        : connection_(receive_capacity, send_capacity) {}
    ErrorCode connect(Ipv4Endpoint remote) noexcept;
    ErrorCode complete_connect() noexcept;
    void close() noexcept {
        connection_.close();
        connecting_ = false;
        connected_ = false;
    }
    bool connecting() const noexcept { return connecting_; }
    bool connected() const noexcept { return connected_; }
    int fd() const noexcept { return connection_.fd(); }
    TcpConnection& connection() noexcept { return connection_; }
    bool send(std::span<const char> bytes) noexcept {
        if (!connected_) return false;
        if (connection_.send_buffer().append(bytes)) return true;
        close();
        return false;
    }
    ReceiveResult receive() noexcept;
    FlushResult flush() noexcept;

   private:
    TcpConnection connection_;
    bool connecting_ = false;
    bool connected_ = false;
};
}  // namespace exsim::net
#endif
