#ifndef EXSIM_SERVER_CORE_ADMIN_TEXT_HPP
#define EXSIM_SERVER_CORE_ADMIN_TEXT_HPP

// Text rendering for admin replies (operator path: allocates freely).
// Every reply is one or more '\n'-terminated lines ending with "OK" or
// "ERR <message>".

#include "engine/matching_engine.hpp"
#include "server/core/user_faults.hpp"
#include "md/packet_faults.hpp"
#include "engine/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace exsim::server {

// The most levels per side `book` prints, whatever the request asks for.
inline constexpr std::size_t kMaxAdminBookLevels = 1000;

void append_line(std::string& reply, std::string_view line);
void append_ok(std::string& reply);
void append_error(std::string& reply, std::string_view message);

// "phase=CONTINUOUS" header line, then asks from the deepest printed level
// to the best, then bids from the best down (a price ladder), then OK.
void append_book(std::string& reply, const MatchingEngine& engine, InstrumentId instrument, std::size_t levels);

// "reject=0 delay_us=0 drop_out=0 disconnect=no"
[[nodiscard]] std::string describe_user_faults(const UserFaultState& faults);

// "drop_ppm=0 dup_ppm=0 reorder_ppm=0 delay=1 seed=1 held=0 submitted=0 ..."
[[nodiscard]] std::string describe_packet_faults(const md::PacketFaultInjector& injector);

}  // namespace exsim::server

#endif
