#ifndef EXSIM_TESTS_MD_FEED_HARNESS_HPP
#define EXSIM_TESTS_MD_FEED_HARNESS_HPP

// End-to-end market-data pipeline for tests, all in one thread:
//
//   MatchingEngine -> MarketDataPublisher -> PacketFaultInjector (feed)
//        -> MoldUdp64Receiver -> ItchBookBuilder
//   receiver requests -> RetransmissionServer -> PacketFaultInjector
//        (responses) -> MoldUdp64Receiver
//
// After every publisher action the engine book digest is recorded against
// the publisher's next sequence ("batch boundary"). Whenever the receiver's
// delivered prefix ends exactly at a boundary, the builder digest must equal
// it, so the feed is checked after all packets of each batch even while
// recovery lags behind the engine.

#include <exsim/matching_engine.hpp>
#include <exsim/md/book_builder.hpp>
#include <exsim/md/publisher.hpp>
#include <exsim/md/receiver.hpp>
#include <exsim/md/retransmission_server.hpp>
#include <exsim/sim/journal.hpp>
#include <exsim/sim/packet_faults.hpp>
#include <tests/exsim/md/md_test_support.hpp>
#include <tests/framework/test.hpp>

#include <deque>
#include <limits>
#include <string>
#include <vector>

namespace exsim::mdtest {

struct FeedHarnessOptions {
    std::size_t max_payload = 1400;
    std::size_t ring_messages = 1u << 16;
    std::uint64_t noii_interval_ns = 20'000'000;
    std::uint64_t heartbeat_interval_ns = 1'000'000'000;
    std::uint64_t request_timeout_ns = 5'000'000;
    sim::PacketFaultConfig feed_faults{};    // all rates 0: clean feed
    std::uint32_t response_drop_ppm = 0;
    std::uint64_t response_fault_seed = 7;
};

struct BatchBoundary {
    std::uint64_t end_sequence;  // publisher.next_sequence() after the batch
    std::uint64_t engine_digest;
    std::size_t command_index;
};

// Receiver handler: applies every message to the builder and checks the
// builder against the engine at every batch boundary it reaches.
struct BoundaryCheckingHandler {
    md::ItchBookBuilder* builder;
    std::deque<BatchBoundary>* pending_boundaries;
    std::size_t instrument_count;
    std::vector<LevelInfo> level_buffer;
    std::vector<OrderId> id_buffer;
    std::uint64_t boundaries_checked = 0;
    std::uint64_t boundary_mismatches = 0;
    std::uint64_t message_type_counts[256] = {};
    std::string first_failure{};

    std::uint64_t builder_digest() {
        return book_digest(*builder, instrument_count, level_buffer, id_buffer,
                           [this](InstrumentId id) { return builder->trading_state(id); });
    }

    void on_message(std::uint64_t sequence, std::span<const char> message) {
        builder->on_message(sequence, message);
        if (!message.empty()) ++message_type_counts[static_cast<unsigned char>(message[0])];
        check_delivered_prefix(sequence + 1);
    }

    void on_messages_lost(std::uint64_t first_sequence, std::uint64_t count) {
        note_failure("messages " + std::to_string(first_sequence) + ".." + std::to_string(first_sequence + count - 1) +
                     " declared lost");
    }

    // `delivered_end`: one past the last delivered sequence.
    void check_delivered_prefix(std::uint64_t delivered_end) {
        while (!pending_boundaries->empty() && pending_boundaries->front().end_sequence <= delivered_end) {
            const BatchBoundary& boundary = pending_boundaries->front();
            if (boundary.end_sequence != delivered_end) {
                note_failure("boundary at sequence " + std::to_string(boundary.end_sequence) + " skipped");
            } else {
                ++boundaries_checked;
                if (builder_digest() != boundary.engine_digest) {
                    ++boundary_mismatches;
                    note_failure("builder differs from engine after command " +
                                 std::to_string(boundary.command_index) + " (sequence " +
                                 std::to_string(boundary.end_sequence) + ")");
                }
            }
            pending_boundaries->pop_front();
        }
    }

    void note_failure(std::string text) {
        if (first_failure.empty()) first_failure = std::move(text);
    }
};

class FeedHarness {
public:
    struct FeedSink {
        sim::PacketFaultInjector* injector;
        std::deque<Bytes>* queue;
        void operator()(std::span<const char> packet) const { injector->submit(packet, PacketQueueSink{queue}); }
    };
    using Publisher = md::MarketDataPublisher<FeedSink>;
    using Receiver = md::MoldUdp64Receiver<BoundaryCheckingHandler, PacketQueueSink>;

    FeedHarness(const sim::JournalSetup& setup, const FeedHarnessOptions& options)
        : engine(setup.engine),
          events(required_event_capacity(setup.engine)),
          feed_injector(options.max_payload, 64),
          response_injector(options.max_payload, 64),
          publisher(publisher_config(setup, options), FeedSink{&feed_injector, &feed_queue}),
          builder(md::BookBuilderConfig{setup.engine.max_instruments, setup.engine.max_orders,
                                        setup.engine.max_orders}),
          handler{&builder, &pending_boundaries, setup.instruments.size(),
                  std::vector<LevelInfo>(setup.engine.max_orders + 1), std::vector<OrderId>(setup.engine.max_orders + 1)},
          receiver(receiver_config(options), handler, PacketQueueSink{&request_queue}),
          server(publisher.ring(), server_config(options)),
          max_orders_(setup.engine.max_orders),
          engine_level_buffer_(setup.engine.max_orders + 1),
          engine_id_buffer_(setup.engine.max_orders + 1) {
        for (const InstrumentConfig& instrument : setup.instruments) REQUIRE(engine.add_instrument(instrument) != 0);
        feed_injector.configure(options.feed_faults);
        sim::PacketFaultConfig response_faults;
        response_faults.drop_ppm = options.response_drop_ppm;
        response_faults.seed = options.response_fault_seed;
        response_injector.configure(response_faults);
    }

    void start(Timestamp now) {
        REQUIRE(publisher.add_instruments_from(engine) == md::PublishStatus::Ok);
        REQUIRE(publisher.start_session(now) == md::PublishStatus::Ok);
        register_boundary(0);
        pump(now);
    }

    // Timers, then the command, each followed by a boundary; then delivery.
    void step(const Command& command, std::size_t command_index, Timestamp now) {
        REQUIRE(publisher.poll(now, engine) == md::PublishStatus::Ok);
        register_boundary(command_index);
        events.clear();
        REQUIRE(engine.handle(command, events) == HandleResult::Applied);
        REQUIRE(publisher.on_events(events) == md::PublishStatus::Ok);
        register_boundary(command_index);
        pump(now);
    }

    // End of day, then runs the clock until the receiver completes the session.
    void finish(Timestamp now) {
        REQUIRE(publisher.end_of_day(now) == md::PublishStatus::Ok);
        register_boundary(std::numeric_limits<std::size_t>::max());
        for (int round = 0; round < 100'000 && !receiver.end_of_session_reached(); ++round) {
            feed_injector.flush(PacketQueueSink{&feed_queue});
            response_injector.flush(PacketQueueSink{&response_queue});
            pump(now);
            now += 100'000'000;
            REQUIRE(publisher.poll_heartbeat(now) == md::PublishStatus::Ok);  // repeats End of Session
        }
        REQUIRE(receiver.end_of_session_reached());
    }

    // Delivers every queued feed packet, request and response, interleaved.
    void pump(std::uint64_t now) {
        for (;;) {
            bool moved = false;
            if (!feed_queue.empty()) {
                const Bytes packet = std::move(feed_queue.front());
                feed_queue.pop_front();
                (void)receiver.on_packet(packet, now);
                moved = true;
            }
            while (!request_queue.empty()) {
                const Bytes request = std::move(request_queue.front());
                request_queue.pop_front();
                (void)server.answer(request, [this](std::span<const char> response) {
                    response_injector.submit(response, PacketQueueSink{&response_queue});
                });
                moved = true;
            }
            if (!response_queue.empty()) {
                const Bytes packet = std::move(response_queue.front());
                response_queue.pop_front();
                (void)receiver.on_packet(packet, now);
                moved = true;
            }
            if (moved) continue;
            receiver.poll(now);
            if (request_queue.empty()) return;
        }
    }

    std::uint64_t engine_digest() {
        return book_digest(engine, engine.instrument_count(), engine_level_buffer_, engine_id_buffer_,
                           [this](InstrumentId id) { return md::itch_trading_state(engine.phase(id)); });
    }

    void register_boundary(std::size_t command_index) {
        const BatchBoundary boundary{publisher.next_sequence(), engine_digest(), command_index};
        if (!pending_boundaries.empty() && pending_boundaries.back().end_sequence == boundary.end_sequence) {
            pending_boundaries.back() = boundary;
        } else {
            pending_boundaries.push_back(boundary);
        }
        handler.check_delivered_prefix(receiver.next_sequence());
    }

    // Clean feed: the receiver has everything and the books agree exactly.
    std::string clean_mismatch() const {
        if (receiver.next_sequence() != publisher.next_sequence())
            return "receiver at " + std::to_string(receiver.next_sequence()) + ", publisher at " +
                   std::to_string(publisher.next_sequence());
        return book_difference(engine, builder, engine.instrument_count(), max_orders_);
    }

    // After finish(): every boundary checked, nothing lost or rejected.
    void require_consistent_end() const {
        if (!handler.first_failure.empty()) FAIL(handler.first_failure);
        CHECK(pending_boundaries.empty());
        CHECK_EQ(handler.boundary_mismatches, 0u);
        CHECK_EQ(receiver.stats().messages_lost, 0u);
        CHECK_EQ(receiver.stats().malformed_packets, 0u);
        CHECK_EQ(receiver.stats().wrong_session_packets, 0u);
        CHECK_EQ(builder.rejected_message_count(), 0u);
        CHECK_EQ(receiver.next_sequence(), publisher.next_sequence());
        CHECK_EQ(book_difference(engine, builder, engine.instrument_count(), max_orders_), std::string());
        CHECK_EQ(builder.last_system_event(), 'C');
    }

    MatchingEngine engine;
    EventBuffer events;
    std::deque<Bytes> feed_queue;
    std::deque<Bytes> request_queue;
    std::deque<Bytes> response_queue;
    sim::PacketFaultInjector feed_injector;
    sim::PacketFaultInjector response_injector;
    Publisher publisher;
    md::ItchBookBuilder builder;
    std::deque<BatchBoundary> pending_boundaries;
    BoundaryCheckingHandler handler;
    Receiver receiver;
    md::RetransmissionServer server;

private:
    static md::PublisherConfig publisher_config(const sim::JournalSetup& setup, const FeedHarnessOptions& options) {
        md::PublisherConfig config;
        config.session = "EXSIM";
        config.max_payload = options.max_payload;
        config.retransmit_buffer_messages = options.ring_messages;
        config.max_instruments = setup.engine.max_instruments;
        config.heartbeat_interval_ns = options.heartbeat_interval_ns;
        config.noii_interval_ns = options.noii_interval_ns;
        return config;
    }
    static md::ReceiverConfig receiver_config(const FeedHarnessOptions& options) {
        md::ReceiverConfig config;
        config.session = "EXSIM";
        config.request_timeout_ns = options.request_timeout_ns;
        config.max_request_attempts = 1'000;
        return config;
    }
    static md::RetransmissionConfig server_config(const FeedHarnessOptions& options) {
        md::RetransmissionConfig config;
        config.session = "EXSIM";
        config.max_payload = options.max_payload;
        return config;
    }

    std::size_t max_orders_;
    std::vector<LevelInfo> engine_level_buffer_;
    std::vector<OrderId> engine_id_buffer_;
};

}  // namespace exsim::mdtest

#endif
