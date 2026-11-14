#ifndef EXSIM_SERVER_CONFIG_HPP
#define EXSIM_SERVER_CONFIG_HPP

// Static exchange configuration. Parsed once at startup, so this is ordinary
// allocating code; nothing here runs on the hot path.
//
// Format: INI-style sections, `key = value`, `#` or `;` comments.
//
//   [exchange]                 name, bind_address, ouch_port, fix_port,
//                              ilink_port, max_connections,
//                              heartbeat_interval_ms, soup_session,
//                              trading_day_start, trading_day_end
//   [market_data]              protocol (itch50 or mdp3), address, port,
//                              interface, ttl, loopback, retransmit_port,
//                              max_payload, market_segment_id
//                              itch50 only: session, retransmit_buffer,
//                              noii_interval_ms
//                              mdp3 only: channel_id, book_depth,
//                              snapshot_port, instrument_port, backup_port,
//                              snapshot_interval_ms, instrument_interval_ms,
//                              replay_buffer_packets, replay_username,
//                              replay_password
//   [engine]                   max_instruments, max_orders,
//                              max_tokens_per_session,
//                              store_messages_per_session,
//                              store_bytes_per_session
//   [journal]                  path, on_failure, flush_interval_ms
//   [instrument <SYMBOL>]      tick_size, lot_size, max_order_qty,
//                              min_price, max_price, reference_price,
//                              initial_phase, cme_security_id,
//                              cme_contract_size, cme_maturity_date,
//                              cme_symbol
//   [user <USERNAME>]          password, protocol, session_id, owner, stp,
//                              cancel_on_disconnect, ilink_access_key,
//                              ilink_secret_key, ilink_session, ilink_firm,
//                              ilink_party_id, ilink_account
// Keys of the other market data protocol are rejected because they would be
// ignored. Prices are decimal dollars ("150.25"), stored with 4 implied
// decimals. Trading-day times are "HH:MM[:SS[.fraction]]" local time; the end
// may be "24:00". See test/fixtures/exchange.conf for a complete example.

#include "engine/instrument.hpp"
#include "engine/matching_engine.hpp"
#include "engine/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::server {

// Active gateways: OUCH, FIX 4.4 and CME iLink 3.
enum class Protocol : std::uint8_t { Ouch42 = 0, Fix44 = 1, Ilink3 = 3 };

constexpr std::string_view to_string(Protocol protocol) noexcept {
    switch (protocol) {
        case Protocol::Ouch42: return "ouch42";
        case Protocol::Fix44: return "fix44";
        case Protocol::Ilink3: return "ilink3";
    }
    return "unknown";
}

struct UserConfig {
    std::string username;  // SoupBinTCP username (<= 6 chars), FIX SenderCompID or FIXP credentials user
    std::string password;  // SoupBinTCP password (<= 10 chars), FIX Password(554) or FIXP credentials; empty = none
    Protocol protocol = Protocol::Ouch42;
    SessionId session_id = 0;  // unique, non-zero
    OwnerId owner = 0;         // STP group; 0 = none
    StpMode stp = StpMode::None;
    bool cancel_on_disconnect = true;
    std::string ilink_access_key;
    std::string ilink_secret_key;  // base64url-encoded 32-byte HMAC key
    std::string ilink_session;
    std::string ilink_firm;
    std::uint64_t ilink_party_id = 0;
    std::string ilink_account;
};

inline constexpr Timestamp kNanosecondsPerDay = Timestamp{86'400} * 1'000'000'000;

struct ExchangeSection {
    std::string name = "EXSIM";  // FIX TargetCompID clients must use
    std::string bind_address = "127.0.0.1";
    std::uint16_t ouch_port = 0;   // 0 = OS-assigned (logged at startup)
    std::uint16_t fix_port = 0;
    std::uint16_t ilink_port = 0;
    std::uint32_t max_connections = 64;
    std::uint32_t heartbeat_interval_ms = 1000;  // SoupBinTCP server heartbeat
    std::string soup_session = "EXSIM";            // SoupBinTCP session (<= 10 chars)
    // Commands are accepted only while their stamped timestamp (ns since
    // local midnight) is in [trading_day_start_ns, trading_day_end_ns).
    Timestamp trading_day_start_ns = 0;
    Timestamp trading_day_end_ns = kNanosecondsPerDay;
};

enum class MarketDataProtocol : std::uint8_t { Itch50, Mdp3 };

struct MarketDataSection {
    MarketDataProtocol protocol = MarketDataProtocol::Itch50;
    std::string address = "127.0.0.1";  // unicast destination or multicast group
    std::uint16_t port = 0;
    std::string interface_address;      // multicast egress interface; empty = default
    std::uint8_t ttl = 1;
    bool loopback = true;
    std::uint16_t retransmit_port = 0;  // MoldUDP64 request server
    std::uint32_t max_payload = 1400;   // bytes per MoldUDP64 packet incl. header; 72..65507 (md::kMinimumMaxPayload)
    std::string session = "EXSIM";      // MoldUDP64 session (<= 10 chars)
    std::uint32_t retransmit_buffer = 1u << 20;  // messages kept for retransmission
    std::uint32_t noii_interval_ms = 1000;
    std::uint16_t channel_id = 1;
    std::uint8_t market_segment_id = 1;
    std::uint8_t book_depth = 10;
    std::uint16_t snapshot_port = 0;    // MDP snapshot destination
    std::uint16_t instrument_port = 0;  // MDP instrument replay destination
    std::uint16_t backup_port = 0;      // optional redundant incremental destination
    std::uint32_t snapshot_interval_ms = 1000;
    std::uint32_t instrument_interval_ms = 5000;
    std::uint32_t replay_buffer_packets = 4096;
    std::string replay_username;
    std::string replay_password;
};

struct EngineSection {
    EngineConfig engine{};
    std::uint32_t max_tokens_per_session = 1u << 20;
    std::size_t store_messages_per_session = 1u << 20;
    std::size_t store_bytes_per_session = std::size_t{64} << 20;
};

// What the exchange core does when a journal write fails (exchange core).
enum class JournalFailurePolicy : std::uint8_t {
    Shutdown = 0,  // end the trading day: the replay guarantee is gone
    Continue = 1,  // keep trading unjournaled; status reports the failure
};

constexpr std::string_view to_string(JournalFailurePolicy p) noexcept {
    return p == JournalFailurePolicy::Shutdown ? "shutdown" : "continue";
}

struct ServerConfig {
    ExchangeSection exchange;
    MarketDataSection market_data;
    EngineSection engine;
    std::string journal_path;  // empty = journaling disabled
    JournalFailurePolicy journal_failure_policy = JournalFailurePolicy::Shutdown;
    std::uint32_t journal_flush_interval_ms = 1000;  // periodic flush from the core timer; 0 = never
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


// "09:30", "09:30:15", "09:30:15.25" -> ns since midnight. Hours 0..23,
// except exactly "24:00[:00]" (end of day); at most 9 fraction digits.
[[nodiscard]] bool parse_day_time(std::string_view text, Timestamp& out) noexcept;

// Inverse of parse_day_time for display: "HH:MM:SS" plus ".fraction" when
// non-zero (trailing zeros removed).
[[nodiscard]] std::string format_day_time(Timestamp ns_since_midnight);

}  // namespace exsim::server

#endif
