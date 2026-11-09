#ifndef EXSIM_FUZZ_TARGETS_SOUPBINTCP_HPP
#define EXSIM_FUZZ_TARGETS_SOUPBINTCP_HPP

// SoupBinTCP 4.1 framing.
//
// 1. The whole input as one packet: decode() matches an independent model of
//    the length/type rules, and a decoded packet re-encodes to the input.
// 2. Typed decoders (login request/accepted/rejected, data and control
//    packets) agree with the generic packet decode and round-trip.
// 3. Stream reassembly: the input (after a small header choosing buffer size,
//    max length and chunking) is fed to FrameReader in arbitrary chunks. The
//    frames and the terminal error must equal a model that walks the
//    contiguous byte stream, no matter how the bytes were split.

#include "fuzz_support.hpp"

#include <exsim/protocol/soupbintcp.hpp>
#include <optional>
#include <vector>

namespace exsim::fuzz {

namespace soupbintcp_fuzz {

namespace soup = protocol::soupbintcp;
using protocol::Status;

inline bool is_known_packet_type(char type) noexcept {
    return std::string_view("LUROAJSHZ+").find(type) != std::string_view::npos;
}

// Model of SoupBinTCP 4.1 section 2 lengths: length counts the type byte.
inline Status model_packet_status(std::span<const char> bytes) noexcept {
    if (bytes.size() < 2) return Status::incomplete;
    const std::size_t declared_length = protocol::wire::load16(bytes.data());
    if (declared_length == 0) return Status::invalid_length;
    if (bytes.size() < declared_length + 2) return Status::incomplete;
    if (bytes.size() != declared_length + 2) return Status::invalid_length;
    const char type = bytes[2];
    if (!is_known_packet_type(type)) return Status::invalid_type;
    switch (type) {
        case 'L': return declared_length == 47 ? Status::ok : Status::invalid_length;
        case 'A': return declared_length == 31 ? Status::ok : Status::invalid_length;
        case 'J':
            if (declared_length != 2) return Status::invalid_length;
            return bytes[3] == 'A' || bytes[3] == 'S' ? Status::ok : Status::invalid_field;
        case 'R':
        case 'O':
        case 'H':
        case 'Z': return declared_length == 1 ? Status::ok : Status::invalid_length;
        default: return Status::ok;  // U, S, +: any payload
    }
}

// wire::numeric reference: leading spaces, then decimal digits; all spaces = 0.
inline std::optional<std::uint64_t> model_space_padded_number(std::string_view field) noexcept {
    while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
    std::uint64_t value = 0;
    for (const char digit : field) {
        if (digit < '0' || digit > '9') return std::nullopt;
        const auto digit_value = static_cast<std::uint64_t>(digit - '0');
        if (value > (UINT64_MAX - digit_value) / 10) return std::nullopt;
        value = value * 10 + digit_value;
    }
    return value;
}

inline void check_whole_input_packet(std::span<const char> input) {
    soup::Packet packet;
    const Status status = soup::decode(input.data(), input.size(), packet);
    EXSIM_FUZZ_REQUIRE(status == model_packet_status(input), "packet status must match the length/type model");
    if (status != Status::ok) return;
    EXSIM_FUZZ_REQUIRE(packet.type == input[2], "packet type is the third byte");
    EXSIM_FUZZ_REQUIRE(same_bytes({packet.payload.data(), packet.payload.size()}, input.subspan(3)),
                       "payload is everything after the type byte");
    std::vector<char> encoded(input.size() + 8, '\x7f');
    const std::size_t written = soup::encode(packet, encoded.data(), encoded.size());
    EXSIM_FUZZ_REQUIRE(written == input.size(), "a decoded packet must re-encode");
    EXSIM_FUZZ_REQUIRE(same_bytes({encoded.data(), written}, input), "packet re-encodes to the input bytes");
    EXSIM_FUZZ_REQUIRE(soup::encode(packet, encoded.data(), input.size() - 1) == 0,
                       "encode must refuse a buffer one byte short");
}

template <typename TypedPacket>
void check_typed_packet_agrees(std::span<const char> input, char expected_type, Status generic_status) {
    TypedPacket typed{};
    const Status status = soup::decode(input.data(), input.size(), typed);
    if (generic_status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(status == generic_status, "typed decode reports the framing error");
        return;
    }
    EXSIM_FUZZ_REQUIRE(status == (input[2] == expected_type ? Status::ok : Status::invalid_type),
                       "typed decode accepts only its own type");
    if (status != Status::ok) return;
    char encoded[65540];
    const std::size_t written = soup::encode(typed, encoded, sizeof encoded);
    EXSIM_FUZZ_REQUIRE(written == input.size() && same_bytes({encoded, written}, input),
                       "typed packet re-encodes to the input bytes");
}

inline void check_typed_decoders(std::span<const char> input) {
    soup::Packet generic_packet;
    const Status generic_status = soup::decode(input.data(), input.size(), generic_packet);

    soup::LoginRequest login_request;
    const Status login_request_status = soup::decode(input.data(), input.size(), login_request);
    if (generic_status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(login_request_status == generic_status, "login request reports the framing error");
    } else if (input[2] != 'L') {
        EXSIM_FUZZ_REQUIRE(login_request_status == Status::invalid_type, "login request needs type L");
    } else {
        const auto expected_sequence = model_space_padded_number({input.data() + 29, 20});
        EXSIM_FUZZ_REQUIRE((login_request_status == Status::ok) == expected_sequence.has_value(),
                           "login request accepts exactly a space-padded decimal sequence");
        if (login_request_status == Status::ok) {
            EXSIM_FUZZ_REQUIRE(login_request.sequence == *expected_sequence, "login sequence value");
            EXSIM_FUZZ_REQUIRE(same_bytes(login_request.username, input.subspan(3, 6)) &&
                                   same_bytes(login_request.password, input.subspan(9, 10)) &&
                                   same_bytes(login_request.session, input.subspan(19, 10)),
                               "login request alpha fields are copied verbatim");
            char encoded[49];
            EXSIM_FUZZ_REQUIRE(soup::encode(login_request, encoded, sizeof encoded) == 49, "login request re-encodes");
            soup::LoginRequest decoded_again;
            EXSIM_FUZZ_REQUIRE(soup::decode(encoded, sizeof encoded, decoded_again) == Status::ok,
                               "canonical login request decodes");
            EXSIM_FUZZ_REQUIRE(decoded_again.sequence == login_request.sequence &&
                                   decoded_again.username == login_request.username &&
                                   decoded_again.password == login_request.password &&
                                   decoded_again.session == login_request.session,
                               "login request canonical form decodes to the same value");
        }
    }

    soup::LoginAccepted login_accepted;
    const Status login_accepted_status = soup::decode(input.data(), input.size(), login_accepted);
    if (generic_status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(login_accepted_status == generic_status, "login accepted reports the framing error");
    } else if (input[2] != 'A') {
        EXSIM_FUZZ_REQUIRE(login_accepted_status == Status::invalid_type, "login accepted needs type A");
    } else {
        const auto expected_sequence = model_space_padded_number({input.data() + 13, 20});
        EXSIM_FUZZ_REQUIRE((login_accepted_status == Status::ok) == expected_sequence.has_value(),
                           "login accepted accepts exactly a space-padded decimal sequence");
        if (login_accepted_status == Status::ok) {
            EXSIM_FUZZ_REQUIRE(login_accepted.sequence == *expected_sequence, "login accepted sequence value");
            char encoded[33];
            EXSIM_FUZZ_REQUIRE(soup::encode(login_accepted, encoded, sizeof encoded) == 33, "login accepted re-encodes");
            soup::LoginAccepted decoded_again;
            EXSIM_FUZZ_REQUIRE(soup::decode(encoded, sizeof encoded, decoded_again) == Status::ok &&
                                   decoded_again.sequence == login_accepted.sequence &&
                                   decoded_again.session == login_accepted.session,
                               "login accepted canonical form decodes to the same value");
        }
    }

    check_typed_packet_agrees<soup::LoginRejected>(input, 'J', generic_status);
    check_typed_packet_agrees<soup::UnsequencedData>(input, 'U', generic_status);
    check_typed_packet_agrees<soup::SequencedData>(input, 'S', generic_status);
    check_typed_packet_agrees<soup::Debug>(input, '+', generic_status);
    check_typed_packet_agrees<soup::ClientHeartbeat>(input, 'R', generic_status);
    check_typed_packet_agrees<soup::ServerHeartbeat>(input, 'H', generic_status);
    check_typed_packet_agrees<soup::LogoutRequest>(input, 'O', generic_status);
    check_typed_packet_agrees<soup::EndOfSession>(input, 'Z', generic_status);
}

struct ObservedFrame {
    Status status;
    std::string bytes_after_length;  // type byte + payload, only for ok frames
    bool operator==(const ObservedFrame&) const = default;
};

struct ReassemblyOutcome {
    std::vector<ObservedFrame> frames;
    std::optional<Status> stuck_status;  // the reader refuses all further input
    std::size_t consumed_bytes = 0;
};

inline ReassemblyOutcome model_reassembly(std::span<const char> stream, std::size_t buffer_capacity,
                                          std::size_t maximum_length) {
    ReassemblyOutcome outcome;
    std::size_t offset = 0;
    if (buffer_capacity < 2) {
        outcome.stuck_status = Status::full;
        return outcome;
    }
    while (true) {
        const std::size_t remaining = stream.size() - offset;
        if (remaining < 2) {
            outcome.consumed_bytes = stream.size();
            return outcome;
        }
        const std::size_t declared_length = protocol::wire::load16(stream.data() + offset);
        std::optional<Status> header_error;
        if (declared_length == 0) header_error = Status::invalid_length;
        else if (declared_length > maximum_length) header_error = Status::overflow;
        else if (declared_length + 2 > buffer_capacity) header_error = Status::full;
        if (header_error) {
            outcome.stuck_status = header_error;
            outcome.consumed_bytes = offset + 2;
            return outcome;
        }
        if (remaining < declared_length + 2) {
            outcome.consumed_bytes = stream.size();
            return outcome;
        }
        const auto frame = stream.subspan(offset, declared_length + 2);
        const Status status = model_packet_status(frame);
        outcome.frames.push_back(
            {status, status == Status::ok ? std::string(frame.data() + 2, frame.size() - 2) : std::string{}});
        offset += frame.size();
    }
}

inline ReassemblyOutcome run_frame_reader(std::span<const char> stream, std::size_t buffer_capacity,
                                          std::size_t maximum_length, std::uint32_t chunk_seed) {
    ReassemblyOutcome outcome;
    std::vector<char> buffer(buffer_capacity);
    soup::FrameReader frame_reader(std::span<char>(buffer.data(), buffer.size()), maximum_length);
    std::uint32_t chunk_generator = chunk_seed | 1;
    std::size_t position = 0;
    do {
        chunk_generator = chunk_generator * 1664525u + 1013904223u;
        const std::size_t chunk_length =
            (chunk_generator >> 31) ? 1 + (chunk_generator >> 8) % 7 : 1 + (chunk_generator >> 8) % 300;
        std::span<const char> chunk = stream.subspan(position, std::min(chunk_length, stream.size() - position));
        while (true) {
            std::size_t consumed = 0;
            soup::Packet packet;
            const Status status = frame_reader.feed(chunk, consumed, packet);
            EXSIM_FUZZ_REQUIRE(consumed <= chunk.size(), "feed never consumes past its input");
            position += consumed;
            chunk = chunk.subspan(consumed);
            if (status == Status::incomplete) {
                EXSIM_FUZZ_REQUIRE(chunk.empty(), "incomplete means every input byte was taken");
                break;
            }
            ObservedFrame frame{status, {}};
            if (status == Status::ok) {
                frame.bytes_after_length.push_back(packet.type);
                frame.bytes_after_length.append(packet.payload);
            }
            // A completed frame resets on the next feed; a stuck reader repeats its error.
            std::size_t probe_consumed = 0;
            soup::Packet probe_packet;
            const Status probe_status = frame_reader.feed({}, probe_consumed, probe_packet);
            EXSIM_FUZZ_REQUIRE(probe_consumed == 0, "empty feed consumes nothing");
            if (probe_status == Status::incomplete) {
                outcome.frames.push_back(std::move(frame));
                continue;
            }
            EXSIM_FUZZ_REQUIRE(probe_status == status, "a stuck reader repeats the same error");
            outcome.stuck_status = status;
            outcome.consumed_bytes = position;
            return outcome;
        }
    } while (position < stream.size());
    outcome.consumed_bytes = position;
    return outcome;
}

}  // namespace soupbintcp_fuzz

inline int fuzz_soupbintcp(const std::uint8_t* data, std::size_t size) {
    using namespace soupbintcp_fuzz;
    const std::span<const char> input = as_chars(data, size);
    check_whole_input_packet(input);
    check_typed_decoders(input);

    FuzzByteReader reader(data, size);
    const std::uint8_t capacity_selector = reader.take_byte();
    const std::size_t buffer_capacity = capacity_selector < 200 ? capacity_selector : 65537;
    const std::uint8_t maximum_length_selector = reader.take_byte();
    const std::size_t maximum_length = maximum_length_selector < 128 ? maximum_length_selector : 65535;
    const std::uint32_t chunk_seed = reader.take_u32();
    const std::string_view stream_text = reader.take_rest();
    const std::span<const char> stream(stream_text.data(), stream_text.size());

    const ReassemblyOutcome expected = model_reassembly(stream, buffer_capacity, maximum_length);
    const ReassemblyOutcome observed = run_frame_reader(stream, buffer_capacity, maximum_length, chunk_seed);
    EXSIM_FUZZ_REQUIRE(observed.frames == expected.frames, "chunked reassembly yields the contiguous-stream frames");
    EXSIM_FUZZ_REQUIRE(observed.stuck_status == expected.stuck_status, "terminal framing error matches the model");
    EXSIM_FUZZ_REQUIRE(observed.consumed_bytes == expected.consumed_bytes, "consumed byte count matches the model");
    return 0;
}

}  // namespace exsim::fuzz

#endif
