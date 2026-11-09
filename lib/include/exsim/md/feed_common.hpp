#ifndef EXSIM_MD_FEED_COMMON_HPP
#define EXSIM_MD_FEED_COMMON_HPP

// Shared vocabulary of the ITCH 5.0 / MoldUDP64 market-data feed: wire size
// limits, the 10-byte MoldUDP64 session name and the ITCH day-timestamp rule
// from gateway mapping section 4.

#include <exsim/protocol/moldudp64.hpp>
#include <exsim/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace exsim::md {

inline constexpr std::size_t kMoldHeaderSize = 20;          // session(10) sequence(8) count(2)
inline constexpr std::size_t kMoldMessageLengthSize = 2;    // per-message length prefix
inline constexpr std::size_t kMoldRequestSize = 20;         // session(10) sequence(8) count(2)
inline constexpr std::uint16_t kMoldEndOfSessionCount = 0xFFFF;
inline constexpr std::uint16_t kMoldMaxMessagesPerPacket = 65534;  // PacketBuilder limit

// Largest ITCH 5.0 message this simulator emits (Net Order Imbalance 'I').
inline constexpr std::size_t kMaxItchMessageSize = 50;

// The smallest max_payload that still fits every ITCH message in one packet.
inline constexpr std::size_t kMinimumMaxPayload = kMoldHeaderSize + kMoldMessageLengthSize + kMaxItchMessageSize;
// IPv4 UDP payload limit.
inline constexpr std::size_t kMaximumMaxPayload = 65507;

// ITCH timestamps are nanoseconds since the trading-day origin and must lie
// in [0, 86,400,000,000,000) (GATEWAYS.md section 4).
inline constexpr std::uint64_t kNanosecondsPerTradingDay = 86'400ULL * 1'000'000'000ULL;

// ITCH Price(4) is an unsigned 32-bit field.
inline constexpr Price kMaxItchPrice = static_cast<Price>(std::numeric_limits<std::uint32_t>::max());

using MoldSessionName = std::array<char, 10>;  // right-space-padded ASCII

// Pads `text` to the 10-byte MoldUDP64 session field. False if it is longer.
[[nodiscard]] constexpr bool make_mold_session_name(std::string_view text, MoldSessionName& out) noexcept {
    if (text.size() > out.size()) return false;
    for (std::size_t index = 0; index < out.size(); ++index) out[index] = index < text.size() ? text[index] : ' ';
    return true;
}

// Event timestamp -> ITCH timestamp. False when the result falls outside the
// trading day (the publisher then stops the session instead of truncating).
[[nodiscard]] constexpr bool itch_timestamp_from_event_time(Timestamp event_time, Timestamp day_origin,
                                                            std::uint64_t& itch_timestamp_out) noexcept {
    if (event_time < day_origin) return false;
    const std::uint64_t since_origin = event_time - day_origin;
    if (since_origin >= kNanosecondsPerTradingDay) return false;
    itch_timestamp_out = since_origin;
    return true;
}

}  // namespace exsim::md

#endif
