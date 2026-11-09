#ifndef EXSIM_PROTOCOL_ITCH50_HPP
#define EXSIM_PROTOCOL_ITCH50_HPP
// Layout reference: Nasdaq TotalView-ITCH 5.0, section 4.
// https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf
// The caller selects the message struct using the wire type byte. Decoders
// require exactly one message; unknown extensible alpha values are preserved.
#include "wire.hpp"
namespace exsim::protocol::itch50 {
using protocol::Status;
struct SystemEvent {
  static constexpr char type = 'S';
  static constexpr std::size_t wire_size = 12;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  char event = 'O';
};
inline std::size_t encode(const SystemEvent &m, char *p,
                          std::size_t n) noexcept {
  if (n < 12 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'S';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  p[11] = m.event;
  return 12;
}
inline Status decode(const char *p, std::size_t n, SystemEvent &m) noexcept {
  if (n < 12)
    return Status::incomplete;
  if (n != 12)
    return Status::invalid_length;
  if (p[0] != 'S')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  if (std::string_view("OSQMEC").find(p[11]) == std::string_view::npos)
    return Status::invalid_field;
  m.event = p[11];
  return Status::ok;
}
inline Status decode(std::span<const char> p, SystemEvent &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct StockDirectory {
  static constexpr char type = 'R';
  static constexpr std::size_t wire_size = 39;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  Symbol stock{};
  char market_category{};
  char financial_status{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  std::array<char, 2> issue_subtype = wire::spaces<2>();
  char authenticity{};
  char short_sale_threshold{};
  char ipo_flag{};
  char luld_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage{};
  char inverse_indicator{};
};
inline std::size_t encode(const StockDirectory &m, char *p,
                          std::size_t n) noexcept {
  if (n < 39 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'R';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 8; ++i)
    p[11 + i] = m.stock.chars[i];
  p[19] = m.market_category;
  p[20] = m.financial_status;
  wire::store<4>(p + 21, static_cast<std::uint64_t>(m.round_lot_size));
  p[25] = m.round_lots_only;
  p[26] = m.issue_classification;
  for (std::size_t i = 0; i < 2; ++i)
    p[27 + i] = m.issue_subtype[i];
  p[29] = m.authenticity;
  p[30] = m.short_sale_threshold;
  p[31] = m.ipo_flag;
  p[32] = m.luld_tier;
  p[33] = m.etp_flag;
  wire::store<4>(p + 34, static_cast<std::uint64_t>(m.etp_leverage));
  p[38] = m.inverse_indicator;
  return 39;
}
inline Status decode(const char *p, std::size_t n, StockDirectory &m) noexcept {
  if (n < 39)
    return Status::incomplete;
  if (n != 39)
    return Status::invalid_length;
  if (p[0] != 'R')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[11 + i];
  m.market_category = p[19];
  m.financial_status = p[20];
  m.round_lot_size = static_cast<std::uint32_t>(wire::load<4>(p + 21));
  m.round_lots_only = p[25];
  m.issue_classification = p[26];
  for (std::size_t i = 0; i < 2; ++i)
    m.issue_subtype[i] = p[27 + i];
  m.authenticity = p[29];
  m.short_sale_threshold = p[30];
  m.ipo_flag = p[31];
  m.luld_tier = p[32];
  m.etp_flag = p[33];
  m.etp_leverage = static_cast<std::uint32_t>(wire::load<4>(p + 34));
  m.inverse_indicator = p[38];
  return Status::ok;
}
inline Status decode(std::span<const char> p, StockDirectory &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct StockTradingAction {
  static constexpr char type = 'H';
  static constexpr std::size_t wire_size = 25;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  Symbol stock{};
  char trading_state{};
  char reserved{};
  std::array<char, 4> reason = wire::spaces<4>();
};
inline std::size_t encode(const StockTradingAction &m, char *p,
                          std::size_t n) noexcept {
  if (n < 25 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'H';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  for (std::size_t i = 0; i < 8; ++i)
    p[11 + i] = m.stock.chars[i];
  p[19] = m.trading_state;
  p[20] = m.reserved;
  for (std::size_t i = 0; i < 4; ++i)
    p[21 + i] = m.reason[i];
  return 25;
}
inline Status decode(const char *p, std::size_t n,
                     StockTradingAction &m) noexcept {
  if (n < 25)
    return Status::incomplete;
  if (n != 25)
    return Status::invalid_length;
  if (p[0] != 'H')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[11 + i];
  m.trading_state = p[19];
  m.reserved = p[20];
  for (std::size_t i = 0; i < 4; ++i)
    m.reason[i] = p[21 + i];
  return Status::ok;
}
inline Status decode(std::span<const char> p, StockTradingAction &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct AddOrder {
  static constexpr char type = 'A';
  static constexpr std::size_t wire_size = 36;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Side side = Side::BUY;
  Quantity shares{};
  Symbol stock{};
  Price price{};
};
inline std::size_t encode(const AddOrder &m, char *p, std::size_t n) noexcept {
  if (n < 36 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'A';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  p[19] = m.side == Side::BUY ? 'B' : 'S';
  wire::store<4>(p + 20, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[24 + i] = m.stock.chars[i];
  wire::store<4>(p + 32, static_cast<std::uint64_t>(m.price));
  return 36;
}
inline Status decode(const char *p, std::size_t n, AddOrder &m) noexcept {
  if (n < 36)
    return Status::incomplete;
  if (n != 36)
    return Status::invalid_length;
  if (p[0] != 'A')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  if (p[19] != 'B' && p[19] != 'S')
    return Status::invalid_field;
  m.side = p[19] == 'B' ? Side::BUY : Side::SELL;
  m.shares = static_cast<Quantity>(wire::load<4>(p + 20));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[24 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 32));
  return Status::ok;
}
inline Status decode(std::span<const char> p, AddOrder &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct AddOrderMPID {
  static constexpr char type = 'F';
  static constexpr std::size_t wire_size = 40;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Side side = Side::BUY;
  Quantity shares{};
  Symbol stock{};
  Price price{};
  std::array<char, 4> attribution = wire::spaces<4>();
};
inline std::size_t encode(const AddOrderMPID &m, char *p,
                          std::size_t n) noexcept {
  if (n < 40 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'F';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  p[19] = m.side == Side::BUY ? 'B' : 'S';
  wire::store<4>(p + 20, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[24 + i] = m.stock.chars[i];
  wire::store<4>(p + 32, static_cast<std::uint64_t>(m.price));
  for (std::size_t i = 0; i < 4; ++i)
    p[36 + i] = m.attribution[i];
  return 40;
}
inline Status decode(const char *p, std::size_t n, AddOrderMPID &m) noexcept {
  if (n < 40)
    return Status::incomplete;
  if (n != 40)
    return Status::invalid_length;
  if (p[0] != 'F')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  if (p[19] != 'B' && p[19] != 'S')
    return Status::invalid_field;
  m.side = p[19] == 'B' ? Side::BUY : Side::SELL;
  m.shares = static_cast<Quantity>(wire::load<4>(p + 20));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[24 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 32));
  for (std::size_t i = 0; i < 4; ++i)
    m.attribution[i] = p[36 + i];
  return Status::ok;
}
inline Status decode(std::span<const char> p, AddOrderMPID &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderExecuted {
  static constexpr char type = 'E';
  static constexpr std::size_t wire_size = 31;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Quantity shares{};
  std::uint64_t match_number{};
};
inline std::size_t encode(const OrderExecuted &m, char *p,
                          std::size_t n) noexcept {
  if (n < 31 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'E';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  wire::store<4>(p + 19, static_cast<std::uint64_t>(m.shares));
  wire::store<8>(p + 23, static_cast<std::uint64_t>(m.match_number));
  return 31;
}
inline Status decode(const char *p, std::size_t n, OrderExecuted &m) noexcept {
  if (n < 31)
    return Status::incomplete;
  if (n != 31)
    return Status::invalid_length;
  if (p[0] != 'E')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  m.shares = static_cast<Quantity>(wire::load<4>(p + 19));
  m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 23));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderExecuted &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderExecutedWithPrice {
  static constexpr char type = 'C';
  static constexpr std::size_t wire_size = 36;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Quantity shares{};
  std::uint64_t match_number{};
  char printable{};
  Price price{};
};
inline std::size_t encode(const OrderExecutedWithPrice &m, char *p,
                          std::size_t n) noexcept {
  if (n < 36 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'C';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  wire::store<4>(p + 19, static_cast<std::uint64_t>(m.shares));
  wire::store<8>(p + 23, static_cast<std::uint64_t>(m.match_number));
  p[31] = m.printable;
  wire::store<4>(p + 32, static_cast<std::uint64_t>(m.price));
  return 36;
}
inline Status decode(const char *p, std::size_t n,
                     OrderExecutedWithPrice &m) noexcept {
  if (n < 36)
    return Status::incomplete;
  if (n != 36)
    return Status::invalid_length;
  if (p[0] != 'C')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  m.shares = static_cast<Quantity>(wire::load<4>(p + 19));
  m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 23));
  m.printable = p[31];
  m.price = static_cast<Price>(wire::load<4>(p + 32));
  return Status::ok;
}
inline Status decode(std::span<const char> p,
                     OrderExecutedWithPrice &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderCancel {
  static constexpr char type = 'X';
  static constexpr std::size_t wire_size = 23;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Quantity shares{};
};
inline std::size_t encode(const OrderCancel &m, char *p,
                          std::size_t n) noexcept {
  if (n < 23 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'X';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  wire::store<4>(p + 19, static_cast<std::uint64_t>(m.shares));
  return 23;
}
inline Status decode(const char *p, std::size_t n, OrderCancel &m) noexcept {
  if (n < 23)
    return Status::incomplete;
  if (n != 23)
    return Status::invalid_length;
  if (p[0] != 'X')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  m.shares = static_cast<Quantity>(wire::load<4>(p + 19));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderCancel &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderDelete {
  static constexpr char type = 'D';
  static constexpr std::size_t wire_size = 19;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
};
inline std::size_t encode(const OrderDelete &m, char *p,
                          std::size_t n) noexcept {
  if (n < 19 || m.timestamp > 0xffffffffffffULL)
    return 0;
  p[0] = 'D';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  return 19;
}
inline Status decode(const char *p, std::size_t n, OrderDelete &m) noexcept {
  if (n < 19)
    return Status::incomplete;
  if (n != 19)
    return Status::invalid_length;
  if (p[0] != 'D')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderDelete &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct OrderReplace {
  static constexpr char type = 'U';
  static constexpr std::size_t wire_size = 35;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t original_reference{};
  std::uint64_t new_reference{};
  Quantity shares{};
  Price price{};
};
inline std::size_t encode(const OrderReplace &m, char *p,
                          std::size_t n) noexcept {
  if (n < 35 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'U';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.original_reference));
  wire::store<8>(p + 19, static_cast<std::uint64_t>(m.new_reference));
  wire::store<4>(p + 27, static_cast<std::uint64_t>(m.shares));
  wire::store<4>(p + 31, static_cast<std::uint64_t>(m.price));
  return 35;
}
inline Status decode(const char *p, std::size_t n, OrderReplace &m) noexcept {
  if (n < 35)
    return Status::incomplete;
  if (n != 35)
    return Status::invalid_length;
  if (p[0] != 'U')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.original_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  m.new_reference = static_cast<std::uint64_t>(wire::load<8>(p + 19));
  m.shares = static_cast<Quantity>(wire::load<4>(p + 27));
  m.price = static_cast<Price>(wire::load<4>(p + 31));
  return Status::ok;
}
inline Status decode(std::span<const char> p, OrderReplace &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct Trade {
  static constexpr char type = 'P';
  static constexpr std::size_t wire_size = 44;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t order_reference{};
  Side side = Side::BUY;
  Quantity shares{};
  Symbol stock{};
  Price price{};
  std::uint64_t match_number{};
};
inline std::size_t encode(const Trade &m, char *p, std::size_t n) noexcept {
  if (n < 44 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'P';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.order_reference));
  p[19] = m.side == Side::BUY ? 'B' : 'S';
  wire::store<4>(p + 20, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[24 + i] = m.stock.chars[i];
  wire::store<4>(p + 32, static_cast<std::uint64_t>(m.price));
  wire::store<8>(p + 36, static_cast<std::uint64_t>(m.match_number));
  return 44;
}
inline Status decode(const char *p, std::size_t n, Trade &m) noexcept {
  if (n < 44)
    return Status::incomplete;
  if (n != 44)
    return Status::invalid_length;
  if (p[0] != 'P')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  if (p[19] != 'B' && p[19] != 'S')
    return Status::invalid_field;
  m.side = p[19] == 'B' ? Side::BUY : Side::SELL;
  m.shares = static_cast<Quantity>(wire::load<4>(p + 20));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[24 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 32));
  m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 36));
  return Status::ok;
}
inline Status decode(std::span<const char> p, Trade &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct CrossTrade {
  static constexpr char type = 'Q';
  static constexpr std::size_t wire_size = 40;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t shares{};
  Symbol stock{};
  Price price{};
  std::uint64_t match_number{};
  char cross_type{};
};
inline std::size_t encode(const CrossTrade &m, char *p,
                          std::size_t n) noexcept {
  if (n < 40 || m.timestamp > 0xffffffffffffULL || m.price < 0 ||
      static_cast<std::uint64_t>(m.price) > UINT32_MAX)
    return 0;
  p[0] = 'Q';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.shares));
  for (std::size_t i = 0; i < 8; ++i)
    p[19 + i] = m.stock.chars[i];
  wire::store<4>(p + 27, static_cast<std::uint64_t>(m.price));
  wire::store<8>(p + 31, static_cast<std::uint64_t>(m.match_number));
  p[39] = m.cross_type;
  return 40;
}
inline Status decode(const char *p, std::size_t n, CrossTrade &m) noexcept {
  if (n < 40)
    return Status::incomplete;
  if (n != 40)
    return Status::invalid_length;
  if (p[0] != 'Q')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.shares = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[19 + i];
  m.price = static_cast<Price>(wire::load<4>(p + 27));
  m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 31));
  m.cross_type = p[39];
  return Status::ok;
}
inline Status decode(std::span<const char> p, CrossTrade &m) noexcept {
  return decode(p.data(), p.size(), m);
}
struct NetOrderImbalance {
  static constexpr char type = 'I';
  static constexpr std::size_t wire_size = 50;
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};
  std::uint64_t paired_shares{};
  std::uint64_t imbalance_shares{};
  char imbalance_direction{};
  Symbol stock{};
  Price far_price{};
  Price near_price{};
  Price current_reference_price{};
  char cross_type{};
  char price_variation{};
};
inline std::size_t encode(const NetOrderImbalance &m, char *p,
                          std::size_t n) noexcept {
  if (n < 50 || m.timestamp > 0xffffffffffffULL || m.far_price < 0 ||
      static_cast<std::uint64_t>(m.far_price) > UINT32_MAX ||
      m.near_price < 0 ||
      static_cast<std::uint64_t>(m.near_price) > UINT32_MAX ||
      m.current_reference_price < 0 ||
      static_cast<std::uint64_t>(m.current_reference_price) > UINT32_MAX)
    return 0;
  p[0] = 'I';
  wire::store<2>(p + 1, static_cast<std::uint64_t>(m.stock_locate));
  wire::store<2>(p + 3, static_cast<std::uint64_t>(m.tracking_number));
  wire::store<6>(p + 5, static_cast<std::uint64_t>(m.timestamp));
  wire::store<8>(p + 11, static_cast<std::uint64_t>(m.paired_shares));
  wire::store<8>(p + 19, static_cast<std::uint64_t>(m.imbalance_shares));
  p[27] = m.imbalance_direction;
  for (std::size_t i = 0; i < 8; ++i)
    p[28 + i] = m.stock.chars[i];
  wire::store<4>(p + 36, static_cast<std::uint64_t>(m.far_price));
  wire::store<4>(p + 40, static_cast<std::uint64_t>(m.near_price));
  wire::store<4>(p + 44, static_cast<std::uint64_t>(m.current_reference_price));
  p[48] = m.cross_type;
  p[49] = m.price_variation;
  return 50;
}
inline Status decode(const char *p, std::size_t n,
                     NetOrderImbalance &m) noexcept {
  if (n < 50)
    return Status::incomplete;
  if (n != 50)
    return Status::invalid_length;
  if (p[0] != 'I')
    return Status::invalid_type;
  m.stock_locate = static_cast<std::uint16_t>(wire::load<2>(p + 1));
  m.tracking_number = static_cast<std::uint16_t>(wire::load<2>(p + 3));
  m.timestamp = static_cast<std::uint64_t>(wire::load<6>(p + 5));
  m.paired_shares = static_cast<std::uint64_t>(wire::load<8>(p + 11));
  m.imbalance_shares = static_cast<std::uint64_t>(wire::load<8>(p + 19));
  m.imbalance_direction = p[27];
  for (std::size_t i = 0; i < 8; ++i)
    m.stock.chars[i] = p[28 + i];
  m.far_price = static_cast<Price>(wire::load<4>(p + 36));
  m.near_price = static_cast<Price>(wire::load<4>(p + 40));
  m.current_reference_price = static_cast<Price>(wire::load<4>(p + 44));
  m.cross_type = p[48];
  m.price_variation = p[49];
  return Status::ok;
}
inline Status decode(std::span<const char> p, NetOrderImbalance &m) noexcept {
  return decode(p.data(), p.size(), m);
}
} // namespace exsim::protocol::itch50
#endif
