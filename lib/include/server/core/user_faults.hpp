#ifndef EXSIM_SERVER_CORE_USER_FAULTS_HPP
#define EXSIM_SERVER_CORE_USER_FAULTS_HPP

// Per-user fault-injection state set by the admin `fault user ...`
// commands (server contract section 4) and consumed by the gateways and the
// server loop through ExchangeCore's query/consume API.

#include <cstdint>

namespace exsim::server {

struct UserFaultState {
    std::uint32_t reject_next_new_orders = 0;    // gateway rejects this many next new orders itself
    std::uint64_t inbound_delay_ns = 0;          // hold each inbound application message this long
    std::uint32_t drop_next_outbound = 0;        // do not transmit this many next sequenced messages
    bool disconnect_requested = false;           // server loop closes the bound connection

    [[nodiscard]] bool any() const noexcept {
        return reject_next_new_orders != 0 || inbound_delay_ns != 0 || drop_next_outbound != 0 ||
               disconnect_requested;
    }
};

}  // namespace exsim::server

#endif
