// One real UDP round trip on 127.0.0.1 (port 0): publisher -> UdpSender ->
// feed socket -> MoldUdp64Receiver -> ItchBookBuilder, with packets dropped
// before the send so the receiver must recover through a RetransmissionServer
// that answers requests on its own UDP socket.

#include <md/book_builder.hpp>
#include <md/publisher.hpp>
#include <md/receiver.hpp>
#include <md/retransmission_server.hpp>
#include <net/clocks.hpp>
#include <net/poller.hpp>
#include <net/udp.hpp>
#include <md/packet_faults.hpp>
#include <test/exsim/md/md_test_support.hpp>
#include <test/exsim/support/command_gen.hpp>
#include <test/framework/test.hpp>

#include <array>
#include <cerrno>
#include <string>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

struct UdpFeedSink {
    md::PacketFaultInjector* injector;
    net::UdpSender* sender;
    net::Ipv4Endpoint destination;
    void operator()(std::span<const char> packet) const {
        injector->submit(packet, [this](std::span<const char> survivor) {
            REQUIRE_EQ(sender->send_to(survivor, destination), static_cast<ssize_t>(survivor.size()));
        });
    }
};

struct UdpRequestSink {
    net::UdpReceiver* socket;
    net::Ipv4Endpoint server;
    void operator()(std::span<const char> request) const {
        REQUIRE_EQ(socket->send_to(request, server), static_cast<ssize_t>(request.size()));
    }
};

}  // namespace

TEST_CASE(md_udp, loopback_round_trip_with_retransmission) {
    net::UdpReceiver feed_socket;
    net::UdpReceiver retransmission_socket;
    net::UdpReceiver client_request_socket;
    net::UdpSender feed_sender;
    REQUIRE_EQ(feed_socket.open(net::loopback_endpoint(0)), 0);
    REQUIRE_EQ(retransmission_socket.open(net::loopback_endpoint(0)), 0);
    REQUIRE_EQ(client_request_socket.open(net::loopback_endpoint(0)), 0);
    REQUIRE_EQ(feed_sender.open(), 0);
    net::Ipv4Endpoint feed_endpoint{}, retransmission_endpoint{};
    REQUIRE_EQ(net::local_endpoint(feed_socket.fd(), feed_endpoint), 0);
    REQUIRE_EQ(net::local_endpoint(retransmission_socket.fd(), retransmission_endpoint), 0);
    CHECK_NE(feed_endpoint.port, 0u);

    const server::journal::JournalSetup setup = testsupport::fuzz_setup();
    MatchingEngine engine(setup.engine);
    for (const auto& instrument : setup.instruments) REQUIRE(engine.add_instrument(instrument) != 0);
    EventBuffer events(required_event_capacity(setup.engine));
    md::PacketFaultInjector injector(1400, 8);
    md::PublisherConfig publisher_config;
    publisher_config.max_instruments = setup.engine.max_instruments;
    publisher_config.noii_interval_ns = 0;
    md::MarketDataPublisher<UdpFeedSink> publisher(publisher_config,
                                                   UdpFeedSink{&injector, &feed_sender, feed_endpoint});
    md::RetransmissionServer server(publisher.ring(), md::RetransmissionConfig{});

    md::ItchBookBuilder builder(md::BookBuilderConfig{setup.engine.max_instruments, setup.engine.max_orders,
                                                      setup.engine.max_orders});
    md::ReceiverConfig receiver_config;
    receiver_config.session = "EXSIM";
    receiver_config.request_timeout_ns = 20'000'000;
    md::MoldUdp64Receiver<md::ItchBookBuilder, UdpRequestSink> receiver(
        receiver_config, builder, UdpRequestSink{&client_request_socket, retransmission_endpoint});

    std::array<char, 2048> datagram{};
    // Drains every socket once; returns true if anything arrived.
    auto service_sockets = [&]() {
        bool any = false;
        net::Ipv4Endpoint peer{};
        for (;;) {
            const ssize_t length = feed_socket.receive_from(datagram, peer);
            if (length < 0) {
                REQUIRE(net::is_would_block(errno));
                break;
            }
            any = true;
            CHECK(receiver.on_packet(std::span<const char>(datagram.data(), static_cast<std::size_t>(length)),
                                     net::monotonic_ns()) == md::PacketStatus::Accepted);
        }
        for (;;) {
            const ssize_t length = retransmission_socket.receive_from(datagram, peer);
            if (length < 0) {
                REQUIRE(net::is_would_block(errno));
                break;
            }
            any = true;
            const md::RetransmitResult result = server.answer(
                std::span<const char>(datagram.data(), static_cast<std::size_t>(length)),
                [&](std::span<const char> response) {
                    REQUIRE_EQ(retransmission_socket.send_to(response, peer), static_cast<ssize_t>(response.size()));
                });
            CHECK(result.status == md::RetransmitStatus::Answered);
        }
        for (;;) {
            const ssize_t length = client_request_socket.receive_from(datagram, peer);
            if (length < 0) {
                REQUIRE(net::is_would_block(errno));
                break;
            }
            any = true;
            CHECK(peer == retransmission_endpoint);
            CHECK(receiver.on_packet(std::span<const char>(datagram.data(), static_cast<std::size_t>(length)),
                                     net::monotonic_ns()) == md::PacketStatus::Accepted);
        }
        receiver.poll(net::monotonic_ns());
        return any;
    };

    net::Poller poller;
    REQUIRE_EQ(poller.open(), 0);
    REQUIRE_EQ(poller.add(feed_socket.fd(), 1, net::PollInterest::Read), 0);
    REQUIRE_EQ(poller.add(retransmission_socket.fd(), 2, net::PollInterest::Read), 0);
    REQUIRE_EQ(poller.add(client_request_socket.fd(), 3, net::PollInterest::Read), 0);

    Timestamp now = 1'000'000;
    REQUIRE(publisher.add_instruments_from(engine) == md::PublishStatus::Ok);
    REQUIRE(publisher.start_session(now) == md::PublishStatus::Ok);
    testsupport::CommandGenerator generator(5);
    for (std::size_t index = 0; index < 400; ++index) {
        if (index == 50) injector.drop_next(2);
        if (index == 200) injector.drop_next(1);
        now += 1'000'000;
        Command command = generator.next(engine);
        set_command_time(command, now);
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        REQUIRE(publisher.on_events(events) == md::PublishStatus::Ok);
        (void)service_sockets();
    }
    REQUIRE(publisher.end_of_day(now + 1'000'000) == md::PublishStatus::Ok);

    const std::uint64_t deadline = net::monotonic_ns() + 5'000'000'000ULL;
    while (!receiver.end_of_session_reached() && net::monotonic_ns() < deadline) {
        if (!service_sockets()) (void)poller.wait(1);
    }
    REQUIRE(receiver.end_of_session_reached());
    CHECK_EQ(injector.stats().dropped, 3u);
    CHECK_GE(receiver.stats().requests_sent, 1u);
    CHECK_GE(server.stats().responses, 1u);
    CHECK_EQ(receiver.stats().messages_lost, 0u);
    CHECK_EQ(receiver.next_sequence(), publisher.next_sequence());
    CHECK_EQ(receiver.stats().messages_delivered, publisher.stats().messages_sequenced);
    CHECK_EQ(builder.rejected_message_count(), 0u);
    CHECK_EQ(book_difference(engine, builder, engine.instrument_count(), setup.engine.max_orders), std::string());
}
