// Zero heap allocation after setup: MarketDataPublisher (on_events, poll,
// flush), MoldUdp64Receiver + ItchBookBuilder (on_packet, poll) and
// RetransmissionServer (answer), in steady state with faults injected so
// the retransmission path runs too. Skipped where allocation counting is
// unavailable (sanitizer builds).

#include "test/exsim/support/clients/itch_book_builder.hpp"
#include <md/publisher.hpp>
#include "test/exsim/support/clients/moldudp64_receiver.hpp"
#include <md/retransmission_server.hpp>
#include <md/packet_faults.hpp>
#include <test/exsim/md/md_test_support.hpp>
#include <test/exsim/support/command_gen.hpp>
#include <test/framework/alloc_counter.hpp>
#include <test/framework/test.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

// Fixed-capacity FIFO of datagrams; never allocates after construction.
class FixedPacketQueue {
public:
    FixedPacketQueue(std::size_t slots, std::size_t slot_size)
        : slots_(slots),
          slot_size_(slot_size),
          storage_(std::make_unique<char[]>(slots * slot_size)),
          lengths_(std::make_unique<std::size_t[]>(slots)) {}

    void push(std::span<const char> packet) {
        REQUIRE(count_ < slots_);
        REQUIRE(packet.size() <= slot_size_);
        const std::size_t slot = (head_ + count_) % slots_;
        std::memcpy(storage_.get() + slot * slot_size_, packet.data(), packet.size());
        lengths_[slot] = packet.size();
        ++count_;
    }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Copies the oldest packet into `out` (valid until the next pop).
    std::span<const char> pop(std::span<char> out) {
        const std::size_t length = lengths_[head_];
        std::memcpy(out.data(), storage_.get() + head_ * slot_size_, length);
        head_ = (head_ + 1) % slots_;
        --count_;
        return out.first(length);
    }

private:
    std::size_t slots_;
    std::size_t slot_size_;
    std::unique_ptr<char[]> storage_;
    std::unique_ptr<std::size_t[]> lengths_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

struct FixedQueueSink {
    FixedPacketQueue* queue;
    void operator()(std::span<const char> packet) const { queue->push(packet); }
};

struct FaultyFeedSink {
    md::PacketFaultInjector* injector;
    FixedPacketQueue* queue;
    void operator()(std::span<const char> packet) const { injector->submit(packet, FixedQueueSink{queue}); }
};

}  // namespace

TEST_CASE(md_allocation, steady_state_feed_allocates_nothing) {
    if (!testfw::allocation_counting_available()) {
        std::printf("allocation counting unavailable, skipped ");
        return;
    }
    const server::journal::JournalSetup setup = testsupport::fuzz_setup();
    MatchingEngine engine(setup.engine);
    for (const auto& instrument : setup.instruments) REQUIRE(engine.add_instrument(instrument) != 0);
    EventBuffer events(required_event_capacity(setup.engine));

    constexpr std::size_t kPayload = 512;
    FixedPacketQueue feed_queue(256, kPayload);
    FixedPacketQueue request_queue(64, md::kMoldRequestSize);
    FixedPacketQueue response_queue(64, kPayload);
    md::PacketFaultInjector injector(kPayload, 16);
    md::PacketFaultConfig faults;
    faults.drop_ppm = 20'000;
    faults.duplicate_ppm = 20'000;
    faults.reorder_ppm = 20'000;
    faults.reorder_delay = 2;
    faults.seed = 77;
    injector.configure(faults);

    md::PublisherConfig publisher_config;
    publisher_config.max_payload = kPayload;
    publisher_config.max_instruments = setup.engine.max_instruments;
    publisher_config.noii_interval_ns = 5'000'000;
    publisher_config.heartbeat_interval_ns = 50'000'000;
    md::MarketDataPublisher<FaultyFeedSink> publisher(publisher_config, FaultyFeedSink{&injector, &feed_queue});
    md::RetransmissionConfig server_config;
    server_config.max_payload = kPayload;
    md::RetransmissionServer server(publisher.ring(), server_config);
    md::ItchBookBuilder builder(md::BookBuilderConfig{setup.engine.max_instruments, setup.engine.max_orders,
                                                      setup.engine.max_orders});
    md::ReceiverConfig receiver_config;
    receiver_config.session = "EXSIM";
    receiver_config.request_timeout_ns = 3'000'000;
    receiver_config.max_request_attempts = 1'000;
    md::MoldUdp64Receiver<md::ItchBookBuilder, FixedQueueSink> receiver(receiver_config, builder,
                                                                         FixedQueueSink{&request_queue});
    auto datagram = std::make_unique<char[]>(kPayload);
    const std::span<char> datagram_buffer(datagram.get(), kPayload);

    std::size_t publisher_allocations = 0;
    std::size_t receiver_allocations = 0;
    std::size_t server_allocations = 0;
    auto deliver = [&](Timestamp now) {
        while (!feed_queue.empty() || !request_queue.empty() || !response_queue.empty()) {
            if (!feed_queue.empty()) {
                const auto packet = feed_queue.pop(datagram_buffer);
                testfw::AllocationScope scope;
                (void)receiver.on_packet(packet, now);
                receiver_allocations += scope.count();
            }
            while (!request_queue.empty()) {
                const auto request = request_queue.pop(datagram_buffer);
                testfw::AllocationScope scope;
                (void)server.answer(request, FixedQueueSink{&response_queue});
                server_allocations += scope.count();
            }
            if (!response_queue.empty()) {
                const auto packet = response_queue.pop(datagram_buffer);
                testfw::AllocationScope scope;
                (void)receiver.on_packet(packet, now);
                receiver_allocations += scope.count();
            }
        }
        testfw::AllocationScope scope;
        receiver.poll(now);
        receiver_allocations += scope.count();
    };

    Timestamp now = 1'000'000;
    REQUIRE(publisher.add_instruments_from(engine) == md::PublishStatus::Ok);
    REQUIRE(publisher.start_session(now) == md::PublishStatus::Ok);
    testsupport::CommandGenerator generator(4242);
    constexpr std::size_t kWarmupCommands = 2'000;
    constexpr std::size_t kCountedCommands = 30'000;
    for (std::size_t index = 0; index < kWarmupCommands + kCountedCommands; ++index) {
        if (index == kWarmupCommands) {
            publisher_allocations = 0;
            receiver_allocations = 0;
            server_allocations = 0;
        }
        now += index % 500 == 499 ? 60'000'000 : 1'000'000;
        Command command = generator.next(engine);  // allocates (test generator), not counted
        set_command_time(command, now);
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        md::PublishStatus timers = md::PublishStatus::Ok;
        md::PublishStatus published = md::PublishStatus::Ok;
        {
            testfw::AllocationScope scope;
            timers = publisher.poll(now, engine);
            published = publisher.on_events(events);
            publisher.flush();
            publisher_allocations += scope.count();
        }
        REQUIRE(timers == md::PublishStatus::Ok);
        REQUIRE(published == md::PublishStatus::Ok);
        deliver(now);
    }
    CHECK_EQ(publisher_allocations, 0u);
    CHECK_EQ(receiver_allocations, 0u);
    CHECK_EQ(server_allocations, 0u);

    // The counted run did real work, including recovery.
    REQUIRE(publisher.end_of_day(now + 1'000'000) == md::PublishStatus::Ok);
    injector.flush(FixedQueueSink{&feed_queue});
    for (int round = 0; round < 10'000 && !receiver.end_of_session_reached(); ++round) {
        now += 10'000'000;
        deliver(now);
        REQUIRE(publisher.poll_heartbeat(now) == md::PublishStatus::Ok);
    }
    REQUIRE(receiver.end_of_session_reached());
    CHECK_GT(injector.stats().dropped, 0u);
    CHECK_GT(server.stats().responses, 0u);
    CHECK_GT(publisher.stats().imbalance_messages_sent, 0u);
    CHECK_GT(publisher.stats().heartbeats_sent, 0u);
    CHECK_EQ(receiver.stats().messages_lost, 0u);
    CHECK_EQ(builder.rejected_message_count(), 0u);
    CHECK_EQ(book_difference(engine, builder, engine.instrument_count(), setup.engine.max_orders), std::string());
}
