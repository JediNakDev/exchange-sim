#include "server/server_runtime.hpp"
#include "test/exsim/gateway/fix/fix_test_support.hpp"
#include "test/framework/test.hpp"

#include <array>
#include <netinet/in.h>

namespace {
using Runtime = exsim::server::ServerRuntime;

exsim::net::Socket connect_fix(Runtime& runtime) {
    exsim::net::Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(socket.is_open());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(runtime.listen_endpoint<exsim::server::runtime::FixGateway>().port);
    REQUIRE_EQ(::connect(socket.fd(), reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    REQUIRE_EQ(exsim::net::set_non_blocking(socket.fd()), 0);
    REQUIRE_EQ(exsim::net::set_no_sigpipe(socket.fd()), 0);
    return socket;
}

std::string exchange_frame(Runtime& runtime, int fd, std::string_view frame) {
    REQUIRE_EQ(exsim::net::send_some(fd, frame.data(), frame.size()), static_cast<ssize_t>(frame.size()));
    std::string output;
    std::array<char, 8192> buffer{};
    const auto deadline = exsim::net::monotonic_ns() + 2'000'000'000;
    while (exsim::net::monotonic_ns() < deadline) {
        runtime.poll_once();
        const auto size = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (size > 0) output.append(buffer.data(), static_cast<std::size_t>(size));
        else if (size < 0) REQUIRE(exsim::net::is_would_block(errno));
        if (output.find("\00110=") != std::string::npos && output.back() == '\001') return output;
    }
    REQUIRE(!output.empty());
    return output;
}
}

TEST_CASE(fix_runtime, tcp_logon_orders_matching_and_logout) {
    exsim::server::ServerConfig config;
    std::string error;
    REQUIRE(exsim::server::load_config("test/fixtures/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.exchange.fix_port = 0;
    config.market_data.port = 0;
    config.market_data.retransmit_port = 0;
    for (auto& user : config.users) user.protocol = exsim::server::Protocol::Fix44;
    exsim::server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.day_time_at_start = fix_test::kSessionStartNs;
    Runtime runtime(config, options);
    REQUIRE(runtime.start().empty());
    CHECK_NE(runtime.listen_endpoint<exsim::server::runtime::FixGateway>().port,
             runtime.listen_endpoint<exsim::server::runtime::OuchGateway>().port);
    auto alice = connect_fix(runtime);
    auto bob = connect_fix(runtime);
    auto send = [&](int fd, std::string_view user, std::string_view type, unsigned seq, std::string_view fields) {
        return fix_test::with_pipes(exchange_frame(runtime, fd, fix_test::client_message(type, seq, fields, user)));
    };
    CHECK(send(alice.fd(), "ALICE", "A", 1, "98=0|108=30|554=alice|").find("35=A|") != std::string::npos);
    CHECK(send(bob.fd(), "BOB", "A", 1, "98=0|108=30|554=bob|").find("35=A|") != std::string::npos);
    CHECK(send(alice.fd(), "ALICE", "D", 2, fix_test::new_order_fields("BUY", "1", 100, "150")).find("39=0|") != std::string::npos);
    CHECK_EQ(runtime.core().engine().live_order_count(), 1u);
    CHECK(send(bob.fd(), "BOB", "D", 2, fix_test::new_order_fields("SELL", "2", 100, "150")).find("39=2|") != std::string::npos);
    CHECK_EQ(runtime.core().engine().live_order_count(), 0u);
    CHECK(send(bob.fd(), "BOB", "5", 3, "").find("35=5|") != std::string::npos);
    // Deliver peer EOF before destroying sessions that are still bound.
    alice.close();
    bob.close();
    const auto deadline = exsim::net::monotonic_ns() + 2'000'000'000;
    while (runtime.core().bound_connection_count() != 0 && exsim::net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK_EQ(runtime.core().bound_connection_count(), 0u);
}
