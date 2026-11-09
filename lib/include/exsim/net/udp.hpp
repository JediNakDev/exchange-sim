#ifndef EXSIM_NET_UDP_HPP
#define EXSIM_NET_UDP_HPP
#include <exsim/net/socket.hpp>
namespace exsim::net {
// Both classes own nonblocking sockets. Return -1/errno for datagram I/O.
class UdpSender {
   public:
    ErrorCode open() noexcept { return open_udp_socket(socket_); }
    void close() noexcept { socket_.close(); }
    int fd() const noexcept { return socket_.fd(); }
    ErrorCode multicast_options(std::uint8_t ttl, bool loop,
                                std::uint32_t interface_address = kIpv4AnyAddress) noexcept;
    ssize_t send_to(std::span<const char> bytes, Ipv4Endpoint destination) noexcept;

   private:
    Socket socket_;
};
class UdpReceiver {
   public:
    ErrorCode open(Ipv4Endpoint local, bool reuse = false) noexcept;
    void close() noexcept { socket_.close(); }
    int fd() const noexcept { return socket_.fd(); }
    ErrorCode join_multicast(std::uint32_t group, std::uint32_t interface_address = kIpv4AnyAddress) noexcept;
    // Oversize datagrams are consumed and return -1/EMSGSIZE rather than
    // exposing a truncated packet as a valid protocol frame.
    ssize_t receive_from(std::span<char> bytes, Ipv4Endpoint& peer) noexcept;
    ssize_t send_to(std::span<const char> bytes, Ipv4Endpoint destination) noexcept;

   private:
    Socket socket_;
};
}  // namespace exsim::net
#endif
