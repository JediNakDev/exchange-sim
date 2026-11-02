// Standalone to avoid replacing operator new in unrelated framework tests.
// Build/run with tests/exsim/net/check_allocations.sh.
#include <cstdio>
#include <cstdlib>
#include <exsim/net/clocks.hpp>
#include <exsim/net/tcp_client.hpp>
#include <exsim/net/tcp_server.hpp>
#include <exsim/net/udp.hpp>
#include <new>

namespace {
bool counting = false;
std::size_t allocations = 0;
}  // namespace
void* operator new(std::size_t size) {
    if (counting) ++allocations;
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
#endif
namespace {
struct Echo {
    exsim::net::TcpServer& server;
    exsim::net::ConnectionId current{};
    bool opened = false;
    void on_connection_opened(exsim::net::ConnectionId id, exsim::net::Ipv4Endpoint) noexcept {
        current = id;
        opened = true;
    }
    void on_connection_closed(exsim::net::ConnectionId, exsim::net::CloseReason, int) noexcept {}
    void on_connection_data(exsim::net::ConnectionId id, exsim::net::ReceiveBuffer& buffer) noexcept {
        server.send(id, buffer.readable());
        buffer.consume(buffer.size());
    }
};
}  // namespace
int main() {
    using namespace exsim::net;
    Poller poller;
    TcpServerConfig config;
    config.max_connections = 1;
    TcpServer server(config, poller, 1);
    Echo echo{server};
    TcpClient client;
    UdpSender sender;
    UdpReceiver receiver;
    LocalClock clock;
    clock.refresh();
    if (poller.open() || server.open() || sender.open() || receiver.open(loopback_endpoint(0))) return 1;
    Ipv4Endpoint destination{};
    if (local_endpoint(receiver.fd(), destination)) return 2;
    bool passed = true;
    counting = true;
    for (int cycle = 0; cycle < 64 && passed; ++cycle) {
        echo.opened = false;
        int error = client.connect(server.listen_endpoint());
        if (error && error != EINPROGRESS) {
            passed = false;
            break;
        }
        bool queued = false, received = false;
        const auto deadline = monotonic_ns() + 1000000000;
        while (!received && monotonic_ns() < deadline) {
            if (poller.wait(0) < 0) {
                passed = false;
                break;
            }
            for (const auto& event : poller.events()) server.handle_event(event, echo);
            if (client.connecting()) {
                error = client.complete_connect();
                if (error && error != EINPROGRESS) {
                    passed = false;
                    break;
                }
            }
            if (client.connected() && !queued) {
                passed = client.send(std::span("echo", 4));
                queued = true;
            }
            if (queued) {
                client.flush();
                client.receive();
            }
            server.flush_pending(echo);
            received = client.connection().receive_buffer().size() == 4;
            (void)clock.midnight_ns(realtime_ns());
        }
        passed = passed && received && echo.opened;
        if (echo.opened) server.close_now(echo.current);
        client.close();
        char bytes[8]{};
        Ipv4Endpoint peer{};
        if (sender.send_to(std::span("udp", 3), destination) != 3) passed = false;
        ssize_t count = -1;
        while (count < 0 && monotonic_ns() < deadline) count = receiver.receive_from(bytes, peer);
        if (count != 3) passed = false;
    }
    counting = false;
    std::printf("64 connect/accept/echo/close/slot-reuse/UDP cycles: %s; allocations after setup: %zu\n",
                passed ? "PASS" : "FAIL", allocations);
    return passed && allocations == 0 ? 0 : 1;
}
