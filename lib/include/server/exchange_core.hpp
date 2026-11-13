#ifndef EXSIM_SERVER_EXCHANGE_CORE_HPP
#define EXSIM_SERVER_EXCHANGE_CORE_HPP

// Protocol-independent exchange core of the exsim server. It owns the
// matching engine, the startup-sized EventBuffer, the configured users and
// their session bindings and fault state, and the optional canonical
// journal, and it is the Context every order-entry gateway calls.
//
//   ExchangeCore<Sessions, MarketData, Clock> core(config, sessions, md, clock);
//
// Sessions, MarketData and Clock are policies supplied by the server
// integrator (contracts in server/core/policies.hpp); the core holds
// references to them and calls them without virtual dispatch. `config`
// must outlive the core: authenticate() returns pointers into it.
// Admin commands (status, phase, book, faults, shutdown) run through
// CoreAdmin (server/admin/core_admin.hpp).
//
// Threading: everything here runs on the single server thread.
// Hot path (submit, dispatch, timer, fault queries, bind/unbind): no heap
// allocation, no exceptions, no std::string. Construction is a setup path
// and may allocate or throw.

#include "engine/command.hpp"
#include "engine/event.hpp"
#include "engine/instrument.hpp"
#include "engine/matching_engine.hpp"
#include "md/publisher.hpp"
#include "server/config.hpp"
#include "server/core/deferred_command_queue.hpp"
#include "server/core/policies.hpp"
#include "server/core/session_dispatch_set.hpp"
#include "server/core/user_faults.hpp"
#include "server/journal/journal.hpp"
#include "md/packet_faults.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
    std::uint64_t commands_after_shutdown = 0;       // refused: the exchange has shut down
    std::uint64_t output_capacity_failures = 0;      // InsufficientOutput on an empty buffer
    std::uint64_t commands_deferred = 0;             // submitted during dispatch, applied afterwards
    std::uint64_t deferred_queue_overflows = 0;      // refused: deferred queue full
    std::uint64_t journal_records = 0;               // accepted by JournalWriter::append
    std::uint64_t journal_failures = 0;              // failed append or flush
    std::uint64_t batches_dispatched = 0;            // non-empty batches
    std::uint64_t session_dispatches = 0;            // Sessions::on_events calls
    // Batches the MarketData policy reported as not published (on_events
    // returned a md::PublishStatus other than Ok); 0 for a policy whose
    // on_events returns void.
    std::uint64_t market_data_failed_batches = 0;
    // Last non-Ok md::PublishStatus from on_events, on_timer or on_shutdown.
    md::PublishStatus market_data_last_failure = md::PublishStatus::Ok;
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

// Deferred-queue slots for ordinary commands are users + kDeferredCommandSlack.
// A further one slot per user is reserved for the cancel-on-disconnect
// MassCancel of unbind, so it is never refused.
inline constexpr std::size_t kDeferredCommandSlack = 64;

template <class Core>
class CoreAdmin;

template <typename Sessions, typename MarketData, CoreClock Clock, typename Journal = journal::JournalWriter>
    requires CoreMarketData<MarketData>
class ExchangeCore {
public:
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
          dispatch_set_(events_.capacity()),
          phase_change_by_instrument_(
              std::make_unique<BatchPhaseChange[]>(std::size_t{config.engine.engine.max_instruments} + 1)),
          deferred_(2 * config.users.size() + kDeferredCommandSlack),
          day_start_(config.exchange.trading_day_start_ns),
          day_end_(config.exchange.trading_day_end_ns),
          journal_flush_interval_ns_(Timestamp{config.journal_flush_interval_ms} * 1'000'000) {
        static_assert(CoreSessions<Sessions, ExchangeCore>,
                      "Sessions needs on_events(SessionId, const SessionEvents&, Core&)");
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
        if (!config_.journal_path.empty()) {
            journal_.emplace(config_.journal_path, journal::JournalSetup{config_.engine.engine, config_.instruments});
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

    // ---- Gateway Context ------------------------------------------------------

    // nullptr: unknown user, wrong protocol, wrong password (an empty
    // configured password accepts any).
    [[nodiscard]] const UserConfig* authenticate(std::string_view username, std::string_view password,
                                                 Protocol protocol) const noexcept {
        for (const UserRuntime& user : users_) {
            if (user.config->username != username) {
                continue;
            }
            if (user.config->protocol != protocol) {
                return nullptr;
            }
            if (!user.config->password.empty() && user.config->password != password) {
                return nullptr;
            }
            return user.config;
        }
        return nullptr;
    }

    // false: unknown session, already bound (second login), or
    // called from a dispatch callback (logins come from inbound data; this
    // bounds the queued unbind MassCancels to one per user).
    [[nodiscard]] bool bind(SessionId session, ConnectionId connection) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || user->bound || dispatching_) {
            return false;
        }
        user->bound = true;
        user->connection = connection;
        ++bound_connection_count_;
        return true;
    }

    // Logout or disconnect of `connection`. No-op unless `session` is bound
    // to that connection, so a stale release from an older connection never
    // unbinds a newer login. Clears a pending disconnect request; submits
    // MassCancel{cancel_on_disconnect=true} for the whole session when the
    // user has cancel_on_disconnect (deferred until the current batch is
    // dispatched when called from a callback, using a reserved slot, so it is
    // never refused for a full queue). After shutdown it is either refused
    // (OutsideTradingDay, after the end of the trading day) or applied with
    // nothing to cancel: the closing SetPhase has already cancelled every order.
    void unbind(SessionId session, ConnectionId connection) noexcept {
        UserRuntime* user = user_for_session(session);
        if (user == nullptr || !user->bound || user->connection != connection) {
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
            (void)submit_or_defer(Command{cancel_all}, deferred_.capacity());
        }
    }

    [[nodiscard]] InstrumentId find_instrument(const Symbol& symbol) const noexcept {
        return engine_.find_instrument(symbol);
    }

    [[nodiscard]] const InstrumentConfig* instrument(InstrumentId id) const noexcept { return engine_.instrument(id); }

    // Stamps, applies, journals and dispatches the command. When it returns
    // Applied, every event of the command (and of any command deferred
    // meanwhile) has reached the sessions and market data. Nothing else is
    // applied, journaled or dispatched.
    SubmitResult submit(const Command& command) noexcept {
        return submit_or_defer(command, deferred_.capacity() - users_.size());
    }

    // ns since local midnight: the Clock, never earlier than a value already
    // returned or a stamped command (monotonic even if the Clock steps back).
    [[nodiscard]] Timestamp now() const noexcept {
        observed_now_ = std::max({observed_now_, static_cast<Timestamp>(clock_.day_ns()), last_command_ts_});
        return observed_now_;
    }

    [[nodiscard]] std::uint64_t realtime_ns() const noexcept { return clock_.realtime_ns(); }
    [[nodiscard]] std::uint64_t epoch_ns(Timestamp timestamp) const noexcept {
        if constexpr (requires { clock_.epoch_ns(timestamp); }) return clock_.epoch_ns(timestamp);
        else {
            const auto day = now(), epoch = realtime_ns();
            return timestamp <= day && day - timestamp <= epoch ? epoch - (day - timestamp) : epoch;
        }
    }

    // ---- Timer (server loop, once per iteration) ------------------------------

    // End of trading day check, market-data timer hook, periodic journal
    // flush. `loop_now` is ns since local midnight; the value handed on is
    // made non-decreasing.
    void timer(Timestamp loop_now) noexcept {
        if (dispatching_) {
            return;
        }
        timer_now_ = std::max({loop_now, timer_now_, last_command_ts_});
        const Timestamp now_for_timers = timer_now_;
        if constexpr (requires(const Journal& journal) { journal.healthy(); }) {
            if (journal_ && !journal_failed_ && !journal_->healthy()) on_journal_failure();
        }
        if (!shutdown_ && now_for_timers >= day_end_) {
            shutdown_pending_ = true;  // the trading day is over
        }
        finish_outer_operation();
        if constexpr (MarketDataTimerWithCore<MarketData, ExchangeCore>) {
            (void)call_market_data_hook([&] { return market_data_.on_timer(now_for_timers, std::as_const(*this)); });
        } else if constexpr (MarketDataTimer<MarketData>) {
            (void)call_market_data_hook([&] { return market_data_.on_timer(now_for_timers); });
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

    // Gateway: true while `fault user X drop_out n` has counts left. Does not
    // consume (OUCH closes the connection after the last dropped message).
    [[nodiscard]] bool has_pending_drop_out(SessionId session) const noexcept {
        const UserRuntime* user = user_for_session(session);
        return user != nullptr && user->faults.drop_next_outbound != 0;
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
    // closed now (config order) and clears those requests. Close each one's
    // connection and let the gateway's disconnect handler unbind it (with its
    // own ConnectionId; cancel-on-disconnect applies). Returns the count.
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

    // Requests a flush. With the synchronous writer this writes the footer;
    // with the runtime worker journal_drained() acknowledges completion.
    // false: disabled or failed (the failure policy has then been applied).
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

    [[nodiscard]] bool journal_drained() const noexcept {
        if constexpr (requires(const Journal& journal) { journal.drained(); }) {
            return !journal_ || journal_failed_ || journal_->drained();
        }
        return true;
    }

    // The runtime's drain deadline expired. The journal is incomplete and
    // must be reported as failed even if a blocked write later completes.
    void abort_journal() noexcept {
        if (!journal_ || journal_failed_) return;
        if constexpr (requires(Journal& journal) { journal.abort(); }) journal_->abort();
        on_journal_failure();
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

    // During dispatch: the phase the current batch's PhaseChanged for
    // `instrument` left; Halted when the batch changes no phase of it. An
    // uncross Fill (aggressor None) precedes that PhaseChanged in the batch,
    // so this tells a gateway which call phase the cross ended.
    [[nodiscard]] Phase phase_left_by_batch(InstrumentId instrument) const noexcept {
        const BatchPhaseChange& change = phase_change_by_instrument_[instrument];
        return change.batch_number == counters_.batches_dispatched + 1 ? change.old_phase : Phase::Halted;
    }

private:
    template <class Core>
    friend class CoreAdmin;

    // PhaseChanged of batch `batch_number` (counters_.batches_dispatched + 1
    // while it is dispatched); older entries are stale.
    struct BatchPhaseChange {
        std::uint64_t batch_number = 0;
        Phase old_phase = Phase::Halted;
    };

    struct UserRuntime {
        const UserConfig* config = nullptr;
        ConnectionId connection = 0;
        bool bound = false;
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

    // Outside dispatch: applies now. During dispatch: queues the command if
    // the queue holds fewer than `deferred_limit` commands. Ordinary submits
    // pass capacity - users; unbind passes the whole capacity, so its
    // MassCancel (at most one per user, bind being refused during dispatch)
    // always fits.
    SubmitResult submit_or_defer(const Command& command, std::size_t deferred_limit) noexcept {
        if (dispatching_) {
            if (deferred_.size() >= deferred_limit || !deferred_.push(command)) {
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

    // Records a md::PublishStatus a MarketData hook returned (policies whose
    // hooks return void report nothing). Returns true on a failure.
    template <typename Status>
    bool observe_market_data_status(const Status& status) noexcept {
        if constexpr (std::same_as<Status, md::PublishStatus>) {
            if (status != md::PublishStatus::Ok) {
                counters_.market_data_last_failure = status;
                return true;
            }
        }
        return false;
    }

    template <typename Call>
    bool call_market_data_hook(Call&& call) noexcept {
        if constexpr (std::is_void_v<decltype(call())>) {
            call();
            return false;
        } else {
            return observe_market_data_status(call());
        }
    }

    // Applies, journals and dispatches one command at `stamp`.
    SubmitResult apply(Command command, Timestamp stamp) noexcept {
        if (shutdown_) {
            // Nothing trades after the closing batch: refuse instead of
            // letting the engine reject in Closed (and journaling that).
            ++counters_.commands_after_shutdown;
            return SubmitResult::ExchangeShutDown;
        }
        if (stamp < day_start_ || stamp >= day_end_) {
            ++counters_.commands_outside_trading_day;
            return SubmitResult::OutsideTradingDay;
        }
        set_command_timestamp(command, stamp);
        // The previous batch has been dispatched, so drain = clear; the full
        // capacity is now available to this command.
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

    // Sessions first (once each, first appearance order, each with only its
    // own events), then market data on_events + flush.
    void dispatch_batch() noexcept {
        if (events_.empty()) {
            return;
        }
        const std::span<const Event> batch(events_.begin(), events_.size());
        dispatching_ = true;
        const std::uint64_t batch_number = counters_.batches_dispatched + 1;
        for (const Event& event : batch) {
            if (event.type == EventType::PhaseChanged) {
                phase_change_by_instrument_[event.instrument] = {batch_number, event.phase_changed.old_phase};
            }
            dispatch_set_.count_sessions(event);
        }
        dispatch_set_.place_events(batch);
        for (std::size_t order = 0; order < dispatch_set_.session_count(); ++order) {
            sessions_.on_events(dispatch_set_.session(order), dispatch_set_.events_of(order, batch), *this);
            ++counters_.session_dispatches;
        }
        dispatch_set_.clear();
        if (call_market_data_hook([&] { return market_data_.on_events(batch); })) {
            ++counters_.market_data_failed_batches;
        }
        market_data_.flush();
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
            (void)call_market_data_hook([&] { return market_data_.on_shutdown(stamp); });
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

    // ---- state ----------------------------------------------------------------------

    const ServerConfig& config_;
    Sessions& sessions_;
    MarketData& market_data_;
    const Clock& clock_;
    MatchingEngine engine_;
    EventBuffer events_;
    std::vector<UserRuntime> users_;
    std::unique_ptr<std::uint16_t[]> user_index_by_session_;  // SessionId -> users_ index + 1, 0 = none
    SessionDispatchSet dispatch_set_;
    std::unique_ptr<BatchPhaseChange[]> phase_change_by_instrument_;  // index = InstrumentId
    DeferredCommandQueue deferred_;
    std::optional<Journal> journal_;
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
};

}  // namespace exsim::server

#endif
