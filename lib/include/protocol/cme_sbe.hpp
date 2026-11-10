#ifndef EXSIM_PROTOCOL_CME_SBE_HPP
#define EXSIM_PROTOCOL_CME_SBE_HPP

// CME framing, not the six-byte standard FIX SOFH. Template-specific
// decoders must validate schema/version and their required root/group sizes.
#include "protocol/wire.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

namespace exsim::protocol::cme {

template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
[[nodiscard]] constexpr T load_le(const char* bytes) noexcept {
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<U>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    return std::bit_cast<T>(value);
}

template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
constexpr void store_le(char* bytes, T value) noexcept {
    const auto bits = std::bit_cast<std::make_unsigned_t<T>>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = static_cast<char>((bits >> (8 * i)) & 0xff);
}

inline constexpr std::uint16_t kEncoding = 0xcafe;
inline constexpr std::size_t kSbeHeaderSize = 8;
inline constexpr std::size_t kIlinkHeaderSize = 12;
inline constexpr std::size_t kMdpMessageHeaderSize = 10;
inline constexpr std::size_t kMdpPacketHeaderSize = 12;

struct Header {
    std::uint16_t block_length = 0;
    std::uint16_t template_id = 0;
    std::uint16_t schema_id = 0;
    std::uint16_t version = 0;
};

[[nodiscard]] inline Header decode_header(const char* bytes) noexcept {
    return {load_le<std::uint16_t>(bytes), load_le<std::uint16_t>(bytes + 2),
            load_le<std::uint16_t>(bytes + 4), load_le<std::uint16_t>(bytes + 6)};
}

inline void encode_header(char* bytes, Header header) noexcept {
    store_le(bytes, header.block_length);
    store_le(bytes + 2, header.template_id);
    store_le(bytes + 4, header.schema_id);
    store_le(bytes + 6, header.version);
}

struct MessageView {
    Header header;
    std::span<const char> body;
    std::size_t frame_size = 0;

    [[nodiscard]] std::span<const char> root() const noexcept { return body.first(header.block_length); }
    [[nodiscard]] std::span<const char> trailing() const noexcept { return body.subspan(header.block_length); }
};

// On failure the output is unchanged. On success body refers to the input,
// and frame_size tells a TCP caller how much of its buffer to consume.
[[nodiscard]] inline Status decode_ilink(std::span<const char> bytes, MessageView& out) noexcept {
    if (bytes.size() < 4) return Status::incomplete;
    const auto size = load_le<std::uint16_t>(bytes.data());
    if (size < kIlinkHeaderSize) return Status::invalid_length;
    if (load_le<std::uint16_t>(bytes.data() + 2) != kEncoding) return Status::invalid_type;
    if (bytes.size() < size) return Status::incomplete;
    const Header header = decode_header(bytes.data() + 4);
    if (header.block_length > size - kIlinkHeaderSize) return Status::invalid_length;
    out = {header, bytes.subspan(kIlinkHeaderSize, size - kIlinkHeaderSize), size};
    return Status::ok;
}

[[nodiscard]] inline Status decode_mdp_message(std::span<const char> bytes, MessageView& out) noexcept {
    if (bytes.size() < 2) return Status::incomplete;
    const auto size = load_le<std::uint16_t>(bytes.data());
    if (size < kMdpMessageHeaderSize) return Status::invalid_length;
    if (bytes.size() < size) return Status::incomplete;
    const Header header = decode_header(bytes.data() + 2);
    if (header.block_length > size - kMdpMessageHeaderSize) return Status::invalid_length;
    out = {header, bytes.subspan(kMdpMessageHeaderSize, size - kMdpMessageHeaderSize), size};
    return Status::ok;
}

// Returns zero without writing if the frame does not fit. body must not
// overlap output, except at its final position after the framing header.
[[nodiscard]] inline std::size_t encode_ilink(Header header, std::span<const char> body,
                                             std::span<char> output) noexcept {
    if (body.size() > UINT16_MAX - kIlinkHeaderSize || header.block_length > body.size()) return 0;
    const std::size_t size = kIlinkHeaderSize + body.size();
    if (output.size() < size) return 0;
    if (!body.empty()) std::memmove(output.data() + kIlinkHeaderSize, body.data(), body.size());
    store_le(output.data(), static_cast<std::uint16_t>(size));
    store_le(output.data() + 2, kEncoding);
    encode_header(output.data() + 4, header);
    return size;
}

[[nodiscard]] inline std::size_t encode_mdp_message(Header header, std::span<const char> body,
                                                   std::span<char> output) noexcept {
    if (body.size() > UINT16_MAX - kMdpMessageHeaderSize || header.block_length > body.size()) return 0;
    const std::size_t size = kMdpMessageHeaderSize + body.size();
    if (output.size() < size) return 0;
    if (!body.empty()) std::memmove(output.data() + kMdpMessageHeaderSize, body.data(), body.size());
    store_le(output.data(), static_cast<std::uint16_t>(size));
    encode_header(output.data() + 2, header);
    return size;
}

struct MdpPacketView {
    std::uint32_t sequence = 0;
    std::uint64_t sending_time = 0;  // UTC epoch nanoseconds
    std::span<const char> messages;
};

// Validate the whole UDP payload before exposing any messages. A truncated
// last message must not leave the consumer with a partially applied packet.
[[nodiscard]] inline Status decode_mdp_packet(std::span<const char> bytes, MdpPacketView& out) noexcept {
    if (bytes.size() < kMdpPacketHeaderSize || bytes.size() > 65507) return Status::invalid_length;
    auto remaining = bytes.subspan(kMdpPacketHeaderSize);
    if (remaining.empty()) return Status::invalid_length;
    while (!remaining.empty()) {
        MessageView message;
        const Status status = decode_mdp_message(remaining, message);
        if (status != Status::ok) return status == Status::incomplete ? Status::invalid_length : status;
        remaining = remaining.subspan(message.frame_size);
    }
    out = {load_le<std::uint32_t>(bytes.data()), load_le<std::uint64_t>(bytes.data() + 4),
           bytes.subspan(kMdpPacketHeaderSize)};
    return Status::ok;
}

[[nodiscard]] inline std::size_t encode_mdp_packet(std::uint32_t sequence, std::uint64_t sending_time,
                                                  std::span<const char> messages, std::span<char> output) noexcept {
    if (messages.empty() || messages.size() > 65507 - kMdpPacketHeaderSize ||
        output.size() < kMdpPacketHeaderSize + messages.size()) return 0;
    auto remaining = messages;
    while (!remaining.empty()) {
        MessageView message;
        if (decode_mdp_message(remaining, message) != Status::ok) return 0;
        remaining = remaining.subspan(message.frame_size);
    }
    std::memmove(output.data() + kMdpPacketHeaderSize, messages.data(), messages.size());
    store_le(output.data(), sequence);
    store_le(output.data() + 4, sending_time);
    return kMdpPacketHeaderSize + messages.size();
}

// CME groupSize has uint16 blockLength and uint8 numInGroup. Each template
// still defines its own minimum entry size; larger acting blocks are skipped
// using the received blockLength, not the compiled entry size.
struct GroupView {
    std::uint16_t block_length = 0;
    std::uint8_t count = 0;
    std::span<const char> entries;
    std::size_t encoded_size = 0;

    [[nodiscard]] std::span<const char> entry(std::size_t index) const noexcept {
        if (index >= count) return {};
        return entries.subspan(index * block_length, block_length);
    }
};

[[nodiscard]] inline Status decode_group(std::span<const char> bytes, std::uint16_t minimum_entry_size,
                                         GroupView& out) noexcept {
    if (bytes.size() < 3) return Status::incomplete;
    const auto block = load_le<std::uint16_t>(bytes.data());
    const auto count = static_cast<std::uint8_t>(bytes[2]);
    if (block < minimum_entry_size) return Status::invalid_length;
    const std::size_t length = std::size_t{block} * count;
    if (length > bytes.size() - 3) return Status::incomplete;
    out = {block, count, bytes.subspan(3, length), length + 3};
    return Status::ok;
}

inline constexpr std::int64_t kNullPrice9 = INT64_MAX;
inline constexpr std::int64_t kPrice9ScaleRatio = 100'000;

// Internal prices have four decimals; CME PRICE9 has nine. Reject loss of
// precision, nulls and overflow instead of rounding or truncating.
[[nodiscard]] constexpr bool price_from_price9(std::int64_t mantissa, Price& out) noexcept {
    if (mantissa == kNullPrice9 || mantissa % kPrice9ScaleRatio != 0) return false;
    out = mantissa / kPrice9ScaleRatio;
    return true;
}

[[nodiscard]] constexpr bool price_to_price9(Price price, std::int64_t& out) noexcept {
    if (price < INT64_MIN / kPrice9ScaleRatio || price > INT64_MAX / kPrice9ScaleRatio) return false;
    out = price * kPrice9ScaleRatio;
    return true;
}

}  // namespace exsim::protocol::cme
#endif
