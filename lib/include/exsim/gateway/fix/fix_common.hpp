#ifndef EXSIM_GATEWAY_FIX_FIX_COMMON_HPP
#define EXSIM_GATEWAY_FIX_FIX_COMMON_HPP

// Shared building blocks of the FIX 4.4 order-entry gateway, used by both the
// exchange side (FixConnection) and the client side (FixClient):
//   * FixByteSink: the prepare/commit output contract of server contract 2.4
//   * FixTime: the injected clock reading every call carries
//   * FixSessionSettings: session bounds and timer policy
//   * FixFieldReader: typed inbound field access that remembers the first
//     problem as a session Reject(3) reason (373) and reference tag (371)
//   * FixFrameWriter: an outbound frame writer that, unlike fix44::Builder,
//     can splice already formatted fields (used for PossDup resends)
//   * stream resynchronisation helpers for garbled input
// Nothing in this header allocates.

#include "exsim/protocol/fix44.hpp"
#include "exsim/types.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace exsim::gateway::fix {

namespace fix44 = ::exsim::protocol::fix44;
using ::exsim::protocol::Status;

template <class Sink>
concept FixByteSink = requires(Sink& sink, std::size_t byte_count) {
    { sink.prepare(byte_count) } -> std::same_as<char*>;
    sink.commit(byte_count);
};

// One reading of the injected clock. monotonic_ns drives timers (the server
// passes Context::now()); realtime_ns is the Unix epoch time written to
// SendingTime(52), OrigSendingTime(122) and TransactTime(60).
struct FixTime {
    Timestamp monotonic_ns = 0;
    std::uint64_t realtime_ns = 0;
};

inline constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;
inline constexpr std::size_t kMaxCompIdLength = 64;
inline constexpr std::size_t kMaxInboundTextFieldLength = 64;
inline constexpr std::size_t kMaxOutboundMessageBytes = 2048;
// A PossDup resend adds 43=Y and 122=<timestamp> to a stored message.
inline constexpr std::size_t kMaxResentMessageBytes = kMaxOutboundMessageBytes + 64;
inline constexpr std::size_t kParserFieldCapacity = 64;
// Highest MsgSeqNum(34) a session accepts: consuming it sets the expected
// sequence to kMaxMsgSeqNum + 1, which must not wrap to 0.
inline constexpr std::uint64_t kMaxMsgSeqNum = UINT64_MAX - 1;

using FixParser = fix44::Parser<kParserFieldCapacity>;

struct FixSessionSettings {
    std::size_t max_inbound_message_bytes = 4096;   // larger frames are garbled
    std::size_t held_inbound_message_slots = 16;    // early (high MsgSeqNum) messages kept for in-order replay
    std::uint64_t max_heartbeat_interval_seconds = 300;
    Timestamp logon_timeout_ns = 10 * kNanosecondsPerSecond;
    Timestamp logout_timeout_ns = 2 * kNanosecondsPerSecond;
    Timestamp resend_request_retry_ns = 2 * kNanosecondsPerSecond;
};

// SessionRejectReason(373) values the gateway emits.
enum class SessionRejectReason : std::uint8_t {
    InvalidTagNumber = 0,
    RequiredTagMissing = 1,
    TagSpecifiedWithoutValue = 4,
    ValueIsIncorrect = 5,
    IncorrectDataFormat = 6,
    CompIdProblem = 9,
    SendingTimeAccuracyProblem = 10,
    InvalidMsgType = 11,
    TagAppearsMoreThanOnce = 13,
    Other = 99,
};

struct FieldProblem {
    unsigned tag = 0;
    SessionRejectReason reason = SessionRejectReason::Other;
    std::string_view description{};  // static text for Reject Text(58)
};

// Small fixed-capacity owned string (CompIDs, TestReqIDs).
template <std::size_t Capacity>
class FixedText {
public:
    bool assign(std::string_view text) noexcept {
        if (text.size() > Capacity) {
            return false;
        }
        if (!text.empty()) {
            std::memcpy(characters_.data(), text.data(), text.size());
        }
        length_ = text.size();
        return true;
    }
    [[nodiscard]] std::string_view view() const noexcept { return {characters_.data(), length_}; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }

private:
    std::array<char, Capacity> characters_{};
    std::size_t length_ = 0;
};

// Decimal rendering of an unsigned value that a codec Text<> can borrow.
class DecimalText {
public:
    explicit DecimalText(std::uint64_t value) noexcept
        : length_(fix44::format_uint(value, digits_.data(), digits_.size())) {}
    [[nodiscard]] std::string_view view() const noexcept { return {digits_.data(), length_}; }

private:
    std::array<char, 20> digits_{};
    std::size_t length_;
};

// Typed access to a successfully parsed message. Every accessor validates its
// field; the first failure is kept and later calls are harmless, so a decoder
// reads all fields and checks ok() once.
class FixFieldReader {
public:
    explicit FixFieldReader(const FixParser& parser) noexcept : parser_(parser) {}

    [[nodiscard]] bool ok() const noexcept { return !has_problem_; }
    [[nodiscard]] const FieldProblem& problem() const noexcept { return problem_; }

    void report(unsigned tag, SessionRejectReason reason, std::string_view description) noexcept {
        if (!has_problem_) {
            has_problem_ = true;
            problem_ = {tag, reason, description};
        }
    }

    std::string_view required_text(unsigned tag,
                                   std::size_t max_length = kMaxInboundTextFieldLength) noexcept {
        return present(tag, true) ? bounded_text(tag, max_length) : std::string_view{};
    }
    std::string_view optional_text(unsigned tag,
                                   std::size_t max_length = kMaxInboundTextFieldLength) noexcept {
        return present(tag, false) ? bounded_text(tag, max_length) : std::string_view{};
    }

    std::uint64_t required_uint(unsigned tag) noexcept {
        std::uint64_t value = 0;
        if (present(tag, true) && !parser_.get_uint(tag, value)) {
            report(tag, SessionRejectReason::IncorrectDataFormat, "incorrect data format");
        }
        return value;
    }

    // An empty `allowed` set accepts any single character.
    char required_char(unsigned tag, std::string_view allowed) noexcept {
        return present(tag, true) ? checked_char(tag, allowed) : '\0';
    }
    char optional_char(unsigned tag, std::string_view allowed, char absent_value) noexcept {
        return present(tag, false) ? checked_char(tag, allowed) : absent_value;
    }

    // Returns true when the field is present and valid.
    bool optional_price(unsigned tag, Price& value) noexcept {
        if (!present(tag, false)) {
            return false;
        }
        if (!parser_.get_price(tag, value)) {
            report(tag, SessionRejectReason::IncorrectDataFormat, "incorrect data format");
            return false;
        }
        return true;
    }

    std::uint64_t required_timestamp(unsigned tag) noexcept {
        std::uint64_t value = 0;
        if (present(tag, true) && !parser_.get_timestamp(tag, value)) {
            report(tag, SessionRejectReason::IncorrectDataFormat, "incorrect data format");
        }
        return value;
    }

private:
    bool present(unsigned tag, bool required) noexcept {
        if (!parser_.has(tag)) {
            if (required) {
                report(tag, SessionRejectReason::RequiredTagMissing, "required tag missing");
            }
            return false;
        }
        if (parser_.get(tag).empty()) {
            report(tag, SessionRejectReason::TagSpecifiedWithoutValue, "tag specified without a value");
            return false;
        }
        return true;
    }

    std::string_view bounded_text(unsigned tag, std::size_t max_length) noexcept {
        const std::string_view value = parser_.get(tag);
        if (value.size() > max_length) {
            report(tag, SessionRejectReason::ValueIsIncorrect, "value is too long");
            return {};
        }
        return value;
    }

    char checked_char(unsigned tag, std::string_view allowed) noexcept {
        const std::string_view value = parser_.get(tag);
        if (value.size() != 1) {
            report(tag, SessionRejectReason::IncorrectDataFormat, "incorrect data format");
            return '\0';
        }
        if (!allowed.empty() && allowed.find(value[0]) == std::string_view::npos) {
            report(tag, SessionRejectReason::ValueIsIncorrect, "value is incorrect for this tag");
            return '\0';
        }
        return value[0];
    }

    const FixParser& parser_;
    FieldProblem problem_{};
    bool has_problem_ = false;
};

// Writes "8=FIX.4.4|9=<n>|" + fields + "10=<sum>|" into a caller buffer.
// Fields are appended in call order; raw_fields() copies preformatted
// "tag=value<SOH>" bytes verbatim.
class FixFrameWriter {
public:
    explicit FixFrameWriter(std::span<char> buffer) noexcept
        : buffer_(buffer), used_(kBodyOffset), good_(buffer.size() > kBodyOffset + kTrailerBytes) {}

    bool field(unsigned tag, std::string_view value) noexcept {
        if (!good_ || value.find(fix44::soh) != std::string_view::npos) {
            good_ = false;
            return false;
        }
        char tag_digits[10];
        const std::size_t tag_length = fix44::format_uint(tag, tag_digits, sizeof tag_digits);
        if (tag_length + value.size() + 2 > buffer_.size() - used_) {
            good_ = false;
            return false;
        }
        std::memcpy(buffer_.data() + used_, tag_digits, tag_length);
        used_ += tag_length;
        buffer_[used_++] = '=';
        if (!value.empty()) {
            std::memcpy(buffer_.data() + used_, value.data(), value.size());
        }
        used_ += value.size();
        buffer_[used_++] = fix44::soh;
        return true;
    }
    bool field(unsigned tag, char value) noexcept { return field(tag, std::string_view(&value, 1)); }
    bool uint_field(unsigned tag, std::uint64_t value) noexcept {
        return field(tag, DecimalText(value).view());
    }
    bool timestamp_field(unsigned tag, std::uint64_t realtime_ns) noexcept {
        char text[21];
        fix44::format_timestamp(realtime_ns, text, sizeof text);
        return field(tag, std::string_view(text, sizeof text));
    }
    bool raw_fields(std::string_view preformatted) noexcept {
        if (!good_ || preformatted.size() > buffer_.size() - used_) {
            good_ = false;
            return false;
        }
        if (!preformatted.empty()) {
            std::memcpy(buffer_.data() + used_, preformatted.data(), preformatted.size());
        }
        used_ += preformatted.size();
        return true;
    }

    // Returns the frame size (frame starts at buffer.data()), or 0 on overflow.
    std::size_t finish() noexcept {
        if (!good_) {
            return 0;
        }
        const std::size_t body_length = used_ - kBodyOffset;
        char head[40] = "8=FIX.4.4\001" "9=";
        const std::size_t digits = fix44::format_uint(body_length, head + 12, 20);
        const std::size_t head_length = 12 + digits + 1;
        head[head_length - 1] = fix44::soh;
        const std::size_t total = head_length + body_length + kTrailerBytes;
        if (total > buffer_.size()) {
            return 0;
        }
        std::memmove(buffer_.data() + head_length, buffer_.data() + kBodyOffset, body_length);
        std::memcpy(buffer_.data(), head, head_length);
        unsigned checksum = 0;
        for (std::size_t index = 0; index < total - kTrailerBytes; ++index) {
            checksum += static_cast<unsigned char>(buffer_[index]);
        }
        checksum %= 256;
        char* trailer = buffer_.data() + total - kTrailerBytes;
        trailer[0] = '1';
        trailer[1] = '0';
        trailer[2] = '=';
        trailer[3] = static_cast<char>('0' + checksum / 100);
        trailer[4] = static_cast<char>('0' + checksum / 10 % 10);
        trailer[5] = static_cast<char>('0' + checksum % 10);
        trailer[6] = fix44::soh;
        good_ = false;
        return total;
    }

private:
    static constexpr std::size_t kBodyOffset = 33;  // room for "8=FIX.4.4|9=" + 20 digits + SOH
    static constexpr std::size_t kTrailerBytes = 7;

    std::span<char> buffer_;
    std::size_t used_;
    bool good_;
};

// "8=FIX.4.4<SOH>9=", the only valid start of a FIX 4.4 frame.
inline constexpr std::string_view kFrameStartPattern{"8=FIX.4.4\001" "9=", 12};

// True when MsgType(35) is the third field, right after BeginString(8) and
// BodyLength(9). `frame` must start with kFrameStartPattern.
[[nodiscard]] inline bool msg_type_is_third_field(std::span<const char> frame) noexcept {
    const std::string_view text(frame.data(), frame.size());
    const std::size_t body_length_end = text.find(fix44::soh, kFrameStartPattern.size());
    return body_length_end != std::string_view::npos && text.substr(body_length_end + 1, 3) == "35=";
}

// True for every MsgType(35) the FIX 4.4 specification defines (whether or
// not this gateway supports it) and for user-defined types ("U" followed by
// at least one character). Anything else is an invalid MsgType (373=11).
[[nodiscard]] inline bool is_fix44_message_type(std::string_view type) noexcept {
    if (type.size() == 1) {
        const char code = type[0];
        return (code >= '0' && code <= '9') || (code >= 'a' && code <= 'z') ||
               (code >= 'A' && code <= 'Z' && code != 'I' && code != 'O' && code != 'U');
    }
    if (type.size() == 2 && type[0] == 'A') {
        return type[1] >= 'A' && type[1] <= 'Z';
    }
    if (type.size() == 2 && type[0] == 'B') {
        return type[1] >= 'A' && type[1] <= 'H';
    }
    return type.size() >= 2 && type[0] == 'U';
}

// First position >= from where a frame may start: a full pattern match, or a
// partial match that runs into the end of `bytes` (more input may complete
// it). Returns bytes.size() when there is neither.
[[nodiscard]] inline std::size_t find_frame_start(std::span<const char> bytes,
                                                  std::size_t from) noexcept {
    for (std::size_t position = from; position < bytes.size(); ++position) {
        if (bytes[position] != '8') {
            continue;
        }
        const std::size_t comparable = std::min(bytes.size() - position, kFrameStartPattern.size());
        if (std::memcmp(bytes.data() + position, kFrameStartPattern.data(), comparable) == 0) {
            return position;
        }
    }
    return bytes.size();
}

// Position of a complete "<SOH>8=FIX.4.4<SOH>9=" inside `bytes` (the start of
// a later frame swallowed by a corrupt BodyLength), or 0 when there is none.
[[nodiscard]] inline std::size_t find_embedded_frame_start(std::span<const char> bytes) noexcept {
    for (std::size_t position = 1; position + kFrameStartPattern.size() <= bytes.size(); ++position) {
        if (bytes[position - 1] == fix44::soh &&
            std::memcmp(bytes.data() + position, kFrameStartPattern.data(), kFrameStartPattern.size()) == 0) {
            return position;
        }
    }
    return 0;
}

// Value of the first "<SOH>tag=" field in a frame, without a full parse
// (used for messages the parser refused). Empty when absent.
[[nodiscard]] inline std::string_view raw_field_value(std::span<const char> frame, unsigned tag) noexcept {
    char needle[16];
    needle[0] = fix44::soh;
    const std::size_t digits = fix44::format_uint(tag, needle + 1, 10);
    needle[digits + 1] = '=';
    const std::string_view needle_view(needle, digits + 2);
    const std::string_view text(frame.data(), frame.size());
    const std::size_t found = text.find(needle_view);
    if (found == std::string_view::npos) {
        return {};
    }
    const std::size_t value_begin = found + needle_view.size();
    const std::size_t value_end = text.find(fix44::soh, value_begin);
    if (value_end == std::string_view::npos) {
        return {};
    }
    return text.substr(value_begin, value_end - value_begin);
}

// First tag that occurs twice in a frame, or 0 (for Reject RefTagID).
[[nodiscard]] inline unsigned find_duplicate_tag(std::span<const char> frame) noexcept {
    constexpr std::size_t kMaxScannedTags = 256;
    std::array<unsigned, kMaxScannedTags> seen_tags{};
    std::size_t seen_count = 0;
    std::size_t position = 0;
    while (position < frame.size() && seen_count < kMaxScannedTags) {
        unsigned tag = 0;
        while (position < frame.size() && frame[position] >= '0' && frame[position] <= '9') {
            tag = tag * 10 + static_cast<unsigned>(frame[position] - '0');
            ++position;
        }
        for (std::size_t index = 0; index < seen_count; ++index) {
            if (seen_tags[index] == tag) {
                return tag;
            }
        }
        seen_tags[seen_count++] = tag;
        while (position < frame.size() && frame[position] != fix44::soh) {
            ++position;
        }
        ++position;
    }
    return 0;
}

// "MsgSeqNum too low, expecting X but received Y" (FIX session wording) in a
// caller buffer of at least 96 bytes.
[[nodiscard]] inline std::string_view format_sequence_too_low(std::span<char> buffer, std::uint64_t expected,
                                                            std::uint64_t received) noexcept {
    constexpr std::string_view kPrefix = "MsgSeqNum too low, expecting ";
    constexpr std::string_view kMiddle = " but received ";
    if (buffer.size() < kPrefix.size() + kMiddle.size() + 40) {
        return "MsgSeqNum too low";
    }
    std::size_t used = 0;
    std::memcpy(buffer.data(), kPrefix.data(), kPrefix.size());
    used += kPrefix.size();
    used += fix44::format_uint(expected, buffer.data() + used, 20);
    std::memcpy(buffer.data() + used, kMiddle.data(), kMiddle.size());
    used += kMiddle.size();
    used += fix44::format_uint(received, buffer.data() + used, 20);
    return {buffer.data(), used};
}

// Rebuilds a stored outbound message for a resend: same MsgType, CompIDs and
// MsgSeqNum, PossDupFlag(43)=Y, a fresh SendingTime(52), OrigSendingTime(122)
// from the original, then the original body. The stored message must have
// been produced by this gateway (header order 35, 49, 56, 34, 52).
// Returns the frame size, or 0 if the input is not such a message.
[[nodiscard]] inline std::size_t write_possible_duplicate(std::span<const char> original,
                                                          std::uint64_t original_sending_time_ns,
                                                          std::uint64_t sending_time_ns,
                                                          std::span<char> output) noexcept {
    const std::string_view text(original.data(), original.size());
    const std::size_t sending_time_tag = text.find("\001" "52=");
    if (sending_time_tag == std::string_view::npos || original.size() < 7) {
        return 0;
    }
    const std::size_t body_begin = text.find(fix44::soh, sending_time_tag + 1);
    const std::size_t body_end = original.size() - 7;  // "10=nnn<SOH>"
    if (body_begin == std::string_view::npos || body_begin + 1 > body_end) {
        return 0;
    }
    FixFrameWriter writer(output);
    writer.field(35, raw_field_value(original, 35));
    writer.field(49, raw_field_value(original, 49));
    writer.field(56, raw_field_value(original, 56));
    writer.field(34, raw_field_value(original, 34));
    writer.field(43, 'Y');
    writer.timestamp_field(52, sending_time_ns);
    writer.timestamp_field(122, original_sending_time_ns);
    writer.raw_fields(text.substr(body_begin + 1, body_end - body_begin - 1));
    return writer.finish();
}

}  // namespace exsim::gateway::fix

#endif
