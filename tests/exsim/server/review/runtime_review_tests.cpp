// Recovered review reproducers, promoted to regressions as fixes land.
#include "tests/exsim/server/e2e/e2e_support.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

// Exercise the actual benchmark recorder without changing the tool.
#define main exsim_review_benchmark_main
#include "tools/exsim_w2w_bench/main.cpp"
#undef main

using namespace exsim;
using namespace exsim::e2e;

TEST_CASE(server_runtime_review, oversized_complete_admin_line_is_rejected) {
    ServerUnderTest server(make_e2e_config(false), 0);
    REQUIRE(server.started());
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    // Fits in one receive buffer but exceeds the documented line limit.
    const std::string command = std::string(5000, ' ') + "bots off";
    const std::string reply = admin.request(command);
    CHECK_EQ(reply, std::string("ERR line too long\n"));
    CHECK(!reply_ok(reply));
}

TEST_CASE(server_runtime_review, fixed_rate_latency_includes_missed_send_deadline) {
    LatencyRecorder recorder(1, 0);
    const std::uint64_t scheduled = net::monotonic_ns();
    recorder.intended_time_ns[0] = scheduled;
    // Model a client scheduling pause before the first actual send.
    ::poll(nullptr, 0, 100);
    recorder.send_time_ns[0] = net::monotonic_ns();
    ouch42::OrderAccepted accepted{};
    accepted.token.fill(' ');
    accepted.token[0] = 'W';
    accepted.token[1] = '0';
    recorder.on_order_accepted(accepted, 0);
    const auto scheduled_latency = recorder.last_accept_time_ns - scheduled;
    REQUIRE_EQ(recorder.scheduled_accept_latency_ns.size(), 1u);
    CHECK_EQ(recorder.scheduled_accept_latency_ns[0], scheduled_latency);
    CHECK_LT(recorder.accept_latency_ns[0], scheduled_latency);
}

TEST_CASE(server_runtime_review, journal_backpressure_does_not_block_reactor) {
    int journal_pipe[2], ready_pipe[2];
    REQUIRE_EQ(::pipe(journal_pipe), 0);
    REQUIRE_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        if (::write(ready_pipe[1], "R", 1) != 1) ::_exit(13);
        try {
        auto config = make_e2e_config(false, "/dev/fd/" + std::to_string(journal_pipe[1]));
        config.journal_flush_interval_ms = 1;
        server::ServerRuntimeOptions options;
        options.log = nullptr;
        options.day_time_at_start = kTenAm;
        options.busy_poll = true;
        server::ServerRuntime runtime(config, options);
        if (!runtime.start().empty()) ::_exit(10);
        const int flags = ::fcntl(journal_pipe[1], F_GETFL);
        ::fcntl(journal_pipe[1], F_SETFL, flags | O_NONBLOCK);
        std::array<char, 1024> bytes{};
        while (::write(journal_pipe[1], bytes.data(), bytes.size()) > 0) {}
        // Fill the remaining tail, including space smaller than PIPE_BUF.
        while (::write(journal_pipe[1], bytes.data(), 1) > 0) {}
        ::fcntl(journal_pipe[1], F_SETFL, flags);
        runtime.poll_once(); // periodic journal flush writes a footer to a full pipe
        ::_exit(0);
        } catch (const std::invalid_argument&) {
            // A pipe is an unsupported journal destination. Rejecting it
            // before the reactor starts is also a bounded outcome.
            ::_exit(0);
        }
    }
    ::close(ready_pipe[1]);
    pollfd readiness{ready_pipe[0], POLLIN, 0};
    const bool ready = ::poll(&readiness, 1, 5000) == 1;
    bool exited = false;
    int status = 0;
    if (ready) {
        const auto deadline = net::monotonic_ns() + 300'000'000;
        do {
            if (::waitpid(child, &status, WNOHANG) == child) { exited = true; break; }
            ::poll(nullptr, 0, 1);
        } while (net::monotonic_ns() < deadline);
    }
    if (!exited) { ::kill(child, SIGKILL); ::waitpid(child, &status, 0); }
    ::close(journal_pipe[0]);
    ::close(journal_pipe[1]);
    ::close(ready_pipe[0]);
    REQUIRE(ready);
    CHECK(exited);
    if (exited) CHECK_EQ(status, 0);
}

TEST_CASE(server_runtime_review, a_full_log_pipe_does_not_block_shutdown) {
    int log_pipe[2], ready_pipe[2];
    REQUIRE_EQ(::pipe(log_pipe), 0);
    REQUIRE_EQ(::pipe(ready_pipe), 0);
    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::FILE* sink = ::fdopen(log_pipe[1], "w");
        if (sink == nullptr || ::setvbuf(sink, nullptr, _IONBF, 0) != 0) ::_exit(10);
        server::ServerRuntimeOptions options;
        options.log = sink;
        options.day_time_at_start = kTenAm;
        options.busy_poll = true;
        options.end_of_day_drain_timeout_ns = 0;
        server::ServerRuntime runtime(make_e2e_config(false), options);
        if (!runtime.start().empty()) ::_exit(11);
        const int flags = ::fcntl(log_pipe[1], F_GETFL);
        ::fcntl(log_pipe[1], F_SETFL, flags | O_NONBLOCK);
        std::array<char, 1024> bytes{};
        while (::write(log_pipe[1], bytes.data(), bytes.size()) > 0) {}
        while (::write(log_pipe[1], bytes.data(), 1) > 0) {}
        ::fcntl(log_pipe[1], F_SETFL, flags);
        if (::write(ready_pipe[1], "R", 1) != 1) ::_exit(13);
        runtime.request_shutdown();
        runtime.poll_once();
        ::_exit(runtime.finished() ? 0 : 12);
    }
    ::close(ready_pipe[1]);
    pollfd readiness{ready_pipe[0], POLLIN, 0};
    const bool ready = ::poll(&readiness, 1, 5000) == 1;
    int status = 0;
    bool exited = false;
    if (ready) {
        const auto deadline = net::monotonic_ns() + 300'000'000;
        do {
            if (::waitpid(child, &status, WNOHANG) == child) { exited = true; break; }
            ::poll(nullptr, 0, 1);
        } while (net::monotonic_ns() < deadline);
    }
    if (!exited) { ::kill(child, SIGKILL); ::waitpid(child, &status, 0); }
    ::close(log_pipe[0]);
    ::close(log_pipe[1]);
    ::close(ready_pipe[0]);
    REQUIRE(ready);
    CHECK(exited);
    if (exited) CHECK_EQ(status, 0);
}

TEST_CASE(server_runtime_review, packed_connection_ids_preserve_transport_generations) {
    // The transport retires slots at 24-bit exhaustion. Higher generations
    // are impossible, so runtime packing does not introduce an alias.
    for (const std::uint32_t generation : {1u, 2u, 0x00fffffeu, 0x00ffffffu}) {
        const net::ConnectionId id{17, generation};
        const auto packed = server::runtime::pack_core_connection_id(2, id);
        CHECK_EQ(server::runtime::core_connection_net_id(packed), id);
        CHECK_EQ(server::runtime::core_connection_endpoint_tag(packed), 2);
    }
}

TEST_CASE(server_runtime_review, retransmission_socket_ignores_unconfigured_sender) {
    struct Handler {
        unsigned delivered = 0;
        void on_message(std::uint64_t, std::span<const char>) { ++delivered; }
        void on_end_of_session(std::uint64_t) {}
    } handler;
    md::ReceiverConfig config;
    config.session = "EXSIM";
    client::MarketDataFeed<Handler> feed(handler, config);
    REQUIRE_EQ(feed.open(net::loopback_endpoint(0)), 0);
    net::UdpReceiver trusted_server, attacker;
    REQUIRE_EQ(trusted_server.open(net::loopback_endpoint(0)), 0);
    REQUIRE_EQ(attacker.open(net::loopback_endpoint(0)), 0);
    net::Ipv4Endpoint trusted{}, target{};
    REQUIRE_EQ(net::local_endpoint(trusted_server.fd(), trusted), 0);
    REQUIRE_EQ(net::local_endpoint(feed.request_fd(), target), 0);
    feed.set_retransmission_server(trusted);
    std::array<char, 64> storage{};
    protocol::moldudp64::PacketBuilder packet(storage);
    REQUIRE(packet.reset("EXSIM", 1));
    const std::array<char, 1> payload{'A'};
    REQUIRE(packet.append(payload) == protocol::Status::ok);
    REQUIRE_EQ(attacker.send_to(packet.packet(), target), static_cast<ssize_t>(packet.packet().size()));
    pollfd ready{feed.request_fd(), POLLIN, 0};
    REQUIRE_EQ(::poll(&ready, 1, 1000), 1);
    feed.poll();
    CHECK_EQ(handler.delivered, 0u);
    CHECK_EQ(feed.next_sequence(), 1u);
    REQUIRE_EQ(trusted_server.send_to(packet.packet(), target), static_cast<ssize_t>(packet.packet().size()));
    REQUIRE_EQ(::poll(&ready, 1, 1000), 1);
    feed.poll();
    CHECK_EQ(handler.delivered, 1u);
    CHECK_EQ(feed.next_sequence(), 2u);
}

TEST_CASE(server_runtime_review, retransmission_drain_yields_after_64_datagrams) {
    server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.day_time_at_start = kTenAm;
    server::ServerRuntime runtime(make_e2e_config(false), options);
    REQUIRE(runtime.start().empty());
    net::UdpReceiver sender;
    REQUIRE_EQ(sender.open(net::loopback_endpoint(0)), 0);
    const std::array<char, 1> malformed{'x'};
    for (int index = 0; index < 65; ++index)
        REQUIRE_EQ(sender.send_to(malformed, runtime.market_data().retransmission_endpoint()), 1);
    runtime.market_data().handle_retransmission_requests();
    CHECK_EQ(runtime.market_data().stats().retransmission_requests, 64u);
    runtime.market_data().handle_retransmission_requests();
    CHECK_EQ(runtime.market_data().stats().retransmission_requests, 65u);
}

TEST_CASE(server_runtime_checks, duplicate_login_preserves_first_binding_and_slot_reuse) {
    ServerUnderTest server(make_e2e_config(false), 0, false, true);
    REQUIRE(server.started());
    OuchRecorder first_events, second_events;
    OuchSession first(first_events), second(second_events);
    auto pump = [&] { first.poll(); second.poll(); };
    REQUIRE(first.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return first_events.logins_accepted == 1; }, pump));
    REQUIRE(second.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return second_events.logins_rejected == 1; }, pump));
    REQUIRE(first.client().enter_order(ouch_limit_order("BOUND", Side::BUY, 10, 140 * kDollar), net::monotonic_ns()));
    first.flush();
    REQUIRE(wait_until([&] { return first_events.has_accepted("BOUND"); }, pump));
    first.disconnect();
    client::AdminLineClient admin;
    REQUIRE(admin.connect(server.admin()));
    REQUIRE(wait_until([&] { return admin.request("book AAPL").find("(empty)") != std::string::npos; }, pump));
    second.disconnect();
    REQUIRE(second.connect_and_login(server.ouch(), {"ALICE", "alice", ""}, 0));
    REQUIRE(wait_until([&] { return second_events.logins_accepted == 1; }, pump));
}

TEST_CASE(server_runtime_checks, trading_day_end_finishes_in_busy_poll_mode) {
    auto config = make_e2e_config(false);
    config.exchange.trading_day_end_ns = kTenAm + 10'000'000;
    server::ServerRuntimeOptions options;
    options.log = nullptr;
    options.day_time_at_start = kTenAm;
    options.busy_poll = true;
    options.end_of_day_drain_timeout_ns = 0;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    const auto deadline = net::monotonic_ns() + 1'000'000'000;
    while (!runtime.finished() && net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK(runtime.finished());
    CHECK(runtime.core().shutdown_requested());
}
