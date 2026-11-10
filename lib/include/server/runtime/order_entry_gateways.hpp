#ifndef EXSIM_SERVER_RUNTIME_ORDER_ENTRY_GATEWAYS_HPP
#define EXSIM_SERVER_RUNTIME_ORDER_ENTRY_GATEWAYS_HPP

// Compile-time gateway registration for the server runtime. Each gateway is
// a type with this static shape (no virtual functions):
//
//   static constexpr std::string_view name;
//   using UserDirectory = ...;                        // day-lifetime user states
//   static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig&);
//   static std::uint16_t configured_port(const ServerConfig&);
//   template <class Core> static void dispatch(UserDirectory&, SessionId,
//                                              std::span<const Event>, Core&);
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
// BasicServerRuntime<Gateways...> (server_runtime.hpp). OUCH and FIX are active. A new protocol adds one such type and one entry in
// that list; the loop, the delay fault, admin disconnects and shutdown
// handling are shared.

#include "event.hpp"
#include "gateway/fix/fix_gateway.hpp"
#include "gateway/ouch/ouch_server.hpp"
#include "server/config.hpp"
#include "server/core/policies.hpp"
#include "server/runtime/tcp_send_sink.hpp"
#include "types.hpp"

#include <cstddef>
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

    [[nodiscard]] static std::unique_ptr<UserDirectory> make_user_directory(const ServerConfig& config) {
        return std::make_unique<UserDirectory>(config);
    }
    [[nodiscard]] static std::uint16_t configured_port(const ServerConfig& config) noexcept {
        return config.exchange.ouch_port;
    }
    template <class Core>
    static void dispatch(UserDirectory& users, SessionId session, std::span<const Event> batch, Core& core) noexcept {
        if (auto* user_state = users.find(session)) user_state->on_events(batch, core);
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
    // Only the batch's own session: the core already calls once per session.
    template <class Core>
    static void dispatch(UserDirectory& users, SessionId session, std::span<const Event> batch, Core& core) noexcept {
        if (auto* user = users.find(session)) user->on_events(batch, core);
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

}  // namespace exsim::server::runtime

#endif
