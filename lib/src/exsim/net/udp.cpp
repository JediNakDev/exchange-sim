#include <arpa/inet.h>

#include <net/udp.hpp>
#include <utility>
namespace exsim::net {
namespace {
ssize_t send_datagram(int fd, std::span<const char> bytes, Ipv4Endpoint destination) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(destination.address);
    address.sin_port = htons(destination.port);
    ssize_t sent;
    do {
        sent = sendto(fd, bytes.data(), bytes.size(), kSendFlags, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address));
    } while (sent < 0 && errno == EINTR);
    return sent;
}
}  // namespace
ssize_t UdpSender::send_to(std::span<const char> bytes, Ipv4Endpoint destination) noexcept {
    return send_datagram(fd(), bytes, destination);
}
ssize_t UdpReceiver::send_to(std::span<const char> bytes, Ipv4Endpoint destination) noexcept {
    return send_datagram(fd(), bytes, destination);
}
ErrorCode UdpSender::multicast_options(std::uint8_t ttl, bool loop, std::uint32_t interface_address) noexcept {
    const unsigned char loop_value = loop;
    const in_addr interface{htonl(interface_address)};
    if (setsockopt(fd(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
        setsockopt(fd(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop_value, sizeof(loop_value)) < 0 ||
        setsockopt(fd(), IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface)) < 0)
        return errno;
    return 0;
}
ErrorCode UdpReceiver::open(Ipv4Endpoint local, bool reuse) noexcept {
    Socket next;
    int error = open_udp_socket(next);
    if (error) return error;
    if (reuse && (error = set_reuse_address(next.fd(), true))) return error;
    if ((error = bind_ipv4(next.fd(), local))) return error;
    socket_ = std::move(next);
    return 0;
}
ErrorCode UdpReceiver::join_multicast(std::uint32_t group, std::uint32_t interface_address) noexcept {
    if (!is_ipv4_multicast_address(group)) return EINVAL;
    const ip_mreq request{{htonl(group)}, {htonl(interface_address)}};
    return setsockopt(fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) < 0 ? errno : 0;
}
ssize_t UdpReceiver::receive_from(std::span<char> bytes, Ipv4Endpoint& peer) noexcept {
    sockaddr_in address{};
    iovec vector{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_name = &address;
    message.msg_namelen = sizeof(address);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    ssize_t received;
    do {
        received = recvmsg(fd(), &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received >= 0) {
        peer = {ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
        if (message.msg_flags & MSG_TRUNC) {
            errno = EMSGSIZE;
            return -1;
        }
    }
    return received;
}
}  // namespace exsim::net
