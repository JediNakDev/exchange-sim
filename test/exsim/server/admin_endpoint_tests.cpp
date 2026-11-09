// AdminEndpoint over loopback TCP with a stand-in core: an exception from a
// command is contained at the operator boundary (ERR, that connection
// closes) and never reaches the reactor's noexcept event path.

#include "exsim/net/clocks.hpp"
#include "exsim/net/tcp_client.hpp"
#include "exsim/server/runtime/admin_endpoint.hpp"
#include "test/framework/test.hpp"

#include <cerrno>
#include <new>
#include <string>
#include <string_view>

using namespace exsim;
using namespace exsim::net;

namespace {

struct AdminCoreThatThrowsOnExplode {
    void execute_line(std::string_view line, std::string& reply) {
        if (line == "explode") throw std::bad_alloc();
        reply.append("echo ").append(line).append("\nOK\n");
    }
};

using TestAdminEndpoint = server::runtime::AdminEndpoint<AdminCoreThatThrowsOnExplode>;

constexpr std::uint8_t kAdminPollTag = 7;

template <class Predicate, class Step>
void run_until(Predicate predicate, Step step) {
    const auto deadline = monotonic_ns() + 5 * kNanosecondsPerSecond;
    bool done = predicate();
    while (!done && monotonic_ns() < deadline) {
        step();
        done = predicate();
    }
    REQUIRE(done);
}

struct AdminConnection {
    TcpClient tcp_client{4096, 4096};
    std::string received;
    bool closed_by_server = false;

    void receive_available() {
        const ReceiveResult result = tcp_client.receive();
        ReceiveBuffer& buffer = tcp_client.connection().receive_buffer();
        received.append(buffer.readable().data(), buffer.size());
        buffer.consume(buffer.size());
        if (result.status == ReceiveStatus::PeerClosed) closed_by_server = true;
    }
};

}  // namespace

TEST_CASE(server_admin_endpoint, command_exception_closes_only_that_connection) {
    Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    TcpServerConfig config;
    config.max_connections = 2;
    AdminCoreThatThrowsOnExplode core;
    TestAdminEndpoint admin(config, poller, kAdminPollTag, core);
    REQUIRE_EQ(admin.open(), 0);
    auto pump_server = [&] {
        REQUIRE(poller.wait(1) >= 0);
        for (const PollEvent& event : poller.events()) admin.handle_event(event);
        admin.flush();
    };

    AdminConnection healthy, failing;
    for (AdminConnection* connection : {&healthy, &failing}) {
        const ErrorCode error = connection->tcp_client.connect(admin.listen_endpoint());
        REQUIRE(error == 0 || error == EINPROGRESS);
        run_until([&] { return connection->tcp_client.connected() || connection->tcp_client.complete_connect() == 0; },
                  pump_server);
    }

    REQUIRE(healthy.tcp_client.send(std::string_view("status\n")));
    REQUIRE(healthy.tcp_client.flush().status != FlushStatus::Error);
    run_until([&] { return healthy.received == "echo status\nOK\n"; },
              [&] { pump_server(); healthy.receive_available(); });

    REQUIRE(failing.tcp_client.send(std::string_view("explode\nstatus\n")));
    REQUIRE(failing.tcp_client.flush().status != FlushStatus::Error);
    run_until([&] { return failing.closed_by_server; }, [&] { pump_server(); failing.receive_available(); });
    CHECK_EQ(failing.received, std::string("ERR internal error\n"));

    // The reactor and the other operator connection keep working.
    REQUIRE(healthy.tcp_client.send(std::string_view("again\n")));
    REQUIRE(healthy.tcp_client.flush().status != FlushStatus::Error);
    run_until([&] { return healthy.received == "echo status\nOK\necho again\nOK\n"; },
              [&] { pump_server(); healthy.receive_available(); });
    CHECK_EQ(admin.commands_executed(), 2u);
}
