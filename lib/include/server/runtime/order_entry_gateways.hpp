#ifndef EXSIM_SERVER_RUNTIME_ORDER_ENTRY_GATEWAYS_HPP
#define EXSIM_SERVER_RUNTIME_ORDER_ENTRY_GATEWAYS_HPP

// Compile-time gateway registration for the server runtime. Each gateway is
// a type with this static shape (no virtual functions):
//
//   static constexpr std::string_view name;
//   using UserDirectory = ...;                        // day-lifetime user states
//   using User = ...;                                 // one user's state:
//       SessionId session_id() const;
//       template <class Events, class Core> void on_events(const Events&, Core&);
//   static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig&);
//       directory.users() lists every User as std::unique_ptr<User>
//   static std::uint16_t configured_port(const ServerConfig&);
//   template <class Core> static void on_shutdown(UserDirectory&, Timestamp, Core&);
//   template <class Core> class Connection {          // one per connection slot
//       Connection(UserDirectory&);                   // startup: preallocated
//       void open(TcpSendSink, ConnectionId, Core&);  // accepted
//       std::size_t on_data(std::span<const char>, Core&);  // bytes consumed
//       void on_timer(Core&);
//       bool wants_close() const;  bool flush_before_close() const;
//       void close_for_end_of_day(Core&);             // shutdown: end the session politely
//       void on_closed(Core&);                        // transport gone: unbind (cancel-on-disconnect)
//       SessionId bound_session() const;              // 0 before login
//   };
//
// The server lists its gateways as template arguments of
// BasicServerRuntime<Gateways...> (server_runtime.hpp): OUCH, FIX and iLink 3.
// A new protocol adds one such type and one entry in that list; the session
// table (GatewaySessions), the loop, the delay fault, admin disconnects and
// shutdown handling are shared.

#include "engine/event.hpp"
#include "gateway/fix/fix_gateway.hpp"
#include "gateway/ouch/ouch_server.hpp"
#include "gateway/ilink/ilink_connection.hpp"
#include "server/config.hpp"
#include "server/core/policies.hpp"
#include "server/runtime/tcp_send_sink.hpp"
#include "engine/types.hpp"

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace exsim::server::runtime {

// ---- OUCH 4.2 over SoupBinTCP 3.0 ------------------------------------------------

template<class ByteSink>
struct BasicOuchGateway {
    static constexpr std::string_view name = "ouch";
    using UserDirectory = gateway::ouch::OuchUserStateDirectory<ByteSink>;
    using User = gateway::ouch::OuchUserState<ByteSink>;

    [[nodiscard]] static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig& config) {
        return std::make_unique<UserDirectory>(config);
    }
    [[nodiscard]] static std::uint16_t configured_port(const ServerConfig& config) noexcept {
        return config.exchange.ouch_port;
    }
    // System Event 'E' for every OUCH user, stored for those not logged in.
    template <class Core>
    static void on_shutdown(UserDirectory& users, Timestamp end_of_day, Core& core) noexcept {
        users.on_shutdown(end_of_day, core);
    }

    template <class Core>
    class Connection {
    public:
        explicit Connection(UserDirectory& users) noexcept : users_(users) {}

        void open(ByteSink sink, server::ConnectionId connection_id, Core& core) noexcept {
            session_.emplace(sink, users_, connection_id,
                             gateway::ouch::OuchSessionSettings::from(core.config().exchange), core.now());
        }
        std::size_t on_data(std::span<const char> bytes, Core& core) noexcept { return session_->on_data(bytes, core); }
        void on_timer(Core& core) noexcept { session_->on_timer(core); }
        [[nodiscard]] bool wants_close() const noexcept { return session_->close_requested(); }
        [[nodiscard]] bool flush_before_close() const noexcept { return session_->flush_before_close(); }
        // SoupBinTCP End of Session after the stored System Event 'E'.
        void close_for_end_of_day(Core& core) noexcept { session_->end_session(core); }
        void on_closed(Core& core) noexcept {
            session_->on_disconnect(core);
            session_.reset();
        }
        [[nodiscard]] SessionId bound_session() const noexcept {
            const auto* user_state = session_->user_state();
            return user_state == nullptr ? SessionId{0} : user_state->session_id();
        }

    private:
        UserDirectory& users_;
        std::optional<gateway::ouch::OuchConnection<ByteSink>> session_;
    };
};

// ---- FIX 4.4 -----------------------------------------------------------------------

template<class ByteSink>
struct BasicFixGateway {
    static constexpr std::string_view name = "fix";
    using UserDirectory = gateway::fix::FixUserDirectory<ByteSink>;
    using User = gateway::fix::FixUserState<ByteSink>;

    [[nodiscard]] static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig& config) {
        gateway::fix::FixUserLimits limits;
        limits.max_tokens = config.engine.max_tokens_per_session;
        limits.store_max_messages = config.engine.store_messages_per_session;
        limits.store_max_bytes = config.engine.store_bytes_per_session;
        return std::make_unique<UserDirectory>(config.users, config.exchange.name, limits);
    }
    [[nodiscard]] static std::uint16_t configured_port(const ServerConfig& config) noexcept {
        return config.exchange.fix_port;
    }
    // FIX has no end-of-day application message: the loop sends Logout on
    // every live connection (close_for_end_of_day).
    template <class Core>
    static void on_shutdown(UserDirectory&, Timestamp, Core&) noexcept {}

    template <class Core>
    class Connection {
    public:
        explicit Connection(UserDirectory& users) noexcept : session_(users) {}

        void open(ByteSink sink, server::ConnectionId connection_id, Core& core) noexcept {
            sink_ = sink;
            close_requested_by_server_ = false;
            session_.start(sink_, connection_id, core);
        }
        std::size_t on_data(std::span<const char> bytes, Core& core) noexcept { return session_.on_data(bytes, core); }
        void on_timer(Core& core) noexcept { session_.on_timer(core); }
        [[nodiscard]] bool wants_close() const noexcept {
            return close_requested_by_server_ || session_.wants_disconnect();
        }
        // Everything sent is worth delivering (a Logout, rejects) unless the
        // send buffer itself overflowed.
        [[nodiscard]] bool flush_before_close() const noexcept {
            const auto* user = session_.user();
            return user == nullptr || !user->session_day_state().output_overflowed();
        }
        // Logout(5) "end of day"; the peer's Logout or the logout timeout
        // completes it. A connection without an active session just closes.
        void close_for_end_of_day(Core& core) noexcept {
            if (!session_.begin_logout("end of day", core)) close_requested_by_server_ = true;
        }
        void on_closed(Core& core) noexcept { session_.on_disconnected(core); }
        [[nodiscard]] SessionId bound_session() const noexcept {
            const auto* user = session_.user();
            return user == nullptr ? SessionId{0} : user->session_id();
        }

    private:
        ByteSink sink_{};  // FixConnection keeps a pointer to it: the slot never moves
        gateway::fix::FixConnection<Core, ByteSink> session_;
        bool close_requested_by_server_ = false;
    };
};

using FixGateway = BasicFixGateway<TcpSendSink>;
using OuchGateway = BasicOuchGateway<TcpSendSink>;

template<class ByteSink> struct BasicIlinkGateway {
    static constexpr std::string_view name = "ilink3";
    using UserDirectory = gateway::ilink::IlinkUsers<ByteSink>;
    using User = gateway::ilink::IlinkUser<ByteSink>;
    static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig& config) {
        return std::make_unique<UserDirectory>(config);
    }
    static std::uint16_t configured_port(const ServerConfig& config) noexcept { return config.exchange.ilink_port; }
    static bool enabled(const ServerConfig& config) noexcept {
        return config.exchange.ilink_port != 0 || std::any_of(config.users.begin(), config.users.end(),
            [](const auto& user) { return user.protocol == Protocol::Ilink3; });
    }
    template<class Core> static void on_shutdown(UserDirectory&, Timestamp, Core&) noexcept {}
    template<class Core> class Connection {
    public:
        explicit Connection(UserDirectory& users) : session_(users) {}
        void open(ByteSink sink, server::ConnectionId id, Core& core) noexcept { session_.open(sink, id, core); }
        std::size_t on_data(std::span<const char> bytes, Core& core) noexcept { return session_.on_data(bytes, core); }
        void on_timer(Core& core) noexcept { session_.on_timer(core); }
        bool wants_close() const noexcept { return session_.wants_close(); }
        bool flush_before_close() const noexcept { return session_.flush_before_close(); }
        void close_for_end_of_day(Core& core) noexcept { session_.shutdown(core); }
        void on_closed(Core& core) noexcept { session_.on_closed(core); }
        SessionId bound_session() const noexcept { return session_.bound_session(); }
    private:
        gateway::ilink::IlinkConnection<ByteSink> session_;
    };
};
using IlinkGateway = BasicIlinkGateway<TcpSendSink>;

}  // namespace exsim::server::runtime

#endif
