#ifndef EXSIM_GATEWAY_OUCH_OUCH_SERVER_HPP
#define EXSIM_GATEWAY_OUCH_OUCH_SERVER_HPP

// Server side of OUCH 4.2 over SoupBinTCP 3.0 (docs/GATEWAYS.md section 2,
// docs/SERVER.md section 2). Pure state machines over byte buffers:
//
//   OuchUserState<Sink>           one per configured OUCH user for the whole
//                                 trading day: token table, per-order
//                                 instructions, sequenced store, OUCH
//                                 application mapping both ways.
//   OuchUserStateDirectory<Sink>  every OUCH user's state by session; also the
//                                 server::ExchangeCore `Sessions` policy.
//   OuchConnection<Sink>          one per TCP connection: SoupBinTCP framing,
//                                 login, replay, heartbeats, idle timeout,
//                                 logout.
//
// All are templated on the byte sink (byte_sink.hpp); their entry points are
// templated on the server Context (OuchServerContext below), which
// server::ExchangeCore satisfies. No virtual dispatch, no exceptions, no
// std::string and no heap allocation after construction: every buffer is
// fixed or sized from configuration.
//
// Driving contract for the server integrator:
//   * build one OuchUserStateDirectory for the day and give it to the core as
//     its Sessions (or call its on_events/on_shutdown from your own Sessions).
//     The core then calls on_events(session, batch, core) once per distinct
//     session of each batch (SERVER.md 2.3) and on_shutdown at end of day.
//   * per accepted TCP connection, construct an OuchConnection with that
//     directory; connection.on_data(bytes, core) consumes every byte.
//   * call connection.on_timer(core) periodically (heartbeat, idle timeout,
//     paused replay) and connection.on_writable(core) after send space frees.
//   * when connection.close_requested(): close the socket (after flushing if
//     flush_before_close()) and then call connection.on_disconnect(core). Also
//     call on_disconnect(core) when the socket drops on its own, or for the
//     core's take_disconnect_requests. on_disconnect detaches the user and
//     calls core.unbind (cancel-on-disconnect).
//   * on_events never calls back into submit/bind/unbind, so it is safe to run
//     during dispatch; closes it causes are completed by the caller later.
//   * fault injection is read from the Context (consume_injected_reject,
//     consume_drop_out); `fault user X delay` belongs to the server loop.

#include "exsim/command.hpp"
#include "exsim/event.hpp"
#include "exsim/gateway/ouch/byte_sink.hpp"
#include "exsim/gateway/ouch/ouch_mapping.hpp"
#include "exsim/gateway/sequenced_store.hpp"
#include "exsim/gateway/token_table.hpp"
#include "exsim/instrument.hpp"
#include "exsim/protocol/ouch42.hpp"
#include "exsim/protocol/soupbintcp.hpp"
#include "exsim/server/config.hpp"
#include "exsim/server/core/policies.hpp"
#include "exsim/types.hpp"
#include "lob/compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace exsim::gateway::ouch {

// Longest sequenced OUCH message the server sends (Order Replaced) and its
// SoupBinTCP frame (2-byte length + 'S').
inline constexpr std::size_t kMaxOutboundOuchPayloadBytes = protocol::ouch42::OrderReplaced::wire_size;
inline constexpr std::size_t kMaxFramedOutboundBytes = kMaxOutboundOuchPayloadBytes + 3;

// SoupBinTCP recommends treating 15 s without any inbound packet as a lost link.
inline constexpr std::uint32_t kDefaultClientIdleTimeoutMs = 15'000;

struct OuchSessionSettings {
    std::array<char, 10> soup_session = protocol::wire::spaces<10>();
    Timestamp server_heartbeat_interval_ns = 1'000'000'000;
    // No inbound packet (data, heartbeat or login) for this long closes the
    // connection. Before login it is also the login deadline.
    Timestamp client_idle_timeout_ns = Timestamp{kDefaultClientIdleTimeoutMs} * 1'000'000;

    [[nodiscard]] static OuchSessionSettings from(const server::ExchangeSection& exchange,
                                                  std::uint32_t client_idle_timeout_ms = kDefaultClientIdleTimeoutMs) noexcept {
        OuchSessionSettings settings;
        const std::string_view session = std::string_view(exchange.soup_session).substr(0, settings.soup_session.size());
        protocol::wire::alpha(settings.soup_session.data(), settings.soup_session.size(), session);
        settings.server_heartbeat_interval_ns = Timestamp{exchange.heartbeat_interval_ms} * 1'000'000;
        settings.client_idle_timeout_ns = Timestamp{client_idle_timeout_ms} * 1'000'000;
        return settings;
    }
};

struct OuchUserStateLimits {
    std::uint32_t max_tokens = 1u << 20;              // day-used tokens (O + replacement tokens)
    std::size_t store_messages = 1u << 20;            // sequenced messages kept for the day
    std::size_t store_bytes = std::size_t{64} << 20;  // their framed bytes
    // New O/U requests are rejected at the gateway (J 'O') once the store has
    // this many or fewer free messages (or bytes for that many maximal
    // messages), keeping room for reports on orders that are already live.
    std::size_t store_reserve_messages = 1024;

    [[nodiscard]] static OuchUserStateLimits from(const server::EngineSection& engine) noexcept {
        OuchUserStateLimits limits;
        limits.max_tokens = engine.max_tokens_per_session;
        limits.store_messages = engine.store_messages_per_session;
        limits.store_bytes = engine.store_bytes_per_session;
        limits.store_reserve_messages = std::min<std::size_t>(1024, engine.store_messages_per_session / 16);
        return limits;
    }
};

struct OuchUserStatistics {
    std::uint64_t duplicate_tokens_ignored = 0;        // O/U with a token already used today
    std::uint64_t unknown_token_requests_ignored = 0;  // U/X naming a token never used today
    std::uint64_t malformed_messages_ignored = 0;      // unknown OUCH type or wrong length
    std::uint64_t gateway_rejects = 0;                 // J sent without reaching the engine
    std::uint64_t messages_lost_to_store_overflow = 0;
    std::uint64_t outbound_encode_failures = 0;
    std::uint64_t outbound_messages_dropped_by_fault = 0;
};

enum class OuchConnectionState : std::uint8_t { AwaitingLogin, LoggedIn, Closed };

enum class OuchCloseReason : std::uint8_t {
    None,
    LoginRejected,      // Login Rejected sent
    ProtocolViolation,  // bad framing, unknown packet type, data before login, second login
    ClientLogout,       // Logout Request received
    ClientIdleTimeout,  // nothing received for client_idle_timeout_ns
    SendBufferFull,     // sink.prepare failed for live output (SERVER.md 2.4)
    StoreFull,          // the user's sequenced store overflowed; End of Session sent
    EndOfSession,       // end_session(): End of Session sent
    TransportClosed,    // on_disconnect() without an earlier close request
};

template <OuchByteSink ByteSink>
class OuchUserState;
template <OuchByteSink ByteSink>
class OuchConnection;

// What the server must provide: SERVER.md 2.1 plus the per-user fault queries
// of SERVER.md 4. server::ExchangeCore satisfies it. unbind triggers
// cancel-on-disconnect when the user is configured for it; submit stamps the
// command, runs the engine and dispatches the batch to the sessions before
// returning. submit returns void (always applied) or server::SubmitResult.
template <class Context>
concept OuchServerContext = requires(Context& context, const Context& const_context, std::string_view text,
                                     SessionId session, server::ConnectionId connection, const Symbol& symbol,
                                     InstrumentId instrument, const Command& command) {
    { context.authenticate(text, text, server::Protocol::Ouch42) } -> std::same_as<const server::UserConfig*>;
    { context.bind(session, connection) } -> std::same_as<bool>;
    context.unbind(session);
    { const_context.find_instrument(symbol) } -> std::same_as<InstrumentId>;
    { const_context.instrument(instrument) } -> std::same_as<const InstrumentConfig*>;
    requires std::is_void_v<decltype(context.submit(command))> ||
                 std::same_as<decltype(context.submit(command)), server::SubmitResult>;
    { const_context.now() } -> std::same_as<Timestamp>;
    { context.consume_injected_reject(session) } -> std::same_as<bool>;
    { context.consume_drop_out(session) } -> std::same_as<bool>;
};

// No gateway reject: the command reached the engine (or will, if deferred).
inline constexpr char kCommandApplied = '\0';

// Submits `command`. Returns the OUCH reject reason for a command the Context
// reports it did not apply (outside the trading day: 'C'), else kCommandApplied.
template <class Context>
[[nodiscard]] char submit_to_engine(Context& context, const Command& command) noexcept {
    if constexpr (std::is_void_v<decltype(context.submit(command))>) {
        context.submit(command);
        return kCommandApplied;
    } else {
        switch (context.submit(command)) {
            case server::SubmitResult::Applied:
            case server::SubmitResult::Deferred: return kCommandApplied;
            case server::SubmitResult::OutsideTradingDay:
            case server::SubmitResult::ExchangeShutDown: return kOuchRejectExchangeClosed;
            case server::SubmitResult::OutputCapacityExceeded:
            case server::SubmitResult::DeferredQueueFull: return kOuchRejectOther;
        }
        return kOuchRejectOther;
    }
}

// ============================================================================
// OuchUserState
// ============================================================================

template <OuchByteSink ByteSink>
class OuchUserState {
public:
    OuchUserState(const server::UserConfig& user, const OuchUserStateLimits& limits)
        : session_id_(user.session_id),
          owner_(user.owner),
          self_trade_prevention_(user.stp),
          tokens_(limits.max_tokens),
          instructions_by_cl_ord_id_(std::make_unique<OuchOrderInstructions[]>(std::size_t{tokens_.max_tokens()} + 1)),
          store_(limits.store_messages, limits.store_bytes),
          store_reserve_messages_(limits.store_reserve_messages) {}

    ~OuchUserState() { assert(attached_connection_ == nullptr); }

    // Connections keep a pointer to their user state: construct in place.
    OuchUserState(const OuchUserState&) = delete;
    OuchUserState& operator=(const OuchUserState&) = delete;
    OuchUserState(OuchUserState&&) = delete;
    OuchUserState& operator=(OuchUserState&&) = delete;

    [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }

    // Encodes the events of this session in batch order (GATEWAYS.md 2,
    // outbound table), stores each message and writes it to the attached
    // connection. Safe during dispatch: never calls back into the context
    // except the read-only instrument() and now().
    template <class Context>
        requires OuchServerContext<Context>
    void on_events(std::span<const Event> batch, Context& context) noexcept {
        AuctionPhaseLookup auction_phase_lookup;
        for (std::size_t index = 0; index < batch.size(); ++index) {
            const Event& event = batch[index];
            switch (event.type) {
                case EventType::Accepted:
                    if (event.accepted.session == session_id_) append_order_accepted(event, context);
                    break;
                case EventType::Rejected:
                    if (event.rejected.session == session_id_) append_engine_reject(event, context);
                    break;
                case EventType::Fill: {
                    const FillEvent& fill = event.fill;
                    if (fill.buy.session != session_id_ && fill.sell.session != session_id_) break;
                    const Phase phase_being_left = fill.aggressor == Aggressor::None
                                                       ? auction_phase_lookup.phase_being_left(batch, index)
                                                       : Phase::Continuous;
                    // Buy leg before sell leg, also when both legs are ours.
                    if (fill.buy.session == session_id_) {
                        append_order_executed(event, fill.buy,
                                              ouch_liquidity_flag(fill.aggressor, Side::BUY, phase_being_left), context);
                    }
                    if (fill.sell.session == session_id_) {
                        append_order_executed(event, fill.sell,
                                              ouch_liquidity_flag(fill.aggressor, Side::SELL, phase_being_left), context);
                    }
                    break;
                }
                case EventType::Canceled:
                    if (event.canceled.session == session_id_) append_order_canceled(event, context);
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == session_id_) append_order_replaced(event, context);
                    break;
                case EventType::Rested:
                case EventType::MassCancelDone:
                case EventType::PhaseChanged:
                case EventType::AuctionSummary: break;
            }
        }
    }

    // System Event 'E' (end of day) stamped `end_of_day`. Call after the
    // closing SetPhase batch (OuchUserStateDirectory::on_shutdown does).
    template <class Context>
        requires OuchServerContext<Context>
    void append_end_of_day_event(Timestamp end_of_day, Context& context) noexcept {
        append_system_event(kOuchSystemEventEndOfDay, end_of_day, context);
    }

    // Start of a new trading day: forget tokens, instructions and messages.
    // Precondition: no connection attached.
    void reset_for_new_trading_day() noexcept {
        assert(attached_connection_ == nullptr);
        tokens_.clear();
        store_.clear();
        start_of_day_sent_ = false;
        store_overflowed_ = false;
    }

    [[nodiscard]] const SequencedStore& store() const noexcept { return store_; }
    [[nodiscard]] const TokenTable& tokens() const noexcept { return tokens_; }
    [[nodiscard]] ClOrdId find_cl_ord_id(std::string_view token) const noexcept { return tokens_.find(token); }
    [[nodiscard]] bool has_attached_connection() const noexcept { return attached_connection_ != nullptr; }
    [[nodiscard]] const OuchConnection<ByteSink>* attached_connection() const noexcept { return attached_connection_; }
    [[nodiscard]] bool store_overflowed() const noexcept { return store_overflowed_; }
    [[nodiscard]] bool start_of_day_sent() const noexcept { return start_of_day_sent_; }
    [[nodiscard]] const OuchUserStatistics& statistics() const noexcept { return statistics_; }

private:
    friend class OuchConnection<ByteSink>;
    using OuchToken = std::array<char, 14>;

    // ---- Called by OuchConnection ------------------------------------------

    void attach_connection(OuchConnection<ByteSink>* connection) noexcept { attached_connection_ = connection; }
    void detach_connection(const OuchConnection<ByteSink>* connection) noexcept {
        if (attached_connection_ == connection) attached_connection_ = nullptr;
    }

    // One OUCH message from a SoupBinTCP Unsequenced Data packet.
    template <class Context>
    void handle_application_message(std::string_view payload, Context& context) noexcept {
        if (LOB_UNLIKELY(payload.empty())) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        switch (payload.front()) {
            case protocol::ouch42::EnterOrder::type: handle_enter_order(payload, context); return;
            case protocol::ouch42::ReplaceOrder::type: handle_replace_order(payload, context); return;
            case protocol::ouch42::CancelOrder::type: handle_cancel_order(payload, context); return;
            case protocol::ouch42::ModifyOrder::type: handle_modify_order(payload, context); return;
            default: ++statistics_.malformed_messages_ignored; return;
        }
    }

    // System Event 'S' once per trading day, right after the first login.
    template <class Context>
    void append_start_of_day_if_needed(Context& context) noexcept {
        if (start_of_day_sent_) return;
        start_of_day_sent_ = true;
        append_system_event(kOuchSystemEventStartOfDay, context.now(), context);
    }

    // Fault injection (SERVER.md 4, `fault user X drop_out n`): true if the
    // next live sequenced message must stay stored but not be transmitted.
    template <class Context>
    [[nodiscard]] bool consume_outbound_drop(Context& context) noexcept {
        if (LOB_LIKELY(!context.consume_drop_out(session_id_))) return false;
        ++statistics_.outbound_messages_dropped_by_fault;
        return true;
    }

    // ---- Inbound mapping (GATEWAYS.md 2, inbound table) ---------------------

    template <class Context>
    void handle_enter_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::EnterOrder request;
        const protocol::Status decoded = protocol::ouch42::decode(payload.data(), payload.size(), request);
        // invalid_field is an unknown side; the codec has already copied the token.
        if (decoded != protocol::Status::ok && decoded != protocol::Status::invalid_field) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        const TokenTable::InternResult reservation = tokens_.intern(token_view(request.token));
        if (reservation.status == TokenTable::InternStatus::Existing) {
            ++statistics_.duplicate_tokens_ignored;  // day-unique tokens: reuse is suppressed silently
            return;
        }
        if (reservation.status != TokenTable::InternStatus::Inserted) {
            reject_at_gateway(request.token, kOuchRejectOther, context);  // token table full
            return;
        }
        if (decoded == protocol::Status::invalid_field) {
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        if (context.consume_injected_reject(session_id_)) {  // `fault user X reject n`
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        if (!store_admits_new_requests()) {
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        const InstrumentId instrument = context.find_instrument(request.stock);
        if (instrument == 0) {
            reject_at_gateway(request.token, kOuchRejectInvalidStock, context);
            return;
        }
        if (!enter_order_instructions_supported(request)) {
            reject_at_gateway(request.token, kOuchRejectOther, context);
            return;
        }
        instructions_by_cl_ord_id_[reservation.id] = instructions_of(request);

        NewOrder order;
        order.instrument = instrument;
        order.session = session_id_;
        order.owner = owner_;
        order.cl_ord_id = reservation.id;
        order.side = engine_side(request.side);
        order.type = request.price == kOuchMarketOrderPrice ? OrdType::Market : OrdType::Limit;
        order.tif = engine_time_in_force(request.time_in_force);
        order.stp = self_trade_prevention_;
        order.post_only = display_is_post_only(request.display);
        order.price = request.price;
        order.qty = request.shares;
        const char not_applied_reason = submit_to_engine(context, Command{order});
        if (LOB_UNLIKELY(not_applied_reason != kCommandApplied)) {
            reject_at_gateway(request.token, not_applied_reason, context);
        }
    }

    template <class Context>
    void handle_replace_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::ReplaceOrder request;
        if (protocol::ouch42::decode(payload.data(), payload.size(), request) != protocol::Status::ok) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        const ClOrdId existing_id = tokens_.find(token_view(request.existing_token));
        const TokenTable::InternResult reservation = tokens_.intern(token_view(request.replacement_token));
        if (reservation.status == TokenTable::InternStatus::Existing) {
            ++statistics_.duplicate_tokens_ignored;
            return;
        }
        if (reservation.status != TokenTable::InternStatus::Inserted) {
            reject_at_gateway(request.replacement_token, kOuchRejectOther, context);
            return;
        }
        if (existing_id == 0) {
            ++statistics_.unknown_token_requests_ignored;  // OUCH ignores replaces of unknown orders
            return;
        }
        if (!store_admits_new_requests()) {
            reject_at_gateway(request.replacement_token, kOuchRejectOther, context);
            return;
        }
        const OuchOrderInstructions& existing = instructions_by_cl_ord_id_[existing_id];
        if (!replace_instructions_supported(existing, request)) {
            reject_at_gateway(request.replacement_token, kOuchRejectOther, context);
            return;
        }
        instructions_by_cl_ord_id_[reservation.id] = instructions_after_replace(existing, request);

        ReplaceOrder replace;
        replace.session = session_id_;
        replace.orig_cl_ord_id = existing_id;
        replace.cl_ord_id = reservation.id;
        replace.price = request.price;
        replace.qty = request.shares;  // OUCH shares = new total quantity
        replace.force_priority_loss = true;
        const char not_applied_reason = submit_to_engine(context, Command{replace});
        if (LOB_UNLIKELY(not_applied_reason != kCommandApplied)) {
            reject_at_gateway(request.replacement_token, not_applied_reason, context);
        }
    }

    // X shares is the new intended total. The engine's reduce_to_total_qty has
    // exactly the OUCH semantics: 0 or shares <= cum cancels fully,
    // cum < shares < cum + leaves reduces keeping priority, larger is a no-op.
    template <class Context>
    void handle_cancel_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::CancelOrder request;
        if (protocol::ouch42::decode(payload.data(), payload.size(), request) != protocol::Status::ok) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        const ClOrdId order_id = tokens_.find(token_view(request.token));
        if (order_id == 0) {
            ++statistics_.unknown_token_requests_ignored;
            return;
        }
        CancelOrder cancel;
        cancel.session = session_id_;
        cancel.orig_cl_ord_id = order_id;
        cancel.reduce_to_total_qty = request.shares;
        (void)submit_to_engine(context, Command{cancel});  // OUCH has no cancel reject: not applied is silent
    }

    template <class Context>
    void handle_modify_order(std::string_view payload, Context& context) noexcept {
        protocol::ouch42::ModifyOrder request;
        const protocol::Status decoded = protocol::ouch42::decode(payload.data(), payload.size(), request);
        if (decoded != protocol::Status::ok && decoded != protocol::Status::invalid_field) {
            ++statistics_.malformed_messages_ignored;
            return;
        }
        reject_at_gateway(request.token, kOuchRejectOther, context);  // Modify is not supported
    }

    template <class Context>
    void reject_at_gateway(const OuchToken& token, char reason, Context& context) noexcept {
        ++statistics_.gateway_rejects;
        protocol::ouch42::OrderRejected message;
        message.timestamp = context.now();
        message.token = token;
        message.reason = reason;
        append_sequenced(message, context);
    }

    [[nodiscard]] bool store_admits_new_requests() const noexcept {
        const std::size_t free_messages = store_.max_messages() - static_cast<std::size_t>(store_.last_seq());
        const std::size_t free_bytes = store_.max_bytes() - store_.bytes_used();
        return free_messages > store_reserve_messages_ && free_bytes / kMaxFramedOutboundBytes > store_reserve_messages_;
    }

    // ---- Outbound mapping ---------------------------------------------------

    // Fills with aggressor None are uncross executions; the PhaseChanged for
    // the same instrument later in the batch says which call phase ended.
    // Cached so a large uncross scans the batch once.
    struct AuctionPhaseLookup {
        std::size_t phase_change_index = 0;
        InstrumentId instrument = 0;
        Phase old_phase = Phase::Halted;
        bool cached = false;

        Phase phase_being_left(std::span<const Event> batch, std::size_t fill_index) noexcept {
            const InstrumentId fill_instrument = batch[fill_index].instrument;
            if (cached && instrument == fill_instrument && phase_change_index > fill_index) return old_phase;
            cached = true;
            instrument = fill_instrument;
            old_phase = Phase::Halted;  // no PhaseChanged found: 'H'
            phase_change_index = batch.size();
            for (std::size_t index = fill_index + 1; index < batch.size(); ++index) {
                if (batch[index].type == EventType::PhaseChanged && batch[index].instrument == fill_instrument) {
                    phase_change_index = index;
                    old_phase = batch[index].phase_changed.old_phase;
                    break;
                }
            }
            return old_phase;
        }
    };

    template <class Context>
    void append_order_accepted(const Event& event, Context& context) noexcept {
        const AcceptedEvent& accepted = event.accepted;
        const OuchOrderInstructions& instructions = instructions_for(accepted.cl_ord_id);
        protocol::ouch42::OrderAccepted message;
        message.timestamp = event.ts;
        copy_token(accepted.cl_ord_id, message.token);
        message.side = instructions.side;
        message.shares = accepted.qty;
        message.stock = symbol_of(event.instrument, context);
        message.price = accepted.price;
        message.time_in_force = instructions.time_in_force_seconds;
        message.firm = instructions.firm;
        message.display = instructions.display;
        message.order_reference = accepted.order_id;
        message.capacity = instructions.capacity;
        message.ise = instructions.intermarket_sweep;
        message.minimum_quantity = instructions.minimum_quantity;
        message.cross_type = instructions.cross_type;
        message.order_state = kOuchOrderStateLive;  // no atomic-dead acknowledgement compression
        message.bbo_weight = kOuchBboWeightUnspecified;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_engine_reject(const Event& event, Context& context) noexcept {
        const RejectedEvent& rejected = event.rejected;
        if (rejected.cmd != CommandType::NewOrder && rejected.cmd != CommandType::Replace) return;  // Cancel: ignored
        if (rejected.cmd == CommandType::Replace && rejected.reason == RejectReason::UnknownOrder) return;
        protocol::ouch42::OrderRejected message;
        message.timestamp = event.ts;
        copy_token(rejected.cl_ord_id, message.token);  // request token (replacement token for U)
        message.reason = ouch_reject_reason(rejected.reason);
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_executed(const Event& event, const FillLeg& leg, char liquidity_flag, Context& context) noexcept {
        protocol::ouch42::OrderExecuted message;
        message.timestamp = event.ts;
        copy_token(leg.cl_ord_id, message.token);
        message.shares = event.fill.qty;
        message.price = event.fill.price;
        message.liquidity = liquidity_flag;
        message.match_number = event.fill.match_id;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_canceled(const Event& event, Context& context) noexcept {
        const CanceledEvent& canceled = event.canceled;
        protocol::ouch42::OrderCanceled message;
        message.timestamp = event.ts;
        copy_token(canceled.cl_ord_id, message.token);
        message.shares = canceled.canceled_qty;  // decrement shares
        message.reason = ouch_cancel_reason(canceled.reason);
        append_sequenced(message, context);
    }

    template <class Context>
    void append_order_replaced(const Event& event, Context& context) noexcept {
        const ReplacedEvent& replaced = event.replaced;
        const OuchOrderInstructions& instructions = instructions_for(replaced.cl_ord_id);
        protocol::ouch42::OrderReplaced message;
        message.timestamp = event.ts;
        copy_token(replaced.cl_ord_id, message.replacement_token);
        message.side = instructions.side;
        message.shares = replaced.leaves_qty;  // post-amend leaves
        message.stock = symbol_of(event.instrument, context);
        message.price = replaced.price;
        message.time_in_force = instructions.time_in_force_seconds;
        message.firm = instructions.firm;
        message.display = instructions.display;
        message.order_reference = replaced.new_order_id;
        message.capacity = instructions.capacity;
        message.ise = instructions.intermarket_sweep;
        message.minimum_quantity = instructions.minimum_quantity;
        message.cross_type = instructions.cross_type;
        message.order_state = kOuchOrderStateLive;
        copy_token(replaced.orig_cl_ord_id, message.previous_token);
        message.bbo_weight = kOuchBboWeightUnspecified;
        append_sequenced(message, context);
    }

    template <class Context>
    void append_system_event(char system_event, Timestamp event_time, Context& context) noexcept {
        protocol::ouch42::SystemEvent message;
        message.timestamp = event_time;
        message.event = system_event;
        append_sequenced(message, context);
    }

    // Frames `message` as SoupBinTCP Sequenced Data, stores it (assigning its
    // sequence number) and hands it to the attached connection. A full store
    // ends the user's session for the day: the message is lost, so replay
    // could never be complete again (see OuchCloseReason::StoreFull).
    template <class Message, class Context>
    void append_sequenced(const Message& message, Context& context) noexcept {
        if (LOB_UNLIKELY(store_overflowed_)) {
            ++statistics_.messages_lost_to_store_overflow;
            return;
        }
        char payload[kMaxOutboundOuchPayloadBytes];
        const std::size_t payload_size = protocol::ouch42::encode(message, payload, sizeof payload);
        char frame[kMaxFramedOutboundBytes];
        const std::size_t frame_size =
            payload_size == 0 ? 0
                              : protocol::soupbintcp::encode(protocol::soupbintcp::SequencedData{{payload, payload_size}},
                                                             frame, sizeof frame);
        if (LOB_UNLIKELY(frame_size == 0)) {
            ++statistics_.outbound_encode_failures;
            return;
        }
        if (LOB_UNLIKELY(!store_.append({frame, frame_size}))) {
            store_overflowed_ = true;
            ++statistics_.messages_lost_to_store_overflow;
            if (attached_connection_ != nullptr) attached_connection_->terminate_for_store_overflow(context.now());
            return;
        }
        if (attached_connection_ != nullptr) attached_connection_->deliver_stored_messages(context);
    }

    [[nodiscard]] const OuchOrderInstructions& instructions_for(ClOrdId cl_ord_id) const noexcept {
        static constexpr OuchOrderInstructions kUnknownOrderInstructions{};
        if (LOB_UNLIKELY(cl_ord_id == 0 || cl_ord_id > tokens_.max_tokens())) return kUnknownOrderInstructions;
        return instructions_by_cl_ord_id_[cl_ord_id];
    }

    void copy_token(ClOrdId cl_ord_id, OuchToken& destination) const noexcept {
        const std::string_view token = tokens_.token(cl_ord_id);
        destination.fill(' ');
        std::memcpy(destination.data(), token.data(), std::min(token.size(), destination.size()));
    }

    [[nodiscard]] static std::string_view token_view(const OuchToken& token) noexcept {
        return {token.data(), token.size()};  // byte-exact, padding included
    }

    template <class Context>
    [[nodiscard]] static Symbol symbol_of(InstrumentId instrument, const Context& context) noexcept {
        const InstrumentConfig* config = context.instrument(instrument);
        return config != nullptr ? config->symbol : Symbol{};
    }

    SessionId session_id_;
    OwnerId owner_;
    StpMode self_trade_prevention_;
    TokenTable tokens_;
    std::unique_ptr<OuchOrderInstructions[]> instructions_by_cl_ord_id_;  // index = ClOrdId
    SequencedStore store_;
    std::size_t store_reserve_messages_;
    OuchConnection<ByteSink>* attached_connection_ = nullptr;
    bool start_of_day_sent_ = false;
    bool store_overflowed_ = false;
    OuchUserStatistics statistics_{};
};

// ============================================================================
// OuchUserStateDirectory
// ============================================================================

// The OUCH users' states for the trading day, found by session id. It
// satisfies server::ExchangeCore's Sessions policy, so an OUCH-only server
// passes it to the core directly; a server that also runs FIX forwards each
// call for OUCH sessions to it. Sessions without an OUCH user state (FIX
// users, bots) are ignored.
template <OuchByteSink ByteSink>
class OuchUserStateDirectory {
public:
    // One user state per configured OUCH user that no bot drives (a bot owns
    // its session exclusively, so it never logs in over OUCH).
    OuchUserStateDirectory(const server::ServerConfig& config, const OuchUserStateLimits& limits) {
        SessionId largest_session = 0;
        for (const server::UserConfig& user : config.users) {
            if (!is_wire_ouch_user(user, config)) continue;
            user_states_.push_back(std::make_unique<OuchUserState<ByteSink>>(user, limits));
            largest_session = std::max(largest_session, user.session_id);
        }
        user_state_by_session_.assign(std::size_t{largest_session} + 1, nullptr);
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            user_state_by_session_[user_state->session_id()] = user_state.get();
        }
    }

    explicit OuchUserStateDirectory(const server::ServerConfig& config)
        : OuchUserStateDirectory(config, OuchUserStateLimits::from(config.engine)) {}

    // User states are referenced by connections: keep the directory in place.
    OuchUserStateDirectory(const OuchUserStateDirectory&) = delete;
    OuchUserStateDirectory& operator=(const OuchUserStateDirectory&) = delete;
    OuchUserStateDirectory(OuchUserStateDirectory&&) = delete;
    OuchUserStateDirectory& operator=(OuchUserStateDirectory&&) = delete;

    // nullptr if `session` is not an OUCH user's.
    [[nodiscard]] OuchUserState<ByteSink>* find(SessionId session) const noexcept {
        return session < user_state_by_session_.size() ? user_state_by_session_[session] : nullptr;
    }

    // SERVER.md 2.3: once per distinct session of each batch.
    template <class Context>
        requires OuchServerContext<Context>
    void on_events(SessionId session, std::span<const Event> batch, Context& context) noexcept {
        if (OuchUserState<ByteSink>* user_state = find(session)) user_state->on_events(batch, context);
    }

    // End of day, after the closing SetPhase batch: System Event 'E' for every
    // user (stored for those not logged in, so a later login recovers it).
    template <class Context>
        requires OuchServerContext<Context>
    void on_shutdown(Timestamp end_of_day, Context& context) noexcept {
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            user_state->append_end_of_day_event(end_of_day, context);
        }
    }

    // Precondition: no connection attached to any user state.
    void reset_for_new_trading_day() noexcept {
        for (const std::unique_ptr<OuchUserState<ByteSink>>& user_state : user_states_) {
            user_state->reset_for_new_trading_day();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return user_states_.size(); }

private:
    [[nodiscard]] static bool is_wire_ouch_user(const server::UserConfig& user, const server::ServerConfig& config) {
        if (user.protocol != server::Protocol::Ouch42 || user.session_id == 0) return false;
        for (const server::BotSectionConfig& bot : config.bots) {
            if (bot.user == user.username) return false;
        }
        return true;
    }

    std::vector<std::unique_ptr<OuchUserState<ByteSink>>> user_states_;
    std::vector<OuchUserState<ByteSink>*> user_state_by_session_;  // index = SessionId
};

// ============================================================================
// OuchConnection
// ============================================================================

template <OuchByteSink ByteSink>
class OuchConnection {
public:
    // Largest inbound SoupBinTCP frame accepted (Enter Order is 52 bytes;
    // longer Debug packets are a protocol violation).
    static constexpr std::size_t kInboundFrameCapacity = 256;

    // `user_states` is where logins find their user; it must outlive the connection.
    OuchConnection(ByteSink sink, const OuchUserStateDirectory<ByteSink>& user_states,
                   server::ConnectionId connection_id, const OuchSessionSettings& settings,
                   Timestamp connected_at) noexcept
        : sink_(std::move(sink)),
          user_states_(&user_states),
          connection_id_(connection_id),
          settings_(settings),
          frame_reader_(std::span<char>(inbound_frame_), kInboundFrameCapacity - 2),
          last_inbound_ns_(connected_at),
          last_outbound_ns_(connected_at) {}

    // Safety net only: the server must call on_disconnect(ctx) first so the
    // session is unbound (and cancel-on-disconnect applied).
    ~OuchConnection() {
        if (user_state_ != nullptr) user_state_->detach_connection(this);
    }

    // The user state keeps a pointer to its connection: construct in place.
    OuchConnection(const OuchConnection&) = delete;
    OuchConnection& operator=(const OuchConnection&) = delete;
    OuchConnection(OuchConnection&&) = delete;
    OuchConnection& operator=(OuchConnection&&) = delete;

    // Consumes every byte (partial frames are buffered internally).
    template <class Context>
        requires OuchServerContext<Context>
    std::size_t on_data(std::span<const char> bytes, Context& context) noexcept {
        if (state_ == OuchConnectionState::Closed) return bytes.size();
        last_inbound_ns_ = context.now();
        std::size_t offset = 0;
        while (offset < bytes.size() && state_ != OuchConnectionState::Closed) {
            std::size_t consumed = 0;
            protocol::soupbintcp::Packet packet;
            const protocol::Status status = frame_reader_.feed(bytes.subspan(offset), consumed, packet);
            offset += consumed;
            if (status == protocol::Status::incomplete) break;
            if (status != protocol::Status::ok) {
                close(OuchCloseReason::ProtocolViolation);
                break;
            }
            handle_packet(packet, context);
        }
        if (state_ == OuchConnectionState::Closed) release_session(context);
        return bytes.size();
    }

    // Idle timeout, server heartbeat, paused replay.
    template <class Context>
        requires OuchServerContext<Context>
    void on_timer(Context& context) noexcept {
        if (state_ != OuchConnectionState::Closed) {
            const Timestamp now = context.now();
            if (elapsed(last_inbound_ns_, now) >= settings_.client_idle_timeout_ns) {
                close(OuchCloseReason::ClientIdleTimeout);
            } else if (state_ == OuchConnectionState::LoggedIn) {
                deliver_stored_messages(context);
                if (state_ == OuchConnectionState::LoggedIn && !replaying_ &&
                    elapsed(last_outbound_ns_, now) >= settings_.server_heartbeat_interval_ns &&
                    !write_unsequenced(protocol::soupbintcp::ServerHeartbeat{}, now)) {
                    close(OuchCloseReason::SendBufferFull);
                }
            }
        }
        if (state_ == OuchConnectionState::Closed) release_session(context);
    }

    // Send space became available: continue a paused replay.
    template <class Context>
        requires OuchServerContext<Context>
    void on_writable(Context& context) noexcept {
        if (state_ == OuchConnectionState::LoggedIn) deliver_stored_messages(context);
        if (state_ == OuchConnectionState::Closed) release_session(context);
    }

    // The TCP connection is gone (or the server closed it after
    // close_requested()). Detaches and unbinds the user: cancel-on-disconnect.
    template <class Context>
        requires OuchServerContext<Context>
    void on_disconnect(Context& context) noexcept {
        if (state_ != OuchConnectionState::Closed) close(OuchCloseReason::TransportClosed);
        release_session(context);
    }

    // Server shutdown: SoupBinTCP End of Session, then close.
    template <class Context>
        requires OuchServerContext<Context>
    void end_session(Context& context) noexcept {
        if (state_ == OuchConnectionState::LoggedIn) {
            deliver_stored_messages(context);
            write_unsequenced(protocol::soupbintcp::EndOfSession{}, context.now());
        }
        if (state_ != OuchConnectionState::Closed) close(OuchCloseReason::EndOfSession);
        release_session(context);
    }

    [[nodiscard]] OuchConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool close_requested() const noexcept { return state_ == OuchConnectionState::Closed; }
    [[nodiscard]] OuchCloseReason close_reason() const noexcept { return close_reason_; }
    // Send what is queued before closing, except for an overloaded connection.
    [[nodiscard]] bool flush_before_close() const noexcept {
        return close_reason_ != OuchCloseReason::SendBufferFull && close_reason_ != OuchCloseReason::TransportClosed;
    }
    [[nodiscard]] bool is_replaying() const noexcept { return replaying_; }
    [[nodiscard]] std::uint64_t next_sequence_to_send() const noexcept { return next_sequence_to_send_; }
    [[nodiscard]] server::ConnectionId connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] OuchUserState<ByteSink>* user_state() const noexcept { return user_state_; }
    [[nodiscard]] ByteSink& sink() noexcept { return sink_; }
    [[nodiscard]] const ByteSink& sink() const noexcept { return sink_; }

private:
    friend class OuchUserState<ByteSink>;

    template <class Context>
    void handle_packet(const protocol::soupbintcp::Packet& packet, Context& context) noexcept {
        if (state_ == OuchConnectionState::AwaitingLogin) {
            switch (packet.type) {
                case 'L': handle_login_request(packet, context); return;
                case 'R':
                case '+': return;
                case 'O': close(OuchCloseReason::ClientLogout); return;
                default: close(OuchCloseReason::ProtocolViolation); return;
            }
        }
        switch (packet.type) {
            case 'U': user_state_->handle_application_message(packet.payload, context); return;
            case 'R':
            case '+': return;
            case 'O': close(OuchCloseReason::ClientLogout); return;
            default: close(OuchCloseReason::ProtocolViolation); return;  // second login or a server-only type
        }
    }

    template <class Context>
    void handle_login_request(const protocol::soupbintcp::Packet& packet, Context& context) noexcept {
        // The packet payload borrows the frame reader's buffer, which holds the
        // whole frame: the codec decodes Login Request from the frame start.
        protocol::soupbintcp::LoginRequest request;
        if (protocol::soupbintcp::decode(packet.payload.data() - 3, packet.payload.size() + 3, request) !=
            protocol::Status::ok) {
            close(OuchCloseReason::ProtocolViolation);
            return;
        }
        const Timestamp now = context.now();
        const server::UserConfig* user =
            context.authenticate(protocol::wire::alpha(request.username.data(), request.username.size()),
                                 protocol::wire::alpha(request.password.data(), request.password.size()),
                                 server::Protocol::Ouch42);
        OuchUserState<ByteSink>* user_state = user != nullptr ? user_states_->find(user->session_id) : nullptr;
        if (user_state == nullptr) {
            reject_login(kSoupLoginRejectNotAuthorized, now);
            return;
        }
        if (!requested_session_available(request.session) || user_state->store_overflowed() ||
            user_state->has_attached_connection() || !context.bind(user->session_id, connection_id_)) {
            reject_login(kSoupLoginRejectSessionNotAvailable, now);
            return;
        }
        accept_login(*user_state, request.sequence, context);
    }

    // Requested sequence 0 starts after the last stored message; N >= 1
    // replays from N, clamped to the next message to be generated.
    template <class Context>
    void accept_login(OuchUserState<ByteSink>& user_state, std::uint64_t requested_sequence, Context& context) noexcept {
        const Timestamp now = context.now();
        const std::uint64_t next_unstored = user_state.store().next_seq();
        const std::uint64_t first_sequence =
            requested_sequence == 0 ? next_unstored : std::min(requested_sequence, next_unstored);
        user_state_ = &user_state;
        state_ = OuchConnectionState::LoggedIn;
        next_sequence_to_send_ = first_sequence;
        replaying_ = first_sequence < next_unstored;
        user_state.attach_connection(this);

        protocol::soupbintcp::LoginAccepted accepted;
        accepted.session = settings_.soup_session;
        accepted.sequence = first_sequence;
        if (!write_unsequenced(accepted, now)) {
            close(OuchCloseReason::SendBufferFull);
            return;
        }
        deliver_stored_messages(context);
        if (state_ == OuchConnectionState::LoggedIn) user_state.append_start_of_day_if_needed(context);
    }

    void reject_login(char reason, Timestamp now) noexcept {
        write_unsequenced(protocol::soupbintcp::LoginRejected{reason}, now);
        close(OuchCloseReason::LoginRejected);
    }

    [[nodiscard]] bool requested_session_available(const std::array<char, 10>& requested) const noexcept {
        return requested == protocol::wire::spaces<10>() || requested == settings_.soup_session;
    }

    // Writes stored messages from next_sequence_to_send_ while the sink has
    // room. A replay pauses on a full sink and continues from on_writable /
    // on_timer; live output that does not fit closes the connection
    // (SendBufferFull) and the client recovers by replay.
    template <class Context>
    void deliver_stored_messages(Context& context) noexcept {
        if (state_ != OuchConnectionState::LoggedIn) return;
        const Timestamp now = context.now();
        const SequencedStore& store = user_state_->store();
        while (next_sequence_to_send_ <= store.last_seq()) {
            if (!replaying_ && user_state_->consume_outbound_drop(context)) {
                ++next_sequence_to_send_;
                continue;
            }
            const SequencedStore::Message message = store.get(next_sequence_to_send_);
            char* destination = sink_.prepare(message.bytes.size());
            if (destination == nullptr) {
                if (!replaying_) close(OuchCloseReason::SendBufferFull);
                return;
            }
            std::memcpy(destination, message.bytes.data(), message.bytes.size());
            sink_.commit(message.bytes.size());
            ++next_sequence_to_send_;
            last_outbound_ns_ = now;
        }
        replaying_ = false;
    }

    // Called by the user state during dispatch: no context calls here.
    void terminate_for_store_overflow(Timestamp now) noexcept {
        if (state_ != OuchConnectionState::LoggedIn) return;
        write_unsequenced(protocol::soupbintcp::EndOfSession{}, now);
        close(OuchCloseReason::StoreFull);
    }

    template <class Message>
    bool write_unsequenced(const Message& message, Timestamp now) noexcept {
        char frame[64];
        const std::size_t frame_size = protocol::soupbintcp::encode(message, frame, sizeof frame);
        char* destination = frame_size == 0 ? nullptr : sink_.prepare(frame_size);
        if (destination == nullptr) return false;
        std::memcpy(destination, frame, frame_size);
        sink_.commit(frame_size);
        last_outbound_ns_ = now;
        return true;
    }

    void close(OuchCloseReason reason) noexcept {
        state_ = OuchConnectionState::Closed;
        close_reason_ = reason;
        replaying_ = false;
    }

    template <class Context>
    void release_session(Context& context) noexcept {
        if (user_state_ == nullptr) return;
        OuchUserState<ByteSink>* user_state = user_state_;
        user_state_ = nullptr;
        user_state->detach_connection(this);
        context.unbind(user_state->session_id());
    }

    [[nodiscard]] static Timestamp elapsed(Timestamp since, Timestamp now) noexcept {
        return now > since ? now - since : 0;
    }

    ByteSink sink_;
    const OuchUserStateDirectory<ByteSink>* user_states_;
    server::ConnectionId connection_id_;
    OuchSessionSettings settings_;
    char inbound_frame_[kInboundFrameCapacity]{};
    protocol::soupbintcp::FrameReader frame_reader_;
    OuchUserState<ByteSink>* user_state_ = nullptr;  // non-null exactly while bound
    Timestamp last_inbound_ns_;
    Timestamp last_outbound_ns_;
    std::uint64_t next_sequence_to_send_ = 1;
    OuchConnectionState state_ = OuchConnectionState::AwaitingLogin;
    OuchCloseReason close_reason_ = OuchCloseReason::None;
    bool replaying_ = false;
};

}  // namespace exsim::gateway::ouch

#endif
