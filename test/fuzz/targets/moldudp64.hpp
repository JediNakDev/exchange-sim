#ifndef EXSIM_FUZZ_TARGETS_MOLDUDP64_HPP
#define EXSIM_FUZZ_TARGETS_MOLDUDP64_HPP

// MoldUDP64 downstream packets and retransmission requests.
//
// 1. The whole input as one downstream datagram: PacketReader::reset accepts
//    exactly what a model of the message-block layout accepts, yields every
//    message in order, and PacketBuilder rebuilds byte-identical packets.
// 2. The whole input as a 20-byte request: exact decode/encode round trip.
// 3. Builder direction: messages taken from the input are appended with a
//    fuzz-chosen payload limit; each flushed packet must read back to the
//    same messages, and sequence numbers advance by the message count.

#include "fuzz_support.hpp"

#include <protocol/moldudp64.hpp>
#include <optional>
#include <vector>

namespace exsim::fuzz {

namespace moldudp64_fuzz {

namespace mold = protocol::moldudp64;
using protocol::Status;

inline constexpr std::uint16_t kEndOfSessionCount = 65535;

// Model of the downstream packet: 20-byte header (session, sequence, count)
// then `count` blocks of a 2-byte length and that many bytes. Count 65535
// is end of session and carries no messages.
inline Status model_packet_status(std::span<const char> packet) noexcept {
    if (packet.size() < 20) return Status::incomplete;
    std::size_t message_count = protocol::wire::load16(packet.data() + 18);
    if (message_count == kEndOfSessionCount) message_count = 0;
    std::size_t offset = 20;
    for (std::size_t index = 0; index < message_count; ++index) {
        if (packet.size() - offset < 2) return Status::incomplete;
        const std::size_t message_length = protocol::wire::load16(packet.data() + offset);
        offset += 2;
        if (message_length == 0) return Status::invalid_length;
        if (message_length > packet.size() - offset) return Status::incomplete;
        offset += message_length;
    }
    return offset == packet.size() ? Status::ok : Status::invalid_length;
}

inline void check_downstream_packet(std::span<const char> input) {
    mold::PacketReader packet_reader;
    const Status status = packet_reader.reset(input);
    EXSIM_FUZZ_REQUIRE(status == model_packet_status(input), "reader status must match the block-layout model");
    std::span<const char> message;
    if (status != Status::ok) {
        EXSIM_FUZZ_REQUIRE(packet_reader.next(message) == Status::end, "a rejected packet yields no messages");
        return;
    }
    const bool end_of_session = packet_reader.header.count == kEndOfSessionCount;
    std::vector<std::span<const char>> messages;
    while (packet_reader.next(message) == Status::ok) {
        EXSIM_FUZZ_REQUIRE(message.data() >= input.data() &&
                               message.data() + message.size() <= input.data() + input.size(),
                           "messages borrow the packet bytes");
        messages.push_back(message);
    }
    EXSIM_FUZZ_REQUIRE(messages.size() == (end_of_session ? 0u : packet_reader.header.count),
                       "reader yields exactly header.count messages");

    std::vector<char> rebuilt(input.size());
    mold::PacketBuilder packet_builder(std::span<char>(rebuilt.data(), rebuilt.size()), input.size());
    const std::string_view session(packet_reader.header.session.data(), packet_reader.header.session.size());
    EXSIM_FUZZ_REQUIRE(packet_builder.reset(session, packet_reader.header.sequence, end_of_session),
                       "builder accepts the decoded header");
    for (const auto& each_message : messages) {
        EXSIM_FUZZ_REQUIRE(packet_builder.append(each_message) == Status::ok,
                           "builder sized to the packet must accept every message");
    }
    EXSIM_FUZZ_REQUIRE(packet_builder.remaining() == 0, "rebuilt packet fills exactly the original size");
    EXSIM_FUZZ_REQUIRE(same_bytes(packet_builder.packet(), input), "rebuilt packet is byte-identical");
}

inline void check_request(std::span<const char> input) {
    mold::Request request;
    const Status status = mold::decode(input.data(), input.size(), request);
    const Status expected = input.size() < 20 ? Status::incomplete
                            : input.size() != 20 ? Status::invalid_length
                                                 : Status::ok;
    EXSIM_FUZZ_REQUIRE(status == expected, "request status depends only on length");
    if (status != Status::ok) return;
    char encoded[20];
    EXSIM_FUZZ_REQUIRE(mold::encode(request, encoded, sizeof encoded) == 20, "request re-encodes");
    EXSIM_FUZZ_REQUIRE(same_bytes(encoded, input), "request round trip is exact");
    EXSIM_FUZZ_REQUIRE(mold::encode(request, encoded, 19) == 0, "encode refuses a short buffer");
}

inline void check_builder(FuzzByteReader& reader) {
    const std::size_t maximum_payload = 20 + reader.take_index(600);
    const std::string_view session = reader.take_text(10);
    std::uint64_t next_sequence = reader.take_u64() >> 1;
    std::vector<char> storage(maximum_payload + reader.take_index(64));
    mold::PacketBuilder packet_builder(std::span<char>(storage.data(), storage.size()), maximum_payload);
    EXSIM_FUZZ_REQUIRE(packet_builder.reset(session, next_sequence), "builder accepts a short session");

    std::vector<std::string> pending_messages;
    auto flush_and_verify = [&]() {
        const auto packet = packet_builder.packet();
        EXSIM_FUZZ_REQUIRE(packet.size() <= maximum_payload, "packet respects the payload limit");
        mold::PacketReader packet_reader;
        EXSIM_FUZZ_REQUIRE(packet_reader.reset(packet) == Status::ok, "built packet must read back");
        EXSIM_FUZZ_REQUIRE(packet_reader.header.sequence == next_sequence, "header sequence is the first message");
        EXSIM_FUZZ_REQUIRE(packet_reader.header.count == pending_messages.size(), "header count is the message count");
        EXSIM_FUZZ_REQUIRE(protocol::wire::alpha(packet_reader.header.session.data(), 10) ==
                               protocol::wire::alpha(session.data(), session.size()),
                           "session is space padded");
        std::span<const char> message;
        for (const auto& expected_message : pending_messages) {
            EXSIM_FUZZ_REQUIRE(packet_reader.next(message) == Status::ok, "every appended message reads back");
            EXSIM_FUZZ_REQUIRE(same_bytes(message, {expected_message.data(), expected_message.size()}),
                               "message bytes survive");
        }
        EXSIM_FUZZ_REQUIRE(packet_reader.next(message) == Status::end, "no extra messages");
        next_sequence += pending_messages.size();
        pending_messages.clear();
        EXSIM_FUZZ_REQUIRE(packet_builder.reset(session, next_sequence), "builder resets");
    };

    while (!reader.exhausted()) {
        const std::string_view message = reader.take_text(maximum_payload);
        const std::size_t remaining_before = packet_builder.remaining();
        const Status status = packet_builder.append({message.data(), message.size()});
        if (message.empty()) {
            EXSIM_FUZZ_REQUIRE(status == Status::invalid_length, "empty messages are not representable");
            continue;
        }
        if (status == Status::full) {
            EXSIM_FUZZ_REQUIRE(remaining_before < message.size() + 2, "full only when the block does not fit");
            EXSIM_FUZZ_REQUIRE(packet_builder.remaining() == remaining_before, "a full append changes nothing");
            flush_and_verify();
            if (packet_builder.append({message.data(), message.size()}) == Status::ok) {
                pending_messages.emplace_back(message);
            } else {
                EXSIM_FUZZ_REQUIRE(message.size() + 2 > maximum_payload - 20,
                                   "a message that fits an empty packet must be accepted");
            }
            continue;
        }
        EXSIM_FUZZ_REQUIRE(status == Status::ok, "a fitting message is appended");
        EXSIM_FUZZ_REQUIRE(packet_builder.remaining() == remaining_before - message.size() - 2,
                           "append uses length + 2 bytes");
        pending_messages.emplace_back(message);
    }
    flush_and_verify();
}

}  // namespace moldudp64_fuzz

inline int fuzz_moldudp64(const std::uint8_t* data, std::size_t size) {
    using namespace moldudp64_fuzz;
    const std::span<const char> input = as_chars(data, size);
    check_downstream_packet(input);
    check_request(input);
    FuzzByteReader reader(data, size);
    check_builder(reader);
    return 0;
}

}  // namespace exsim::fuzz

#endif
