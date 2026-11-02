#ifndef EXSIM_PROTOCOL_FIX44_HPP
#define EXSIM_PROTOCOL_FIX44_HPP
// Single-message FIX 4.4 codec. Parser and decoded Text values borrow input;
// keep that buffer alive and unchanged until all views have been consumed.
// No repeating groups are supported by the scoped message set: duplicate tags
// are rejected. Message decoders check required fields and typed values.
// Builder buffers can be reused after finish(); construct a new Builder per
// message.
#include "wire.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
namespace exsim::protocol::fix44 {
using protocol::Status;
inline constexpr char soh = '\1';
inline bool parse_uint(std::string_view s, std::uint64_t &v) noexcept {
  if (s.empty())
    return false;
  v = 0;
  for (char c : s) {
    if (c < '0' || c > '9' ||
        v > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
      return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  return true;
}
inline bool parse_price(std::string_view s, Price &price) noexcept {
  bool negative = !s.empty() && s.front() == '-';
  if (negative)
    s.remove_prefix(1);
  if (s.empty())
    return false;
  std::uint64_t v = 0;
  unsigned decimals = 0;
  bool dot = false, digit = false;
  const auto limit = std::uint64_t(INT64_MAX) + (negative ? 1 : 0);
  for (char c : s) {
    if (c == '.' && !dot && digit) {
      dot = true;
      continue;
    }
    if (c < '0' || c > '9' || (dot && ++decimals > 4))
      return false;
    digit = true;
    auto d = static_cast<unsigned>(c - '0');
    if (v > (limit - d) / 10)
      return false;
    v = v * 10 + d;
  }
  if (!digit || (dot && !decimals))
    return false;
  for (unsigned i = decimals; i < 4; ++i) {
    if (v > limit / 10)
      return false;
    v *= 10;
  }
  price = negative
              ? (v == std::uint64_t(INT64_MAX) + 1 ? INT64_MIN
                                                   : -static_cast<Price>(v))
              : static_cast<Price>(v);
  return true;
}
inline std::size_t format_uint(std::uint64_t v, char *p,
                               std::size_t n) noexcept {
  char tmp[20];
  auto result = std::to_chars(tmp, tmp + 20, v);
  auto len = static_cast<std::size_t>(result.ptr - tmp);
  if (n < len)
    return 0;
  std::memcpy(p, tmp, len);
  return len;
}
inline std::size_t format_price(Price v, char *p, std::size_t n) noexcept {
  char tmp[32];
  std::size_t pos = 0;
  bool neg = v < 0;
  std::uint64_t mag =
      neg ? std::uint64_t(-(v + 1)) + 1 : static_cast<std::uint64_t>(v);
  if (neg)
    tmp[pos++] = '-';
  pos += format_uint(mag / 10000, tmp + pos, sizeof(tmp) - pos);
  auto fraction = mag % 10000;
  if (fraction) {
    tmp[pos++] = '.';
    for (unsigned div = 1000; div; div /= 10)
      tmp[pos++] = static_cast<char>('0' + (fraction / div) % 10);
    while (tmp[pos - 1] == '0')
      --pos;
  }
  if (n < pos)
    return 0;
  std::memcpy(p, tmp, pos);
  return pos;
}
// Gregorian calendar conversion, independent of libc timezone and locale.
inline std::int64_t days_from_civil(int year, unsigned month,
                                    unsigned day) noexcept {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const auto y = static_cast<unsigned>(year - era * 400);
  const auto doy =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  return static_cast<std::int64_t>(era) * 146097 + y * 365 + y / 4 - y / 100 +
         doy - 719468;
}
inline std::size_t format_timestamp(std::uint64_t ns, char *p,
                                    std::size_t n) noexcept {
  if (n < 21)
    return 0;
  auto seconds = ns / 1000000000;
  auto z = static_cast<std::int64_t>(seconds / 86400) + 719468;
  auto era = (z >= 0 ? z : z - 146096) / 146097;
  auto doe = static_cast<unsigned>(z - era * 146097);
  auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  auto mp = (5 * doy + 2) / 153;
  auto d = doy - (153 * mp + 2) / 5 + 1;
  auto m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  auto put = [&](unsigned v, unsigned off, unsigned len) {
    while (len) {
      p[off + --len] = static_cast<char>('0' + v % 10);
      v /= 10;
    }
  };
  put(static_cast<unsigned>(y), 0, 4);
  put(m, 4, 2);
  put(d, 6, 2);
  p[8] = '-';
  put(static_cast<unsigned>(seconds % 86400 / 3600), 9, 2);
  p[11] = ':';
  put(static_cast<unsigned>(seconds % 3600 / 60), 12, 2);
  p[14] = ':';
  put(static_cast<unsigned>(seconds % 60), 15, 2);
  p[17] = '.';
  put(static_cast<unsigned>(ns / 1000000 % 1000), 18, 3);
  return 21;
}
inline bool parse_timestamp(std::string_view s, std::uint64_t &ns) noexcept {
  if ((s.size() != 17 && s.size() != 21) || s[8] != '-' || s[11] != ':' ||
      s[14] != ':' || (s.size() == 21 && s[17] != '.'))
    return false;
  std::uint64_t y, m, d, h, mi, sec, ms = 0;
  if (!parse_uint(s.substr(0, 4), y) || !parse_uint(s.substr(4, 2), m) ||
      !parse_uint(s.substr(6, 2), d) || !parse_uint(s.substr(9, 2), h) ||
      !parse_uint(s.substr(12, 2), mi) || !parse_uint(s.substr(15, 2), sec) ||
      (s.size() == 21 && !parse_uint(s.substr(18, 3), ms)))
    return false;
  if (y < 1970 || m < 1 || m > 12 || d < 1 || h > 23 || mi > 59 || sec > 59)
    return false;
  constexpr unsigned lengths[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  unsigned max =
      lengths[m - 1] +
      (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 1 : 0);
  if (d > max)
    return false;
  auto days = days_from_civil(static_cast<int>(y), static_cast<unsigned>(m),
                              static_cast<unsigned>(d));
  auto seconds =
      static_cast<std::uint64_t>(days) * 86400 + h * 3600 + mi * 60 + sec;
  if (seconds > (UINT64_MAX - ms * 1000000) / 1000000000)
    return false;
  ns = seconds * 1000000000 + ms * 1000000;
  return true;
}
// Determines exactly one frame's extent; extra bytes belong to the next frame.
inline Status frame_size(std::span<const char> p, std::size_t &size,
                         std::size_t max = 1048576) noexcept {
  constexpr std::string_view prefix = "8=FIX.4.4\1";
  for (std::size_t i = 0; i < std::min(p.size(), prefix.size()); ++i)
    if (p[i] != prefix[i])
      return Status::invalid_field;
  if (p.size() < 12)
    return Status::incomplete;
  if (p[10] != '9' || p[11] != '=')
    return Status::invalid_field;
  std::size_t end = 12;
  while (end < p.size() && p[end] != soh) {
    if (end - 12 >= 20)
      return Status::overflow;
    ++end;
  }
  if (end == p.size())
    return Status::incomplete;
  std::uint64_t body;
  if (!parse_uint({p.data() + 12, end - 12}, body))
    return Status::invalid_field;
  if (end + 8 > max || body > max - end - 8)
    return Status::overflow;
  size = end + 1 + static_cast<std::size_t>(body) + 7;
  return p.size() < size ? Status::incomplete : Status::ok;
}
struct Field {
  unsigned tag{};
  std::size_t offset{}, length{};
};
template <std::size_t N = 64> class Parser {
  std::array<Field, N> fields_{};
  std::size_t count_ = 0;
  std::string_view bytes_{};

public:
  Status parse(std::span<const char> p) noexcept {
    count_ = 0;
    bytes_ = {};
    std::size_t size;
    auto status = frame_size(p, size);
    if (status != Status::ok)
      return status;
    if (size != p.size())
      return Status::invalid_length;
    if (size < 7 || std::string_view(p.data() + size - 7, 3) != "10=" ||
        p.back() != soh)
      return Status::invalid_length;
    std::uint64_t expected;
    if (!parse_uint({p.data() + size - 4, 3}, expected) || expected > 255)
      return Status::checksum;
    unsigned sum = 0;
    for (std::size_t i = 0; i < size - 7; ++i)
      sum += static_cast<unsigned char>(p[i]);
    if (sum % 256 != expected)
      return Status::checksum;
    std::size_t pos = 0;
    while (pos < size) {
      std::size_t eq = pos;
      while (eq < size && p[eq] != '=' && p[eq] != soh)
        ++eq;
      std::uint64_t tag;
      if (eq == size || p[eq] != '=' ||
          !parse_uint({p.data() + pos, eq - pos}, tag) || !tag ||
          tag > UINT32_MAX)
        return Status::invalid_field;
      std::size_t end = eq + 1;
      while (end < size && p[end] != soh)
        ++end;
      if (end == size)
        return Status::invalid_field;
      if (tag == 10 && pos != size - 7)
        return Status::invalid_length;
      for (std::size_t i = 0; i < count_; ++i)
        if (fields_[i].tag == tag)
          return Status::duplicate_field;
      if (count_ == N)
        return Status::full;
      fields_[count_++] = {static_cast<unsigned>(tag), eq + 1, end - eq - 1};
      pos = end + 1;
    }
    bytes_ = {p.data(), p.size()};
    return Status::ok;
  }
  Status parse(const char *p, std::size_t n) noexcept { return parse({p, n}); }
  bool has(unsigned tag) const noexcept {
    if (bytes_.empty())
      return false;
    for (std::size_t i = 0; i < count_; ++i)
      if (fields_[i].tag == tag)
        return true;
    return false;
  }
  std::string_view get(unsigned tag) const noexcept {
    if (bytes_.empty())
      return {};
    for (std::size_t i = 0; i < count_; ++i)
      if (fields_[i].tag == tag)
        return bytes_.substr(fields_[i].offset, fields_[i].length);
    return {};
  }
  bool get_uint(unsigned tag, std::uint64_t &v) const noexcept {
    return parse_uint(get(tag), v);
  }
  bool get_int(unsigned tag, std::int64_t &v) const noexcept {
    auto s = get(tag);
    if (s.empty())
      return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
  }
  bool get_char(unsigned tag, char &v) const noexcept {
    auto s = get(tag);
    if (s.size() != 1)
      return false;
    v = s[0];
    return true;
  }
  bool get_price(unsigned tag, Price &v) const noexcept {
    return parse_price(get(tag), v);
  }
  bool get_timestamp(unsigned tag, std::uint64_t &v) const noexcept {
    return parse_timestamp(get(tag), v);
  }
};
struct Header {
  std::string_view sender{}, target{};
  std::uint64_t sequence{}, sending_time{};
};
class Builder {
  std::span<char> buffer_;
  std::size_t used_ = 14;
  bool good_ = true, finished_ = false;

public:
  explicit Builder(std::span<char> b) noexcept
      : buffer_(b), good_(b.size() >= 14) {}
  bool field(unsigned tag, std::string_view value) noexcept {
    if (!good_ || finished_ || value.find(soh) != std::string_view::npos) {
      good_ = false;
      return false;
    }
    char key[10];
    auto len = format_uint(tag, key, sizeof key);
    if (!len || len + value.size() + 2 > buffer_.size() - used_) {
      good_ = false;
      return false;
    }
    std::memcpy(buffer_.data() + used_, key, len);
    used_ += len;
    buffer_[used_++] = '=';
    if (!value.empty())
      std::memcpy(buffer_.data() + used_, value.data(), value.size());
    used_ += value.size();
    buffer_[used_++] = soh;
    return true;
  }
  bool field(unsigned tag, char value) noexcept {
    return field(tag, {&value, 1});
  }
  bool uint_field(unsigned tag, std::uint64_t v) noexcept {
    char tmp[20];
    auto n = format_uint(v, tmp, sizeof tmp);
    return field(tag, {tmp, n});
  }
  bool price_field(unsigned tag, Price v) noexcept {
    char tmp[32];
    auto n = format_price(v, tmp, sizeof tmp);
    return field(tag, {tmp, n});
  }
  bool timestamp_field(unsigned tag, std::uint64_t v) noexcept {
    char tmp[21];
    format_timestamp(v, tmp, sizeof tmp);
    return field(tag, {tmp, 21});
  }
  bool header(char type, const Header &h) noexcept {
    return field(35, type) && field(49, h.sender) && field(56, h.target) &&
           uint_field(34, h.sequence) && timestamp_field(52, h.sending_time);
  }
  std::size_t finish() noexcept {
    if (!good_ || finished_)
      return 0;
    finished_ = true;
    char head[32] = "8=FIX.4.4\1";
    head[10] = '9';
    head[11] = '=';
    auto digits = format_uint(used_ - 14, head + 12, 20);
    std::size_t hlen = 13 + digits;
    head[hlen - 1] = soh;
    std::size_t total = hlen + used_ - 14 + 7;
    if (total > buffer_.size())
      return 0;
    std::memmove(buffer_.data() + hlen, buffer_.data() + 14, used_ - 14);
    std::memcpy(buffer_.data(), head, hlen);
    unsigned sum = 0;
    for (std::size_t i = 0; i < total - 7; ++i)
      sum += static_cast<unsigned char>(buffer_[i]);
    sum %= 256;
    char *p = buffer_.data() + total - 7;
    p[0] = '1';
    p[1] = '0';
    p[2] = '=';
    p[3] = static_cast<char>('0' + sum / 100);
    p[4] = static_cast<char>('0' + sum / 10 % 10);
    p[5] = static_cast<char>('0' + sum % 10);
    p[6] = soh;
    return total;
  }
};
class FrameReader {
  std::span<char> buffer_;
  std::size_t used_ = 0;
  bool ready_ = false;

public:
  explicit FrameReader(std::span<char> b) noexcept : buffer_(b) {}
  void reset() noexcept {
    used_ = 0;
    ready_ = false;
  }
  Status feed(std::span<const char> input, std::size_t &consumed,
              std::span<const char> &message) noexcept {
    consumed = 0;
    if (ready_)
      reset();
    while (true) {
      std::size_t size = 0;
      auto status = frame_size(buffer_.first(used_), size, buffer_.size());
      if (status == Status::ok) {
        ready_ = true;
        message = buffer_.first(size);
        return status;
      }
      if (status != Status::incomplete)
        return status;
      if (consumed == input.size())
        return status;
      if (used_ == buffer_.size())
        return Status::full;
      buffer_[used_++] = input[consumed++];
    }
  }
};
// Encoded text owns fixed storage; decoded text borrows the original parse
// buffer. Borrowed values remain valid only while that buffer is unchanged.
template <std::size_t N = 64> struct Text {
  std::array<char, N> storage{};
  std::size_t length{};
  std::string_view borrowed{};
  bool assign(std::string_view s) noexcept {
    if (s.size() > N)
      return false;
    borrowed = {};
    length = s.size();
    std::copy(s.begin(), s.end(), storage.begin());
    return true;
  }
  void borrow(std::string_view s) noexcept {
    borrowed = s;
    length = 0;
  }
  std::string_view view() const noexcept {
    return borrowed.data() ? borrowed
                           : std::string_view(storage.data(), length);
  }
};
constexpr char tif_char(TimeInForce t) noexcept {
  switch (t) {
  case TimeInForce::Day:
    return '0';
  case TimeInForce::GTC:
    return '1';
  case TimeInForce::IOC:
    return '3';
  case TimeInForce::FOK:
    return '4';
  }
  return '?';
}
constexpr char stp_char(StpMode t) noexcept {
  switch (t) {
  case StpMode::None:
    return 'N';
  case StpMode::CancelResting:
    return 'R';
  case StpMode::CancelAggressor:
    return 'A';
  case StpMode::CancelBoth:
    return 'B';
  }
  return '?';
}
struct Logon {
  std::uint64_t encrypt_method{};
  std::uint64_t heartbeat_interval{};
  char reset_sequence{};
  bool has_reset_sequence = false;
};
inline std::size_t encode(const Logon &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('A', h);
  b.uint_field(98, m.encrypt_method);
  b.uint_field(108, m.heartbeat_interval);
  if (m.has_reset_sequence)
    b.field(141, m.reset_sequence);
  return b.finish();
}
template <std::size_t N> Status decode(const Parser<N> &p, Logon &m) noexcept {
  if (p.get(35) != "A")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(98))
    return Status::missing_field;
  {
    if (!p.get_uint(98, m.encrypt_method))
      return Status::invalid_field;
  }
  if (!p.has(108))
    return Status::missing_field;
  {
    if (!p.get_uint(108, m.heartbeat_interval))
      return Status::invalid_field;
  }
  m.has_reset_sequence = p.has(141);
  if (m.has_reset_sequence) {
    if (!p.get_char(141, m.reset_sequence) ||
        std::string_view("YN").find(m.reset_sequence) == std::string_view::npos)
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, Logon &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct Heartbeat {
  Text<> test_request_id{};
  bool has_test_request_id = false;
};
inline std::size_t encode(const Heartbeat &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('0', h);
  if (m.has_test_request_id)
    b.field(112, m.test_request_id.view());
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, Heartbeat &m) noexcept {
  if (p.get(35) != "0")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  m.has_test_request_id = p.has(112);
  if (m.has_test_request_id) {
    m.test_request_id.borrow(p.get(112));
    if (m.test_request_id.view().empty())
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, Heartbeat &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct TestRequest {
  Text<> test_request_id{};
};
inline std::size_t encode(const TestRequest &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('1', h);
  b.field(112, m.test_request_id.view());
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, TestRequest &m) noexcept {
  if (p.get(35) != "1")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(112))
    return Status::missing_field;
  {
    m.test_request_id.borrow(p.get(112));
    if (m.test_request_id.view().empty())
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, TestRequest &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct ResendRequest {
  std::uint64_t begin_sequence{};
  std::uint64_t end_sequence{};
};
inline std::size_t encode(const ResendRequest &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('2', h);
  b.uint_field(7, m.begin_sequence);
  b.uint_field(16, m.end_sequence);
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, ResendRequest &m) noexcept {
  if (p.get(35) != "2")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(7))
    return Status::missing_field;
  {
    if (!p.get_uint(7, m.begin_sequence))
      return Status::invalid_field;
  }
  if (!p.has(16))
    return Status::missing_field;
  {
    if (!p.get_uint(16, m.end_sequence))
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, ResendRequest &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct Reject {
  std::uint64_t reference_sequence{};
  std::uint64_t reference_tag{};
  Text<> reference_type{};
  std::uint64_t reason{};
  Text<> text{};
};
inline std::size_t encode(const Reject &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('3', h);
  b.uint_field(45, m.reference_sequence);
  b.uint_field(371, m.reference_tag);
  b.field(372, m.reference_type.view());
  b.uint_field(373, m.reason);
  b.field(58, m.text.view());
  return b.finish();
}
template <std::size_t N> Status decode(const Parser<N> &p, Reject &m) noexcept {
  if (p.get(35) != "3")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(45))
    return Status::missing_field;
  {
    if (!p.get_uint(45, m.reference_sequence))
      return Status::invalid_field;
  }
  if (!p.has(371))
    return Status::missing_field;
  {
    if (!p.get_uint(371, m.reference_tag))
      return Status::invalid_field;
  }
  if (!p.has(372))
    return Status::missing_field;
  {
    m.reference_type.borrow(p.get(372));
    if (m.reference_type.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(373))
    return Status::missing_field;
  {
    if (!p.get_uint(373, m.reason))
      return Status::invalid_field;
  }
  if (!p.has(58))
    return Status::missing_field;
  {
    m.text.borrow(p.get(58));
    if (m.text.view().empty())
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, Reject &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct SequenceReset {
  char gap_fill{};
  std::uint64_t new_sequence{};
};
inline std::size_t encode(const SequenceReset &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('4', h);
  b.field(123, m.gap_fill);
  b.uint_field(36, m.new_sequence);
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, SequenceReset &m) noexcept {
  if (p.get(35) != "4")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(123))
    return Status::missing_field;
  {
    if (!p.get_char(123, m.gap_fill) ||
        std::string_view("YN").find(m.gap_fill) == std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(36))
    return Status::missing_field;
  {
    if (!p.get_uint(36, m.new_sequence))
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, SequenceReset &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct Logout {
  Text<> text{};
  bool has_text = false;
};
inline std::size_t encode(const Logout &m, const Header &h, char *buffer,
                          std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('5', h);
  if (m.has_text)
    b.field(58, m.text.view());
  return b.finish();
}
template <std::size_t N> Status decode(const Parser<N> &p, Logout &m) noexcept {
  if (p.get(35) != "5")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  m.has_text = p.has(58);
  if (m.has_text) {
    m.text.borrow(p.get(58));
    if (m.text.view().empty())
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, Logout &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct NewOrderSingle {
  bool post_only() const noexcept {
    return has_exec_inst &&
           exec_inst.view().find('6') != std::string_view::npos;
  }
  Text<> client_order_id{};
  Text<> account{};
  bool has_account = false;
  Text<> symbol{};
  Side side{};
  Quantity quantity{};
  OrdType order_type{};
  Price price{};
  bool has_price = false;
  TimeInForce time_in_force{};
  Text<> exec_inst{};
  bool has_exec_inst = false;
  Timestamp transact_time{};
  StpMode stp_mode{};
};
inline std::size_t encode(const NewOrderSingle &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('D', h);
  b.field(11, m.client_order_id.view());
  if (m.has_account)
    b.field(1, m.account.view());
  b.field(55, m.symbol.view());
  b.field(54, (m.side == Side::BUY ? '1' : '2'));
  b.uint_field(38, m.quantity);
  b.field(40, (m.order_type == OrdType::Market ? '1' : '2'));
  if (m.has_price)
    b.price_field(44, m.price);
  b.field(59, tif_char(m.time_in_force));
  if (m.has_exec_inst)
    b.field(18, m.exec_inst.view());
  b.timestamp_field(60, m.transact_time);
  b.field(7928, stp_char(m.stp_mode));
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, NewOrderSingle &m) noexcept {
  if (p.get(35) != "D")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  m.has_account = p.has(1);
  if (m.has_account) {
    m.account.borrow(p.get(1));
    if (m.account.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(55))
    return Status::missing_field;
  {
    m.symbol.borrow(p.get(55));
    if (m.symbol.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(54))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(54, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.side = Side::BUY;
      break;
    case '2':
      m.side = Side::SELL;
      break;
    default:
      return Status::invalid_field;
    }
  }
  if (!p.has(38))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(38, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.quantity = static_cast<Quantity>(value);
  }
  if (!p.has(40))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(40, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.order_type = OrdType::Market;
      break;
    case '2':
      m.order_type = OrdType::Limit;
      break;
    default:
      return Status::invalid_field;
    }
  }
  m.has_price = p.has(44);
  if (m.has_price) {
    if (!p.get_price(44, m.price))
      return Status::invalid_field;
  }
  if (!p.has(59))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(59, c))
      return Status::invalid_field;
    switch (c) {
    case '0':
      m.time_in_force = TimeInForce::Day;
      break;
    case '1':
      m.time_in_force = TimeInForce::GTC;
      break;
    case '3':
      m.time_in_force = TimeInForce::IOC;
      break;
    case '4':
      m.time_in_force = TimeInForce::FOK;
      break;
    default:
      return Status::invalid_field;
    }
  }
  m.has_exec_inst = p.has(18);
  if (m.has_exec_inst) {
    m.exec_inst.borrow(p.get(18));
    if (m.exec_inst.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(60))
    return Status::missing_field;
  {
    if (!p.get_timestamp(60, m.transact_time))
      return Status::invalid_field;
  }
  if (!p.has(7928))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(7928, c))
      return Status::invalid_field;
    switch (c) {
    case 'N':
      m.stp_mode = StpMode::None;
      break;
    case 'R':
      m.stp_mode = StpMode::CancelResting;
      break;
    case 'A':
      m.stp_mode = StpMode::CancelAggressor;
      break;
    case 'B':
      m.stp_mode = StpMode::CancelBoth;
      break;
    default:
      return Status::invalid_field;
    }
  }
  if (m.order_type == OrdType::Limit && !m.has_price)
    return Status::missing_field;
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, NewOrderSingle &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct OrderCancelRequest {
  Text<> original_client_order_id{};
  Text<> client_order_id{};
  Text<> symbol{};
  Side side{};
};
inline std::size_t encode(const OrderCancelRequest &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('F', h);
  b.field(41, m.original_client_order_id.view());
  b.field(11, m.client_order_id.view());
  b.field(55, m.symbol.view());
  b.field(54, (m.side == Side::BUY ? '1' : '2'));
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, OrderCancelRequest &m) noexcept {
  if (p.get(35) != "F")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(41))
    return Status::missing_field;
  {
    m.original_client_order_id.borrow(p.get(41));
    if (m.original_client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(55))
    return Status::missing_field;
  {
    m.symbol.borrow(p.get(55));
    if (m.symbol.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(54))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(54, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.side = Side::BUY;
      break;
    case '2':
      m.side = Side::SELL;
      break;
    default:
      return Status::invalid_field;
    }
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     OrderCancelRequest &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct OrderCancelReplaceRequest {
  Text<> original_client_order_id{};
  Text<> client_order_id{};
  Text<> symbol{};
  Side side{};
  Quantity quantity{};
  OrdType order_type{};
  Price price{};
  bool has_price = false;
};
inline std::size_t encode(const OrderCancelReplaceRequest &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('G', h);
  b.field(41, m.original_client_order_id.view());
  b.field(11, m.client_order_id.view());
  b.field(55, m.symbol.view());
  b.field(54, (m.side == Side::BUY ? '1' : '2'));
  b.uint_field(38, m.quantity);
  b.field(40, (m.order_type == OrdType::Market ? '1' : '2'));
  if (m.has_price)
    b.price_field(44, m.price);
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, OrderCancelReplaceRequest &m) noexcept {
  if (p.get(35) != "G")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(41))
    return Status::missing_field;
  {
    m.original_client_order_id.borrow(p.get(41));
    if (m.original_client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(55))
    return Status::missing_field;
  {
    m.symbol.borrow(p.get(55));
    if (m.symbol.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(54))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(54, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.side = Side::BUY;
      break;
    case '2':
      m.side = Side::SELL;
      break;
    default:
      return Status::invalid_field;
    }
  }
  if (!p.has(38))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(38, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.quantity = static_cast<Quantity>(value);
  }
  if (!p.has(40))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(40, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.order_type = OrdType::Market;
      break;
    case '2':
      m.order_type = OrdType::Limit;
      break;
    default:
      return Status::invalid_field;
    }
  }
  m.has_price = p.has(44);
  if (m.has_price) {
    if (!p.get_price(44, m.price))
      return Status::invalid_field;
  }
  if (m.order_type == OrdType::Limit && !m.has_price)
    return Status::missing_field;
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     OrderCancelReplaceRequest &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct OrderMassCancelRequest {
  Text<> client_order_id{};
  char request_type{};
  Text<> symbol{};
  bool has_symbol = false;
};
inline std::size_t encode(const OrderMassCancelRequest &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('q', h);
  b.field(11, m.client_order_id.view());
  b.field(530, m.request_type);
  if (m.has_symbol)
    b.field(55, m.symbol.view());
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, OrderMassCancelRequest &m) noexcept {
  if (p.get(35) != "q")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(530))
    return Status::missing_field;
  {
    if (!p.get_char(530, m.request_type) ||
        std::string_view("17").find(m.request_type) == std::string_view::npos)
      return Status::invalid_field;
  }
  m.has_symbol = p.has(55);
  if (m.has_symbol) {
    m.symbol.borrow(p.get(55));
    if (m.symbol.view().empty())
      return Status::invalid_field;
  }
  if (m.request_type != '1' && m.request_type != '7')
    return Status::invalid_field;
  if (m.request_type == '1' && !m.has_symbol)
    return Status::missing_field;
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     OrderMassCancelRequest &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct ExecutionReport {
  Text<> order_id{};
  Text<> client_order_id{};
  Text<> original_client_order_id{};
  bool has_original_client_order_id = false;
  Text<> execution_id{};
  char execution_type{};
  char order_status{};
  Text<> symbol{};
  Side side{};
  Quantity quantity{};
  Price price{};
  Quantity last_quantity{};
  Price last_price{};
  Quantity leaves_quantity{};
  Quantity cumulative_quantity{};
  Price average_price{};
  std::uint64_t reject_reason{};
  bool has_reject_reason = false;
  Text<> text{};
  bool has_text = false;
  Timestamp transact_time{};
};
inline std::size_t encode(const ExecutionReport &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('8', h);
  b.field(37, m.order_id.view());
  b.field(11, m.client_order_id.view());
  if (m.has_original_client_order_id)
    b.field(41, m.original_client_order_id.view());
  b.field(17, m.execution_id.view());
  b.field(150, m.execution_type);
  b.field(39, m.order_status);
  b.field(55, m.symbol.view());
  b.field(54, (m.side == Side::BUY ? '1' : '2'));
  b.uint_field(38, m.quantity);
  b.price_field(44, m.price);
  b.uint_field(32, m.last_quantity);
  b.price_field(31, m.last_price);
  b.uint_field(151, m.leaves_quantity);
  b.uint_field(14, m.cumulative_quantity);
  b.price_field(6, m.average_price);
  if (m.has_reject_reason)
    b.uint_field(103, m.reject_reason);
  if (m.has_text)
    b.field(58, m.text.view());
  b.timestamp_field(60, m.transact_time);
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, ExecutionReport &m) noexcept {
  if (p.get(35) != "8")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(37))
    return Status::missing_field;
  {
    m.order_id.borrow(p.get(37));
    if (m.order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  m.has_original_client_order_id = p.has(41);
  if (m.has_original_client_order_id) {
    m.original_client_order_id.borrow(p.get(41));
    if (m.original_client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(17))
    return Status::missing_field;
  {
    m.execution_id.borrow(p.get(17));
    if (m.execution_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(150))
    return Status::missing_field;
  {
    if (!p.get_char(150, m.execution_type) ||
        std::string_view("0458F").find(m.execution_type) ==
            std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(39))
    return Status::missing_field;
  {
    if (!p.get_char(39, m.order_status) ||
        std::string_view("01248").find(m.order_status) ==
            std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(55))
    return Status::missing_field;
  {
    m.symbol.borrow(p.get(55));
    if (m.symbol.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(54))
    return Status::missing_field;
  {
    char c;
    if (!p.get_char(54, c))
      return Status::invalid_field;
    switch (c) {
    case '1':
      m.side = Side::BUY;
      break;
    case '2':
      m.side = Side::SELL;
      break;
    default:
      return Status::invalid_field;
    }
  }
  if (!p.has(38))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(38, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.quantity = static_cast<Quantity>(value);
  }
  if (!p.has(44))
    return Status::missing_field;
  {
    if (!p.get_price(44, m.price))
      return Status::invalid_field;
  }
  if (!p.has(32))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(32, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.last_quantity = static_cast<Quantity>(value);
  }
  if (!p.has(31))
    return Status::missing_field;
  {
    if (!p.get_price(31, m.last_price))
      return Status::invalid_field;
  }
  if (!p.has(151))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(151, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.leaves_quantity = static_cast<Quantity>(value);
  }
  if (!p.has(14))
    return Status::missing_field;
  {
    std::uint64_t value;
    if (!p.get_uint(14, value) || value > UINT32_MAX)
      return Status::invalid_field;
    m.cumulative_quantity = static_cast<Quantity>(value);
  }
  if (!p.has(6))
    return Status::missing_field;
  {
    if (!p.get_price(6, m.average_price))
      return Status::invalid_field;
  }
  m.has_reject_reason = p.has(103);
  if (m.has_reject_reason) {
    if (!p.get_uint(103, m.reject_reason))
      return Status::invalid_field;
  }
  m.has_text = p.has(58);
  if (m.has_text) {
    m.text.borrow(p.get(58));
    if (m.text.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(60))
    return Status::missing_field;
  {
    if (!p.get_timestamp(60, m.transact_time))
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     ExecutionReport &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct OrderCancelReject {
  Text<> order_id{};
  Text<> client_order_id{};
  Text<> original_client_order_id{};
  char order_status{};
  char response_to{};
  std::uint64_t reason{};
  Text<> text{};
  bool has_text = false;
};
inline std::size_t encode(const OrderCancelReject &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('9', h);
  b.field(37, m.order_id.view());
  b.field(11, m.client_order_id.view());
  b.field(41, m.original_client_order_id.view());
  b.field(39, m.order_status);
  b.field(434, m.response_to);
  b.uint_field(102, m.reason);
  if (m.has_text)
    b.field(58, m.text.view());
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, OrderCancelReject &m) noexcept {
  if (p.get(35) != "9")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(37))
    return Status::missing_field;
  {
    m.order_id.borrow(p.get(37));
    if (m.order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(41))
    return Status::missing_field;
  {
    m.original_client_order_id.borrow(p.get(41));
    if (m.original_client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(39))
    return Status::missing_field;
  {
    if (!p.get_char(39, m.order_status) ||
        std::string_view("01248").find(m.order_status) ==
            std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(434))
    return Status::missing_field;
  {
    if (!p.get_char(434, m.response_to) ||
        std::string_view("12").find(m.response_to) == std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(102))
    return Status::missing_field;
  {
    if (!p.get_uint(102, m.reason))
      return Status::invalid_field;
  }
  m.has_text = p.has(58);
  if (m.has_text) {
    m.text.borrow(p.get(58));
    if (m.text.view().empty())
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     OrderCancelReject &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
struct OrderMassCancelReport {
  Text<> client_order_id{};
  Text<> order_id{};
  char request_type{};
  char response{};
  std::uint64_t total_affected_orders{};
};
inline std::size_t encode(const OrderMassCancelReport &m, const Header &h,
                          char *buffer, std::size_t capacity) noexcept {
  Builder b({buffer, capacity});
  b.header('r', h);
  b.field(11, m.client_order_id.view());
  b.field(37, m.order_id.view());
  b.field(530, m.request_type);
  b.field(531, m.response);
  b.uint_field(533, m.total_affected_orders);
  return b.finish();
}
template <std::size_t N>
Status decode(const Parser<N> &p, OrderMassCancelReport &m) noexcept {
  if (p.get(35) != "r")
    return Status::invalid_type;
  if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52))
    return Status::missing_field;
  std::uint64_t header_value;
  if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value))
    return Status::invalid_field;
  if (!p.has(11))
    return Status::missing_field;
  {
    m.client_order_id.borrow(p.get(11));
    if (m.client_order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(37))
    return Status::missing_field;
  {
    m.order_id.borrow(p.get(37));
    if (m.order_id.view().empty())
      return Status::invalid_field;
  }
  if (!p.has(530))
    return Status::missing_field;
  {
    if (!p.get_char(530, m.request_type) ||
        std::string_view("17").find(m.request_type) == std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(531))
    return Status::missing_field;
  {
    if (!p.get_char(531, m.response) ||
        std::string_view("01234567").find(m.response) == std::string_view::npos)
      return Status::invalid_field;
  }
  if (!p.has(533))
    return Status::missing_field;
  {
    if (!p.get_uint(533, m.total_affected_orders))
      return Status::invalid_field;
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n,
                     OrderMassCancelReport &m) noexcept {
  Parser<> parser;
  auto s = parser.parse(p, n);
  return s == Status::ok ? decode(parser, m) : s;
}
} // namespace exsim::protocol::fix44
#endif
