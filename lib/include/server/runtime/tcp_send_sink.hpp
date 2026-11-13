#ifndef EXSIM_SERVER_RUNTIME_TCP_SEND_SINK_HPP
#define EXSIM_SERVER_RUNTIME_TCP_SEND_SINK_HPP

// Byte sink of the order-entry gateways over one
// connection's net::TcpServer send buffer. prepare() returning nullptr means
// the send buffer cannot take the message: the gateway then drops the
// connection (live output) or pauses (OUCH replay). A refused prepare leaves
// the queued bytes and the connection intact, so a paused replay resumes once
// the socket drains; every gateway closes on a refused live write.

#include "net/tcp_server.hpp"

#include <cstddef>

namespace exsim::server::runtime {

struct TcpSendSink {
    net::TcpServer* tcp_server = nullptr;
    net::ConnectionId connection{};

    [[nodiscard]] char* prepare(std::size_t byte_count) noexcept {
        const std::span<char> space = tcp_server->reserve_send_if_room(connection, byte_count);
        return space.size() == byte_count && byte_count != 0 ? space.data() : nullptr;
    }
    void commit(std::size_t byte_count) noexcept { tcp_server->commit_send(connection, byte_count); }
};

}  // namespace exsim::server::runtime

#endif
