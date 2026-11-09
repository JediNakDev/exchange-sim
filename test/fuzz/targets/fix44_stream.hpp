#ifndef EXSIM_FUZZ_TARGETS_FIX44_STREAM_HPP
#define EXSIM_FUZZ_TARGETS_FIX44_STREAM_HPP

// FIX 4.4 stream reassembly and the encode direction. The first input byte
// picks one of three modes so each mode keeps its own corpus shape:
//
//  0. FrameReader: the rest of the input is a TCP byte stream fed in
//     arbitrary chunks. Frames, terminal error and consumed bytes must equal
//     the contiguous-stream frame_size() walk.
//  1. Builder: arbitrary tag=value fields. A correctly framed message must
//     never be classified as garbled: parse is ok unless a tag repeats
//     (duplicate_field) or the field table overflows (full), and every value
//     reads back unchanged.
//  2. Typed encode: NewOrderSingle / ExecutionReport values built from the
//     input encode and decode back to the same value (timestamps at the wire's
//     millisecond precision), or fail with the documented status.

#include "fuzz_support.hpp"

#include <protocol/fix44.hpp>
#include <optional>
#include <set>
#include <vector>

namespace exsim::fuzz {

namespace fix44_stream_fuzz {

namespace fix = protocol::fix44;
using protocol::Status;

struct ReassemblyOutcome {
    std::vector<std::string> frames;
    std::optional<Status> stuck_status;
    std::size_t consumed_bytes = 0;
};

// Contiguous model: grow the candidate prefix one byte at a time, exactly as
// "determine one frame's extent; extra bytes belong to the next frame" says.
inline ReassemblyOutcome model_reassembly(std::span<const char> stream, std::size_t buffer_capacity) {
    ReassemblyOutcome outcome;
    std::size_t frame_start = 0;
    std::size_t prefix_length = 0;
    while (true) {
        std::size_t frame_extent = 0;
        const Status status = fix::frame_size(stream.subspan(frame_start, prefix_length), frame_extent, buffer_capacity);
        if (status == Status::ok) {
            outcome.frames.emplace_back(stream.data() + frame_start, frame_extent);
            frame_start += frame_extent;
            prefix_length = 0;
            continue;
        }
        if (status != Status::incomplete) {
            outcome.stuck_status = status;
            outcome.consumed_bytes = frame_start + prefix_length;
            return outcome;
        }
        if (frame_start + prefix_length == stream.size()) {
            outcome.consumed_bytes = stream.size();
            return outcome;
        }
        if (prefix_length == buffer_capacity) {
            outcome.stuck_status = Status::full;
            outcome.consumed_bytes = frame_start + prefix_length;
            return outcome;
        }
        ++prefix_length;
    }
}

inline ReassemblyOutcome run_frame_reader(std::span<const char> stream, std::size_t buffer_capacity,
                                          std::uint32_t chunk_seed) {
    ReassemblyOutcome outcome;
    std::vector<char> buffer(buffer_capacity);
    fix::FrameReader frame_reader(std::span<char>(buffer.data(), buffer.size()));
    std::uint32_t chunk_generator = chunk_seed | 1;
    std::size_t position = 0;
    do {
        chunk_generator = chunk_generator * 1664525u + 1013904223u;
        const std::size_t chunk_length =
            (chunk_generator >> 31) ? 1 + (chunk_generator >> 8) % 5 : 1 + (chunk_generator >> 8) % 400;
        std::span<const char> chunk = stream.subspan(position, std::min(chunk_length, stream.size() - position));
        while (true) {
            std::size_t consumed = 0;
            std::span<const char> message;
            const Status status = frame_reader.feed(chunk, consumed, message);
            EXSIM_FUZZ_REQUIRE(consumed <= chunk.size(), "feed never consumes past its input");
            position += consumed;
            chunk = chunk.subspan(consumed);
            if (status == Status::incomplete) {
                EXSIM_FUZZ_REQUIRE(chunk.empty(), "incomplete means every input byte was taken");
                break;
            }
            if (status == Status::ok) {
                outcome.frames.emplace_back(message.data(), message.size());
                fix::Parser<> parser;
                (void)parser.parse(message);  // the gateway parses every extracted frame
                continue;
            }
            // A framing error is sticky. `full` is reported only while input is
            // offered (an empty feed is just incomplete), so probe with one byte.
            const char probe_byte = '8';
            std::size_t probe_consumed = 0;
            std::span<const char> probe_message;
            EXSIM_FUZZ_REQUIRE(frame_reader.feed({&probe_byte, 1}, probe_consumed, probe_message) == status &&
                                   probe_consumed == 0,
                               "a framing error is sticky until reset");
            outcome.stuck_status = status;
            outcome.consumed_bytes = position;
            return outcome;
        }
    } while (position < stream.size());
    outcome.consumed_bytes = position;
    return outcome;
}

inline void check_frame_reader(FuzzByteReader& reader) {
    const std::uint8_t capacity_selector = reader.take_byte();
    const std::size_t buffer_capacity = capacity_selector < 128 ? capacity_selector : 4096;
    const std::uint32_t chunk_seed = reader.take_u32();
    const std::string_view stream_text = reader.take_rest();
    const std::span<const char> stream(stream_text.data(), stream_text.size());
    const ReassemblyOutcome expected = model_reassembly(stream, buffer_capacity);
    const ReassemblyOutcome observed = run_frame_reader(stream, buffer_capacity, chunk_seed);
    EXSIM_FUZZ_REQUIRE(observed.frames == expected.frames, "chunked reassembly yields the contiguous-stream frames");
    EXSIM_FUZZ_REQUIRE(observed.stuck_status == expected.stuck_status, "terminal framing error matches the model");
    EXSIM_FUZZ_REQUIRE(observed.consumed_bytes == expected.consumed_bytes, "consumed byte count matches the model");
}

inline std::size_t decimal_digit_count(std::uint64_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

inline void check_builder_framing(FuzzByteReader& reader) {
    struct BodyField {
        unsigned tag;
        std::string_view value;
    };
    std::vector<BodyField> body_fields;
    const std::size_t field_count = reader.take_index(70);
    for (std::size_t index = 0; index < field_count && !reader.exhausted(); ++index) {
        unsigned tag = reader.take_bool() ? 1 + static_cast<unsigned>(reader.take_index(200))
                                          : static_cast<unsigned>(reader.take_u32());
        if (tag == 0 || tag == 8 || tag == 9 || tag == 10) tag = 11;  // header/trailer tags are the builder's own
        body_fields.push_back({tag, reader.take_text(40)});
    }
    const std::size_t buffer_capacity = reader.take_bool() ? 65536 : reader.take_index(400);

    std::size_t body_size = 0;
    bool value_has_separator = false;
    for (const auto& field : body_fields) {
        body_size += decimal_digit_count(field.tag) + 1 + field.value.size() + 1;
        value_has_separator = value_has_separator || field.value.find('\x01') != std::string_view::npos;
    }
    const std::size_t frame_size_needed = 12 + decimal_digit_count(body_size) + 1 + body_size + 7;

    std::vector<char> buffer(buffer_capacity);
    fix::Builder builder(std::span<char>(buffer.data(), buffer.size()));
    for (const auto& field : body_fields) (void)builder.field(field.tag, field.value);
    const std::size_t written = builder.finish();
    if (value_has_separator || frame_size_needed > buffer_capacity) {
        EXSIM_FUZZ_REQUIRE(written == 0, "unrepresentable or oversized messages are refused");
        return;
    }
    EXSIM_FUZZ_REQUIRE(written == frame_size_needed, "frame size is header + body + trailer");
    EXSIM_FUZZ_REQUIRE(builder.finish() == 0, "finish is single use");

    Status expected_status = Status::ok;
    std::set<unsigned> seen_tags{8, 9};
    std::size_t parsed_field_count = 2;
    for (const auto& field : body_fields) {
        if (!seen_tags.insert(field.tag).second) {
            expected_status = Status::duplicate_field;
            break;
        }
        if (parsed_field_count == 64) {
            expected_status = Status::full;
            break;
        }
        ++parsed_field_count;
    }
    if (expected_status == Status::ok && parsed_field_count == 64) expected_status = Status::full;

    fix::Parser<> parser;
    const Status status = parser.parse(buffer.data(), written);
    EXSIM_FUZZ_REQUIRE(status == expected_status, "a well-framed message is never reported as garbled");
    if (status != Status::ok) return;
    for (const auto& field : body_fields) {
        EXSIM_FUZZ_REQUIRE(parser.get(field.tag) == field.value, "every built field reads back unchanged");
    }
}

template <std::size_t N>
fix::Text<N> take_fix_text(FuzzByteReader& reader) {
    fix::Text<N> text;
    const std::string_view source = reader.take_text(N);
    (void)text.assign(source);
    return text;
}

inline bool has_separator(std::string_view text) noexcept { return text.find('\x01') != std::string_view::npos; }

inline constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;

inline fix::Header take_header(FuzzByteReader& reader, std::string& sender_storage, std::string& target_storage) {
    sender_storage = reader.take_text(12);
    target_storage = reader.take_text(12);
    fix::Header header;
    header.sender = sender_storage;
    header.target = target_storage;
    header.sequence = reader.take_u64();
    header.sending_time = reader.take_u64();
    return header;
}

inline void check_decoded_header(const fix::Parser<>& parser, const fix::Header& header) {
    std::uint64_t sequence = 0;
    std::uint64_t sending_time = 0;
    EXSIM_FUZZ_REQUIRE(parser.get(49) == header.sender && parser.get(56) == header.target, "comp ids survive");
    EXSIM_FUZZ_REQUIRE(parser.get_uint(34, sequence) && sequence == header.sequence, "MsgSeqNum survives");
    EXSIM_FUZZ_REQUIRE(parser.get_timestamp(52, sending_time) &&
                           sending_time == header.sending_time - header.sending_time % kNanosecondsPerMillisecond,
                       "SendingTime survives at millisecond precision");
}

inline void check_new_order_single(FuzzByteReader& reader) {
    std::string sender_storage, target_storage;
    const fix::Header header = take_header(reader, sender_storage, target_storage);
    fix::NewOrderSingle order;
    order.client_order_id = take_fix_text<64>(reader);
    order.has_account = reader.take_bool();
    order.account = take_fix_text<64>(reader);
    order.symbol = take_fix_text<64>(reader);
    order.side = reader.take_bool() ? Side::BUY : Side::SELL;
    order.quantity = reader.take_u32();
    order.order_type = reader.take_bool() ? OrdType::Limit : OrdType::Market;
    order.has_price = reader.take_bool();
    order.price = reader.take_interesting_int64();
    order.time_in_force = static_cast<TimeInForce>(reader.take_index(4));
    order.has_exec_inst = reader.take_bool();
    order.exec_inst = take_fix_text<64>(reader);
    order.transact_time = reader.take_u64();
    order.stp_mode = static_cast<StpMode>(reader.take_index(4));

    const bool unrepresentable = has_separator(header.sender) || has_separator(header.target) ||
                                 has_separator(order.client_order_id.view()) ||
                                 (order.has_account && has_separator(order.account.view())) ||
                                 has_separator(order.symbol.view()) ||
                                 (order.has_exec_inst && has_separator(order.exec_inst.view()));
    char buffer[2048];
    const std::size_t written = fix::encode(order, header, buffer, sizeof buffer);
    EXSIM_FUZZ_REQUIRE((written == 0) == unrepresentable, "encode fails exactly on embedded SOH");
    if (written == 0) return;

    fix::Parser<> parser;
    EXSIM_FUZZ_REQUIRE(parser.parse(buffer, written) == Status::ok, "an encoded order parses");
    check_decoded_header(parser, header);
    fix::NewOrderSingle decoded;
    const Status status = fix::decode(parser, decoded);
    const bool empty_text = order.client_order_id.view().empty() || order.symbol.view().empty() ||
                            (order.has_account && order.account.view().empty()) ||
                            (order.has_exec_inst && order.exec_inst.view().empty());
    const Status expected = empty_text ? Status::invalid_field
                            : order.order_type == OrdType::Limit && !order.has_price ? Status::missing_field
                                                                                    : Status::ok;
    EXSIM_FUZZ_REQUIRE(status == expected, "decode status follows the documented required-field rules");
    if (status != Status::ok) return;
    EXSIM_FUZZ_REQUIRE(decoded.client_order_id.view() == order.client_order_id.view() &&
                           decoded.symbol.view() == order.symbol.view() && decoded.side == order.side &&
                           decoded.quantity == order.quantity && decoded.order_type == order.order_type &&
                           decoded.has_price == order.has_price && (!order.has_price || decoded.price == order.price) &&
                           decoded.time_in_force == order.time_in_force && decoded.stp_mode == order.stp_mode,
                       "order fields survive the round trip");
    EXSIM_FUZZ_REQUIRE(decoded.has_account == order.has_account &&
                           (!order.has_account || decoded.account.view() == order.account.view()),
                       "account survives");
    EXSIM_FUZZ_REQUIRE(decoded.has_exec_inst == order.has_exec_inst &&
                           (!order.has_exec_inst || decoded.exec_inst.view() == order.exec_inst.view()) &&
                           decoded.post_only() == order.post_only(),
                       "exec inst and post-only survive");
    EXSIM_FUZZ_REQUIRE(decoded.transact_time == order.transact_time - order.transact_time % kNanosecondsPerMillisecond,
                       "TransactTime survives at millisecond precision");
}

inline void check_execution_report(FuzzByteReader& reader) {
    std::string sender_storage, target_storage;
    const fix::Header header = take_header(reader, sender_storage, target_storage);
    fix::ExecutionReport report;
    report.order_id = take_fix_text<64>(reader);
    report.client_order_id = take_fix_text<64>(reader);
    report.has_original_client_order_id = reader.take_bool();
    report.original_client_order_id = take_fix_text<64>(reader);
    report.execution_id = take_fix_text<64>(reader);
    constexpr std::string_view valid_execution_types = "0458F";
    constexpr std::string_view valid_order_statuses = "01248";
    report.execution_type = reader.take_bool() ? valid_execution_types[reader.take_index(5)]
                                               : static_cast<char>(reader.take_byte());
    report.order_status = reader.take_bool() ? valid_order_statuses[reader.take_index(5)]
                                             : static_cast<char>(reader.take_byte());
    report.symbol = take_fix_text<64>(reader);
    report.side = reader.take_bool() ? Side::BUY : Side::SELL;
    report.quantity = reader.take_u32();
    report.price = reader.take_interesting_int64();
    report.last_quantity = reader.take_u32();
    report.last_price = reader.take_interesting_int64();
    report.leaves_quantity = reader.take_u32();
    report.cumulative_quantity = reader.take_u32();
    report.average_price = reader.take_interesting_int64();
    report.has_reject_reason = reader.take_bool();
    report.reject_reason = reader.take_u64();
    report.has_text = reader.take_bool();
    report.text = take_fix_text<64>(reader);
    report.transact_time = reader.take_u64();

    const bool unrepresentable =
        has_separator(header.sender) || has_separator(header.target) || has_separator(report.order_id.view()) ||
        has_separator(report.client_order_id.view()) ||
        (report.has_original_client_order_id && has_separator(report.original_client_order_id.view())) ||
        has_separator(report.execution_id.view()) || report.execution_type == '\x01' ||
        report.order_status == '\x01' || has_separator(report.symbol.view()) ||
        (report.has_text && has_separator(report.text.view()));
    char buffer[2048];
    const std::size_t written = fix::encode(report, header, buffer, sizeof buffer);
    EXSIM_FUZZ_REQUIRE((written == 0) == unrepresentable, "encode fails exactly on embedded SOH");
    if (written == 0) return;

    fix::Parser<> parser;
    EXSIM_FUZZ_REQUIRE(parser.parse(buffer, written) == Status::ok, "an encoded report parses");
    check_decoded_header(parser, header);
    fix::ExecutionReport decoded;
    const Status status = fix::decode(parser, decoded);
    const bool invalid = report.order_id.view().empty() || report.client_order_id.view().empty() ||
                         (report.has_original_client_order_id && report.original_client_order_id.view().empty()) ||
                         report.execution_id.view().empty() ||
                         valid_execution_types.find(report.execution_type) == std::string_view::npos ||
                         valid_order_statuses.find(report.order_status) == std::string_view::npos ||
                         report.symbol.view().empty() || (report.has_text && report.text.view().empty());
    EXSIM_FUZZ_REQUIRE(status == (invalid ? Status::invalid_field : Status::ok),
                       "decode status follows the documented field rules");
    if (status != Status::ok) return;
    EXSIM_FUZZ_REQUIRE(decoded.order_id.view() == report.order_id.view() &&
                           decoded.client_order_id.view() == report.client_order_id.view() &&
                           decoded.has_original_client_order_id == report.has_original_client_order_id &&
                           (!report.has_original_client_order_id ||
                            decoded.original_client_order_id.view() == report.original_client_order_id.view()) &&
                           decoded.execution_id.view() == report.execution_id.view() &&
                           decoded.execution_type == report.execution_type &&
                           decoded.order_status == report.order_status && decoded.symbol.view() == report.symbol.view(),
                       "report identifiers survive the round trip");
    EXSIM_FUZZ_REQUIRE(decoded.side == report.side && decoded.quantity == report.quantity &&
                           decoded.price == report.price && decoded.last_quantity == report.last_quantity &&
                           decoded.last_price == report.last_price &&
                           decoded.leaves_quantity == report.leaves_quantity &&
                           decoded.cumulative_quantity == report.cumulative_quantity &&
                           decoded.average_price == report.average_price,
                       "report quantities and prices survive the round trip");
    EXSIM_FUZZ_REQUIRE(decoded.has_reject_reason == report.has_reject_reason &&
                           (!report.has_reject_reason || decoded.reject_reason == report.reject_reason) &&
                           decoded.has_text == report.has_text &&
                           (!report.has_text || decoded.text.view() == report.text.view()),
                       "optional report fields survive");
    EXSIM_FUZZ_REQUIRE(decoded.transact_time == report.transact_time - report.transact_time % kNanosecondsPerMillisecond,
                       "TransactTime survives at millisecond precision");
}

}  // namespace fix44_stream_fuzz

inline int fuzz_fix44_stream(const std::uint8_t* data, std::size_t size) {
    using namespace fix44_stream_fuzz;
    FuzzByteReader reader(data, size);
    switch (reader.take_byte() % 4) {
        case 0: check_frame_reader(reader); break;
        case 1: check_builder_framing(reader); break;
        case 2: check_new_order_single(reader); break;
        default: check_execution_report(reader); break;
    }
    return 0;
}

}  // namespace exsim::fuzz

#endif
