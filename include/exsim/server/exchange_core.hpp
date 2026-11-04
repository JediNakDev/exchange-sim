#ifndef EXSIM_SERVER_EXCHANGE_CORE_HPP
#define EXSIM_SERVER_EXCHANGE_CORE_HPP

// Protocol-independent exchange core of the exsim server (docs/SERVER.md,
// docs/EXCHANGE_CORE.md). It owns the matching engine, the startup-sized
// EventBuffer, the configured users and their session bindings, the
// optional canonical journal, the simulated-liquidity bots and the admin
// and fault-injection state, and it is the gateway Context of SERVER.md 2.1.
//
//   ExchangeCore<Sessions, MarketData, Clock> core(config, sessions, md, clock);
//
// Sessions, MarketData and Clock are policies supplied by the server
// integrator (contracts in exsim/server/core/policies.hpp); the core holds
// references to them and calls them without virtual dispatch.
//
// Threading: everything here runs on the single server thread.
// Hot path (submit, dispatch, timer, fault queries, bind/unbind): no heap
// allocation, no exceptions, no std::string. Construction and admin
// execution are setup/operator paths and may allocate or throw.

#include "exsim/command.hpp"
#include "exsim/event.hpp"
#include "exsim/instrument.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/server/admin.hpp"
#include "exsim/server/config.hpp"
#include "exsim/server/core/admin_text.hpp"
#include "exsim/server/core/deferred_command_queue.hpp"
#include "exsim/server/core/policies.hpp"
#include "exsim/server/core/session_dispatch_set.hpp"
#include "exsim/server/core/user_faults.hpp"
#include "exsim/sim/bots/bot_set.hpp"
#include "exsim/sim/journal.hpp"
#include "exsim/sim/packet_faults.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::server {

struct CoreOptions {
    // 0 (production): required_event_capacity(engine config), the engine's
    // bound for any single command. A smaller value is accepted only so
    // tests can drive the engine's InsufficientOutput path.
    std::size_t event_buffer_capacity = 0;
};

struct CoreCounters {
    std::uint64_t commands_applied = 0;              // handed to the engine and Applied
    std::uint64_t commands_outside_trading_day = 0;  // refused by the trading-day check
    std::uint64_t output_capacity_failures = 0;      // InsufficientOutput on an empty buffer
    std::uint64_t commands_deferred = 0;             // submitted during dispatch, applied afterwards
    std::uint64_t deferred_queue_overflows = 0;      // refused: deferred queue full
    std::uint64_t journal_records = 0;               // accepted by JournalWriter::append
    std::uint64_t journal_failures = 0;              // failed append or flush
    std::uint64_t batches_dispatched = 0;            // non-empty batches
    std::uint64_t session_dispatches = 0;            // Sessions::on_events calls
};

enum class JournalState : std::uint8_t { Disabled = 0, Healthy = 1, Failed = 2 };

[[nodiscard]] constexpr std::string_view to_string(JournalState s) noexcept {
    switch (s) {
        case JournalState::Disabled: return "disabled";
        case JournalState::Healthy: return "ok";
        case JournalState::Failed: return "failed";
    }
    return "?";
}

// Deferred-queue slots beyond one cancel-on-disconnect MassCancel per user.
inline constexpr std::size_t kDeferredCommandSlack = 64;

template <typename Sessions, typename MarketData, CoreClock Clock>
    requires CoreMarketData<MarketData>
class ExchangeCore {
public:
    using BotRegistry = sim::bots::BotSet<kMaxBots>;

    // Read-only view of one configured bot, for status and tests.
    struct BotEntry {
        std::string name;
        BotType type = BotType::MarketMaker;
        std::string user;
        std::string instrument_symbol;
        const sim::bots::BotState* state = nullptr;
    };

    ExchangeCore(const ServerConfig& config, Sessions& sessions, MarketData& market_data, const Clock& clock,
                 CoreOptions options = {})
        : config_(config),
          sessions_(sessions),
          market_data_(market_data),
          clock_(clock),
          engine_(config.engine.engine),
          events_(options.event_buffer_capacity != 0 ? options.event_buffer_capacity
                                                     : required_event_capacity(config.engine.engine)),
          user_index_by_session_(std::make_unique<std::uint16_t[]>(std::size_t{1} << 16)),
          deferred_(config.users.size() + kDeferredCommandSlack),
          day_start_(config.exchange.trading_day_start_ns),
          day_end_(config.exchange.trading_day_end_ns),
          journal_flush_interval_ns_(Timestamp{config.journal_flush_interval_ms} * 1'000'000) {
        static_assert(CoreSessions<Sessions, ExchangeCore>,
                      "Sessions needs on_events(SessionId, std::span<const Event>, Core&)");
        static_assert(sim::bots::BotContext<ExchangeCore>);
        if (day_start_ >= day_end_ || day_end_ > kNanosecondsPerDay) {
            throw std::invalid_argument("trading day range");
        }
        for (std::size_t i = 0; i < config_.instruments.size(); ++i) {
            if (engine_.add_instrument(config_.instruments[i]) != static_cast<InstrumentId>(i + 1)) {
                throw std::invalid_argument("instrument " + std::string(config_.instruments[i].symbol.view()) +
                                            " rejected by the engine");
            }
        }
        if (config_.users.size() >= (std::size_t{1} << 16)) {
            throw std::invalid_argument("too many users");
        }
        users_.reserve(config_.users.size());
        for (const UserConfig& user : config_.users) {
            if (user.session_id == 0 || user_index_by_session_[user.session_id] != 0) {
                throw std::invalid_argument("user " + user.username + ": session_id must be unique and non-zero");
            }
            users_.push_back(UserRuntime{&user});
            user_index_by_session_[user.session_id] = static_cast<std::uint16_t>(users_.size());
        }
        add_configured_bots();
        if (!config_.journal_path.empty()) {
            journal_.emplace(config_.journal_path, sim::JournalSetup{config_.engine.engine, config_.instruments});
        }
    }

    ~ExchangeCore() {
        if (journal_ && !journal_failed_) {
            (void)journal_->flush();  // destructor cannot report; call flush_journal() first to check
        }
    }

    ExchangeCore(const ExchangeCore&) = delete;
    ExchangeCore& operator=(const ExchangeCore&) = delete;
    ExchangeCore(ExchangeCore&&) = delete;
    ExchangeCore& operator=(ExchangeCore&&) = delete;

    // ---- Gateway Context (SERVER.md 2.1) ------------------------------------

    // nullptr: unknown user, wrong protocol, wrong password (an empty
    // configured password accepts any), or a user driven by a bot.
    [[nodiscard]] const UserConfig* authenticate(std::string_view username, std::string_view password,
                                                 Protocol protocol) const noexcept {
        for (const UserRuntime& user : users_) {
            if (user.config->username != username) {
                continue;
            }
            if (user.bot_owned || user.config->protocol != protocol) {
                return nullptr;
            }
            if (!user.config->password.empty() && user.config->password != password) {
                return nullptr;
            }
            return user.config;
        }
        return nullptr;
    }

    // false: unknown session, bot session, or already bound (second login).
    [[nodiscard]] bool bind(SessionId session, ConnectionId connection) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || user->bot_owned || user->bound) {
            return false;
        }
        user->bound = true;
        user->connection = connection;
        ++bound_connection_count_;
        return true;
    }

    // Logout or disconnect. No-op when not bound. Clears a pending
    // disconnect request; submits MassCancel{cancel_on_disconnect=true} for
    // the whole session when the user has cancel_on_disconnect (deferred
    // until the current batch is dispatched when called from a callback).
    void unbind(SessionId session) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || !user->bound) {
            return;
        }
        user->bound = false;
        user->connection = 0;
        --bound_connection_count_;
        clear_disconnect_request(*user);
        if (user->config->cancel_on_disconnect) {
            MassCancel cancel_all{};
            cancel_all.session = session;
            cancel_all.instrument = 0;
            cancel_all.cancel_on_disconnect = true;
            (void)submit(Command{cancel_all});
        }
    }

    [[nodiscard]] InstrumentId find_instrument(const Symbol& symbol) const noexcept {
        return engine_.find_instrument(symbol);
    }

    [[nodiscard]] const InstrumentConfig* instrument(InstrumentId id) const noexcept { return engine_.instrument(id); }

    // SERVER.md section 1, steps 1-5. When it returns Applied, every event of
    // the command (and of any command deferred meanwhile) has reached the
    // sessions, market data and bots. Nothing else is applied, journaled or
    // dispatched.
    SubmitResult submit(const Command& command) noexcept {
        if (dispatching_) {
            if (!deferred_.push(command)) {
                ++counters_.deferred_queue_overflows;
                return SubmitResult::DeferredQueueFull;
            }
            ++counters_.commands_deferred;
            return SubmitResult::Deferred;
        }
        const SubmitResult result = apply(command, next_command_stamp());
        finish_outer_operation();
        return result;
    }

    // ns since local midnight: the Clock, never earlier than a value already
    // returned or a stamped command (monotonic even if the Clock steps back).
    [[nodiscard]] Timestamp now() const noexcept {
        observed_now_ = std::max({observed_now_, static_cast<Timestamp>(clock_.day_ns()), last_command_ts_});
        return observed_now_;
    }

    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return clock_.realtime_ns(); }

    // ---- Timer (server loop, once per iteration) ------------------------------

    // End of trading day check, bots, market-data timer hook, periodic journal
    // flush. `loop_now` is ns since local midnight; the value handed on is
    // made non-decreasing.
    void timer(Timestamp loop_now) noexcept {
        if (dispatching_) {
            return;
        }
        timer_now_ = std::max({loop_now, timer_now_, last_command_ts_});
        const Timestamp now_for_timers = timer_now_;
        if (!shutdown_ && now_for_timers >= day_end_) {
            shutdown_pending_ = true;  // the trading day is over
        }
        finish_outer_operation();
        if (!shutdown_ && now_for_timers >= day_start_) {
            bots_.on_timer(now_for_timers, *this);
        }
        if constexpr (MarketDataTimerWithCore<MarketData, ExchangeCore>) {
            market_data_.on_timer(now_for_timers, std::as_const(*this));
        } else if constexpr (MarketDataTimer<MarketData>) {
            market_data_.on_timer(now_for_timers);
        }
        if (journal_ && !journal_failed_ && journal_flush_interval_ns_ != 0 &&
            now_for_timers - last_journal_flush_ >= journal_flush_interval_ns_) {
            last_journal_flush_ = now_for_timers;
            if (!journal_->flush()) {
                on_journal_failure();
            }
        }
        finish_outer_operation();
    }

    // ---- Fault-injection queries (gateways and server loop) -------------------

    // Gateway, per inbound new order: true = reject it yourself (OUCH J 'O',
    // FIX ER 39=8 Text "injected"), do not submit. Consumes one count.
    [[nodiscard]] bool consume_injected_reject(SessionId session) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || user->faults.reject_next_new_orders == 0) {
            return false;
        }
        --user->faults.reject_next_new_orders;
        return true;
    }

    // Server loop / gateway: hold each inbound application message this long.
    [[nodiscard]] std::uint64_t inbound_delay_ns(SessionId session) const noexcept {
        const UserRuntime* user = user_for_session(session);
        return user == nullptr ? 0 : user->faults.inbound_delay_ns;
    }

    // Gateway, per sequenced outbound message about to be transmitted: true =
    // store it but do not transmit. Consumes one count.
    [[nodiscard]] bool consume_drop_out(SessionId session) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || user->faults.drop_next_outbound == 0) {
            return false;
        }
        --user->faults.drop_next_outbound;
        return true;
    }

    [[nodiscard]] bool has_disconnect_requests() const noexcept { return disconnect_requests_pending_ != 0; }

    // Server loop: writes up to out.size() sessions whose connection must be
    // closed now (config order) and clears those requests. Close each one
    // and call unbind (cancel-on-disconnect applies). Returns the count.
    std::size_t take_disconnect_requests(std::span<SessionId> out) noexcept {
        std::size_t taken = 0;
        for (UserRuntime& user : users_) {
            if (taken == out.size() || disconnect_requests_pending_ == 0) {
                break;
            }
            if (user.faults.disconnect_requested) {
                clear_disconnect_request(user);
                out[taken++] = user.config->session_id;
            }
        }
        return taken;
    }

    [[nodiscard]] const UserFaultState* user_faults(SessionId session) const noexcept {
        const UserRuntime* user = user_for_session(session);
        return user == nullptr ? nullptr : &user->faults;
    }

    // ---- Admin (operator path, allocates) -------------------------------------

    void execute_line(std::string_view line, std::string& reply) {
        AdminCommand command;
        std::string error;
        if (!parse_admin(line, command, error)) {
            append_error(reply, error);
            return;
        }
        execute(command, reply);
    }

    // Appends the reply lines, ending with "OK" or "ERR <message>".
    void execute(const AdminCommand& command, std::string& reply) {
        switch (command.op) {
            case AdminOp::Help:
                reply.append(admin_help());
                append_ok(reply);
                return;
            case AdminOp::Status: append_status(reply); return;
            case AdminOp::Phase: execute_phase(command, reply); return;
            case AdminOp::Book: {
                const InstrumentId id = admin_instrument(command.symbol);
                if (id == 0) {
                    append_error(reply, "unknown instrument " + command.symbol);
                    return;
                }
                append_book(reply, engine_, id, command.count);
                return;
            }
            case AdminOp::FaultMdDrop:
            case AdminOp::FaultMdDup:
            case AdminOp::FaultMdReorder:
            case AdminOp::FaultMdRate:
            case AdminOp::FaultMdOff: execute_market_data_fault(command, reply); return;
            case AdminOp::FaultUserReject:
            case AdminOp::FaultUserDelay:
            case AdminOp::FaultUserDropOut:
            case AdminOp::FaultUserDisconnect:
            case AdminOp::FaultUserOff: execute_user_fault(command, reply); return;
            case AdminOp::Bots:
                set_bots_enabled(command.on);
                append_line(reply, std::string("bots ") + (command.on ? "on" : "off") + " (" +
                                       std::to_string(bot_entries_.size()) + " configured)");
                append_ok(reply);
                return;
            case AdminOp::Shutdown:
                if (shutdown_) {
                    append_line(reply, "already shut down");
                } else {
                    shutdown();
                    append_line(reply, shutdown_close_result_ == SubmitResult::Applied
                                           ? "all instruments closed"
                                           : "SetPhase Closed not applied: " +
                                                 std::string(to_string(shutdown_close_result_)));
                }
                append_ok(reply);
                return;
        }
        append_error(reply, "unsupported admin command");
    }

    // ---- Lifecycle -------------------------------------------------------------

    // End of day: SetPhase Closed for every instrument through the normal
    // submit path (journaled), then the optional on_shutdown hooks, then a
    // journal flush; shutdown_requested() turns true. Idempotent. The
    // SetPhase is stamped inside the trading day (clamped to its last
    // nanosecond) so the closing batch is representable on the wires.
    void shutdown() noexcept {
        shutdown_pending_ = true;
        if (!dispatching_) {
            finish_outer_operation();
        }
    }

    [[nodiscard]] bool shutdown_requested() const noexcept { return shutdown_; }

    void set_bots_enabled(bool enabled) noexcept {
        bots_.set_enabled(enabled);
        bots_enabled_ = enabled;
    }
    [[nodiscard]] bool bots_enabled() const noexcept { return bots_enabled_; }
    [[nodiscard]] std::span<const BotEntry> bots() const noexcept { return bot_entries_; }

    // Writes buffered journal records and a valid footer. false: journaling
    // disabled or failed (the failure policy has then been applied).
    bool flush_journal() noexcept {
        if (!journal_ || journal_failed_) {
            return false;
        }
        if (!journal_->flush()) {
            on_journal_failure();
            finish_outer_operation();
            return false;
        }
        return true;
    }

    [[nodiscard]] JournalState journal_state() const noexcept {
        return !journal_ ? JournalState::Disabled : journal_failed_ ? JournalState::Failed : JournalState::Healthy;
    }

    // ---- Queries -----------------------------------------------------------------

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CoreCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t event_buffer_capacity() const noexcept { return events_.capacity(); }
    [[nodiscard]] Timestamp last_command_timestamp() const noexcept { return last_command_ts_; }

    [[nodiscard]] const UserConfig* user(SessionId session) const noexcept {
        const UserRuntime* runtime = user_for_session(session);
        return runtime == nullptr ? nullptr : runtime->config;
    }
    [[nodiscard]] bool is_bound(SessionId session) const noexcept {
        const UserRuntime* runtime = user_for_session(session);
        return runtime != nullptr && runtime->bound;
    }
    [[nodiscard]] std::optional<ConnectionId> connection(SessionId session) const noexcept {
        const UserRuntime* runtime = user_for_session(session);
        if (runtime == nullptr || !runtime->bound) {
            return std::nullopt;
        }
        return runtime->connection;
    }
    [[nodiscard]] std::size_t bound_connection_count() const noexcept { return bound_connection_count_; }
    [[nodiscard]] bool is_bot_session(SessionId session) const noexcept {
        const UserRuntime* runtime = user_for_session(session);
        return runtime != nullptr && runtime->bot_owned;
    }

private:
    struct UserRuntime {
        const UserConfig* config = nullptr;
        ConnectionId connection = 0;
        bool bound = false;
        bool bot_owned = false;
        UserFaultState faults{};
    };

    [[nodiscard]] UserRuntime* user_for_session(SessionId session) noexcept {
        const std::uint16_t index = user_index_by_session_[session];
        return index == 0 ? nullptr : &users_[index - 1];
    }
    [[nodiscard]] const UserRuntime* user_for_session(SessionId session) const noexcept {
        const std::uint16_t index = user_index_by_session_[session];
        return index == 0 ? nullptr : &users_[index - 1];
    }
    [[nodiscard]] UserRuntime* user_for_name(std::string_view username) noexcept {
        for (UserRuntime& user : users_) {
            if (user.config->username == username) {
                return &user;
            }
        }
        return nullptr;
    }

    void add_configured_bots() {
        for (const BotSectionConfig& bot : config_.bots) {
            UserRuntime* owner = user_for_name(bot.user);
            const sim::bots::BotConfig& identity = bot.identity();
            if (owner == nullptr || owner->bot_owned || owner->config->session_id != identity.session_id ||
                engine_.instrument(identity.instrument) == nullptr) {
                throw std::invalid_argument("bot " + bot.name + ": unresolved user or instrument");
            }
            const sim::bots::BotState* state = nullptr;
            if (bot.type == BotType::MarketMaker) {
                state = bots_.add(bot.market_maker);
            } else {
                state = bots_.add(bot.noise_taker);
            }
            if (state == nullptr) {
                throw std::invalid_argument("bot " + bot.name + " rejected by BotSet");
            }
            owner->bot_owned = true;
            bot_entries_.push_back(BotEntry{bot.name, bot.type, bot.user, bot.instrument_symbol, state});
        }
    }

    // Step 1: strictly increasing day-relative stamp.
    [[nodiscard]] Timestamp next_command_stamp() const noexcept {
        const Timestamp clock_now = clock_.day_ns();
        return any_command_stamped_ && clock_now <= last_command_ts_ ? last_command_ts_ + 1 : clock_now;
    }

    static void set_command_timestamp(Command& command, Timestamp stamp) noexcept {
        switch (command.type) {
            case CommandType::NewOrder: command.new_order.ts = stamp; return;
            case CommandType::Cancel: command.cancel.ts = stamp; return;
            case CommandType::Replace: command.replace.ts = stamp; return;
            case CommandType::MassCancel: command.mass_cancel.ts = stamp; return;
            case CommandType::SetPhase: command.set_phase.ts = stamp; return;
        }
    }

    // Steps 1-5 for one command at `stamp`.
    SubmitResult apply(Command command, Timestamp stamp) noexcept {
        if (stamp < day_start_ || stamp >= day_end_) {
            ++counters_.commands_outside_trading_day;
            return SubmitResult::OutsideTradingDay;
        }
        set_command_timestamp(command, stamp);
        // Step 2: the previous batch has been dispatched, so drain = clear;
        // the full capacity is now available to this command.
        events_.clear();
        if (engine_.handle(command, events_) != HandleResult::Applied) {
            // Engine and buffer are unchanged and the buffer is already empty:
            // a retry would fail identically. Only a buffer configured below
            // the engine's bound (CoreOptions, tests) gets here.
            ++counters_.output_capacity_failures;
            return SubmitResult::OutputCapacityExceeded;
        }
        last_command_ts_ = stamp;
        any_command_stamped_ = true;
        ++counters_.commands_applied;
        journal_applied_command(command);
        dispatch_batch();
        return SubmitResult::Applied;
    }

    void journal_applied_command(const Command& command) noexcept {
        if (!journal_ || journal_failed_) {
            return;
        }
        if (journal_->append(command)) {
            ++counters_.journal_records;
            return;
        }
        on_journal_failure();
    }

    void on_journal_failure() noexcept {
        journal_failed_ = true;
        ++counters_.journal_failures;
        if (config_.journal_failure_policy == JournalFailurePolicy::Shutdown && !shutdown_) {
            shutdown_pending_ = true;  // run once the current batch is fully dispatched
        }
    }

    // Steps 4-5: sessions (once each, first appearance, bots excluded), then
    // market data on_events + flush, then bots.
    void dispatch_batch() noexcept {
        if (events_.empty()) {
            return;
        }
        const std::span<const Event> batch(events_.begin(), events_.size());
        dispatching_ = true;
        for (const Event& event : batch) {
            visit_event_sessions(event, [this](SessionId session) {
                if (!is_bot_session(session)) {
                    dispatch_set_.add(session);
                }
            });
        }
        for (const SessionId session : dispatch_set_.sessions()) {
            sessions_.on_events(session, batch, *this);
            ++counters_.session_dispatches;
        }
        dispatch_set_.clear();
        market_data_.on_events(batch);
        market_data_.flush();
        bots_.on_events(batch);
        ++counters_.batches_dispatched;
        dispatching_ = false;
    }

    // Applies deferred commands in order, then a pending shutdown, until
    // neither is left.
    void finish_outer_operation() noexcept {
        for (;;) {
            Command deferred_command;
            while (deferred_.pop(deferred_command)) {
                (void)apply(deferred_command, next_command_stamp());
            }
            if (!shutdown_pending_) {
                return;
            }
            shutdown_pending_ = false;
            run_shutdown();
        }
    }

    void run_shutdown() noexcept {
        if (shutdown_) {
            return;
        }
        Timestamp stamp = std::max(next_command_stamp(), day_start_);
        if (stamp >= day_end_) {
            stamp = day_end_ - 1;
        }
        if (!any_command_stamped_ || stamp > last_command_ts_) {
            SetPhase close_all{};
            close_all.instrument = 0;
            close_all.phase = Phase::Closed;
            shutdown_close_result_ = apply(Command{close_all}, stamp);
        } else {
            shutdown_close_result_ = SubmitResult::OutsideTradingDay;
        }
        shutdown_ = true;
        if constexpr (SessionsShutdown<Sessions, ExchangeCore>) {
            sessions_.on_shutdown(stamp, *this);
        }
        if constexpr (MarketDataShutdown<MarketData>) {
            market_data_.on_shutdown(stamp);
        }
        if (journal_ && !journal_failed_ && !journal_->flush()) {
            on_journal_failure();
        }
    }

    void clear_disconnect_request(UserRuntime& user) noexcept {
        if (user.faults.disconnect_requested) {
            user.faults.disconnect_requested = false;
            --disconnect_requests_pending_;
        }
    }

    // ---- admin helpers ------------------------------------------------------------

    [[nodiscard]] InstrumentId admin_instrument(std::string_view symbol) const noexcept {
        if (symbol.empty() || symbol.size() > Symbol{}.chars.size()) {
            return 0;
        }
        return engine_.find_instrument(Symbol(symbol));
    }

    void execute_phase(const AdminCommand& command, std::string& reply) {
        if (shutdown_) {
            append_error(reply, "exchange is shut down");
            return;
        }
        InstrumentId target = 0;
        if (command.symbol != "*") {
            target = admin_instrument(command.symbol);
            if (target == 0) {
                append_error(reply, "unknown instrument " + command.symbol);
                return;
            }
        }
        const std::size_t count = engine_.instrument_count();
        std::vector<Phase> before(count + 1, Phase::Closed);
        for (std::size_t id = 1; id <= count; ++id) {
            before[id] = engine_.phase(static_cast<InstrumentId>(id));
        }
        SetPhase set_phase{};
        set_phase.instrument = target;
        set_phase.phase = command.phase;
        const SubmitResult result = submit(Command{set_phase});
        if (result != SubmitResult::Applied) {
            append_error(reply, std::string("phase not applied: ") + std::string(to_string(result)));
            return;
        }
        for (std::size_t id = 1; id <= count; ++id) {
            if (target != 0 && id != target) {
                continue;
            }
            const auto instrument_id = static_cast<InstrumentId>(id);
            std::string line(engine_.instrument(instrument_id)->symbol.view());
            line += ' ';
            line += to_string(before[id]);
            line += " -> ";
            line += to_string(engine_.phase(instrument_id));
            append_line(reply, line);
        }
        append_ok(reply);
    }

    void execute_market_data_fault(const AdminCommand& command, std::string& reply) {
        if constexpr (MarketDataPacketFaults<MarketData>) {
            sim::PacketFaultInjector& injector = market_data_.packet_faults();
            switch (command.op) {
                case AdminOp::FaultMdDrop: injector.drop_next(command.count); break;
                case AdminOp::FaultMdDup: injector.duplicate_next(command.count); break;
                case AdminOp::FaultMdReorder: {
                    sim::PacketFaultConfig with_delay = injector.config();
                    with_delay.reorder_delay = command.delay;
                    injector.configure(with_delay);
                    injector.reorder_next(command.count);
                    break;
                }
                case AdminOp::FaultMdRate: injector.configure(command.rate); break;
                case AdminOp::FaultMdOff:
                    if constexpr (MarketDataPacketFaultReset<MarketData>) {
                        market_data_.reset_packet_faults();
                    } else {
                        injector.configure(sim::PacketFaultConfig{});
                        if (!injector.idle()) {
                            append_line(reply, "note: pending one-shot faults or held packets remain "
                                               "(market data exposes no reset_packet_faults)");
                        }
                    }
                    break;
                default: break;
            }
            append_line(reply, "md faults " + describe_packet_faults(market_data_.packet_faults()));
            append_ok(reply);
        } else {
            (void)command;
            append_error(reply, "market data exposes no packet fault injector");
        }
    }

    void execute_user_fault(const AdminCommand& command, std::string& reply) {
        UserRuntime* user = user_for_name(command.user);
        if (user == nullptr) {
            append_error(reply, "unknown user " + command.user);
            return;
        }
        if (user->bot_owned) {
            append_error(reply, "user " + command.user + " is driven by a bot; gateway faults do not apply");
            return;
        }
        UserFaultState& faults = user->faults;
        switch (command.op) {
            case AdminOp::FaultUserReject: faults.reject_next_new_orders = command.count; break;
            case AdminOp::FaultUserDelay: faults.inbound_delay_ns = std::uint64_t{command.delay} * 1000; break;
            case AdminOp::FaultUserDropOut: faults.drop_next_outbound = command.count; break;
            case AdminOp::FaultUserDisconnect:
                if (!user->bound) {
                    append_error(reply, "user " + command.user + " is not connected");
                    return;
                }
                if (!faults.disconnect_requested) {
                    faults.disconnect_requested = true;
                    ++disconnect_requests_pending_;
                }
                break;
            case AdminOp::FaultUserOff:
                clear_disconnect_request(*user);
                faults = UserFaultState{};
                break;
            default: break;
        }
        append_line(reply, "user " + command.user + " session=" + std::to_string(user->config->session_id) + " " +
                               describe_user_faults(faults));
        append_ok(reply);
    }

    void append_status(std::string& reply) {
        append_line(reply, "exchange " + config_.exchange.name + " shutdown=" + (shutdown_ ? "yes" : "no"));
        append_line(reply, "trading_day start=" + format_day_time(day_start_) + " end=" + format_day_time(day_end_) +
                               " last_command_ts=" + std::to_string(last_command_ts_));
        std::size_t bot_users = 0;
        for (const UserRuntime& user : users_) {
            bot_users += user.bot_owned ? 1 : 0;
        }
        append_line(reply, "connections bound=" + std::to_string(bound_connection_count_) +
                               " users=" + std::to_string(users_.size()) + " bot_users=" + std::to_string(bot_users));
        for (const UserRuntime& user : users_) {
            std::string line = "user " + user.config->username + " session=" + std::to_string(user.config->session_id) +
                               " protocol=" + std::string(to_string(user.config->protocol));
            if (user.bot_owned) {
                line += " bot=yes";
            } else {
                line += std::string(" bound=") + (user.bound ? "yes connection=" + std::to_string(user.connection) : "no");
                line += " " + describe_user_faults(user.faults);
            }
            append_line(reply, line);
        }
        append_line(reply, "orders live=" + std::to_string(engine_.live_order_count()));
        for (std::size_t id = 1; id <= engine_.instrument_count(); ++id) {
            const auto instrument_id = static_cast<InstrumentId>(id);
            std::string line = "instrument " + std::string(engine_.instrument(instrument_id)->symbol.view()) +
                               " id=" + std::to_string(id) + " phase=" + std::string(to_string(engine_.phase(instrument_id)));
            const auto bid = engine_.best_bid(instrument_id);
            const auto ask = engine_.best_ask(instrument_id);
            const auto last = engine_.last_trade_price(instrument_id);
            line += " bid=" + (bid ? format_price(*bid) : std::string("-"));
            line += " ask=" + (ask ? format_price(*ask) : std::string("-"));
            line += " last=" + (last ? format_price(*last) : std::string("-"));
            append_line(reply, line);
        }
        if constexpr (MarketDataSequence<MarketData>) {
            append_line(reply, "md next_sequence=" + std::to_string(std::as_const(market_data_).next_sequence()));
        } else {
            append_line(reply, "md next_sequence=unavailable");
        }
        if constexpr (MarketDataPacketFaults<MarketData>) {
            append_line(reply, "md faults " + describe_packet_faults(market_data_.packet_faults()));
        } else {
            append_line(reply, "md faults unavailable");
        }
        append_line(reply, std::string("bots ") + (bots_enabled_ ? "on" : "off") +
                               " count=" + std::to_string(bot_entries_.size()));
        for (const BotEntry& bot : bot_entries_) {
            const sim::bots::BotStats& stats = bot.state->stats();
            append_line(reply, "bot " + bot.name + " type=" + std::string(to_string(bot.type)) + " user=" + bot.user +
                                   " instrument=" + bot.instrument_symbol + " enabled=" +
                                   (bot.state->enabled() ? "yes" : "no") + " inventory=" +
                                   std::to_string(bot.state->inventory()) + " orders=" +
                                   std::to_string(bot.state->orders().size()) + " submitted=" +
                                   std::to_string(stats.commands_submitted) + " fills=" +
                                   std::to_string(stats.fills_received) + " rejects=" +
                                   std::to_string(stats.rejects_received));
        }
        append_line(reply, "journal state=" + std::string(to_string(journal_state())) +
                               (config_.journal_path.empty() ? std::string() : " path=" + config_.journal_path) +
                               " records=" + std::to_string(counters_.journal_records) +
                               " failures=" + std::to_string(counters_.journal_failures) +
                               " on_failure=" + std::string(to_string(config_.journal_failure_policy)));
        append_line(reply, "commands applied=" + std::to_string(counters_.commands_applied) +
                               " outside_trading_day=" + std::to_string(counters_.commands_outside_trading_day) +
                               " output_capacity_failures=" + std::to_string(counters_.output_capacity_failures) +
                               " deferred=" + std::to_string(counters_.commands_deferred) +
                               " deferred_overflows=" + std::to_string(counters_.deferred_queue_overflows));
        append_ok(reply);
    }

    // ---- state ----------------------------------------------------------------------

    ServerConfig config_;  // own copy: authenticate() returns pointers into it
    Sessions& sessions_;
    MarketData& market_data_;
    const Clock& clock_;
    MatchingEngine engine_;
    EventBuffer events_;
    std::vector<UserRuntime> users_;
    std::unique_ptr<std::uint16_t[]> user_index_by_session_;  // SessionId -> users_ index + 1, 0 = none
    SessionDispatchSet dispatch_set_;
    DeferredCommandQueue deferred_;
    BotRegistry bots_;
    std::vector<BotEntry> bot_entries_;
    std::optional<sim::JournalWriter> journal_;
    CoreCounters counters_{};

    Timestamp day_start_;
    Timestamp day_end_;
    Timestamp journal_flush_interval_ns_;
    Timestamp last_journal_flush_ = 0;
    Timestamp last_command_ts_ = 0;
    Timestamp timer_now_ = 0;
    mutable Timestamp observed_now_ = 0;
    std::size_t bound_connection_count_ = 0;
    std::size_t disconnect_requests_pending_ = 0;
    SubmitResult shutdown_close_result_ = SubmitResult::Applied;
    bool any_command_stamped_ = false;
    bool dispatching_ = false;
    bool journal_failed_ = false;
    bool shutdown_pending_ = false;
    bool shutdown_ = false;
    bool bots_enabled_ = true;
};

}  // namespace exsim::server

#endif
