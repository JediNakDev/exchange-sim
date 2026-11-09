#ifndef EXSIM_TOOLS_MD_CLIENT_RETRANSMISSION_SOURCE_HPP
#define EXSIM_TOOLS_MD_CLIENT_RETRANSMISSION_SOURCE_HPP

// exsim_md_client feeds datagrams from its retransmission socket into the
// MoldUDP64 receiver as retransmissions. Only the configured retransmission
// server may supply them: anything else that reaches the client's ephemeral
// port (and knows the session name) would otherwise be injected into the book.

#include "net/socket.hpp"

#include <optional>

namespace exsim::tools::md_client {

// True if a datagram from `peer` on the retransmission socket is accepted:
// a retransmission server is configured and `peer` is exactly that endpoint.
[[nodiscard]] constexpr bool is_configured_retransmission_source(
    const std::optional<net::Ipv4Endpoint>& configured_retransmission_server, const net::Ipv4Endpoint& peer) noexcept {
    return configured_retransmission_server.has_value() && *configured_retransmission_server == peer;
}

}  // namespace exsim::tools::md_client

#endif
