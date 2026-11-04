#ifndef EXSIM_FUZZ_TARGETS_FIX44_MESSAGE_HPP
#define EXSIM_FUZZ_TARGETS_FIX44_MESSAGE_HPP

// FIX 4.4 single-message parse and typed decode. The whole input is one
// candidate frame.
//
// Framing contract (docs/GATEWAYS.md section 3): garbled framing, checksum or
// BodyLength must be told apart from parseable semantic errors. So:
//  * Parser::parse ok implies the frame really is well formed: BeginString,
//    BodyLength equal to the real body size, CheckSum last and correct.
//  * For a parsed frame, changing any one body byte must give Status::checksum
//    (never a semantic status), adding a byte gives invalid_length, and
//    removing the last byte gives incomplete.
//  * Typed decoders only report semantic statuses on a parsed frame, and a
//    decoded message re-encodes to a canonical frame that decodes to the
//    same canonical frame again.

#include "fuzz_support.hpp"

#include <exsim/protocol/fix44.hpp>
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

inline void check_parsed_frame_is_well_formed(std::span<const char> input, const fix::Parser<>& parser) {
    const std::string_view text(input.data(), input.size());
    EXSIM_FUZZ_REQUIRE(text.substr(0, 12) == std::string_view("8=FIX.4.4\x01" "9=", 12), "BeginString then BodyLength");
    EXSIM_FUZZ_REQUIRE(parser.get(8) == "FIX.4.4", "BeginString value");
    const std::size_t body_length_end = text.find('\x01', 12);
    EXSIM_FUZZ_REQUIRE(body_length_end != std::string_view::npos, "BodyLength is terminated");
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
