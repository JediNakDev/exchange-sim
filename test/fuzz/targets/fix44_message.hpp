#ifndef EXSIM_FUZZ_TARGETS_FIX44_MESSAGE_HPP
#define EXSIM_FUZZ_TARGETS_FIX44_MESSAGE_HPP

// FIX 4.4 single-message parse and typed decode. The whole input is one
// candidate frame.
//
// Framing contract (gateway mapping section 3): garbled framing, checksum or
// BodyLength must be told apart from parseable semantic errors. So:
//  * Parser::parse ok implies the frame really is well formed: BeginString,
//    BodyLength equal to the real body size, CheckSum last and correct.
//  * For a parsed frame, changing any one body byte must give Status::checksum
//    (never a semantic status), adding a byte gives invalid_length, and
//    removing the last byte gives incomplete.
//  * frame_size reports overflow exactly for a declared frame longer than
//    its 1 MiB default limit; a parsed frame has at most 20 BodyLength
//    digits and no tag 0.
//  * Typed decoders only report semantic statuses on a parsed frame, and a
//    decoded message re-encodes to a canonical frame that decodes to the
//    same canonical frame again. Every field of the canonical frame that the
//    input also has keeps its value, up to the canonical spelling of
//    numbers, prices and timestamps.

#include "fuzz_support.hpp"

#include <charconv>
#include <exsim/protocol/fix44.hpp>
#include <string>
#include <vector>

namespace exsim::fuzz {

namespace fix44_message_fuzz {

namespace fix = protocol::fix44;
using protocol::Status;

inline bool is_semantic_status(Status status) noexcept {
    return status == Status::ok || status == Status::invalid_type || status == Status::missing_field ||
           status == Status::invalid_field;
}

inline unsigned checksum_of(std::span<const char> bytes) noexcept {
    unsigned sum = 0;
    for (const char byte : bytes) sum += static_cast<unsigned char>(byte);
    return sum % 256;
}

inline constexpr std::size_t kMaximumFrameBytes = 1048576;  // frame_size's default limit

// A declared frame (header, BodyLength digits, SOH, body, 7-byte trailer)
// longer than the limit is overflow, never incomplete or ok.
inline void check_frame_size_limit(std::span<const char> input, Status frame_status) {
    const std::string_view text(input.data(), input.size());
    if (text.substr(0, 12) != std::string_view("8=FIX.4.4\x01" "9=", 12)) return;
    const std::size_t body_length_end = text.find('\x01', 12);
    // 19 digits cannot overflow the extent below; longer BodyLength fields are frame_size's own rule.
    if (body_length_end == std::string_view::npos || body_length_end == 12 || body_length_end - 12 > 19) return;
    std::uint64_t body_length = 0;
    for (const char digit : text.substr(12, body_length_end - 12)) {
        if (digit < '0' || digit > '9') return;
        body_length = body_length * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    const std::uint64_t frame_extent = body_length_end + 8 + body_length;
    EXSIM_FUZZ_REQUIRE((frame_status == Status::overflow) == (frame_extent > kMaximumFrameBytes),
                       "overflow exactly when the declared frame exceeds 1 MiB");
}

// "0042" -> "42", "150.2500" -> "150.25", "-0.0" -> "0"; any other text is returned unchanged.
inline std::string canonical_decimal_spelling(std::string_view value) {
    std::string_view unsigned_part = value;
    const bool negative = !unsigned_part.empty() && unsigned_part.front() == '-';
    if (negative) unsigned_part.remove_prefix(1);
    const std::size_t dot = unsigned_part.find('.');
    std::string_view whole = unsigned_part.substr(0, dot);
    std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : unsigned_part.substr(dot + 1);
    auto all_digits = [](std::string_view digits) {
        return digits.find_first_not_of("0123456789") == std::string_view::npos;
    };
    if (whole.empty() || !all_digits(whole) || !all_digits(fraction) || (dot != std::string_view::npos && fraction.empty())) {
        return std::string(value);
    }
    while (whole.size() > 1 && whole.front() == '0') whole.remove_prefix(1);
    while (!fraction.empty() && fraction.back() == '0') fraction.remove_suffix(1);
    std::string spelled(whole);
    if (!fraction.empty()) spelled.append(".").append(fraction);
    if (negative && spelled != "0") spelled.insert(0, "-");
    return spelled;
}

inline bool same_field_value(std::string_view input_value, std::string_view canonical_value) {
    if (input_value == canonical_value) return true;
    // A UTCTimestamp without milliseconds is re-encoded with ".000".
    if (input_value.size() == 17 && canonical_value == std::string(input_value) + ".000") return true;
    return canonical_decimal_spelling(input_value) == canonical_decimal_spelling(canonical_value);
}

// Decoding must not drop, clamp or misread a value: every body field of the
// canonical frame that the input also carries has the input's value.
inline void check_canonical_frame_keeps_values(const fix::Parser<>& input_parser, std::string_view canonical_frame) {
    const std::size_t trailer_start = canonical_frame.size() - 7;
    std::size_t position = canonical_frame.find('\x01', 12) + 1;
    while (position < trailer_start) {
        const std::size_t equals = canonical_frame.find('=', position);
        const std::size_t end = canonical_frame.find('\x01', equals);
        unsigned tag = 0;
        (void)std::from_chars(canonical_frame.data() + position, canonical_frame.data() + equals, tag);
        const std::string_view canonical_value = canonical_frame.substr(equals + 1, end - equals - 1);
        if (input_parser.has(tag)) {
            EXSIM_FUZZ_REQUIRE(same_field_value(input_parser.get(tag), canonical_value),
                               "decoding keeps the value of tag " + std::to_string(tag));
        }
        position = end + 1;
    }
}

// Insert a premature CheckSum before any body fields. Rebuild framing so
// only the field placement is invalid, independent of later semantic errors.
inline void check_premature_checksum_is_rejected(std::span<const char> input) {
    const std::string_view text(input.data(), input.size());
    const std::size_t body_start = text.find('\x01', 12) + 1;
    const std::string_view body = text.substr(body_start, text.size() - body_start - 7);
    std::string frame = "8=FIX.4.4\x01" "9=" + std::to_string(body.size() + 7) + "\x01";
    frame.append("10=000\x01").append(body);
    const unsigned checksum = checksum_of({frame.data(), frame.size()});
    frame.append("10=");
    frame.push_back(static_cast<char>('0' + checksum / 100));
    frame.push_back(static_cast<char>('0' + checksum / 10 % 10));
    frame.push_back(static_cast<char>('0' + checksum % 10));
    frame.push_back('\x01');
    if (frame.size() > kMaximumFrameBytes) return;
    fix::Parser<> parser;
    EXSIM_FUZZ_REQUIRE(parser.parse(frame.data(), frame.size()) == Status::invalid_length,
                       "CheckSum before the trailer is invalid_length");
}

inline void check_parsed_frame_is_well_formed(std::span<const char> input, const fix::Parser<>& parser) {
    const std::string_view text(input.data(), input.size());
    EXSIM_FUZZ_REQUIRE(text.substr(0, 12) == std::string_view("8=FIX.4.4\x01" "9=", 12), "BeginString then BodyLength");
    EXSIM_FUZZ_REQUIRE(parser.get(8) == "FIX.4.4", "BeginString value");
    const std::size_t body_length_end = text.find('\x01', 12);
    EXSIM_FUZZ_REQUIRE(body_length_end != std::string_view::npos, "BodyLength is terminated");
    EXSIM_FUZZ_REQUIRE(body_length_end - 12 <= 20, "BodyLength has at most 20 digits");
    EXSIM_FUZZ_REQUIRE(!parser.has(0), "tag 0 is never a field");
    std::uint64_t declared_body_length = 0;
    EXSIM_FUZZ_REQUIRE(parser.get_uint(9, declared_body_length), "BodyLength is numeric");
    EXSIM_FUZZ_REQUIRE(declared_body_length == input.size() - (body_length_end + 1) - 7,
                       "BodyLength counts the bytes between BodyLength and CheckSum");
    EXSIM_FUZZ_REQUIRE(text.substr(text.size() - 7, 3) == "10=" && text.back() == '\x01', "CheckSum is last");
    std::uint64_t declared_checksum = 0;
    EXSIM_FUZZ_REQUIRE(parser.get_uint(10, declared_checksum), "CheckSum is numeric");
    EXSIM_FUZZ_REQUIRE(declared_checksum == checksum_of(input.first(input.size() - 7)), "CheckSum is correct");
}

inline void check_framing_contract(std::span<const char> input) {
    const std::size_t body_start = std::string_view(input.data(), input.size()).find('\x01', 12) + 1;
    const std::size_t trailer_start = input.size() - 7;
    std::vector<char> mutated(input.begin(), input.end());
    fix::Parser<> parser;
    // Body changes keep the frame extent. Corrupting the final SOH is a
    // trailer-framing error; other body changes must fail the checksum.
    const std::size_t body_size = trailer_start - body_start;
    const std::size_t step = body_size / 16 + 1;
    for (std::size_t position = body_start; position < trailer_start; position += step) {
        const char original = mutated[position];
        mutated[position] = static_cast<char>(original + 1);
        const auto expected = position + 1 == trailer_start ? Status::invalid_length : Status::checksum;
        EXSIM_FUZZ_REQUIRE(parser.parse({mutated.data(), mutated.size()}) == expected,
                           "a changed body byte is a framing or checksum error, not a semantic one");
        mutated[position] = original;
    }
    mutated.push_back('\x01');
    EXSIM_FUZZ_REQUIRE(parser.parse({mutated.data(), mutated.size()}) == Status::invalid_length,
                       "a trailing extra byte is a BodyLength error");
    mutated.pop_back();
    EXSIM_FUZZ_REQUIRE(parser.parse({mutated.data(), mutated.size() - 1}) == Status::incomplete,
                       "a truncated frame is incomplete");
}

template <typename Message>
void check_typed_decode(const fix::Parser<>& parser, std::size_t input_size) {
    Message decoded{};
    const Status status = fix::decode(parser, decoded);
    EXSIM_FUZZ_REQUIRE(is_semantic_status(status), "typed decode of a parsed frame reports a semantic status");
    if (status != Status::ok) return;

    fix::Header header;
    header.sender = parser.get(49);
    header.target = parser.get(56);
    EXSIM_FUZZ_REQUIRE(parser.get_uint(34, header.sequence), "decoded message has a numeric MsgSeqNum");
    EXSIM_FUZZ_REQUIRE(parser.get_timestamp(52, header.sending_time), "decoded message has a SendingTime");

    // Canonical fields are never longer than the original ones, plus fixed header/number widths.
    std::vector<char> first_encoding(2 * input_size + 8192);
    const std::size_t first_size = fix::encode(decoded, header, first_encoding.data(), first_encoding.size());
    EXSIM_FUZZ_REQUIRE(first_size != 0, "a decoded message must re-encode");

    fix::Parser<> canonical_parser;
    EXSIM_FUZZ_REQUIRE(canonical_parser.parse(first_encoding.data(), first_size) == Status::ok,
                       "the canonical frame parses");
    check_canonical_frame_keeps_values(parser, {first_encoding.data(), first_size});
    Message decoded_again{};
    EXSIM_FUZZ_REQUIRE(fix::decode(canonical_parser, decoded_again) == Status::ok, "the canonical frame decodes");
    fix::Header header_again;
    header_again.sender = canonical_parser.get(49);
    header_again.target = canonical_parser.get(56);
    EXSIM_FUZZ_REQUIRE(canonical_parser.get_uint(34, header_again.sequence) &&
                           canonical_parser.get_timestamp(52, header_again.sending_time),
                       "canonical header fields");
    EXSIM_FUZZ_REQUIRE(header_again.sender == header.sender && header_again.target == header.target &&
                           header_again.sequence == header.sequence &&
                           header_again.sending_time == header.sending_time,
                       "header survives the round trip");
    std::vector<char> second_encoding(first_encoding.size());
    const std::size_t second_size =
        fix::encode(decoded_again, header_again, second_encoding.data(), second_encoding.size());
    EXSIM_FUZZ_REQUIRE(second_size == first_size &&
                           same_bytes({first_encoding.data(), first_size}, {second_encoding.data(), second_size}),
                       "encode(decode(canonical frame)) is the same canonical frame");
}

}  // namespace fix44_message_fuzz

inline int fuzz_fix44_message(const std::uint8_t* data, std::size_t size) {
    using namespace fix44_message_fuzz;
    const std::span<const char> input = as_chars(data, size);

    std::size_t frame_extent = 0;
    const Status frame_status = fix::frame_size(input, frame_extent);
    fix::Parser<> parser;
    const Status parse_status = parser.parse(input);
    check_frame_size_limit(input, frame_status);
    if (frame_status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(parse_status == frame_status, "parse reports the frame_size error first");
    }
    if (parse_status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(!parser.has(35) && parser.get(35).empty(), "a failed parse exposes no fields");
        return 0;
    }
    EXSIM_FUZZ_REQUIRE(frame_status == Status::ok && frame_extent == input.size(), "parsed frame is exactly one frame");
    check_parsed_frame_is_well_formed(input, parser);
    check_framing_contract(input);
    check_premature_checksum_is_rejected(input);

    check_typed_decode<fix::Logon>(parser, input.size());
    check_typed_decode<fix::Heartbeat>(parser, input.size());
    check_typed_decode<fix::TestRequest>(parser, input.size());
    check_typed_decode<fix::ResendRequest>(parser, input.size());
    check_typed_decode<fix::Reject>(parser, input.size());
    check_typed_decode<fix::SequenceReset>(parser, input.size());
    check_typed_decode<fix::Logout>(parser, input.size());
    check_typed_decode<fix::NewOrderSingle>(parser, input.size());
    check_typed_decode<fix::OrderCancelRequest>(parser, input.size());
    check_typed_decode<fix::OrderCancelReplaceRequest>(parser, input.size());
    check_typed_decode<fix::OrderMassCancelRequest>(parser, input.size());
    check_typed_decode<fix::ExecutionReport>(parser, input.size());
    check_typed_decode<fix::OrderCancelReject>(parser, input.size());
    check_typed_decode<fix::OrderMassCancelReport>(parser, input.size());
    return 0;
}

}  // namespace exsim::fuzz

#endif
