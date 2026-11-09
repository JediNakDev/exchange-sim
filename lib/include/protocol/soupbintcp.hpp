#ifndef EXSIM_PROTOCOL_SOUPBINTCP_HPP
#define EXSIM_PROTOCOL_SOUPBINTCP_HPP
// Length includes the type byte, excludes the two-byte length itself. Data
// payload views borrow the input; the session layer supplies sequence state.
#include "wire.hpp"
#include <algorithm>
namespace exsim::protocol::soupbintcp {
using protocol::Status;
struct Packet {
  char type{};
  std::string_view payload{};
};
inline bool known(char t) noexcept {
  return std::string_view("LUROAJSHZ+").find(t) != std::string_view::npos &&
         t != ' ';
}
inline Status decode(const char *p, std::size_t n, Packet &out) noexcept {
  if (n < 2)
    return Status::incomplete;
  const auto len = wire::load16(p);
  if (!len)
    return Status::invalid_length;
  if (n < std::size_t(len) + 2)
    return Status::incomplete;
  if (n != std::size_t(len) + 2)
    return Status::invalid_length;
  if (!known(p[2]))
    return Status::invalid_type;
  const std::size_t expected =
      p[2] == 'L'                                                     ? 47
      : p[2] == 'A'                                                   ? 31
      : p[2] == 'J'                                                   ? 2
      : std::string_view("ROHZ").find(p[2]) != std::string_view::npos ? 1
                                                                      : len;
  if (len != expected)
    return Status::invalid_length;
  if (p[2] == 'J' && p[3] != 'A' && p[3] != 'S')
    return Status::invalid_field;
  out = {p[2], {p + 3, std::size_t(len) - 1}};
  return Status::ok;
}
inline Status decode(std::span<const char> s, Packet &out) noexcept {
  return decode(s.data(), s.size(), out);
}
inline std::size_t encode(Packet m, char *p, std::size_t n) noexcept {
  if (m.payload.size() > 65534 || n < m.payload.size() + 3 || !known(m.type))
    return 0;
  wire::store16(p, static_cast<std::uint16_t>(m.payload.size() + 1));
  p[2] = m.type;
  for (std::size_t i = 0; i < m.payload.size(); ++i)
    p[i + 3] = m.payload[i];
  Packet check;
  return decode(p, m.payload.size() + 3, check) == Status::ok
             ? m.payload.size() + 3
             : 0;
}
struct LoginRequest {
  std::array<char, 6> username = wire::spaces<6>();
  std::array<char, 10> password = wire::spaces<10>(),
                       session = wire::spaces<10>();
  std::uint64_t sequence{};
};
struct LoginAccepted {
  std::array<char, 10> session = wire::spaces<10>();
  std::uint64_t sequence{};
};
inline std::size_t encode(const LoginRequest &m, char *p,
                          std::size_t n) noexcept {
  if (n < 49)
    return 0;
  wire::store16(p, 47);
  p[2] = 'L';
  for (unsigned i = 0; i < 6; ++i)
    p[3 + i] = m.username[i];
  for (unsigned i = 0; i < 10; ++i) {
    p[9 + i] = m.password[i];
    p[19 + i] = m.session[i];
  }
  wire::numeric(p + 29, 20, m.sequence);
  return 49;
}
inline std::size_t encode(const LoginAccepted &m, char *p,
                          std::size_t n) noexcept {
  if (n < 33)
    return 0;
  wire::store16(p, 31);
  p[2] = 'A';
  for (unsigned i = 0; i < 10; ++i)
    p[3 + i] = m.session[i];
  wire::numeric(p + 13, 20, m.sequence);
  return 33;
}
inline Status decode(const char *p, std::size_t n, LoginRequest &m) noexcept {
  Packet pkt;
  auto s = decode(p, n, pkt);
  if (s != Status::ok)
    return s;
  if (pkt.type != 'L')
    return Status::invalid_type;
  if (!wire::numeric({p + 29, 20}, m.sequence))
    return Status::invalid_field;
  for (unsigned i = 0; i < 6; ++i)
    m.username[i] = p[3 + i];
  for (unsigned i = 0; i < 10; ++i) {
    m.password[i] = p[9 + i];
    m.session[i] = p[19 + i];
  }
  return Status::ok;
}
inline Status decode(const char *p, std::size_t n, LoginAccepted &m) noexcept {
  Packet pkt;
  auto s = decode(p, n, pkt);
  if (s != Status::ok)
    return s;
  if (pkt.type != 'A')
    return Status::invalid_type;
  if (!wire::numeric({p + 13, 20}, m.sequence))
    return Status::invalid_field;
  for (unsigned i = 0; i < 10; ++i)
    m.session[i] = p[3 + i];
  return Status::ok;
}
// feed consumes only through the next complete frame. A returned payload is
// valid until the next feed/reset. Call again with the unconsumed input suffix.
class FrameReader {
  std::span<char> buffer_;
  std::size_t used_ = 0, max_;
  bool ready_ = false;

public:
  explicit FrameReader(std::span<char> buffer,
                       std::size_t max_length = 65535) noexcept
      : buffer_(buffer), max_(max_length) {}
  void reset() noexcept {
    used_ = 0;
    ready_ = false;
  }
  Status feed(std::span<const char> input, std::size_t &consumed,
              Packet &packet) noexcept {
    consumed = 0;
    if (ready_)
      reset();
    while (true) {
      std::size_t target = 2;
      if (used_ >= 2) {
        auto n = wire::load16(buffer_.data());
        if (!n)
          return Status::invalid_length;
        if (n > max_)
          return Status::overflow;
        target = std::size_t(n) + 2;
      }
      if (target > buffer_.size())
        return Status::full;
      if (used_ == target && target > 2) {
        ready_ = true;
        return decode(buffer_.data(), used_, packet);
      }
      if (consumed == input.size())
        return Status::incomplete;
      auto count = std::min(target - used_, input.size() - consumed);
      for (std::size_t i = 0; i < count; ++i)
        buffer_[used_++] = input[consumed++];
    }
  }
};
// Typed wrappers for payload and control packet kinds.
template <char Type> struct DataPacket {
  std::string_view payload{};
};
using UnsequencedData = DataPacket<'U'>;
using SequencedData = DataPacket<'S'>;
using Debug = DataPacket<'+'>;
template <char Type> struct ControlPacket {};
using ClientHeartbeat = ControlPacket<'R'>;
using ServerHeartbeat = ControlPacket<'H'>;
using LogoutRequest = ControlPacket<'O'>;
using EndOfSession = ControlPacket<'Z'>;
struct LoginRejected {
  char reason = 'A';
};
template <char T>
std::size_t encode(DataPacket<T> m, char *p, std::size_t n) noexcept {
  return encode(Packet{T, m.payload}, p, n);
}
template <char T>
std::size_t encode(ControlPacket<T>, char *p, std::size_t n) noexcept {
  return encode(Packet{T, {}}, p, n);
}
inline std::size_t encode(LoginRejected m, char *p, std::size_t n) noexcept {
  return encode(Packet{'J', {&m.reason, 1}}, p, n);
}
template <char T>
Status decode(const char *p, std::size_t n, DataPacket<T> &m) noexcept {
  Packet packet;
  auto status = decode(p, n, packet);
  if (status != Status::ok)
    return status;
  if (packet.type != T)
    return Status::invalid_type;
  m.payload = packet.payload;
  return Status::ok;
}
template <char T>
Status decode(const char *p, std::size_t n, ControlPacket<T> &) noexcept {
  Packet packet;
  auto status = decode(p, n, packet);
  return status != Status::ok ? status
         : packet.type == T   ? Status::ok
                              : Status::invalid_type;
}
inline Status decode(const char *p, std::size_t n, LoginRejected &m) noexcept {
  Packet packet;
  auto status = decode(p, n, packet);
  if (status != Status::ok)
    return status;
  if (packet.type != 'J')
    return Status::invalid_type;
  m.reason = packet.payload[0];
  return Status::ok;
}
} // namespace exsim::protocol::soupbintcp
#endif
