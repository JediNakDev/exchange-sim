#ifndef EXSIM_SERVER_RUNTIME_SERVER_CLOCK_HPP
#define EXSIM_SERVER_RUNTIME_SERVER_CLOCK_HPP

// The server's CoreClock (exsim/server/core/policies.hpp): nanoseconds since
// local midnight, derived from the monotonic clock so it never steps back
// when the wall clock is adjusted. The day time is read once at startup
// (local midnight from the wall clock and the local UTC offset, or an
// explicit override for tests and simulations) and then advanced by the
// monotonic clock. A server that runs past midnight sees day times beyond
// 24:00, which the core treats as after the trading day (it shuts down).

#include "exsim/net/clocks.hpp"
#include "exsim/types.hpp"

#include <cstdint>
#include <optional>

namespace exsim::server::runtime {

class ServerClock {
public:
    // day_time_at_start: ns since local midnight to report right now;
    // nullopt reads the local wall clock.
    explicit ServerClock(std::optional<Timestamp> day_time_at_start = std::nullopt) noexcept
        : monotonic_at_start_ns_(net::monotonic_ns()) {
        if (day_time_at_start) {
            day_time_at_start_ns_ = *day_time_at_start;
        } else {
            net::LocalClock local_clock;
            local_clock.refresh();
            day_time_at_start_ns_ = local_clock.midnight_ns(net::realtime_ns());
        }
    }

    [[nodiscard]] Timestamp day_ns() const noexcept {
        return day_time_at_start_ns_ + (net::monotonic_ns() - monotonic_at_start_ns_);
    }
    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return net::realtime_ns(); }
    [[nodiscard]] std::uint64_t monotonic_ns() const noexcept { return net::monotonic_ns(); }

private:
    std::uint64_t monotonic_at_start_ns_;
    Timestamp day_time_at_start_ns_ = 0;
};

}  // namespace exsim::server::runtime

#endif
