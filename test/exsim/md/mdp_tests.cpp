#include "md/mdp_publisher.hpp"
#include "md/mdp_replay_session.hpp"
#include "test/framework/test.hpp"
#include "test/framework/alloc_counter.hpp"

#include <map>
#include <random>

namespace {
using namespace exsim;
namespace cme = protocol::cme;
struct Packet { std::vector<char> bytes; md::MdpStream stream; };
struct Capture {
    std::vector<Packet>* packets;
    void operator()(std::span<const char> bytes, md::MdpStream stream) { packets->push_back({{bytes.begin(), bytes.end()}, stream}); }
};
server::ServerConfig mdp_config() {
    server::ServerConfig config;
    config.engine.engine = {2, 32};
    config.market_data.protocol = server::MarketDataProtocol::Mdp3;
    config.market_data.replay_buffer_packets = 64;
    config.market_data.replay_username = "CME";
    config.market_data.replay_password = "secret";
    config.market_data.snapshot_interval_ms = 1;
    InstrumentConfig instrument;
    instrument.symbol = Symbol("ES");
    instrument.cme_security_id = 800001;
    instrument.tick_size = 100;
    instrument.min_price = 100;
    instrument.max_price = 100000;
    instrument.reference_price = 10000;
    instrument.max_order_qty = 1000000000;
    const std::string_view name = "LONG-CME-SYMBOL-1234";
    std::copy(name.begin(), name.end(), instrument.cme_symbol.begin());
    config.instruments.push_back(instrument);
    return config;
}

// Independent MBP consumer: applies New/Change/Delete rank semantics and
// checks report sequence continuity using the schema's wire offsets.
struct Book {
    struct Level { std::int64_t price; std::int32_t qty; std::int32_t orders; };
    std::vector<Level> bids, asks;
    std::uint32_t report = 0;
    unsigned end_events = 0;
    void packet(const Packet& packet) {
        cme::MdpPacketView view;
        REQUIRE_EQ(cme::decode_mdp_packet(packet.bytes, view), protocol::Status::ok);
        cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_message(view.messages, message), protocol::Status::ok);
        if (packet.stream == md::MdpStream::Snapshot) {
            REQUIRE_EQ(message.header.template_id, 52);
            report = cme::load_le<std::uint32_t>(message.body.data() + 12);
            bids.clear(); asks.clear();
            cme::GroupView entries;
            REQUIRE_EQ(cme::decode_group(message.trailing(), 22, entries), protocol::Status::ok);
            for (unsigned i = 0; i < entries.count; ++i) {
                const auto entry = entries.entry(i);
                auto& side = entry[21] == '0' ? bids : asks;
                side.push_back({cme::load_le<std::int64_t>(entry.data()), cme::load_le<std::int32_t>(entry.data() + 8),
                                cme::load_le<std::int32_t>(entry.data() + 12)});
            }
        } else if (packet.stream == md::MdpStream::Incremental &&
                   (message.header.template_id == 46 || message.header.template_id == 48)) {
            if (static_cast<unsigned char>(message.body[8]) & 0x80) ++end_events;
            cme::GroupView entries;
            REQUIRE_EQ(cme::decode_group(message.trailing(), 32, entries), protocol::Status::ok);
            for (unsigned i = 0; i < entries.count; ++i) {
                const auto entry = entries.entry(i);
                CHECK_EQ(cme::load_le<std::uint32_t>(entry.data() + 16), ++report);
                if (message.header.template_id == 48) continue;
                auto& side = entry[26] == '0' ? bids : asks;
                const auto rank = static_cast<std::size_t>(static_cast<unsigned char>(entry[24]) - 1);
                const auto action = static_cast<unsigned char>(entry[25]);
                Level level{cme::load_le<std::int64_t>(entry.data()), cme::load_le<std::int32_t>(entry.data() + 8),
                            cme::load_le<std::int32_t>(entry.data() + 20)};
                if (action == 0) {
                    REQUIRE(rank <= side.size());
                    side.insert(side.begin() + rank, level);
                    if (side.size() > 10) side.resize(10);
                } else {
                    REQUIRE(rank < side.size());
                    if (action == 1) side[rank] = level;
                    else { REQUIRE_EQ(action, 2); side.erase(side.begin() + rank); }
                }
                for (std::size_t level_index = 1; level_index < side.size(); ++level_index)
                    CHECK(entry[26] == '0' ? side[level_index - 1].price > side[level_index].price :
                                           side[level_index - 1].price < side[level_index].price);
            }
        }
    }
    void compare(const MatchingEngine& engine) {
        for (const auto side : {Side::BUY, Side::SELL}) {
            std::array<LevelInfo, 10> levels{};
            const auto count = engine.depth(1, side, levels.data(), levels.size());
            const auto& actual = side == Side::BUY ? bids : asks;
            REQUIRE_EQ(actual.size(), count);
            for (std::size_t i = 0; i < count; ++i) {
                CHECK_EQ(actual[i].price, levels[i].price * 100000);
                CHECK_EQ(static_cast<std::uint64_t>(actual[i].qty), levels[i].qty);
                CHECK_EQ(static_cast<std::uint32_t>(actual[i].orders), levels[i].order_count);
            }
        }
    }
};
struct Exchange {
    server::ServerConfig config = mdp_config();
    MatchingEngine engine{config.engine.engine};
    EventBuffer events{required_event_capacity(config.engine.engine)};
    std::vector<Packet> packets;
    md::MdpPublisher<Capture> publisher{config, Capture{&packets}};
    Timestamp now = 1000;
    Exchange() {
        REQUIRE_EQ(engine.add_instrument(config.instruments[0]), 1);
        REQUIRE_EQ(publisher.start(now, 1700000000000000000ull + now, engine), md::PublishStatus::Ok);
    }
    void submit(Command command, Book* book = nullptr) {
        events.clear(); packets.clear();
        ++now;
        switch (command.type) {
            case CommandType::NewOrder: command.new_order.ts = now; break;
            case CommandType::Cancel: command.cancel.ts = now; break;
            case CommandType::Replace: command.replace.ts = now; break;
            case CommandType::MassCancel: command.mass_cancel.ts = now; break;
            case CommandType::SetPhase: command.set_phase.ts = now; break;
        }
        REQUIRE_EQ(engine.handle(command, events), HandleResult::Applied);
        REQUIRE_EQ(publisher.on_events({events.begin(), events.size()}), md::PublishStatus::Ok);
        if (book) { for (const auto& packet : packets) book->packet(packet); book->compare(engine); }
    }
    void order(ClOrdId id, Side side, Price price, Quantity quantity, Book* book = nullptr) {
        NewOrder order;
        order.instrument = 1; order.session = 1; order.cl_ord_id = id;
        order.side = side; order.price = price; order.qty = quantity;
        submit(Command{order}, book);
    }
};
struct Sink {
    std::vector<char>* bytes = nullptr;
    std::size_t* used = nullptr;
    char* prepare(std::size_t size) { return size <= bytes->size() - *used ? bytes->data() + *used : nullptr; }
    void commit(std::size_t size) { *used += size; }
};
std::string request(std::string_view type, std::string_view fields = {}, bool compact = true, bool request_id = true) {
    std::array<char, 1024> bytes{};
    protocol::fix44::Builder builder(bytes);
    builder.field(35, type);
    if (type == "V" && request_id) builder.field(262, "RECOVERY");
    std::size_t start = 0;
    while (start < fields.size()) {
        const auto end = fields.find('|', start);
        const auto item = fields.substr(start, end == std::string_view::npos ? end : end - start);
        const auto equal = item.find('=');
        std::uint64_t tag = 0;
        REQUIRE(protocol::fix44::parse_uint(item.substr(0, equal), tag));
        REQUIRE(builder.field(static_cast<unsigned>(tag), item.substr(equal + 1)));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    const auto size = builder.finish();
    REQUIRE(size);
    std::string result(bytes.data() + (compact ? 10 : 0), size - (compact ? 10 : 0));
    if (compact) {
        unsigned sum = 0;
        for (std::size_t i = 0; i < result.size() - 7; ++i) sum += static_cast<unsigned char>(result[i]);
        sum %= 256;
        result[result.size() - 4] = static_cast<char>('0' + sum / 100);
        result[result.size() - 3] = static_cast<char>('0' + sum / 10 % 10);
        result[result.size() - 2] = static_cast<char>('0' + sum % 10);
    }
    return result;
}
}

TEST_CASE(mdp_publisher, startup_definitions_and_empty_snapshot) {
    Exchange exchange;
    REQUIRE_EQ(exchange.packets.size(), 3u);
    cme::MdpPacketView packet;
    cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(exchange.packets[1].bytes, packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    REQUIRE_EQ(message.header.template_id, 54);
    CHECK_EQ(cme::load_le<std::int32_t>(message.body.data() + 55), 800001);
    CHECK_EQ(std::string_view(message.body.data() + 35, 20), "LONG-CME-SYMBOL-1234");
    Book book;
    book.packet(exchange.packets[2]);
    book.compare(exchange.engine);
    CHECK_EQ(book.report, 0u);
    CHECK_EQ(exchange.publisher.ring().next_sequence(), 2u);
}

TEST_CASE(mdp_publisher, matching_cancel_replace_and_rank_changes_reconstruct) {
    Exchange exchange;
    Book book;
    exchange.order(1, Side::BUY, 10000, 5, &book);
    exchange.order(2, Side::BUY, 9900, 7, &book);
    exchange.order(3, Side::BUY, 10100, 2, &book);
    exchange.order(4, Side::SELL, 10100, 1, &book);
    CancelOrder cancel; cancel.session = 1; cancel.orig_cl_ord_id = 3;
    exchange.submit(Command{cancel}, &book);
    ReplaceOrder replace; replace.session = 1; replace.orig_cl_ord_id = 2;
    replace.cl_ord_id = 5; replace.price = 10200; replace.qty = 6;
    exchange.submit(Command{replace}, &book);
    MassCancel mass; mass.session = 1;
    exchange.submit(Command{mass}, &book);
    CHECK(book.bids.empty());
    CHECK(book.asks.empty());
    CHECK_EQ(book.end_events, 7u);
}

TEST_CASE(mdp_publisher, seeded_command_stream_matches_engine_at_every_boundary) {
    Exchange exchange;
    Book book;
    std::mt19937 random(8173);
    for (ClOrdId id = 1; id <= 500; ++id) {
        if (id % 17 == 0) {
            MassCancel cancel; cancel.session = 1;
            exchange.submit(Command{cancel}, &book);
        } else exchange.order(id, (random() & 1) ? Side::BUY : Side::SELL,
                               9000 + (random() % 21) * 100, 1 + random() % 9, &book);
    }
}

TEST_CASE(mdp_publisher, snapshot_boundary_recovers_a_missed_incremental) {
    Exchange exchange;
    Book book;
    exchange.order(1, Side::BUY, 10000, 4);  // lost by the consumer
    exchange.order(2, Side::SELL, 11000, 9);
    exchange.packets.clear();
    exchange.now += 1000000;
    exchange.publisher.poll(exchange.now, UINT64_MAX);
    bool snapshot_seen = false;
    for (const auto& packet : exchange.packets) {
        if (packet.stream != md::MdpStream::Snapshot) continue;
        book.packet(packet); snapshot_seen = true;
        cme::MdpPacketView view; cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_packet(packet.bytes, view), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(view.messages, message), protocol::Status::ok);
        CHECK_EQ(cme::load_le<std::uint32_t>(message.body.data()), exchange.publisher.next_sequence() - 1);
    }
    REQUIRE(snapshot_seen);
    book.compare(exchange.engine);
    exchange.order(3, Side::BUY, 11000, 3, &book);
}

TEST_CASE(mdp_publisher, unrepresentable_aggregate_stops_before_partial_event) {
    Exchange exchange;
    exchange.order(1, Side::BUY, 10000, 1000000000);
    exchange.order(2, Side::BUY, 10000, 1000000000);
    const auto sequence = exchange.publisher.next_sequence();
    exchange.packets.clear(); exchange.events.clear();
    NewOrder order; order.instrument = 1; order.session = 1; order.cl_ord_id = 3;
    order.price = 10000; order.qty = 1000000000; order.ts = ++exchange.now;
    REQUIRE_EQ(exchange.engine.handle(order, exchange.events), HandleResult::Applied);
    CHECK_EQ(exchange.publisher.on_events({exchange.events.begin(), exchange.events.size()}), md::PublishStatus::UnrepresentableField);
    CHECK_EQ(exchange.publisher.state(), md::PublisherState::Stopped);
    CHECK_EQ(exchange.publisher.next_sequence(), sequence);
    CHECK(exchange.packets.empty());
}

TEST_CASE(mdp_replay, ring_eviction_and_exact_original_packets) {
    md::MdpReplayRing ring(2, 32);
    const std::array<char, 3> bytes{1, 2, 3};
    CHECK(!ring.append(2, bytes));
    REQUIRE(ring.append(1, bytes)); REQUIRE(ring.append(2, bytes)); REQUIRE(ring.append(3, bytes));
    CHECK(ring.get(1).empty()); CHECK(ring.get(4).empty());
    CHECK_EQ(ring.first_sequence(), 2u);
    CHECK_EQ(ring.get(2).size(), 3u);
    CHECK_EQ(ring.get(3)[2], 3);
}

TEST_CASE(mdp_replay, compact_login_fragmentation_replay_and_logout) {
    Exchange exchange;
    std::vector<char> bytes(65536);
    std::size_t used = 0;
    md::MdpReplaySession<Sink> session(exchange.config.market_data, exchange.publisher.ring());
    session.open(Sink{&bytes, &used}, 0);
    const auto login = request("A", "553=CME|554=secret|");
    for (std::size_t i = 0; i < login.size(); ++i)
        CHECK_EQ(session.on_data(std::span(login).first(i), 1, 1700000000000000000ull), 0u);
    CHECK_EQ(session.on_data(login, 1, 1700000000000000000ull), login.size());
    REQUIRE(used > 2);
    CHECK_EQ(cme::load_le<std::uint16_t>(bytes.data()), used - 2);
    cme::MdpPacketView packet; cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(std::span(bytes).subspan(2, used - 2), packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    CHECK_EQ(message.header.schema_id, 9); CHECK_EQ(message.header.template_id, 408);
    used = 0;
    const auto recover = request("V", "1180=1|1182=1|1183=1|");
    CHECK_EQ(session.on_data(recover, 2, 1700000000000000000ull), recover.size());
    const auto original = exchange.publisher.ring().get(1);
    REQUIRE(used > original.size() + 2);
    CHECK_EQ(std::string_view(bytes.data() + 2, original.size()), std::string_view(original.data(), original.size()));
    const auto logout = request("5");
    CHECK_EQ(session.on_data(logout, 3, 1700000000000000000ull), logout.size());
    CHECK(session.wants_close());
}

TEST_CASE(mdp_replay, invalid_credentials_ranges_checksum_and_timeout) {
    Exchange exchange;
    for (unsigned variant = 0; variant < 6; ++variant) {
        std::vector<char> bytes(65536); std::size_t used = 0;
        md::MdpReplaySession<Sink> session(exchange.config.market_data, exchange.publisher.ring());
        session.open(Sink{&bytes, &used}, 0);
        auto login = request("A", variant == 0 ? "553=CME|554=wrong|" : "553=CME|554=secret|", false);
        if (variant == 1) login[login.size() - 2] = 'x';
        CHECK_EQ(session.on_data(login, 1, 1700000000000000000ull), login.size());
        if (variant == 2 || variant == 3 || variant == 5) {
            used = 0;
            const auto recover = request("V", variant == 2 ? "1180=1|1182=1|1183=2001|" : "1180=1|1182=2|1183=2|", true, variant != 5);
            CHECK_EQ(session.on_data(recover, 2, 1700000000000000000ull), recover.size());
        } else if (variant == 4) { used = 0; session.on_timer(5000000001ull, 1700000000000000000ull); }
        const auto size = cme::load_le<std::uint16_t>(bytes.data());
        cme::MdpPacketView packet; cme::MessageView message;
        // Bad credentials/checksum reply is Logout; other variants cleared
        // their successful Logon first and now must contain only Logout.
        REQUIRE_EQ(cme::decode_mdp_packet(std::span(bytes).subspan(2, size), packet), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
        CHECK_EQ(message.header.template_id, 409);
        session.on_timer(11000000000ull, 1700000000000000000ull);
        CHECK(session.wants_close());
    }
}

TEST_CASE(mdp_publisher, publication_and_snapshot_paths_do_not_allocate) {
    if (!testfw::allocation_counting_available()) return;
    struct Count {
        std::size_t* count;
        void operator()(std::span<const char>, md::MdpStream) const noexcept { ++*count; }
    };
    auto config = mdp_config();
    MatchingEngine engine(config.engine.engine);
    REQUIRE_EQ(engine.add_instrument(config.instruments[0]), 1);
    EventBuffer events(required_event_capacity(config.engine.engine));
    std::size_t count = 0;
    md::MdpPublisher<Count> publisher(config, Count{&count});
    REQUIRE_EQ(publisher.start(1000, 1700000000000001000ull, engine), md::PublishStatus::Ok);
    NewOrder order;
    order.ts = 1001; order.instrument = 1; order.session = 1; order.cl_ord_id = 1; order.price = 10000; order.qty = 5;
    REQUIRE_EQ(engine.handle(order, events), HandleResult::Applied);
    std::size_t allocations = 0;
    md::PublishStatus status;
    { testfw::AllocationScope scope;
      status = publisher.on_events({events.begin(), events.size()}); publisher.poll(1001001, UINT64_MAX);
      allocations = scope.count(); }
    CHECK_EQ(status, md::PublishStatus::Ok); CHECK_EQ(allocations, 0u); CHECK(count >= 5);
}
