#ifndef EXSIM_TESTS_MD_TEST_SUPPORT_HPP
#define EXSIM_TESTS_MD_TEST_SUPPORT_HPP

// Shared helpers for the market-data suites: golden hex bytes, packet
// capture, MoldUDP64 message extraction, and engine/builder book comparison.

#include <matching_engine.hpp>
#include <md/book_builder.hpp>
#include <md/publisher.hpp>
#include <protocol/moldudp64.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::mdtest {

using Bytes = std::vector<char>;

// "41 0001 ff" -> {0x41, 0x00, 0x01, 0xff}. Whitespace is ignored.
inline Bytes hex_bytes(std::string_view hex) {
    Bytes out;
    int high = -1;
    for (char c : hex) {
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else continue;
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<char>((high << 4) | value));
            high = -1;
        }
    }
    return out;
}

inline std::string hex_of(std::span<const char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (char c : bytes) {
        const auto value = static_cast<unsigned char>(c);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 15]);
    }
    return out;
}

// Appends every packet to a queue (test-only; allocates).
struct PacketQueueSink {
    std::deque<Bytes>* queue;
    void operator()(std::span<const char> packet) const { queue->emplace_back(packet.begin(), packet.end()); }
};

struct SequencedMessage {
    std::uint64_t sequence;
    Bytes bytes;
};

struct DecodedPacket {
    std::array<char, 10> session{};
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;
    std::vector<SequencedMessage> messages;
};

inline DecodedPacket decode_packet(std::span<const char> packet) {
    DecodedPacket decoded;
    protocol::moldudp64::PacketReader reader;
    if (reader.reset(packet) != protocol::Status::ok) return decoded;
    decoded.session = reader.header.session;
    decoded.sequence = reader.header.sequence;
    decoded.count = reader.header.count;
    std::span<const char> message;
    for (std::uint64_t sequence = reader.header.sequence; reader.next(message) == protocol::Status::ok; ++sequence)
        decoded.messages.push_back({sequence, Bytes(message.begin(), message.end())});
    return decoded;
}

// Every ITCH message in `packets`, in order.
inline std::vector<SequencedMessage> messages_of(const std::deque<Bytes>& packets) {
    std::vector<SequencedMessage> out;
    for (const Bytes& packet : packets)
        for (auto& message : decode_packet(packet).messages) out.push_back(std::move(message));
    return out;
}

inline std::string message_types(const std::vector<SequencedMessage>& messages) {
    std::string out;
    for (const auto& message : messages) out.push_back(message.bytes.empty() ? '?' : message.bytes[0]);
    return out;
}

inline InstrumentConfig test_instrument(std::string_view symbol, Phase initial_phase = Phase::Continuous) {
    InstrumentConfig config;
    config.symbol = Symbol(symbol);
    config.tick_size = 100;
    config.lot_size = 1;
    config.max_order_qty = 1'000'000;
    config.min_price = 990'000;
    config.max_price = 1'010'000;
    config.reference_price = 1'000'000;
    config.initial_phase = initial_phase;
    return config;
}

inline NewOrder limit_order(Timestamp ts, InstrumentId instrument, SessionId session, ClOrdId token, Side side,
                            Price price, Quantity qty, TimeInForce tif = TimeInForce::Day) {
    NewOrder order;
    order.ts = ts;
    order.instrument = instrument;
    order.session = session;
    order.cl_ord_id = token;
    order.side = side;
    order.type = OrdType::Limit;
    order.tif = tif;
    order.price = price;
    order.qty = qty;
    return order;
}

inline void set_command_time(Command& command, Timestamp ts) {
    switch (command.type) {
        case CommandType::NewOrder: command.new_order.ts = ts; break;
        case CommandType::Cancel: command.cancel.ts = ts; break;
        case CommandType::Replace: command.replace.ts = ts; break;
        case CommandType::MassCancel: command.mass_cancel.ts = ts; break;
        case CommandType::SetPhase: command.set_phase.ts = ts; break;
    }
}

// First difference between two books exposing depth()/level_orders(), or
// an empty string. Also compares the ITCH trading state of each instrument.
template <class EngineBook>
std::string book_difference(const EngineBook& engine, const md::ItchBookBuilder& builder,
                            std::size_t instrument_count, std::size_t max_orders) {
    std::vector<LevelInfo> engine_levels(max_orders + 1), builder_levels(max_orders + 1);
    std::vector<OrderId> engine_ids(max_orders + 1), builder_ids(max_orders + 1);
    for (std::size_t index = 1; index <= instrument_count; ++index) {
        const auto id = static_cast<InstrumentId>(index);
        const char expected_state = md::itch_trading_state(engine.phase(id));
        if (builder.trading_state(id) != expected_state)
            return "instrument " + std::to_string(index) + " trading state " + builder.trading_state(id) +
                   " expected " + expected_state;
        for (Side side : {Side::BUY, Side::SELL}) {
            const std::size_t engine_count = engine.depth(id, side, engine_levels.data(), engine_levels.size());
            const std::size_t builder_count = builder.depth(id, side, builder_levels.data(), builder_levels.size());
            const std::string where = "instrument " + std::to_string(index) + (side == Side::BUY ? " bid" : " ask");
            if (engine_count != builder_count)
                return where + " level count " + std::to_string(builder_count) + " expected " +
                       std::to_string(engine_count);
            for (std::size_t level = 0; level < engine_count; ++level) {
                const LevelInfo& e = engine_levels[level];
                const LevelInfo& b = builder_levels[level];
                if (e.price != b.price || e.qty != b.qty || e.order_count != b.order_count)
                    return where + " level " + std::to_string(level) + " builder " + std::to_string(b.price) + "x" +
                           std::to_string(b.qty) + "/" + std::to_string(b.order_count) + " engine " +
                           std::to_string(e.price) + "x" + std::to_string(e.qty) + "/" +
                           std::to_string(e.order_count);
                const std::size_t engine_fifo = engine.level_orders(id, side, e.price, engine_ids.data(), engine_ids.size());
                const std::size_t builder_fifo =
                    builder.level_orders(id, side, e.price, builder_ids.data(), builder_ids.size());
                if (engine_fifo != builder_fifo ||
                    !std::equal(engine_ids.begin(), engine_ids.begin() + static_cast<std::ptrdiff_t>(engine_fifo),
                                builder_ids.begin()))
                    return where + " level " + std::to_string(level) + " FIFO differs";
            }
        }
    }
    return {};
}

// Order-sensitive 64-bit digest of every level (price, qty, count, FIFO ids)
// and trading state. `trading_state_of(id)` gives the ITCH state character.
template <class Book, class TradingState>
std::uint64_t book_digest(const Book& book, std::size_t instrument_count, std::vector<LevelInfo>& levels,
                          std::vector<OrderId>& ids, TradingState trading_state_of) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    for (std::size_t index = 1; index <= instrument_count; ++index) {
        const auto id = static_cast<InstrumentId>(index);
        mix(static_cast<unsigned char>(trading_state_of(id)));
        for (Side side : {Side::BUY, Side::SELL}) {
            const std::size_t count = book.depth(id, side, levels.data(), levels.size());
            mix(count);
            for (std::size_t level = 0; level < count; ++level) {
                mix(static_cast<std::uint64_t>(levels[level].price));
                mix(levels[level].qty);
                mix(levels[level].order_count);
                const std::size_t fifo = book.level_orders(id, side, levels[level].price, ids.data(), ids.size());
                for (std::size_t order = 0; order < fifo; ++order) mix(ids[order]);
            }
        }
    }
    return hash;
}

}  // namespace exsim::mdtest

#endif
