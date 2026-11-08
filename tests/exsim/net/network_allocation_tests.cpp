// Runtime networking cycles must not allocate on the C++ heap after setup.
// Counting uses the shared per-thread counter in tests/framework/alloc_counter.cpp,
// which reports nothing under ASan/TSan, so the test is skipped there.
#include <cerrno>
#include <exsim/net/clocks.hpp>
#include <exsim/net/tcp_client.hpp>
#include <exsim/net/tcp_server.hpp>
#include <exsim/net/udp.hpp>
#include <span>

#include "tests/framework/alloc_counter.hpp"
#include "tests/framework/test.hpp"

namespace {
struct EchoEveryByteHandler {
    exsim::net::TcpServer& server;
    exsim::net::ConnectionId current_connection{};
    bool connection_opened = false;
    void on_connection_opened(exsim::net::ConnectionId id, exsim::net::Ipv4Endpoint) noexcept {
        current_connection = id;
        connection_opened = true;
    }
    void on_connection_closed(exsim::net::ConnectionId, exsim::net::CloseReason, int) noexcept {}
    void on_connection_data(exsim::net::ConnectionId id, exsim::net::ReceiveBuffer& buffer) noexcept {
        server.send(id, buffer.readable());
        buffer.consume(buffer.size());
    }
};
}  // namespace

TEST_CASE(Network, connect_echo_close_and_udp_cycles_allocate_nothing_after_setup) {
    if (!testfw::allocation_counting_available()) return;
    using namespace exsim::net;
    Poller poller;
    TcpServerConfig config;
    config.max_connections = 1;
    TcpServer server(config, poller, 1);
    EchoEveryByteHandler echo{server};
    TcpClient client;
    UdpSender sender;
    UdpReceiver receiver;
    LocalClock clock;
    clock.refresh();
    REQUIRE(poller.open() == 0);
    REQUIRE(server.open() == 0);
    REQUIRE(sender.open() == 0);
    REQUIRE(receiver.open(loopback_endpoint(0)) == 0);
    Ipv4Endpoint udp_destination{};
    REQUIRE(local_endpoint(receiver.fd(), udp_destination) == 0);

    bool every_cycle_passed = true;
    int completed_cycles = 0;
    std::size_t allocations_after_setup = 0;
    {
        testfw::AllocationScope scope;
        for (int cycle = 0; cycle < 64 && every_cycle_passed; ++cycle) {
            echo.connection_opened = false;
            int error = client.connect(server.listen_endpoint());
            if (error && error != EINPROGRESS) {
                every_cycle_passed = false;
                break;
            }
            bool request_queued = false, echo_received = false;
            const auto deadline = monotonic_ns() + 5 * kNanosecondsPerSecond;
            while (!echo_received && monotonic_ns() < deadline) {
                if (poller.wait(0) < 0) {
                    every_cycle_passed = false;
                    break;
                }
                for (const auto& event : poller.events()) server.handle_event(event, echo);
                if (client.connecting()) {
                    error = client.complete_connect();
                    if (error && error != EINPROGRESS) {
                        every_cycle_passed = false;
                        break;
                    }
                }
                if (client.connected() && !request_queued) {
                    every_cycle_passed = client.send(std::span("echo", 4));
                    request_queued = true;
                }
                if (request_queued) {
                    client.flush();
                    client.receive();
                }
                server.flush_pending(echo);
                echo_received = client.connection().receive_buffer().size() == 4;
                (void)clock.midnight_ns(realtime_ns());
            }
            every_cycle_passed = every_cycle_passed && echo_received && echo.connection_opened;
            if (echo.connection_opened) server.close_now(echo.current_connection);
            client.close();
            char datagram[8]{};
            Ipv4Endpoint datagram_peer{};
            if (sender.send_to(std::span("udp", 3), udp_destination) != 3) every_cycle_passed = false;
            ssize_t received_bytes = -1;
            while (received_bytes < 0 && monotonic_ns() < deadline)
                received_bytes = receiver.receive_from(datagram, datagram_peer);
            if (received_bytes != 3) every_cycle_passed = false;
            if (every_cycle_passed) ++completed_cycles;
        }
        allocations_after_setup = scope.count();
    }
    CHECK(every_cycle_passed);
    CHECK_EQ(completed_cycles, 64);
    CHECK_EQ(allocations_after_setup, 0u);
}
