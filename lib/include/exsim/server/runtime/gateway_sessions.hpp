#ifndef EXSIM_SERVER_RUNTIME_GATEWAY_SESSIONS_HPP
#define EXSIM_SERVER_RUNTIME_GATEWAY_SESSIONS_HPP

// The core's Sessions policy for a server running several gateways: holds
// each gateway's day-lifetime user directory and routes every per-session
// dispatch to the gateway that owns the session (a session belongs to
// exactly one protocol; the others' directories return nullptr for it).
// Never redispatches a whole directory.

#include "exsim/event.hpp"
#include "exsim/server/config.hpp"
#include "exsim/types.hpp"

#include <memory>
#include <span>
#include <tuple>

namespace exsim::server::runtime {

template <class... Gateways>
class GatewaySessions {
public:
    explicit GatewaySessions(const ServerConfig& config)
        : user_directories_(Gateways::make_user_directory(config)...) {}

    GatewaySessions(const GatewaySessions&) = delete;
    GatewaySessions& operator=(const GatewaySessions&) = delete;

    template <class Core>
    void on_events(SessionId session, std::span<const Event> batch, Core& core) noexcept {
        (Gateways::dispatch(*std::get<std::unique_ptr<typename Gateways::UserDirectory>>(user_directories_), session,
                            batch, core),
         ...);
    }

    template <class Core>
    void on_shutdown(Timestamp end_of_day, Core& core) noexcept {
        (Gateways::on_shutdown(*std::get<std::unique_ptr<typename Gateways::UserDirectory>>(user_directories_),
                               end_of_day, core),
         ...);
    }

    template <class Gateway>
    [[nodiscard]] typename Gateway::UserDirectory& user_directory() noexcept {
        return *std::get<std::unique_ptr<typename Gateway::UserDirectory>>(user_directories_);
    }

private:
    std::tuple<std::unique_ptr<typename Gateways::UserDirectory>...> user_directories_;
};

}  // namespace exsim::server::runtime

#endif
