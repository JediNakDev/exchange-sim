// exsim_md_client accepts retransmission-socket datagrams only from the
// configured retransmission server (QA review F6).

#include "tools/exsim_md_client/retransmission_source.hpp"

#include <tests/framework/test.hpp>

#include <optional>

using exsim::net::Ipv4Endpoint;
using exsim::net::kIpv4LoopbackAddress;
using exsim::tools::md_client::is_configured_retransmission_source;

TEST_CASE(md_client_retransmission_source, only_the_configured_server_is_accepted) {
    const std::optional<Ipv4Endpoint> server = Ipv4Endpoint{kIpv4LoopbackAddress, 31002};
    CHECK(is_configured_retransmission_source(server, Ipv4Endpoint{kIpv4LoopbackAddress, 31002}));
    CHECK(!is_configured_retransmission_source(server, Ipv4Endpoint{kIpv4LoopbackAddress, 31003}));  // other port
    CHECK(!is_configured_retransmission_source(server, Ipv4Endpoint{0x0A000001, 31002}));           // other host
}

TEST_CASE(md_client_retransmission_source, nothing_is_accepted_without_a_configured_server) {
    const std::optional<Ipv4Endpoint> no_server;
    CHECK(!is_configured_retransmission_source(no_server, Ipv4Endpoint{kIpv4LoopbackAddress, 31002}));
    CHECK(!is_configured_retransmission_source(no_server, Ipv4Endpoint{}));
}
