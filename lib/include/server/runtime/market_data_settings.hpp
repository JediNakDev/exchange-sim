#ifndef EXSIM_SERVER_RUNTIME_MARKET_DATA_SETTINGS_HPP
#define EXSIM_SERVER_RUNTIME_MARKET_DATA_SETTINGS_HPP

#include <cstddef>
#include <cstdint>

namespace exsim::server::runtime {

// Process settings shared by the ITCH and MDP market data runtimes.
struct MarketDataRuntimeSettings {
    std::uint64_t heartbeat_interval_ns = 1'000'000'000;  // heartbeat after this much feed silence
    std::size_t max_held_packets = 256;                  // reorder fault capacity
    int send_buffer_bytes = 4 << 20;                     // SO_SNDBUF of the feed socket; 0 = OS default
};

}  // namespace exsim::server::runtime

#endif
