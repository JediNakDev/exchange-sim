#ifndef EXSIM_PROTOCOL_MDP3_HPP
#define EXSIM_PROTOCOL_MDP3_HPP

// CME MDP 3.0, schema 1 version 13. Offsets and acting block sizes come
// from templates_FixBinary.xml, SHA256 recorded in docs/ilink-and-mdp.md.
#include "protocol/cme_sbe.hpp"

#include <algorithm>
#include <array>

namespace exsim::protocol::mdp3 {
inline constexpr std::uint16_t kSchemaId = 1;
inline constexpr std::uint16_t kVersion = 13;
inline constexpr std::uint8_t kEndOfEvent = 0x80;
inline constexpr std::uint8_t kLastTrade = 0x01;
inline constexpr std::int32_t kNullInt32 = INT32_MAX;

enum class UpdateAction : std::uint8_t { New = 0, Change = 1, Delete = 2, DeleteThru = 3, DeleteFrom = 4, Overlay = 5 };
enum class BookSide : char { Bid = '0', Ask = '1', Reset = 'J' };

struct BookEntry {
    std::int64_t price = cme::kNullPrice9;
    std::int32_t quantity = kNullInt32;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::int32_t order_count = kNullInt32;
    std::uint8_t level = 0;
    UpdateAction action = UpdateAction::New;
    BookSide side = BookSide::Bid;
    std::int32_t tradeable_size = kNullInt32;
};

struct TradeEntry {
    std::int64_t price = 0;
    std::int32_t quantity = 0;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::int32_t order_count = 0;
    std::uint8_t aggressor_side = 0;  // 0 none, 1 buy, 2 sell
    UpdateAction action = UpdateAction::New;
    std::uint32_t trade_entry_id = UINT32_MAX;
};

struct TradeOrder {
    std::uint64_t order_id = 0;
    std::int32_t quantity = 0;
};

struct SnapshotEntry {
    std::int64_t price = cme::kNullPrice9;
    std::int32_t quantity = kNullInt32;
    std::int32_t order_count = kNullInt32;
    std::int8_t level = INT8_MAX;
    std::uint16_t reference_date = UINT16_MAX;
    std::uint8_t open_close_settle = UINT8_MAX;
    std::uint8_t settlement_price_type = 0;
    char entry_type = '0';
};

struct Snapshot {
    std::uint32_t last_packet_sequence = 0;
    std::uint32_t total_reports = 0;
    std::int32_t security_id = 0;
    std::uint32_t report_sequence = 0;
    std::uint64_t transact_time = 0;
    std::uint64_t last_update_time = 0;
    std::uint16_t trade_date = UINT16_MAX;
    std::uint8_t trading_status = 17;
    std::int64_t high_limit = cme::kNullPrice9;
    std::int64_t low_limit = cme::kNullPrice9;
    std::int64_t max_variation = cme::kNullPrice9;
};

// One message is bounded by the wire's uint8 group counts. The caller
// supplies all storage and decides packet boundaries and channel sequences.
[[nodiscard]] inline std::size_t encode_book(std::uint64_t transact_time, std::uint8_t event_indicator,
                                            std::span<const BookEntry> entries, std::span<char> output) noexcept {
    constexpr std::size_t fixed = 10 + 11 + 3 + 8;
    if (entries.size() > 255 || output.size() < fixed + 32 * entries.size()) return 0;
    const std::size_t size = fixed + 32 * entries.size();
    std::fill_n(output.data(), size, char{0});
    cme::store_le(output.data(), static_cast<std::uint16_t>(size));
    cme::encode_header(output.data() + 2, {11, 46, kSchemaId, kVersion});
    cme::store_le(output.data() + 10, transact_time);
    output[18] = static_cast<char>(event_indicator);
    cme::store_le(output.data() + 21, std::uint16_t{32});
    output[23] = static_cast<char>(entries.size());
    char* cursor = output.data() + 24;
    for (const BookEntry& entry : entries) {
        cme::store_le(cursor, entry.price);
        cme::store_le(cursor + 8, entry.quantity);
        cme::store_le(cursor + 12, entry.security_id);
        cme::store_le(cursor + 16, entry.report_sequence);
        cme::store_le(cursor + 20, entry.order_count);
        cursor[24] = static_cast<char>(entry.level);
        cursor[25] = static_cast<char>(entry.action);
        cursor[26] = static_cast<char>(entry.side);
        cme::store_le(cursor + 27, entry.tradeable_size);
        cursor += 32;
    }
    // NoOrderIDEntries uses an eight-byte dimension header, even when empty.
    cme::store_le(cursor, std::uint16_t{24});
    return size;
}

[[nodiscard]] inline std::size_t encode_trades(std::uint64_t transact_time, std::uint8_t event_indicator,
                                              std::span<const TradeEntry> entries,
                                              std::span<const TradeOrder> orders, std::span<char> output) noexcept {
    constexpr std::size_t fixed = 10 + 11 + 3 + 8;
    if (entries.size() > 255 || orders.size() > 255 ||
        output.size() < fixed + 32 * entries.size() + 16 * orders.size()) return 0;
    const std::size_t size = fixed + 32 * entries.size() + 16 * orders.size();
    std::fill_n(output.data(), size, char{0});
    cme::store_le(output.data(), static_cast<std::uint16_t>(size));
    cme::encode_header(output.data() + 2, {11, 48, kSchemaId, kVersion});
    cme::store_le(output.data() + 10, transact_time);
    output[18] = static_cast<char>(event_indicator);
    cme::store_le(output.data() + 21, std::uint16_t{32});
    output[23] = static_cast<char>(entries.size());
    char* cursor = output.data() + 24;
    for (const TradeEntry& entry : entries) {
        cme::store_le(cursor, entry.price);
        cme::store_le(cursor + 8, entry.quantity);
        cme::store_le(cursor + 12, entry.security_id);
        cme::store_le(cursor + 16, entry.report_sequence);
        cme::store_le(cursor + 20, entry.order_count);
        cursor[24] = static_cast<char>(entry.aggressor_side);
        cursor[25] = static_cast<char>(entry.action);
        cme::store_le(cursor + 26, entry.trade_entry_id);
        cursor += 32;
    }
    cme::store_le(cursor, std::uint16_t{16});
    cursor[7] = static_cast<char>(orders.size());
    cursor += 8;
    for (const TradeOrder& order : orders) {
        cme::store_le(cursor, order.order_id);
        cme::store_le(cursor + 8, order.quantity);
        cursor += 16;
    }
    return size;
}

[[nodiscard]] inline std::size_t encode_snapshot(const Snapshot& snapshot, std::span<const SnapshotEntry> entries,
                                                std::span<char> output) noexcept {
    constexpr std::size_t fixed = 10 + 59 + 3;
    if (entries.size() > 255 || output.size() < fixed + 22 * entries.size()) return 0;
    const std::size_t size = fixed + 22 * entries.size();
    std::fill_n(output.data(), size, char{0});
    cme::store_le(output.data(), static_cast<std::uint16_t>(size));
    cme::encode_header(output.data() + 2, {59, 52, kSchemaId, kVersion});
    char* root = output.data() + 10;
    cme::store_le(root, snapshot.last_packet_sequence);
    cme::store_le(root + 4, snapshot.total_reports);
    cme::store_le(root + 8, snapshot.security_id);
    cme::store_le(root + 12, snapshot.report_sequence);
    cme::store_le(root + 16, snapshot.transact_time);
    cme::store_le(root + 24, snapshot.last_update_time);
    cme::store_le(root + 32, snapshot.trade_date);
    root[34] = static_cast<char>(snapshot.trading_status);
    cme::store_le(root + 35, snapshot.high_limit);
    cme::store_le(root + 43, snapshot.low_limit);
    cme::store_le(root + 51, snapshot.max_variation);
    cme::store_le(root + 59, std::uint16_t{22});
    root[61] = static_cast<char>(entries.size());
    char* cursor = root + 62;
    for (const SnapshotEntry& entry : entries) {
        cme::store_le(cursor, entry.price);
        cme::store_le(cursor + 8, entry.quantity);
        cme::store_le(cursor + 12, entry.order_count);
        cursor[16] = static_cast<char>(entry.level);
        cme::store_le(cursor + 17, entry.reference_date);
        cursor[19] = static_cast<char>(entry.open_close_settle);
        cursor[20] = static_cast<char>(entry.settlement_price_type);
        cursor[21] = entry.entry_type;
        cursor += 22;
    }
    return size;
}

struct IncrementalView {
    std::uint64_t transact_time = 0;
    std::uint8_t event_indicator = 0;
    cme::GroupView entries;
    cme::GroupView orders;
};

// Applies only to schema-13 MBP book (46) and trade summary (48).
[[nodiscard]] inline Status decode_incremental(const cme::MessageView& message, IncrementalView& out) noexcept {
    const auto& header = message.header;
    if (header.schema_id != kSchemaId || header.version != kVersion ||
        (header.template_id != 46 && header.template_id != 48)) return Status::invalid_type;
    if (header.block_length < 11 || message.body.size() < header.block_length) return Status::invalid_length;
    IncrementalView view;
    view.transact_time = cme::load_le<std::uint64_t>(message.body.data());
    view.event_indicator = static_cast<std::uint8_t>(message.body[8]);
    if (cme::decode_group(message.trailing(), 32, view.entries) != Status::ok) return Status::invalid_length;
    auto bytes = message.trailing().subspan(view.entries.encoded_size);
    if (bytes.size() < 8) return Status::invalid_length;
    const auto block = cme::load_le<std::uint16_t>(bytes.data());
    const auto count = static_cast<std::uint8_t>(bytes[7]);
    if (block < (header.template_id == 46 ? 24 : 16)) return Status::invalid_length;
    const std::size_t size = std::size_t{block} * count;
    if (size != bytes.size() - 8) return Status::invalid_length;
    view.orders = {block, count, bytes.subspan(8), size + 8};
    out = view;
    return Status::ok;
}

[[nodiscard]] inline Status decode_snapshot(const cme::MessageView& message, Snapshot& out,
                                            cme::GroupView& entries) noexcept {
    const auto& header = message.header;
    if (header.schema_id != kSchemaId || header.version != kVersion || header.template_id != 52)
        return Status::invalid_type;
    if (header.block_length < 59 || message.body.size() < header.block_length) return Status::invalid_length;
    cme::GroupView group;
    if (cme::decode_group(message.trailing(), 22, group) != Status::ok ||
        group.encoded_size != message.trailing().size()) return Status::invalid_length;
    const char* root = message.body.data();
    out = {cme::load_le<std::uint32_t>(root), cme::load_le<std::uint32_t>(root + 4),
           cme::load_le<std::int32_t>(root + 8), cme::load_le<std::uint32_t>(root + 12),
           cme::load_le<std::uint64_t>(root + 16), cme::load_le<std::uint64_t>(root + 24),
           cme::load_le<std::uint16_t>(root + 32), static_cast<std::uint8_t>(root[34]),
           cme::load_le<std::int64_t>(root + 35), cme::load_le<std::int64_t>(root + 43),
           cme::load_le<std::int64_t>(root + 51)};
    entries = group;
    return Status::ok;
}

[[nodiscard]] inline std::size_t encode_heartbeat(std::span<char> output) noexcept {
    return cme::encode_mdp_message({0, 12, kSchemaId, kVersion}, {}, output);
}

[[nodiscard]] inline std::uint8_t trading_status(Phase phase) noexcept {
    switch (phase) {
        case Phase::Continuous: return 17;
        case Phase::Closed: return 4;
        case Phase::Halted: return 2;
        case Phase::PreOpen: return 21;
        case Phase::PreClose: return 24;
    }
    return 20;
}

[[nodiscard]] inline std::size_t encode_security_status(std::uint64_t timestamp, std::int32_t security_id,
                                                       std::uint16_t trade_date, Phase phase,
                                                       std::uint8_t indicator, std::span<char> output) noexcept {
    std::array<char, 30> root{};
    cme::store_le(root.data(), timestamp);
    std::memcpy(root.data() + 8, "EXSIM", 5);
    std::memcpy(root.data() + 14, "EXSIM", 5);
    cme::store_le(root.data() + 20, security_id);
    cme::store_le(root.data() + 24, trade_date);
    root[26] = static_cast<char>(indicator);
    root[27] = static_cast<char>(trading_status(phase));
    return cme::encode_mdp_message({30, 30, kSchemaId, kVersion}, root, output);
}

[[nodiscard]] inline std::size_t encode_channel_reset(std::uint64_t timestamp, std::int16_t channel,
                                                     std::span<char> output) noexcept {
    std::array<char, 14> body{};
    cme::store_le(body.data(), timestamp);
    body[8] = static_cast<char>(kEndOfEvent);
    cme::store_le(body.data() + 9, std::uint16_t{2});
    body[11] = 1;
    cme::store_le(body.data() + 12, channel);
    return cme::encode_mdp_message({9, 4, kSchemaId, kVersion}, body, output);
}

struct FutureDefinition {
    std::string_view symbol;
    std::int32_t security_id = 0;
    std::int16_t channel_id = 1;
    std::uint8_t market_segment = 1;
    std::uint8_t depth = 10;
    Phase phase = Phase::Continuous;
    std::uint32_t total_reports = 1;
    std::uint64_t timestamp = 0;
    std::uint16_t trade_date = UINT16_MAX;
    std::uint32_t lot_size = 1;
    std::uint32_t max_quantity = 99999;
    std::int64_t tick_price9 = 0;
    std::int64_t reference_price9 = 0;
    std::int64_t low_price9 = 0;
    std::int64_t high_price9 = 0;
    std::uint32_t maturity_date = 20991231;
    std::uint32_t contract_size = 1;
};

[[nodiscard]] inline std::size_t encode_future_definition(const FutureDefinition& definition,
                                                         std::span<char> output) noexcept {
    if (definition.symbol.empty() || definition.symbol.size() > 20 || definition.lot_size > INT32_MAX / 10000u)
        return 0;
    // Root plus NoEvents, NoMDFeedTypes, NoInstAttrib, NoLotTypeRules.
    std::array<char, 249> body{};
    char* root = body.data();
    root[0] = static_cast<char>(kEndOfEvent);
    cme::store_le(root + 1, definition.total_reports);
    root[5] = 'A';
    cme::store_le(root + 6, definition.timestamp);
    root[14] = static_cast<char>(trading_status(definition.phase));
    cme::store_le(root + 15, definition.channel_id);
    root[17] = static_cast<char>(definition.market_segment);
    root[18] = 7;  // index futures
    std::memcpy(root + 19, "XCME", 4);
    std::memcpy(root + 23, "EXSIM", 5);
    std::memcpy(root + 29, "EXSIM", 5);
    std::memcpy(root + 35, definition.symbol.data(), definition.symbol.size());
    cme::store_le(root + 55, definition.security_id);
    std::memcpy(root + 59, "FUT", 3);
    std::memcpy(root + 65, "FFICXX", 6);
    cme::store_le(root + 71, static_cast<std::uint16_t>(definition.maturity_date / 10000));
    root[73] = static_cast<char>(definition.maturity_date / 100 % 100);
    root[74] = static_cast<char>(definition.maturity_date % 100);
    root[75] = char(0xff);
    std::memcpy(root + 76, "USD", 3);
    std::memcpy(root + 79, "USD", 3);
    root[82] = 'F';  // simulator price-time matching
    cme::store_le(root + 83, definition.lot_size);
    cme::store_le(root + 87, definition.max_quantity);
    cme::store_le(root + 91, definition.tick_price9);
    cme::store_le(root + 99, std::int64_t{1000000000});
    std::fill_n(root + 107, 3, char(0xff));
    std::memcpy(root + 110, "IPNT", 4);
    cme::store_le(root + 140, std::int64_t{definition.contract_size} * 1000000000);
    cme::store_le(root + 148, definition.reference_price9);
    cme::store_le(root + 157, kNullInt32);
    cme::store_le(root + 161, kNullInt32);
    cme::store_le(root + 165, definition.high_price9);
    cme::store_le(root + 173, definition.low_price9);
    cme::store_le(root + 181, cme::kNullPrice9);
    cme::store_le(root + 189, kNullInt32);
    cme::store_le(root + 193, std::uint16_t{UINT16_MAX});
    cme::store_le(root + 195, kNullInt32);
    cme::store_le(root + 199, kNullInt32);
    root[203] = char(0x7f);
    root[204] = char(0x7f);
    cme::store_le(root + 205, cme::kNullPrice9);
    root[213] = 'N';
    cme::store_le(root + 214, definition.trade_date);
    cme::store_le(root + 216, std::uint64_t{UINT64_MAX});
    cme::store_le(root + 224, std::uint16_t{9});  // no event dates
    cme::store_le(root + 227, std::uint16_t{4});
    root[229] = 1;
    std::memcpy(root + 230, "GBX", 3);
    root[233] = static_cast<char>(definition.depth);
    cme::store_le(root + 234, std::uint16_t{4});
    root[236] = 1;
    cme::store_le(root + 237, std::uint32_t{1});
    cme::store_le(root + 241, std::uint16_t{5});
    root[243] = 1;
    root[244] = 4;
    cme::store_le(root + 245, static_cast<std::int32_t>(definition.lot_size * 10000));
    return cme::encode_mdp_message({224, 54, kSchemaId, kVersion}, body, output);
}
}  // namespace exsim::protocol::mdp3
#endif
