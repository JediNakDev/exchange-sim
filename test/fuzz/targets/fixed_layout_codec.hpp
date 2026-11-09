#ifndef EXSIM_FUZZ_TARGETS_FIXED_LAYOUT_CODEC_HPP
#define EXSIM_FUZZ_TARGETS_FIXED_LAYOUT_CODEC_HPP

// Properties shared by the fixed-layout binary codecs (OUCH 4.2, ITCH 5.0).
// Every message type has `type`, `wire_size`, and ADL-visible
// `decode(const char*, std::size_t, Message&)` / `encode(const Message&, char*, std::size_t)`.

#include "fuzz_support.hpp"

#include <array>
#include <protocol/wire.hpp>
#include <initializer_list>
#include <string_view>
#include <types.hpp>

namespace exsim::fuzz {

using protocol::Status;

inline bool fits_unsigned_32_bit_price(Price price) noexcept {
    return price >= 0 && static_cast<std::uint64_t>(price) <= 0xffffffffULL;
}

// A one-byte field whose value must be one of `allowed_codes` (side, event
// code, order state). decode() rejects a message with invalid_field exactly
// when one of its enumerated fields holds another byte.
struct EnumeratedField {
    std::size_t offset;
    std::string_view allowed_codes;
};

// decode(arbitrary bytes): the status follows the documented length/type
// order, and a successful decode re-encodes to exactly the input bytes
// (every field is fixed width, so the wire form is already canonical).
template <typename Message>
void check_fixed_layout_decode(std::span<const char> input,
                               std::initializer_list<EnumeratedField> enumerated_fields) {
    Message decoded{};
    const Status status = decode(input.data(), input.size(), decoded);
    if (input.size() < Message::wire_size) {
        EXSIM_FUZZ_REQUIRE(status == Status::incomplete, "short input must be incomplete");
        return;
    }
    if (input.size() > Message::wire_size) {
        EXSIM_FUZZ_REQUIRE(status == Status::invalid_length, "long input must be invalid_length");
        return;
    }
    if (input[0] != Message::type) {
        EXSIM_FUZZ_REQUIRE(status == Status::invalid_type, "foreign type byte must be invalid_type");
        return;
    }
    bool enumerated_fields_valid = true;
    for (const EnumeratedField& field : enumerated_fields) {
        enumerated_fields_valid = enumerated_fields_valid &&
                                  field.allowed_codes.find(input[field.offset]) != std::string_view::npos;
    }
    EXSIM_FUZZ_REQUIRE(status == (enumerated_fields_valid ? Status::ok : Status::invalid_field),
                       "exact-size message decodes unless an enumerated field holds an unknown code");
    if (status != Status::ok) return;

    // Two differently pre-filled buffers catch any byte encode() leaves unwritten.
    std::array<char, Message::wire_size> zero_filled{};
    std::array<char, Message::wire_size> one_filled{};
    one_filled.fill(static_cast<char>(0xff));
    EXSIM_FUZZ_REQUIRE(encode(decoded, zero_filled.data(), zero_filled.size()) == Message::wire_size,
                       "a decoded message must re-encode");
    EXSIM_FUZZ_REQUIRE(encode(decoded, one_filled.data(), one_filled.size()) == Message::wire_size,
                       "a decoded message must re-encode");
    EXSIM_FUZZ_REQUIRE(same_bytes(zero_filled, input), "decode then encode must reproduce the input");
    EXSIM_FUZZ_REQUIRE(same_bytes(one_filled, input), "encode must write every byte");
    EXSIM_FUZZ_REQUIRE(encode(decoded, zero_filled.data(), Message::wire_size - 1) == 0,
                       "encode must refuse a buffer one byte short");
}

// A valid base message built from fuzz bytes: wire_size bytes with the type
// byte forced. Falls back to the default-constructed message when the bytes
// hold an invalid enumerated field.
template <typename Message>
Message take_base_message(FuzzByteReader& reader) {
    std::array<char, Message::wire_size> bytes{};
    for (char& byte : bytes) byte = static_cast<char>(reader.take_byte());
    bytes[0] = Message::type;
    Message message{};
    if (decode(bytes.data(), bytes.size(), message) != Status::ok) message = Message{};
    return message;
}

// encode(structured value): the wide in-memory fields (64-bit prices and
// timestamps) are set to boundary values. encode must succeed exactly when
// they fit the wire width, and then decode back to the same values.
template <typename Message, unsigned TimestampWireBits>
void check_wide_field_encode(Message message, FuzzByteReader& reader) {
    bool representable = true;
    if constexpr (requires { message.price; }) {
        message.price = reader.take_interesting_int64();
        representable = representable && fits_unsigned_32_bit_price(message.price);
    }
    if constexpr (requires { message.far_price; }) {
        message.far_price = reader.take_interesting_int64();
        message.near_price = reader.take_interesting_int64();
        message.current_reference_price = reader.take_interesting_int64();
        representable = representable && fits_unsigned_32_bit_price(message.far_price) &&
                        fits_unsigned_32_bit_price(message.near_price) &&
                        fits_unsigned_32_bit_price(message.current_reference_price);
    }
    if constexpr (requires { message.timestamp; }) {
        message.timestamp = static_cast<std::uint64_t>(reader.take_interesting_int64());
        if constexpr (TimestampWireBits < 64) {
            representable = representable && message.timestamp < (std::uint64_t{1} << TimestampWireBits);
        }
    }

    std::array<char, Message::wire_size> encoded{};
    const std::size_t written = encode(message, encoded.data(), encoded.size());
    EXSIM_FUZZ_REQUIRE((written != 0) == representable, "encode succeeds exactly for wire-representable values");
    if (written == 0) return;
    EXSIM_FUZZ_REQUIRE(written == Message::wire_size, "encode writes exactly wire_size bytes");

    Message decoded{};
    EXSIM_FUZZ_REQUIRE(decode(encoded.data(), encoded.size(), decoded) == Status::ok,
                       "an encoded valid message must decode");
    if constexpr (requires { message.price; }) {
        EXSIM_FUZZ_REQUIRE(decoded.price == message.price, "price must survive the round trip");
    }
    if constexpr (requires { message.far_price; }) {
        EXSIM_FUZZ_REQUIRE(decoded.far_price == message.far_price && decoded.near_price == message.near_price &&
                               decoded.current_reference_price == message.current_reference_price,
                           "imbalance prices must survive the round trip");
    }
    if constexpr (requires { message.timestamp; }) {
        EXSIM_FUZZ_REQUIRE(decoded.timestamp == message.timestamp, "timestamp must survive the round trip");
    }
    std::array<char, Message::wire_size> re_encoded{};
    EXSIM_FUZZ_REQUIRE(encode(decoded, re_encoded.data(), re_encoded.size()) == Message::wire_size,
                       "a decoded message must re-encode");
    EXSIM_FUZZ_REQUIRE(same_bytes(encoded, re_encoded), "encode(decode(encode(value))) must be stable");
}

}  // namespace exsim::fuzz

#endif
