#ifndef EXSIM_PROTOCOL_OUCH42_HPP
#define EXSIM_PROTOCOL_OUCH42_HPP
// Layout reference: Nasdaq OUCH 4.2 (October 2025), sections 2 and 3.
// https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf
// Typed overloads disambiguate inbound and outbound U/M messages. Tokens are
// fixed alpha fields; use wire::alpha(token.data(), token.size(), text) to set
// them. Unknown extensible alpha codes are preserved; business validation
// belongs to the gateway. Prices use exsim::Price, checked against the 32-bit
// wire range.
#include "wire.hpp"
namespace exsim::protocol::ouch42 {
using protocol::Status;
struct EnterOrder {
  static constexpr char type = 'O';
  static constexpr std::size_t wire_size = 49;
  std::array<char, 14> token = wire::spaces<14>();
  char side = 'B';
  Quantity shares{};
  Symbol stock{};
  Price price{};
  std::uint32_t time_in_force{};
  std::array<char, 4> firm = wire::spaces<4>();
  char display{};
  char capacity{};
  char ise{};
  Quantity minimum_quantity{};
  char cross_type{};
  char customer_type{};
};
inline std::size_t encode(const EnterOrder &m, char *p,
                          std::size_t n) noexcept {
  if (n < 49 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'O';
  for (std::size_t i = 0; i < 14; ++i)
    p[1 + i] = m.token[i];
  p[15] = m.side;
  wire::store<4>(p + 16, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[20 + i] = m.stock.chars[i];
  wire::store<4>(p + 28, static_cast<std::uint64_t>(m.price));
  wire::store<4>(p + 32, static_cast<std::uint64_t>(m.time_in_force));
  for (std::size_t i = 0; i < 4; ++i)
    p[36 + i] = m.firm[i];
  p[40] = m.display;
  p[41] = m.capacity;
  p[42] = m.ise;
  wire::store<4>(p + 43, static_cast<std::uint64_t>(m.minimum_quantity));
  p[47] = m.cross_type;
  p[48] = m.customer_type;
  return 49;
}
inline Status decode(const char *p, std::size_t n, EnterOrder &m) noexcept {
  if (n < 49)
    return Status::incomplete;
  if (n != 49)
    return Status::invalid_length;
  if (p[0] != 'O')
    return Status::invalid_type;
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[1 + i];
  if (std::string_view("BSTE").find(p[15]) == std::string_view::npos)
    return Status::invalid_field;
  m.side = p[15];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 16));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[20 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 28));
  m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 32));
  for (std::size_t i = 0; i < 4; ++i)
    m.firm[i] = p[36 + i];
  m.display = p[40];
  m.capacity = p[41];
  m.ise = p[42];
  m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 43));
  m.cross_type = p[47];
  m.customer_type = p[48];
  return Status::ok;
}
inline Status decode(std::span<const char> p, EnterOrder &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct ReplaceOrder {
  static constexpr char type = 'U';
  static constexpr std::size_t wire_size = 47;
  std::array<char, 14> existing_token = wire::spaces<14>();
  std::array<char, 14> replacement_token = wire::spaces<14>();
  Quantity shares{};
  Price price{};
  std::uint32_t time_in_force{};
  char display{};
  char ise{};
  Quantity minimum_quantity{};
};
inline std::size_t encode(const ReplaceOrder &m, char *p,
                          std::size_t n) noexcept {
  if (n < 47 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'U';
  for (std::size_t i = 0; i < 14; ++i)
    p[1 + i] = m.existing_token[i];
  for (std::size_t i = 0; i < 14; ++i)
    p[15 + i] = m.replacement_token[i];
  wire::store<4>(p + 29, static_cast<std::uint64_t>(m.shares));
  wire::store<4>(p + 33, static_cast<std::uint64_t>(m.price));
  wire::store<4>(p + 37, static_cast<std::uint64_t>(m.time_in_force));
  p[41] = m.display;
  p[42] = m.ise;
  wire::store<4>(p + 43, static_cast<std::uint64_t>(m.minimum_quantity));
  return 47;
}
inline Status decode(const char *p, std::size_t n, ReplaceOrder &m) noexcept {
  if (n < 47)
    return Status::incomplete;
  if (n != 47)
    return Status::invalid_length;
  if (p[0] != 'U')
    return Status::invalid_type;
  for (std::size_t i = 0; i < 14; ++i)
    m.existing_token[i] = p[1 + i];
  for (std::size_t i = 0; i < 14; ++i)
    m.replacement_token[i] = p[15 + i];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 29));
  m.price = static_cast<Price>(wire::load<4>(p + 33));
  m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 37));
  m.display = p[41];
  m.ise = p[42];
  m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 43));
  return Status::ok;
}
inline Status decode(std::span<const char> p, ReplaceOrder &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct CancelOrder {
  static constexpr char type = 'X';
  static constexpr std::size_t wire_size = 19;
  std::array<char, 14> token = wire::spaces<14>();
  Quantity shares{};
};
inline std::size_t encode(const CancelOrder &m, char *p,
                          std::size_t n) noexcept {
  if (n < 19)
    return 0;
  p[0] = 'X';
  for (std::size_t i = 0; i < 14; ++i)
    p[1 + i] = m.token[i];
  wire::store<4>(p + 15, static_cast<std::uint64_t>(m.shares));
  return 19;
}
inline Status decode(const char *p, std::size_t n, CancelOrder &m) noexcept {
  if (n < 19)
    return Status::incomplete;
  if (n != 19)
    return Status::invalid_length;
  if (p[0] != 'X')
    return Status::invalid_type;
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[1 + i];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 15));
  return Status::ok;
}
inline Status decode(std::span<const char> p, CancelOrder &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct ModifyOrder {
  static constexpr char type = 'M';
  static constexpr std::size_t wire_size = 20;
  std::array<char, 14> token = wire::spaces<14>();
  char side = 'B';
  Quantity shares{};
};
inline std::size_t encode(const ModifyOrder &m, char *p,
                          std::size_t n) noexcept {
  if (n < 20)
    return 0;
  p[0] = 'M';
  for (std::size_t i = 0; i < 14; ++i)
    p[1 + i] = m.token[i];
  p[15] = m.side;
  wire::store<4>(p + 16, static_cast<std::uint64_t>(m.shares));
  return 20;
}
inline Status decode(const char *p, std::size_t n, ModifyOrder &m) noexcept {
  if (n < 20)
    return Status::incomplete;
  if (n != 20)
    return Status::invalid_length;
  if (p[0] != 'M')
    return Status::invalid_type;
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[1 + i];
  if (std::string_view("BSTE").find(p[15]) == std::string_view::npos)
    return Status::invalid_field;
  m.side = p[15];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 16));
  return Status::ok;
}
inline Status decode(std::span<const char> p, ModifyOrder &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct SystemEvent {
  static constexpr char type = 'S';
  static constexpr std::size_t wire_size = 10;
  std::uint64_t timestamp{};
  char event = 'S';
};
inline std::size_t encode(const SystemEvent &m, char *p,
                          std::size_t n) noexcept {
  if (n < 10)
    return 0;
  p[0] = 'S';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  p[9] = m.event;
  return 10;
}
inline Status decode(const char *p, std::size_t n, SystemEvent &m) noexcept {
  if (n < 10)
    return Status::incomplete;
  if (n != 10)
    return Status::invalid_length;
  if (p[0] != 'S')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  if (std::string_view("SE").find(p[9]) == std::string_view::npos)
    return Status::invalid_field;
  m.event = p[9];
  return Status::ok;
}
inline Status decode(std::span<const char> p, SystemEvent &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderAccepted {
  static constexpr char type = 'A';
  static constexpr std::size_t wire_size = 66;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
  char side = 'B';
  Quantity shares{};
  Symbol stock{};
  Price price{};
  std::uint32_t time_in_force{};
  std::array<char, 4> firm = wire::spaces<4>();
  char display{};
  std::uint64_t order_reference{};
  char capacity{};
  char ise{};
  Quantity minimum_quantity{};
  char cross_type{};
  char order_state = 'L';
  char bbo_weight{};
};
inline std::size_t encode(const OrderAccepted &m, char *p,
                          std::size_t n) noexcept {
  if (n < 66 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'A';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  p[23] = m.side;
  wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[28 + i] = m.stock.chars[i];
  wire::store<4>(p + 36, static_cast<std::uint64_t>(m.price));
  wire::store<4>(p + 40, static_cast<std::uint64_t>(m.time_in_force));
  for (std::size_t i = 0; i < 4; ++i)
    p[44 + i] = m.firm[i];
  p[48] = m.display;
  wire::store<8>(p + 49, static_cast<std::uint64_t>(m.order_reference));
  p[57] = m.capacity;
  p[58] = m.ise;
  wire::store<4>(p + 59, static_cast<std::uint64_t>(m.minimum_quantity));
  p[63] = m.cross_type;
  p[64] = m.order_state;
  p[65] = m.bbo_weight;
  return 66;
}
inline Status decode(const char *p, std::size_t n, OrderAccepted &m) noexcept {
  if (n < 66)
    return Status::incomplete;
  if (n != 66)
    return Status::invalid_length;
  if (p[0] != 'A')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  if (std::string_view("BSTE").find(p[23]) == std::string_view::npos)
    return Status::invalid_field;
  m.side = p[23];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 24));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[28 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 36));
  m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 40));
  for (std::size_t i = 0; i < 4; ++i)
    m.firm[i] = p[44 + i];
  m.display = p[48];
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 49));
  m.capacity = p[57];
  m.ise = p[58];
  m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 59));
  m.cross_type = p[63];
  if (std::string_view("LD").find(p[64]) == std::string_view::npos)
    return Status::invalid_field;
  m.order_state = p[64];
  m.bbo_weight = p[65];
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderAccepted &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderCanceled {
  static constexpr char type = 'C';
  static constexpr std::size_t wire_size = 28;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
  Quantity shares{};
  char reason{};
};
inline std::size_t encode(const OrderCanceled &m, char *p,
                          std::size_t n) noexcept {
  if (n < 28)
    return 0;
  p[0] = 'C';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  wire::store<4>(p + 23, static_cast<std::uint64_t>(m.shares));
  p[27] = m.reason;
  return 28;
}
inline Status decode(const char *p, std::size_t n, OrderCanceled &m) noexcept {
  if (n < 28)
    return Status::incomplete;
  if (n != 28)
    return Status::invalid_length;
  if (p[0] != 'C')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 23));
  m.reason = p[27];
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderCanceled &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderExecuted {
  static constexpr char type = 'E';
  static constexpr std::size_t wire_size = 40;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
  Quantity shares{};
  Price price{};
  char liquidity{};
  std::uint64_t match_number{};
};
inline std::size_t encode(const OrderExecuted &m, char *p,
                          std::size_t n) noexcept {
  if (n < 40 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'E';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  wire::store<4>(p + 23, static_cast<std::uint64_t>(m.shares));
  wire::store<4>(p + 27, static_cast<std::uint64_t>(m.price));
  p[31] = m.liquidity;
  wire::store<8>(p + 32, static_cast<std::uint64_t>(m.match_number));
  return 40;
}
inline Status decode(const char *p, std::size_t n, OrderExecuted &m) noexcept {
  if (n < 40)
    return Status::incomplete;
  if (n != 40)
    return Status::invalid_length;
  if (p[0] != 'E')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 23));
  m.price = static_cast<Price>(wire::load<4>(p + 27));
  m.liquidity = p[31];
  m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 32));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderExecuted &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderRejected {
  static constexpr char type = 'J';
  static constexpr std::size_t wire_size = 24;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
  char reason{};
};
inline std::size_t encode(const OrderRejected &m, char *p,
                          std::size_t n) noexcept {
  if (n < 24)
    return 0;
  p[0] = 'J';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  p[23] = m.reason;
  return 24;
}
inline Status decode(const char *p, std::size_t n, OrderRejected &m) noexcept {
  if (n < 24)
    return Status::incomplete;
  if (n != 24)
    return Status::invalid_length;
  if (p[0] != 'J')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  m.reason = p[23];
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderRejected &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct CancelPending {
  static constexpr char type = 'P';
  static constexpr std::size_t wire_size = 23;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
};
inline std::size_t encode(const CancelPending &m, char *p,
                          std::size_t n) noexcept {
  if (n < 23)
    return 0;
  p[0] = 'P';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  return 23;
}
inline Status decode(const char *p, std::size_t n, CancelPending &m) noexcept {
  if (n < 23)
    return Status::incomplete;
  if (n != 23)
    return Status::invalid_length;
  if (p[0] != 'P')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  return Status::ok;
}
inline Status decode(std::span<const char> p, CancelPending &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct CancelReject {
  static constexpr char type = 'I';
  static constexpr std::size_t wire_size = 23;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
};
inline std::size_t encode(const CancelReject &m, char *p,
                          std::size_t n) noexcept {
  if (n < 23)
    return 0;
  p[0] = 'I';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  return 23;
}
inline Status decode(const char *p, std::size_t n, CancelReject &m) noexcept {
  if (n < 23)
    return Status::incomplete;
  if (n != 23)
    return Status::invalid_length;
  if (p[0] != 'I')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  return Status::ok;
}
inline Status decode(std::span<const char> p, CancelReject &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderModified {
  static constexpr char type = 'M';
  static constexpr std::size_t wire_size = 28;
  std::uint64_t timestamp{};
  std::array<char, 14> token = wire::spaces<14>();
  char side = 'B';
  Quantity shares{};
};
inline std::size_t encode(const OrderModified &m, char *p,
                          std::size_t n) noexcept {
  if (n < 28)
    return 0;
  p[0] = 'M';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.token[i];
  p[23] = m.side;
  wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
  return 28;
}
inline Status decode(const char *p, std::size_t n, OrderModified &m) noexcept {
  if (n < 28)
    return Status::incomplete;
  if (n != 28)
    return Status::invalid_length;
  if (p[0] != 'M')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.token[i] = p[9 + i];
  if (std::string_view("BSTE").find(p[23]) == std::string_view::npos)
    return Status::invalid_field;
  m.side = p[23];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 24));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderModified &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderReplaced {
  static constexpr char type = 'U';
  static constexpr std::size_t wire_size = 80;
  std::uint64_t timestamp{};
  std::array<char, 14> replacement_token = wire::spaces<14>();
  char side = 'B';
  Quantity shares{};
  Symbol stock{};
  Price price{};
  std::uint32_t time_in_force{};
  std::array<char, 4> firm = wire::spaces<4>();
  char display{};
  std::uint64_t order_reference{};
  char capacity{};
  char ise{};
  Quantity minimum_quantity{};
  char cross_type{};
  char order_state = 'L';
  std::array<char, 14> previous_token = wire::spaces<14>();
  char bbo_weight{};
};
inline std::size_t encode(const OrderReplaced &m, char *p,
                          std::size_t n) noexcept {
  if (n < 80 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'U';
  wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 14; ++i)
    p[9 + i] = m.replacement_token[i];
  p[23] = m.side;
  wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[28 + i] = m.stock.chars[i];
  wire::store<4>(p + 36, static_cast<std::uint64_t>(m.price));
  wire::store<4>(p + 40, static_cast<std::uint64_t>(m.time_in_force));
  for (std::size_t i = 0; i < 4; ++i)
    p[44 + i] = m.firm[i];
  p[48] = m.display;
  wire::store<8>(p + 49, static_cast<std::uint64_t>(m.order_reference));
  p[57] = m.capacity;
  p[58] = m.ise;
  wire::store<4>(p + 59, static_cast<std::uint64_t>(m.minimum_quantity));
  p[63] = m.cross_type;
  p[64] = m.order_state;
  for (std::size_t i = 0; i < 14; ++i)
    p[65 + i] = m.previous_token[i];
  p[79] = m.bbo_weight;
  return 80;
}
inline Status decode(const char *p, std::size_t n, OrderReplaced &m) noexcept {
  if (n < 80)
    return Status::incomplete;
  if (n != 80)
    return Status::invalid_length;
  if (p[0] != 'U')
    return Status::invalid_type;
  m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
  for (std::size_t i = 0; i < 14; ++i)
    m.replacement_token[i] = p[9 + i];
  if (std::string_view("BSTE").find(p[23]) == std::string_view::npos)
    return Status::invalid_field;
  m.side = p[23];
  m.shares = static_cast<Quantity>(wire::load<4>(p + 24));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[28 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 36));
  m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 40));
  for (std::size_t i = 0; i < 4; ++i)
    m.firm[i] = p[44 + i];
  m.display = p[48];
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 49));
  m.capacity = p[57];
  m.ise = p[58];
  m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 59));
  m.cross_type = p[63];
  if (std::string_view("LD").find(p[64]) == std::string_view::npos)
    return Status::invalid_field;
  m.order_state = p[64];
  for (std::size_t i = 0; i < 14; ++i)
    m.previous_token[i] = p[65 + i];
  m.bbo_weight = p[79];
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderReplaced &m) noexcept {
  return decode(p.data(), p.size(), m);
}
} // namespace exsim::protocol::ouch42
#endif
