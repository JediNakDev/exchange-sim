#ifndef EXSIM_SERVER_ADMIN_HPP
#define EXSIM_SERVER_ADMIN_HPP

// Admin console command language (docs/SERVER.md §4). One command per line;
// parsing is off the hot path (operator commands), so std::string is fine.

#include "exsim/sim/packet_faults.hpp"
#include "exsim/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace exsim::server {

enum class AdminOp : std::uint8_t {
    Help,
    Status,
    Phase,             // symbol ("*" = all), phase
    Book,              // symbol, count = levels
    FaultMdDrop,       // count
    FaultMdDup,        // count
    FaultMdReorder,    // count, delay (packets)
    FaultMdRate,       // rate
    FaultMdOff,
    FaultUserReject,     // user, count
    FaultUserDelay,      // user, delay (microseconds)
    FaultUserDropOut,    // user, count
    FaultUserDisconnect, // user
    FaultUserOff,        // user
    Bots,              // on
    Shutdown,
};

struct AdminCommand {
    AdminOp op = AdminOp::Help;
    std::string symbol;
    std::string user;
    Phase phase = Phase::Closed;
    std::uint32_t count = 0;
    std::uint32_t delay = 0;
    sim::PacketFaultConfig rate{};
    bool on = false;
};

// Parses one line (case-insensitive keywords; symbols and user names are
// kept as typed). On failure, leaves out unchanged and sets error. On
// success, replaces out and clears error. Repeated rate keys are rejected.
[[nodiscard]] bool parse_admin(std::string_view line, AdminCommand& out, std::string& error);

// Multi-line usage text for `help`.
[[nodiscard]] std::string_view admin_help() noexcept;

}  // namespace exsim::server

#endif
