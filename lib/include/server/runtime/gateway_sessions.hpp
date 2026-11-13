#ifndef EXSIM_SERVER_RUNTIME_GATEWAY_SESSIONS_HPP
#define EXSIM_SERVER_RUNTIME_GATEWAY_SESSIONS_HPP

// The core's Sessions policy for a server running several gateways: holds
// each gateway's day-lifetime user directory and the one session table of
// the runtime, SessionId -> the gateway user that owns the session (a
// session belongs to exactly one protocol). Each per-session dispatch is
// one table read and one call into that user with its events.

#include "engine/event.hpp"
#include "server/config.hpp"
#include "server/core/session_dispatch_set.hpp"
#include "engine/types.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace exsim::server::runtime {

template <class... Gateways>
class GatewaySessions {
public:
    explicit GatewaySessions(const ServerConfig& config)
        : user_directories_(Gateways::make_user_directory(config)...) {
        SessionId largest_session = 0;
        for (const UserConfig& user : config.users) largest_session = std::max(largest_session, user.session_id);
        user_by_session_.resize(std::size_t{largest_session} + 1);
        (add_users_of<Gateways>(), ...);
    }

    GatewaySessions(const GatewaySessions&) = delete;
    GatewaySessions& operator=(const GatewaySessions&) = delete;

    template <class Core>
    void on_events(SessionId session, const SessionEvents& events, Core& core) noexcept {
        if (session >= user_by_session_.size()) return;
        std::visit(
            [&](auto user) {
                if constexpr (std::is_pointer_v<decltype(user)>) user->on_events(events, core);
            },
            user_by_session_[session]);
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
    using GatewayUser = std::variant<std::monostate, typename Gateways::User*...>;

    template <class Gateway>
    void add_users_of() {
        for (const auto& user : user_directory<Gateway>().users()) {
            user_by_session_[user->session_id()] = user.get();
        }
    }

    std::tuple<std::unique_ptr<typename Gateways::UserDirectory>...> user_directories_;
    std::vector<GatewayUser> user_by_session_;  // index = SessionId; monostate: no gateway user
};

}  // namespace exsim::server::runtime

#endif
