#ifndef EXSIM_SERVER_CONFIG_HPP
#define EXSIM_SERVER_CONFIG_HPP

// Static exchange configuration (exsim.conf). Parsed once at startup, so this
// is ordinary allocating code; nothing here runs on the hot path.
//
// Format: INI-style sections, `key = value`, `#` or `;` comments.
//
//   [exchange]                 name, bind_address, ouch_port, fix_port,
//                              admin_port, max_connections,
//                              heartbeat_interval_ms, soup_session
//   [market_data]              address, port, interface, ttl, loopback,
//                              retransmit_port, max_payload, session,
//                              retransmit_buffer, noii_interval_ms
//   [engine]                   max_instruments, max_orders,
//                              max_tokens_per_session,
//                              store_messages_per_session,
//                              store_bytes_per_session
//   [journal]                  path
//   [instrument <SYMBOL>]      tick_size, lot_size, max_order_qty,
//                              min_price, max_price, reference_price,
//                              initial_phase
//   [user <USERNAME>]          password, protocol, session_id, owner, stp,
//                              cancel_on_disconnect
//
// Prices are decimal dollars ("150.25"), stored with 4 implied decimals.
// See config/exsim.conf for a complete example.

#include "exsim/instrument.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::server {

enum class Protocol : std::uint8_t { Ouch42 = 0, Fix44 = 1 };

constexpr std::string_view to_string(Protocol p) noexcept {
    return p == Protocol::Ouch42 ? "ouch42" : "fix44";
}

struct UserConfig {
    std::string username;  // SoupBinTCP username (<= 6 chars) or FIX SenderCompID
    std::string password;  // SoupBinTCP password (<= 10 chars) or FIX Password(554); empty = none
    Protocol protocol = Protocol::Ouch42;
    SessionId session_id = 0;  // unique, non-zero
    OwnerId owner = 0;         // STP group; 0 = none
    StpMode stp = StpMode::None;
    bool cancel_on_disconnect = true;
};

struct ExchangeSection {
    std::string name = "EXSIM";  // FIX TargetCompID clients must use
    std::string bind_address = "127.0.0.1";
    std::uint16_t ouch_port = 0;   // 0 = OS-assigned (logged at startup)
    std::uint16_t fix_port = 0;
    std::uint16_t admin_port = 0;
    std::uint32_t max_connections = 64;
    std::uint32_t heartbeat_interval_ms = 1000;  // SoupBinTCP server heartbeat
    std::string soup_session = "EXSIM";            // SoupBinTCP session (<= 10 chars)
};

struct MarketDataSection {
    std::string address = "127.0.0.1";  // unicast destination or multicast group
    std::uint16_t port = 0;
    std::string interface_address;      // multicast egress interface; empty = default
    std::uint8_t ttl = 1;
    bool loopback = true;
    std::uint16_t retransmit_port = 0;  // MoldUDP64 request server
    std::uint32_t max_payload = 1400;   // bytes per MoldUDP64 packet incl. header
    std::string session = "EXSIM";      // MoldUDP64 session (<= 10 chars)
    std::uint32_t retransmit_buffer = 1u << 20;  // messages kept for retransmission
    std::uint32_t noii_interval_ms = 1000;
};

struct EngineSection {
    EngineConfig engine{};
    std::uint32_t max_tokens_per_session = 1u << 20;
    std::size_t store_messages_per_session = 1u << 20;
    std::size_t store_bytes_per_session = std::size_t{64} << 20;
};

struct ServerConfig {
    ExchangeSection exchange;
    MarketDataSection market_data;
    EngineSection engine;
    std::string journal_path;  // empty = journaling disabled
    std::vector<InstrumentConfig> instruments;
    std::vector<UserConfig> users;

    [[nodiscard]] const UserConfig* find_user(std::string_view username) const noexcept;
};

// Parses `text`; on failure returns false and sets `error` to
// "<line>: <message>" (line 0 = whole-file validation error).
[[nodiscard]] bool parse_config(std::string_view text, ServerConfig& out, std::string& error);

// Reads and parses a file.
[[nodiscard]] bool load_config(const char* path, ServerConfig& out, std::string& error);

// "150.25" -> 1'502'500. Accepts an optional leading '-', at most 4
// decimals; rejects anything else, including overflow.
[[nodiscard]] bool parse_price(std::string_view text, Price& out) noexcept;

// Inverse of parse_price for display: 1'502'500 -> "150.25".
[[nodiscard]] std::string format_price(Price p);

}  // namespace exsim::server

#endif
