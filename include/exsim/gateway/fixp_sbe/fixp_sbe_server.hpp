#ifndef EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_SERVER_HPP
#define EXSIM_GATEWAY_FIXP_SBE_FIXP_SBE_SERVER_HPP

// Server side of exsim binary order entry: SBE 1.0 application messages over a
// FIXP 1.0 point-to-point session, framed with SOFH 1.0
// (docs/protocols/FIXP_SBE.md, docs/protocols/exsim_order_entry_sbe.xml,
// docs/SERVER.md section 2). Pure state machines over byte buffers:
//
//   FixpSbeUserState<Sink>   one per configured user for the whole trading day:
//                            token table, sequenced store (the recoverable
//                            server flow), the idempotent client flow high-water
//                            mark, the current FIXP logical session and the
//                            application mapping both ways.
//   FixpSbeUserStateDirectory<Sink>
//                            every FIXP/SBE user's state by session; also a
//                            server::ExchangeCore `Sessions` policy.
//   FixpSbeConnection<Sink>  one per TCP connection: SOFH framing, Negotiate and
//                            Establish, implicit sequencing, keepalive,
//                            retransmission, Terminate.
//
// Flows (FIXP 1.0 Appendix A 7.1.3): client to server Idempotent, server to
// client Recoverable. Server flow sequence s of the current logical session is
// stored at SequencedStore sequence (base + s), where base is the store size
// when the session was negotiated.
//
// All are templated on the byte sink and their entry points on the server
// Context (FixpSbeServerContext below), which server::ExchangeCore satisfies.
// No virtual dispatch, no exceptions, no std::string and no heap allocation
// after construction.
//
// Driving contract for the server integrator (same as the OUCH gateway):
//   * build one FixpSbeUserStateDirectory for the day and give it to the core
//     as its Sessions (or call its on_events from your own Sessions router).
//     The core then calls on_events(session, batch, core) once per distinct
//     session of each batch (SERVER.md 2.3).
//   * per accepted TCP connection, construct a FixpSbeConnection with that
//     directory; connection.on_data(bytes, core) consumes whole frames and
//     returns the bytes consumed; the remainder stays in the receive buffer.
//   * call connection.on_timer(ctx) periodically (keepalive, timeouts).
//   * when connection.close_requested(): close the socket (after flushing if
//     flush_before_close()) and then call connection.on_disconnect(ctx). Also
//     call on_disconnect(ctx) when the socket drops on its own. Releasing an
//     established connection calls ctx.unbind (cancel-on-disconnect).
//   * on_events never calls ctx.submit/bind/unbind.

#include "exsim/command.hpp"
#include "exsim/event.hpp"
#include "exsim/gateway/fixp_sbe/byte_sink.hpp"
#include "exsim/gateway/fixp_sbe/fixp_sbe_mapping.hpp"
#include "exsim/gateway/sequenced_store.hpp"
#include "exsim/gateway/token_table.hpp"
#include "exsim/instrument.hpp"
#include "exsim/protocol/sbe_order_entry.hpp"
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
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace exsim::gateway::fixp_sbe {

// 2000-01-01T00:00:00Z in nanoseconds: FIXP timestamps below this are taken to
// be seconds, milliseconds or microseconds and rejected.
inline constexpr std::uint64_t kEarliestPlausibleUnixTimestampNs = 946'684'800'000'000'000ull;
// Largest application frame the server stores (ExecutionReportReplace).
inline constexpr std::size_t kMaxApplicationFrameSize =
    sbe::kFramePrefixSize + sbe::ExecutionReportReplace::kBlockLength;
// Session UUIDs remembered per user to answer a reused id with DuplicateId.
inline constexpr std::size_t kRememberedSessionUuidsPerUser = 64;

struct FixpSbeSessionSettings {
    server::Protocol authentication_protocol = server::Protocol::FixpSbe;
    // EstablishmentAck.KeepaliveInterval: the server sends Sequence after this
    // much outbound silence (FIXP 4.5).
    std::uint32_t server_keepalive_interval_ms = 1000;
    // Accepted range for Establish.KeepaliveInterval (FIXP 4.3.1).
    std::uint32_t minimum_client_keepalive_interval_ms = 100;
    std::uint32_t maximum_client_keepalive_interval_ms = 60'000;
    // Inbound silence longer than multiplier x client keepalive terminates the
    // session (FIXP 4.4 recommends leniency; FIX gateway uses 2 x HeartBtInt).
    std::uint32_t keepalive_expiry_multiplier = 2;
    // Negotiate and Establish must complete this long after connect.
    Timestamp establishment_timeout_ns = 5'000'000'000ull;
    // Messages per Retransmission batch; clients iterate (FIXP 4.6.2.2).
    std::uint32_t maximum_retransmission_batch = 256;

    [[nodiscard]] static FixpSbeSessionSettings from(const server::ExchangeSection& exchange) noexcept {
        FixpSbeSessionSettings settings;
        settings.server_keepalive_interval_ms = exchange.heartbeat_interval_ms;
        return settings;
    }
};

struct FixpSbeUserStateSettings {
    std::uint32_t max_tokens = 1u << 20;              // day-used ClOrdIds of every request type
    std::size_t store_messages = 1u << 20;            // server flow messages kept for the day
    std::size_t store_bytes = std::size_t{64} << 20;  // their framed bytes
    // New orders and replaces are rejected at the gateway once the store has
    // this many or fewer free messages, keeping room for reports on live orders.
    std::size_t store_reserve_messages = 1024;

    [[nodiscard]] static FixpSbeUserStateSettings from(const server::EngineSection& engine) noexcept {
        FixpSbeUserStateSettings settings;
        settings.max_tokens = engine.max_tokens_per_session;
        settings.store_messages = engine.store_messages_per_session;
        settings.store_bytes = engine.store_bytes_per_session;
        settings.store_reserve_messages = std::min<std::size_t>(1024, engine.store_messages_per_session / 16);
        return settings;
    }
};

struct FixpSbeUserStatistics {
    std::uint64_t duplicate_client_messages_dropped = 0;  // idempotent flow, sequence below high-water
    std::uint64_t not_applied_messages_sent = 0;
    std::uint64_t business_message_rejects_sent = 0;
    // Rejects the gateway generates itself: validation failures and requests the
    // engine answered with no event (RequestHadNoEffect).
    std::uint64_t gateway_generated_rejects = 0;
    std::uint64_t messages_lost_to_store_overflow = 0;
    std::uint64_t outbound_messages_dropped_by_fault = 0;
    std::uint64_t retransmitted_messages = 0;
};

enum class FixpSbeConnectionState : std::uint8_t { AwaitingNegotiate, Negotiated, Established, Closed };

enum class FixpSbeCloseReason : std::uint8_t {
    None,
    NegotiationRejected,    // NegotiationReject sent
    EstablishmentRejected,  // EstablishmentReject sent
    ProtocolViolation,      // malformed frame or unexpected message; Terminate sent when a session is known
    PeerTerminated,         // Terminate received and answered
    KeepaliveExpired,       // client silent for multiplier x its keepalive interval
    EstablishmentTimeout,   // not established within establishment_timeout_ns
    RetransmitOutOfBounds,  // RetransmitRequest for messages never sent: Terminate ReRequestOutOfBounds
    SendBufferFull,         // sink.prepare failed (SERVER.md 2.4)
    StoreFull,              // sequenced store overflowed; Terminate sent
    EndOfSession,           // end_session(): Terminate Finished sent
    TransportClosed,        // on_disconnect() without an earlier close request
};

template <FixpSbeByteSink ByteSink>
class FixpSbeUserState;
template <FixpSbeByteSink ByteSink>
class FixpSbeConnection;
template <FixpSbeByteSink ByteSink>
class FixpSbeUserStateDirectory;

// What the server must provide (SERVER.md 2.1). submit may return void or
// server::SubmitResult. Optional fault hooks, used when present:
// consume_injected_reject(SessionId) and consume_drop_out(SessionId), both
// returning bool (server::ExchangeCore has them).
template <class Context>
concept FixpSbeServerContext = requires(Context& context, const Context& const_context, std::string_view text,
                                        server::Protocol protocol, SessionId session, server::ConnectionId connection,
                                        const Symbol& symbol, InstrumentId instrument, const Command& command) {
    { context.authenticate(text, text, protocol) } -> std::same_as<const server::UserConfig*>;
    { context.bind(session, connection) } -> std::same_as<bool>;
    context.unbind(session, connection);
    { const_context.find_instrument(symbol) } -> std::same_as<InstrumentId>;
    { const_context.instrument(instrument) } -> std::same_as<const InstrumentConfig*>;
    requires std::is_void_v<decltype(context.submit(command))> ||
                 std::same_as<decltype(context.submit(command)), server::SubmitResult>;
    { const_context.now() } -> std::same_as<Timestamp>;
    { const_context.realtime_ns() } -> std::same_as<std::uint64_t>;
};

namespace detail {

[[nodiscard]] constexpr Timestamp elapsed_since(Timestamp since, Timestamp now) noexcept {
    return now >= since ? now - since : 0;
}

struct CredentialParts {
    std::string_view username;
    std::string_view password;
};

// Credentials are ASCII "username:password", split at the first ':'.
[[nodiscard]] constexpr CredentialParts split_credentials(std::string_view credentials) noexcept {
    const std::size_t colon = credentials.find(':');
    if (colon == std::string_view::npos) return {credentials, {}};
    return {credentials.substr(0, colon), credentials.substr(colon + 1)};
}

template <class Context>
[[nodiscard]] bool consume_injected_reject(Context& context, SessionId session) noexcept {
    if constexpr (requires { { context.consume_injected_reject(session) } -> std::convertible_to<bool>; }) {
        return context.consume_injected_reject(session);
    } else {
        return false;
    }
}

template <class Context>
[[nodiscard]] bool consume_drop_out(Context& context, SessionId session) noexcept {
    if constexpr (requires { { context.consume_drop_out(session) } -> std::convertible_to<bool>; }) {
        return context.consume_drop_out(session);
    } else {
        return false;
    }
}

}  // namespace detail

// ============================================================================
// FixpSbeUserState
// ============================================================================

template <FixpSbeByteSink ByteSink>
class FixpSbeUserState {
public:
    FixpSbeUserState(const server::UserConfig& user, const FixpSbeUserStateSettings& settings)
        : session_id_(user.session_id),
          owner_(user.owner),
          default_self_trade_prevention_(user.stp),
          store_reserve_messages_(settings.store_reserve_messages),
          tokens_(settings.max_tokens),
          store_(settings.store_messages, settings.store_bytes) {}

    ~FixpSbeUserState() { assert(attached_connection_ == nullptr); }

    // Connections keep a pointer to their user state: construct in place.
    FixpSbeUserState(const FixpSbeUserState&) = delete;
    FixpSbeUserState& operator=(const FixpSbeUserState&) = delete;
    FixpSbeUserState(FixpSbeUserState&&) = delete;
    FixpSbeUserState& operator=(FixpSbeUserState&&) = delete;

    [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }
    [[nodiscard]] bool has_logical_session() const noexcept { return has_logical_session_; }
    [[nodiscard]] const sbe::SessionUuid& logical_session_uuid() const noexcept { return logical_session_uuid_; }
    // FIXP sequence number the next server flow message will carry.
    [[nodiscard]] std::uint64_t next_server_sequence_number() const noexcept {
        return store_.next_seq() - store_sequence_base_;
    }
    [[nodiscard]] std::uint64_t last_server_sequence_number() const noexcept {
        return store_.last_seq() - store_sequence_base_;
    }
    // Idempotent flow: client messages numbered below this were already applied.
    [[nodiscard]] std::uint64_t next_expected_client_sequence_number() const noexcept {
        return next_expected_client_sequence_number_;
    }
    [[nodiscard]] bool has_attached_connection() const noexcept { return attached_connection_ != nullptr; }
    [[nodiscard]] const FixpSbeUserStatistics& statistics() const noexcept { return statistics_; }
    [[nodiscard]] const SequencedStore& store() const noexcept { return store_; }
    [[nodiscard]] const TokenTable& tokens() const noexcept { return tokens_; }

    // Encodes the events of this session in batch order, stores each message
    // and writes it to the attached connection. Safe during dispatch.
    template <class Context>
        requires FixpSbeServerContext<Context>
    void on_events(std::span<const Event> batch, Context& context) noexcept {
        const Timestamp now = context.now();
        for (const Event& event : batch) {
            switch (event.type) {
                case EventType::Accepted:
                    if (event.accepted.session == session_id_) report_accepted(event, context, now);
                    break;
                case EventType::Rejected:
                    if (event.rejected.session == session_id_) report_rejected(event, context, now);
                    break;
                case EventType::Fill:
                    // Buy leg before sell leg (docs/FOUNDATION_REPAIRS.md, deterministic output).
                    if (event.fill.buy.session == session_id_) report_fill_leg(event, event.fill.buy, Side::BUY, context, now);
                    if (event.fill.sell.session == session_id_) report_fill_leg(event, event.fill.sell, Side::SELL, context, now);
                    break;
                case EventType::Canceled:
                    if (event.canceled.session == session_id_) report_canceled(event, context, now);
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == session_id_) report_replaced(event, context, now);
                    break;
                case EventType::MassCancelDone:
                    if (event.mass_cancel_done.session == session_id_) report_mass_cancel_done(event, context, now);
                    break;
                case EventType::Rested:
                case EventType::PhaseChanged:
                case EventType::AuctionSummary:
                    break;
            }
        }
    }

private:
    friend class FixpSbeConnection<ByteSink>;

    // ---- logical session ---------------------------------------------------

    [[nodiscard]] bool session_uuid_used_today(const sbe::SessionUuid& uuid) const noexcept {
        const std::size_t remembered = std::min(remembered_session_uuid_count_, remembered_session_uuids_.size());
        for (std::size_t index = 0; index < remembered; ++index) {
            if (remembered_session_uuids_[index] == uuid) return true;
        }
        return false;
    }

    // A new Negotiate supersedes the previous logical session: its messages are
    // no longer recoverable and both flows restart at sequence number 1.
    void begin_logical_session(const sbe::SessionUuid& uuid) noexcept {
        remembered_session_uuids_[remembered_session_uuid_count_ % remembered_session_uuids_.size()] = uuid;
        ++remembered_session_uuid_count_;
        logical_session_uuid_ = uuid;
        has_logical_session_ = true;
        store_sequence_base_ = store_.last_seq();
        next_expected_client_sequence_number_ = 1;
    }

    void attach_connection(FixpSbeConnection<ByteSink>* connection) noexcept { attached_connection_ = connection; }
    void detach_connection(const FixpSbeConnection<ByteSink>* connection) noexcept {
        if (attached_connection_ == connection) attached_connection_ = nullptr;
    }

    [[nodiscard]] SequencedStore::Message stored_server_message(std::uint64_t server_sequence_number) const noexcept {
        return store_.get(store_sequence_base_ + server_sequence_number);
    }

    // ---- idempotent client flow (FIXP 4.8) ----------------------------------

    // Sequence(N) from the client. A forward jump means messages were never
    // received: report them with NotApplied (FIXP 4.8.2). False if the gap does
    // not fit NotApplied.Count.
    template <class Context>
    [[nodiscard]] bool note_client_sequence(std::uint64_t next_client_sequence_number, Context& context,
                                            Timestamp now) noexcept {
        if (next_client_sequence_number <= next_expected_client_sequence_number_) return true;
        const std::uint64_t gap = next_client_sequence_number - next_expected_client_sequence_number_;
        if (gap > std::numeric_limits<std::uint32_t>::max()) return false;
        sbe::NotApplied not_applied;
        not_applied.from_seq_no = next_expected_client_sequence_number_;
        not_applied.count = static_cast<std::uint32_t>(gap);
        next_expected_client_sequence_number_ = next_client_sequence_number;
        ++statistics_.not_applied_messages_sent;
        emit_application_message(not_applied, context, now);
        return true;
    }

    // True when the message numbered client_sequence_number must be applied;
    // false for a duplicate, which is silently dropped (FIXP 4.8).
    [[nodiscard]] bool accept_client_sequence_number(std::uint64_t client_sequence_number) noexcept {
        if (client_sequence_number < next_expected_client_sequence_number_) {
            ++statistics_.duplicate_client_messages_dropped;
            return false;
        }
        next_expected_client_sequence_number_ = client_sequence_number + 1;
        return true;
    }

    // ---- inbound application messages ---------------------------------------

    template <class Context>
    void handle_new_order_single(const sbe::NewOrderSingle& request, Context& context, Timestamp now) noexcept {
        const auto reject = [&](sbe::ExsimRejectReasonCode reason) noexcept {
            ++statistics_.gateway_generated_rejects;
            sbe::ExecutionReportReject report;
            report.cl_ord_id = request.cl_ord_id;
            report.exec_id = next_exec_id();
            report.transact_time_ns = context.realtime_ns();
            report.symbol = request.symbol;
            report.ord_rej_reason = ord_rej_reason_for(reason);
            report.exsim_reject_reason = reason;
            emit_application_message(report, context, now);
        };
        ClOrdId cl_ord_id = 0;
        if (!reserve_request_token(request.cl_ord_id, cl_ord_id, reject)) return;
        if (!store_has_room_for_new_order()) return reject(sbe::ExsimRejectReasonCode::OutboundStoreFull);
        if (detail::consume_injected_reject(context, session_id_)) return reject(sbe::ExsimRejectReasonCode::InjectedReject);

        NewOrder order;
        order.ts = now;
        order.session = session_id_;
        order.owner = owner_;
        order.cl_ord_id = cl_ord_id;
        order.qty = request.order_qty;
        order.post_only = (request.exec_inst & sbe::kExecInstParticipateDontInitiate) != 0;
        const bool fields_valid =
            exsim_side_from_wire(request.side, order.side) && exsim_ord_type_from_wire(request.ord_type, order.type) &&
            exsim_time_in_force_from_wire(request.time_in_force, order.tif) &&
            exsim_stp_mode_from_wire(request.self_trade_prevention, default_self_trade_prevention_, order.stp) &&
            (request.exec_inst & ~sbe::kExecInstParticipateDontInitiate) == 0;
        if (!fields_valid) return reject(sbe::ExsimRejectReasonCode::InvalidField);
        const bool price_is_null = request.price_mantissa == sbe::kNullPriceMantissa;
        if (order.type == OrdType::Limit && price_is_null) return reject(sbe::ExsimRejectReasonCode::InvalidPrice);
        order.price = price_is_null ? 0 : request.price_mantissa;
        Symbol symbol;
        if (!sbe::exsim_symbol_from_wire(request.symbol, symbol)) return reject(sbe::ExsimRejectReasonCode::UnknownInstrument);
        order.instrument = context.find_instrument(symbol);
        if (order.instrument == 0) return reject(sbe::ExsimRejectReasonCode::UnknownInstrument);
        submit_request(Command(order), context, reject);
    }

    template <class Context>
    void handle_order_cancel_request(const sbe::OrderCancelRequest& request, Context& context, Timestamp now) noexcept {
        const auto reject = [&](sbe::ExsimRejectReasonCode reason) noexcept {
            ++statistics_.gateway_generated_rejects;
            emit_order_cancel_reject(request.cl_ord_id, request.orig_cl_ord_id, 0,
                                     sbe::CxlRejResponseToCode::OrderCancelRequest, reason, context, now);
        };
        ClOrdId cl_ord_id = 0;
        if (!reserve_request_token(request.cl_ord_id, cl_ord_id, reject)) return;
        const ClOrdId orig_cl_ord_id = find_token(request.orig_cl_ord_id);
        if (orig_cl_ord_id == 0) return reject(sbe::ExsimRejectReasonCode::UnknownOrder);
        CancelOrder cancel;
        cancel.ts = now;
        cancel.session = session_id_;
        cancel.orig_cl_ord_id = orig_cl_ord_id;
        cancel.cl_ord_id = cl_ord_id;
        cancel.reduce_to_total_qty = request.reduce_to_total_qty;
        submit_request(Command(cancel), context, reject);
    }

    template <class Context>
    void handle_order_cancel_replace_request(const sbe::OrderCancelReplaceRequest& request, Context& context,
                                             Timestamp now) noexcept {
        const auto reject = [&](sbe::ExsimRejectReasonCode reason) noexcept {
            ++statistics_.gateway_generated_rejects;
            emit_order_cancel_reject(request.cl_ord_id, request.orig_cl_ord_id, 0,
                                     sbe::CxlRejResponseToCode::OrderCancelReplaceRequest, reason, context, now);
        };
        ClOrdId cl_ord_id = 0;
        if (!reserve_request_token(request.cl_ord_id, cl_ord_id, reject)) return;
        if (!store_has_room_for_new_order()) return reject(sbe::ExsimRejectReasonCode::OutboundStoreFull);
        if (request.force_priority_loss != sbe::BooleanCode::False && request.force_priority_loss != sbe::BooleanCode::True) {
            return reject(sbe::ExsimRejectReasonCode::InvalidField);
        }
        const ClOrdId orig_cl_ord_id = find_token(request.orig_cl_ord_id);
        if (orig_cl_ord_id == 0) return reject(sbe::ExsimRejectReasonCode::UnknownOrder);
        ReplaceOrder replace;
        replace.ts = now;
        replace.session = session_id_;
        replace.orig_cl_ord_id = orig_cl_ord_id;
        replace.cl_ord_id = cl_ord_id;
        replace.price = request.price_mantissa;
        replace.qty = request.order_qty;
        replace.force_priority_loss = request.force_priority_loss == sbe::BooleanCode::True;
        submit_request(Command(replace), context, reject);
    }

    template <class Context>
    void handle_order_mass_cancel_request(const sbe::OrderMassCancelRequest& request, Context& context,
                                          Timestamp now) noexcept {
        const auto reject = [&](sbe::ExsimRejectReasonCode reason) noexcept {
            ++statistics_.gateway_generated_rejects;
            sbe::OrderMassCancelReport report;
            report.cl_ord_id = request.cl_ord_id;
            report.transact_time_ns = context.realtime_ns();
            report.total_affected_orders = 0;
            report.symbol = request.symbol;
            report.mass_cancel_request_type = request.mass_cancel_request_type;
            report.mass_cancel_response = sbe::MassCancelResponseCode::CancelRequestRejected;
            report.mass_cancel_reject_reason = mass_cancel_reject_reason_for(reason);
            report.exsim_reject_reason = reason;
            emit_application_message(report, context, now);
        };
        ClOrdId cl_ord_id = 0;
        if (!reserve_request_token(request.cl_ord_id, cl_ord_id, reject)) return;
        MassCancel mass_cancel;
        mass_cancel.ts = now;
        mass_cancel.session = session_id_;
        mass_cancel.cl_ord_id = cl_ord_id;
        switch (request.mass_cancel_request_type) {
            case sbe::MassCancelRequestTypeCode::CancelAllOrders:
                mass_cancel.instrument = 0;
                break;
            case sbe::MassCancelRequestTypeCode::CancelOrdersForSecurity: {
                // Never let an unknown symbol become instrument 0 (= all).
                Symbol symbol;
                if (!sbe::exsim_symbol_from_wire(request.symbol, symbol)) {
                    return reject(sbe::ExsimRejectReasonCode::UnknownInstrument);
                }
                mass_cancel.instrument = context.find_instrument(symbol);
                if (mass_cancel.instrument == 0) return reject(sbe::ExsimRejectReasonCode::UnknownInstrument);
                break;
            }
            default:
                return reject(sbe::ExsimRejectReasonCode::InvalidField);
        }
        submit_request(Command(mass_cancel), context, reject);
    }

    // Unknown or unsupported application template (FIX BusinessMessageReject).
    template <class Context>
    void reject_unsupported_application_message(std::uint64_t client_sequence_number, std::uint16_t template_id,
                                                Context& context, Timestamp now) noexcept {
        sbe::BusinessMessageReject reject;
        reject.ref_seq_num = client_sequence_number;
        reject.ref_msg_type = template_id;
        reject.business_reject_reason = sbe::BusinessRejectReasonCode::UnsupportedMessageType;
        ++statistics_.business_message_rejects_sent;
        emit_application_message(reject, context, now);
    }

    // Day-token history (docs/FOUNDATION_REPAIRS.md): every well-framed request
    // with a nonzero ClOrdId reserves it for the day, even when it is rejected.
    template <class RejectFunction>
    [[nodiscard]] bool reserve_request_token(std::uint64_t wire_cl_ord_id, ClOrdId& cl_ord_id,
                                             const RejectFunction& reject) noexcept {
        if (wire_cl_ord_id == 0) {
            reject(sbe::ExsimRejectReasonCode::InvalidClOrdId);
            return false;
        }
        const TokenBytes token = token_bytes_from_wire_cl_ord_id(wire_cl_ord_id);
        const TokenTable::InternResult interned = tokens_.intern(std::string_view(token.data(), token.size()));
        switch (interned.status) {
            case TokenTable::InternStatus::Inserted:
                cl_ord_id = interned.id;
                return true;
            case TokenTable::InternStatus::Existing:
                reject(sbe::ExsimRejectReasonCode::ClOrdIdAlreadyUsedToday);
                return false;
            case TokenTable::InternStatus::Full:
            case TokenTable::InternStatus::Invalid:
                break;
        }
        reject(sbe::ExsimRejectReasonCode::TokenTableFull);
        return false;
    }

    [[nodiscard]] ClOrdId find_token(std::uint64_t wire_cl_ord_id) const noexcept {
        if (wire_cl_ord_id == 0) return 0;
        const TokenBytes token = token_bytes_from_wire_cl_ord_id(wire_cl_ord_id);
        return tokens_.find(std::string_view(token.data(), token.size()));
    }

    [[nodiscard]] std::uint64_t wire_cl_ord_id_for(ClOrdId cl_ord_id) const noexcept {
        return cl_ord_id == 0 ? 0 : wire_cl_ord_id_from_token_bytes(tokens_.token(cl_ord_id));
    }

    [[nodiscard]] bool store_has_room_for_new_order() const noexcept {
        const std::size_t free_messages = store_.max_messages() - static_cast<std::size_t>(store_.last_seq());
        const std::size_t free_bytes = store_.max_bytes() - store_.bytes_used();
        return free_messages > store_reserve_messages_ &&
               free_bytes / kMaxApplicationFrameSize > store_reserve_messages_;
    }

    // Every request gets a response. If the engine emitted nothing for this
    // session (a reduction that changes nothing) the gateway answers with
    // RequestHadNoEffect; a submission the core refused (SubmitResult) is
    // answered with the matching reason.
    template <class Context, class RejectFunction>
    void submit_request(const Command& command, Context& context, const RejectFunction& reject) noexcept {
        request_in_flight_ = true;
        request_answered_ = false;
        bool reports_follow_later = false;
        sbe::ExsimRejectReasonCode unanswered_reason = sbe::ExsimRejectReasonCode::RequestHadNoEffect;
        if constexpr (std::same_as<decltype(context.submit(command)), server::SubmitResult>) {
            switch (context.submit(command)) {
                case server::SubmitResult::Applied:
                    break;
                case server::SubmitResult::Deferred:  // only when submitted during dispatch
                    reports_follow_later = true;
                    break;
                case server::SubmitResult::OutsideTradingDay:
                case server::SubmitResult::ExchangeShutDown:
                    unanswered_reason = sbe::ExsimRejectReasonCode::MarketClosed;
                    break;
                case server::SubmitResult::OutputCapacityExceeded:
                case server::SubmitResult::DeferredQueueFull:
                    unanswered_reason = sbe::ExsimRejectReasonCode::CapacityExceeded;
                    break;
            }
        } else {
            context.submit(command);
        }
        request_in_flight_ = false;
        if (!request_answered_ && !reports_follow_later) reject(unanswered_reason);
    }

    // ---- outbound application messages --------------------------------------

    [[nodiscard]] std::uint64_t next_exec_id() noexcept { return ++last_exec_id_; }

    template <class Context>
    [[nodiscard]] static sbe::WireSymbol wire_symbol_for(const Context& context, InstrumentId instrument) noexcept {
        if (instrument == 0) return {};
        const InstrumentConfig* config = context.instrument(instrument);
        return config == nullptr ? sbe::WireSymbol{} : sbe::wire_symbol_from(config->symbol);
    }

    template <class Context>
    void report_accepted(const Event& event, Context& context, Timestamp now) noexcept {
        const AcceptedEvent& accepted = event.accepted;
        sbe::ExecutionReportNew report;
        report.order_id = accepted.order_id;
        report.cl_ord_id = wire_cl_ord_id_for(accepted.cl_ord_id);
        report.exec_id = next_exec_id();
        report.transact_time_ns = context.realtime_ns();
        report.price_mantissa = accepted.ord_type == OrdType::Market ? sbe::kNullPriceMantissa : accepted.price;
        report.order_qty = accepted.qty;
        report.leaves_qty = accepted.qty;
        report.cum_qty = 0;
        report.symbol = wire_symbol_for(context, event.instrument);
        report.side = wire_side_from_exsim(accepted.side);
        report.ord_type = wire_ord_type_from_exsim(accepted.ord_type);
        report.time_in_force = wire_time_in_force_from_exsim(accepted.tif);
        report.ord_status = sbe::OrdStatusCode::New;
        report.exec_inst = accepted.post_only ? sbe::kExecInstParticipateDontInitiate : 0;
        report.self_trade_prevention = wire_stp_from_exsim(accepted.stp);
        emit_application_message(report, context, now);
    }

    template <class Context>
    void report_rejected(const Event& event, Context& context, Timestamp now) noexcept {
        const RejectedEvent& rejected = event.rejected;
        const sbe::ExsimRejectReasonCode reason = wire_reject_reason_from_exsim(rejected.reason);
        switch (rejected.cmd) {
            case CommandType::NewOrder: {
                sbe::ExecutionReportReject report;
                report.cl_ord_id = wire_cl_ord_id_for(rejected.cl_ord_id);
                report.exec_id = next_exec_id();
                report.transact_time_ns = context.realtime_ns();
                report.symbol = wire_symbol_for(context, event.instrument);
                report.ord_rej_reason = ord_rej_reason_for(reason);
                report.exsim_reject_reason = reason;
                emit_application_message(report, context, now);
                break;
            }
            case CommandType::Cancel:
            case CommandType::Replace:
                emit_order_cancel_reject(wire_cl_ord_id_for(rejected.cl_ord_id), wire_cl_ord_id_for(rejected.orig_cl_ord_id),
                                         rejected.order_id,
                                         rejected.cmd == CommandType::Cancel
                                             ? sbe::CxlRejResponseToCode::OrderCancelRequest
                                             : sbe::CxlRejResponseToCode::OrderCancelReplaceRequest,
                                         reason, context, now);
                break;
            case CommandType::MassCancel: {
                sbe::OrderMassCancelReport report;
                report.cl_ord_id = wire_cl_ord_id_for(rejected.cl_ord_id);
                report.transact_time_ns = context.realtime_ns();
                report.symbol = wire_symbol_for(context, event.instrument);
                report.mass_cancel_request_type = event.instrument == 0
                                                      ? sbe::MassCancelRequestTypeCode::CancelAllOrders
                                                      : sbe::MassCancelRequestTypeCode::CancelOrdersForSecurity;
                report.mass_cancel_response = sbe::MassCancelResponseCode::CancelRequestRejected;
                report.mass_cancel_reject_reason = mass_cancel_reject_reason_for(reason);
                report.exsim_reject_reason = reason;
                emit_application_message(report, context, now);
                break;
            }
            case CommandType::SetPhase:
                break;
        }
    }

    template <class Context>
    void report_fill_leg(const Event& event, const FillLeg& leg, Side leg_side, Context& context, Timestamp now) noexcept {
        const FillEvent& fill = event.fill;
        sbe::ExecutionReportTrade report;
        report.order_id = leg.order_id;
        report.cl_ord_id = wire_cl_ord_id_for(leg.cl_ord_id);
        report.exec_id = next_exec_id();
        report.transact_time_ns = context.realtime_ns();
        report.last_px_mantissa = fill.price;
        report.trd_match_id = fill.match_id;
        report.last_qty = fill.qty;
        report.leaves_qty = leg.leaves_qty;
        report.cum_qty = leg.cum_qty;
        report.symbol = wire_symbol_for(context, event.instrument);
        report.side = wire_side_from_exsim(leg_side);
        report.ord_status = leg.leaves_qty == 0 ? sbe::OrdStatusCode::Filled : sbe::OrdStatusCode::PartiallyFilled;
        report.last_liquidity_ind = liquidity_indicator_for(fill.aggressor, leg_side);
        emit_application_message(report, context, now);
    }

    template <class Context>
    void report_canceled(const Event& event, Context& context, Timestamp now) noexcept {
        const CanceledEvent& canceled = event.canceled;
        const std::uint64_t order_token = wire_cl_ord_id_for(canceled.cl_ord_id);
        sbe::ExecutionReportCancel report;
        report.order_id = canceled.order_id;
        report.cl_ord_id = canceled.request_cl_ord_id != 0 ? wire_cl_ord_id_for(canceled.request_cl_ord_id) : order_token;
        report.orig_cl_ord_id = order_token;
        report.exec_id = next_exec_id();
        report.transact_time_ns = context.realtime_ns();
        report.canceled_qty = canceled.canceled_qty;
        report.leaves_qty = canceled.leaves_qty;
        report.cum_qty = canceled.cum_qty;
        report.symbol = wire_symbol_for(context, event.instrument);
        report.side = wire_side_from_exsim(canceled.side);
        report.ord_status = canceled.leaves_qty == 0 ? sbe::OrdStatusCode::Canceled : live_order_status(canceled.cum_qty);
        report.cancel_reason = wire_cancel_reason_from_exsim(canceled.reason);
        emit_application_message(report, context, now);
    }

    template <class Context>
    void report_replaced(const Event& event, Context& context, Timestamp now) noexcept {
        const ReplacedEvent& replaced = event.replaced;
        sbe::ExecutionReportReplace report;
        report.order_id = replaced.new_order_id;
        report.previous_order_id = replaced.old_order_id;
        report.cl_ord_id = wire_cl_ord_id_for(replaced.cl_ord_id);
        report.orig_cl_ord_id = wire_cl_ord_id_for(replaced.orig_cl_ord_id);
        report.exec_id = next_exec_id();
        report.transact_time_ns = context.realtime_ns();
        report.price_mantissa = replaced.price;
        report.order_qty = replaced.qty;
        report.leaves_qty = replaced.leaves_qty;
        report.cum_qty = replaced.cum_qty;
        report.symbol = wire_symbol_for(context, event.instrument);
        report.side = wire_side_from_exsim(replaced.side);
        report.ord_status = live_order_status(replaced.cum_qty);
        report.priority_retained = replaced.priority_kept ? sbe::BooleanCode::True : sbe::BooleanCode::False;
        emit_application_message(report, context, now);
    }

    template <class Context>
    void report_mass_cancel_done(const Event& event, Context& context, Timestamp now) noexcept {
        const MassCancelDoneEvent& done = event.mass_cancel_done;
        const bool all_instruments = event.instrument == 0;
        sbe::OrderMassCancelReport report;
        report.cl_ord_id = wire_cl_ord_id_for(done.request_cl_ord_id);
        report.transact_time_ns = context.realtime_ns();
        report.total_affected_orders = done.canceled_count;
        report.symbol = wire_symbol_for(context, event.instrument);
        report.mass_cancel_request_type = all_instruments ? sbe::MassCancelRequestTypeCode::CancelAllOrders
                                                          : sbe::MassCancelRequestTypeCode::CancelOrdersForSecurity;
        report.mass_cancel_response = all_instruments ? sbe::MassCancelResponseCode::CancelAllOrders
                                                      : sbe::MassCancelResponseCode::CancelOrdersForSecurity;
        report.mass_cancel_reject_reason = sbe::MassCancelRejectReasonCode::NullValue;
        report.exsim_reject_reason = sbe::ExsimRejectReasonCode::None;
        emit_application_message(report, context, now);
    }

    template <class Context>
    void emit_order_cancel_reject(std::uint64_t wire_cl_ord_id, std::uint64_t wire_orig_cl_ord_id, OrderId order_id,
                                  sbe::CxlRejResponseToCode response_to, sbe::ExsimRejectReasonCode reason,
                                  Context& context, Timestamp now) noexcept {
        sbe::OrderCancelReject reject;
        reject.cl_ord_id = wire_cl_ord_id;
        reject.orig_cl_ord_id = wire_orig_cl_ord_id;
        reject.order_id = order_id;
        reject.transact_time_ns = context.realtime_ns();
        reject.cxl_rej_response_to = response_to;
        reject.cxl_rej_reason = cxl_rej_reason_for(reason);
        reject.exsim_reject_reason = reason;
        emit_application_message(reject, context, now);
    }

    // Stores the message (assigning its server flow sequence number) and
    // writes it to the attached connection unless a drop-out fault eats it.
    template <class Message, class Context>
    void emit_application_message(const Message& message, Context& context, Timestamp now) noexcept {
        char frame[sbe::kMaxEncodedFrameSize];
        const std::size_t frame_size = sbe::encode(message, frame, sizeof frame);
        assert(frame_size != 0);
        if (request_in_flight_) request_answered_ = true;
        if (LOB_UNLIKELY(!store_.append(std::span<const char>(frame, frame_size)))) {
            ++statistics_.messages_lost_to_store_overflow;
            if (attached_connection_ != nullptr) attached_connection_->terminate_after_store_overflow(now);
            return;
        }
        if (attached_connection_ == nullptr) return;
        if (detail::consume_drop_out(context, session_id_)) {
            ++statistics_.outbound_messages_dropped_by_fault;
            attached_connection_->note_dropped_realtime_message();
            return;
        }
        attached_connection_->write_realtime_application_frame(frame, frame_size, last_server_sequence_number(), now);
    }

    SessionId session_id_;
    OwnerId owner_;
    StpMode default_self_trade_prevention_;
    std::size_t store_reserve_messages_;
    TokenTable tokens_;
    SequencedStore store_;
    FixpSbeUserStatistics statistics_;
    std::uint64_t last_exec_id_ = 0;

    bool has_logical_session_ = false;
    sbe::SessionUuid logical_session_uuid_{};
    std::uint64_t store_sequence_base_ = 0;
    std::uint64_t next_expected_client_sequence_number_ = 1;
    std::array<sbe::SessionUuid, kRememberedSessionUuidsPerUser> remembered_session_uuids_{};
    std::size_t remembered_session_uuid_count_ = 0;

    FixpSbeConnection<ByteSink>* attached_connection_ = nullptr;
    bool request_in_flight_ = false;
    bool request_answered_ = false;
};

// ============================================================================
// FixpSbeUserStateDirectory
// ============================================================================

template <FixpSbeByteSink ByteSink>
class FixpSbeUserStateDirectory {
public:
    FixpSbeUserStateDirectory() = default;

    // One user state per configured user of `protocol` that no bot drives (a
    // bot owns its session exclusively, so it never logs in over FIXP).
    FixpSbeUserStateDirectory(const server::ServerConfig& config, const FixpSbeUserStateSettings& settings,
                              server::Protocol protocol = server::Protocol::FixpSbe) {
        for (const server::UserConfig& user : config.users) {
            if (user.protocol != protocol || user.session_id == 0 || is_bot_user(user, config)) continue;
            add_user(user, settings);
        }
    }

    explicit FixpSbeUserStateDirectory(const server::ServerConfig& config)
        : FixpSbeUserStateDirectory(config, FixpSbeUserStateSettings::from(config.engine)) {}

    // User states are referenced by connections: keep the directory in place.
    FixpSbeUserStateDirectory(const FixpSbeUserStateDirectory&) = delete;
    FixpSbeUserStateDirectory& operator=(const FixpSbeUserStateDirectory&) = delete;
    FixpSbeUserStateDirectory(FixpSbeUserStateDirectory&&) = delete;
    FixpSbeUserStateDirectory& operator=(FixpSbeUserStateDirectory&&) = delete;

    // Setup only: allocates. Throws std::invalid_argument for a zero or
    // duplicate session id.
    FixpSbeUserState<ByteSink>& add_user(const server::UserConfig& user, const FixpSbeUserStateSettings& settings) {
        if (user.session_id == 0 || find(user.session_id) != nullptr) {
            throw std::invalid_argument("FIXP/SBE user " + user.username + ": session_id must be unique and non-zero");
        }
        user_states_.push_back(std::make_unique<FixpSbeUserState<ByteSink>>(user, settings));
        if (user.session_id >= user_state_by_session_.size()) {
            user_state_by_session_.resize(std::size_t{user.session_id} + 1, nullptr);
        }
        user_state_by_session_[user.session_id] = user_states_.back().get();
        return *user_states_.back();
    }

    // nullptr if `session` is not a FIXP/SBE user's.
    [[nodiscard]] FixpSbeUserState<ByteSink>* find(SessionId session) const noexcept {
        return session < user_state_by_session_.size() ? user_state_by_session_[session] : nullptr;
    }

    // SERVER.md 2.3: once per distinct session of each batch.
    template <class Context>
        requires FixpSbeServerContext<Context>
    void on_events(SessionId session, std::span<const Event> batch, Context& context) noexcept {
        if (FixpSbeUserState<ByteSink>* user_state = find(session)) user_state->on_events(batch, context);
    }

    [[nodiscard]] std::size_t size() const noexcept { return user_states_.size(); }

private:
    [[nodiscard]] static bool is_bot_user(const server::UserConfig& user, const server::ServerConfig& config) {
        for (const server::BotSectionConfig& bot : config.bots) {
            if (bot.user == user.username) return true;
        }
        return false;
    }

    std::vector<std::unique_ptr<FixpSbeUserState<ByteSink>>> user_states_;
    std::vector<FixpSbeUserState<ByteSink>*> user_state_by_session_;  // index = SessionId
};

// ============================================================================
// FixpSbeConnection
// ============================================================================

template <FixpSbeByteSink ByteSink>
class FixpSbeConnection {
public:
    FixpSbeConnection(ByteSink sink, const FixpSbeUserStateDirectory<ByteSink>& user_states,
                      server::ConnectionId connection_id, const FixpSbeSessionSettings& settings,
                      Timestamp connected_at) noexcept
        : sink_(std::move(sink)),
          user_states_(&user_states),
          connection_id_(connection_id),
          settings_(settings),
          connected_at_ns_(connected_at),
          last_inbound_ns_(connected_at),
          last_outbound_ns_(connected_at) {}

    // Safety net only: the server must call on_disconnect(ctx) first so the
    // session is unbound (and cancel-on-disconnect applied).
    ~FixpSbeConnection() {
        if (user_state_ != nullptr) user_state_->detach_connection(this);
    }

    // The user state keeps a pointer to its connection: construct in place.
    FixpSbeConnection(const FixpSbeConnection&) = delete;
    FixpSbeConnection& operator=(const FixpSbeConnection&) = delete;
    FixpSbeConnection(FixpSbeConnection&&) = delete;
    FixpSbeConnection& operator=(FixpSbeConnection&&) = delete;

    // Consumes whole frames; returns the bytes consumed. After a close request
    // every byte is consumed and ignored.
    template <class Context>
        requires FixpSbeServerContext<Context>
    std::size_t on_data(std::span<const char> bytes, Context& context) noexcept {
        if (state_ == FixpSbeConnectionState::Closed) return bytes.size();
        const Timestamp now = context.now();
        std::size_t consumed = 0;
        while (consumed < bytes.size() && state_ != FixpSbeConnectionState::Closed) {
            sbe::FrameView view;
            const sbe::DecodeStatus status = sbe::decode_frame(bytes.data() + consumed, bytes.size() - consumed, view);
            if (status == sbe::DecodeStatus::Incomplete) break;
            last_inbound_ns_ = now;
            if (status != sbe::DecodeStatus::Ok) {
                protocol_violation("malformed frame", now);
                break;
            }
            consumed += view.frame_size;
            handle_frame(view, context, now);
        }
        if (state_ == FixpSbeConnectionState::Closed) {
            release_session(context);
            return bytes.size();
        }
        return consumed;
    }

    // Keepalive in both directions and the establishment deadline.
    template <class Context>
        requires FixpSbeServerContext<Context>
    void on_timer(Context& context) noexcept {
        const Timestamp now = context.now();
        switch (state_) {
            case FixpSbeConnectionState::AwaitingNegotiate:
            case FixpSbeConnectionState::Negotiated:
                if (detail::elapsed_since(connected_at_ns_, now) >= settings_.establishment_timeout_ns) {
                    close(FixpSbeCloseReason::EstablishmentTimeout);
                }
                break;
            case FixpSbeConnectionState::Established: {
                const Timestamp client_silence_limit_ns = Timestamp{client_keepalive_interval_ms_} * 1'000'000ull *
                                                          settings_.keepalive_expiry_multiplier;
                if (detail::elapsed_since(last_inbound_ns_, now) > client_silence_limit_ns) {
                    send_terminate(sbe::TerminationCode::UnspecifiedError, "keepalive interval expired", now);
                    close(FixpSbeCloseReason::KeepaliveExpired);
                } else if (detail::elapsed_since(last_outbound_ns_, now) >=
                           Timestamp{settings_.server_keepalive_interval_ms} * 1'000'000ull) {
                    write_sequence(now);
                }
                break;
            }
            case FixpSbeConnectionState::Closed:
                break;
        }
        if (state_ == FixpSbeConnectionState::Closed) release_session(context);
    }

    // The TCP connection is gone (or the server closed it after
    // close_requested()). Detaches and unbinds the user: cancel-on-disconnect.
    template <class Context>
        requires FixpSbeServerContext<Context>
    void on_disconnect(Context& context) noexcept {
        if (state_ != FixpSbeConnectionState::Closed) close(FixpSbeCloseReason::TransportClosed);
        release_session(context);
    }

    // Server shutdown: Terminate Finished, then close.
    template <class Context>
        requires FixpSbeServerContext<Context>
    void end_session(Context& context) noexcept {
        if (state_ == FixpSbeConnectionState::Established) {
            send_terminate(sbe::TerminationCode::Finished, "end of session", context.now());
        }
        if (state_ != FixpSbeConnectionState::Closed) close(FixpSbeCloseReason::EndOfSession);
        release_session(context);
    }

    [[nodiscard]] FixpSbeConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool close_requested() const noexcept { return state_ == FixpSbeConnectionState::Closed; }
    [[nodiscard]] FixpSbeCloseReason close_reason() const noexcept { return close_reason_; }
    // Send what is queued before closing, except for an overloaded or lost connection.
    [[nodiscard]] bool flush_before_close() const noexcept {
        return close_reason_ != FixpSbeCloseReason::SendBufferFull && close_reason_ != FixpSbeCloseReason::TransportClosed;
    }
    [[nodiscard]] server::ConnectionId connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] FixpSbeUserState<ByteSink>* user_state() const noexcept { return user_state_; }
    [[nodiscard]] std::uint32_t client_keepalive_interval_ms() const noexcept { return client_keepalive_interval_ms_; }
    [[nodiscard]] ByteSink& sink() noexcept { return sink_; }
    [[nodiscard]] const ByteSink& sink() const noexcept { return sink_; }

private:
    friend class FixpSbeUserState<ByteSink>;

    template <class Context>
    void handle_frame(const sbe::FrameView& view, Context& context, Timestamp now) noexcept {
        const std::uint16_t template_id = view.header.template_id;
        if (template_id >= sbe::kFirstApplicationTemplateId) {
            handle_application_frame(view, context, now);
            return;
        }
        switch (static_cast<sbe::TemplateId>(template_id)) {
            case sbe::TemplateId::Negotiate: {
                sbe::Negotiate negotiate;
                if (!decode_or_violate(view, negotiate, now)) return;
                if (state_ != FixpSbeConnectionState::AwaitingNegotiate) {
                    protocol_violation("unexpected Negotiate", now);
                    return;
                }
                handle_negotiate(negotiate, context, now);
                return;
            }
            case sbe::TemplateId::Establish: {
                sbe::Establish establish;
                if (!decode_or_violate(view, establish, now)) return;
                if (state_ == FixpSbeConnectionState::Established) {
                    // FIXP 4.3.3 AlreadyEstablished; the established session continues.
                    send_establishment_reject(establish, sbe::EstablishmentRejectCode::AlreadyEstablished,
                                              "session already established on this connection", now);
                    return;
                }
                handle_establish(establish, context, now);
                return;
            }
            case sbe::TemplateId::Sequence: {
                sbe::Sequence sequence;
                if (!decode_or_violate(view, sequence, now)) return;
                if (state_ != FixpSbeConnectionState::Established || sequence.next_seq_no == 0 ||
                    sequence.next_seq_no == sbe::kNullSequenceNumber) {
                    protocol_violation("unexpected Sequence", now);
                    return;
                }
                next_implicit_client_sequence_number_ = sequence.next_seq_no;
                client_sequence_known_ = true;
                if (!user_state_->note_client_sequence(sequence.next_seq_no, context, now)) {
                    protocol_violation("sequence gap too large", now);
                }
                return;
            }
            case sbe::TemplateId::UnsequencedHeartbeat: {
                sbe::UnsequencedHeartbeat heartbeat;
                if (!decode_or_violate(view, heartbeat, now)) return;
                // Accepted as a keepalive only; Sequence is the keepalive of this flow.
                if (state_ != FixpSbeConnectionState::Established) protocol_violation("unexpected UnsequencedHeartbeat", now);
                return;
            }
            case sbe::TemplateId::RetransmitRequest: {
                sbe::RetransmitRequest request;
                if (!decode_or_violate(view, request, now)) return;
                if (state_ != FixpSbeConnectionState::Established) {
                    protocol_violation("unexpected RetransmitRequest", now);
                    return;
                }
                retransmit(request, now);
                return;
            }
            case sbe::TemplateId::Terminate: {
                sbe::Terminate terminate;
                if (!decode_or_violate(view, terminate, now)) return;
                if (terminate.session_id != session_uuid_) {
                    protocol_violation("Terminate SessionId mismatch", now);
                    return;
                }
                // FIXP 4.4.1: the receiver must answer Terminate with Terminate.
                if (state_ == FixpSbeConnectionState::Established) {
                    send_terminate(sbe::TerminationCode::Finished, "terminate acknowledged", now);
                }
                close(FixpSbeCloseReason::PeerTerminated);
                return;
            }
            default:
                // Server-only, unsupported (Context, Topic, FinishedSending, ...)
                // or unknown session templates.
                protocol_violation("unsupported session message", now);
                return;
        }
    }

    template <class Context>
    void handle_application_frame(const sbe::FrameView& view, Context& context, Timestamp now) noexcept {
        if (state_ != FixpSbeConnectionState::Established || !client_sequence_known_ ||
            next_implicit_client_sequence_number_ >= sbe::kNullSequenceNumber - 1) {
            protocol_violation("application message before Sequence", now);
            return;
        }
        FixpSbeUserState<ByteSink>& user_state = *user_state_;
        switch (static_cast<sbe::TemplateId>(view.header.template_id)) {
            case sbe::TemplateId::NewOrderSingle: {
                sbe::NewOrderSingle request;
                if (!decode_or_violate(view, request, now)) return;
                if (user_state.accept_client_sequence_number(next_implicit_client_sequence_number_++)) {
                    user_state.handle_new_order_single(request, context, now);
                }
                return;
            }
            case sbe::TemplateId::OrderCancelRequest: {
                sbe::OrderCancelRequest request;
                if (!decode_or_violate(view, request, now)) return;
                if (user_state.accept_client_sequence_number(next_implicit_client_sequence_number_++)) {
                    user_state.handle_order_cancel_request(request, context, now);
                }
                return;
            }
            case sbe::TemplateId::OrderCancelReplaceRequest: {
                sbe::OrderCancelReplaceRequest request;
                if (!decode_or_violate(view, request, now)) return;
                if (user_state.accept_client_sequence_number(next_implicit_client_sequence_number_++)) {
                    user_state.handle_order_cancel_replace_request(request, context, now);
                }
                return;
            }
            case sbe::TemplateId::OrderMassCancelRequest: {
                sbe::OrderMassCancelRequest request;
                if (!decode_or_violate(view, request, now)) return;
                if (user_state.accept_client_sequence_number(next_implicit_client_sequence_number_++)) {
                    user_state.handle_order_mass_cancel_request(request, context, now);
                }
                return;
            }
            default: {
                // Unknown or server-to-client template: it still consumes a
                // client sequence number (FIXP 4.8 implicit numbering).
                if (view.header.num_groups != 0) {
                    protocol_violation("repeating groups are not used", now);
                    return;
                }
                const std::uint64_t client_sequence_number = next_implicit_client_sequence_number_++;
                if (user_state.accept_client_sequence_number(client_sequence_number)) {
                    user_state.reject_unsupported_application_message(client_sequence_number, view.header.template_id,
                                                                      context, now);
                }
                return;
            }
        }
    }

    template <class Message>
    [[nodiscard]] bool decode_or_violate(const sbe::FrameView& view, Message& message, Timestamp now) noexcept {
        if (sbe::decode(view, message) == sbe::DecodeStatus::Ok) return true;
        protocol_violation("malformed message", now);
        return false;
    }

    // ---- Negotiate (FIXP 4.2) -----------------------------------------------

    template <class Context>
    void handle_negotiate(const sbe::Negotiate& negotiate, Context& context, Timestamp now) noexcept {
        const auto reject = [&](sbe::NegotiationRejectCode code, std::string_view reason) noexcept {
            sbe::NegotiationReject message;
            message.session_id = negotiate.session_id;
            message.request_timestamp_ns = negotiate.timestamp_ns;
            message.code = code;
            message.reason = reason;
            write_session_message(message, now);
            close(FixpSbeCloseReason::NegotiationRejected);
        };
        if (!sbe::is_version4_uuid(negotiate.session_id)) {
            return reject(sbe::NegotiationRejectCode::Unspecified, "SessionId must be an RFC 4122 version 4 UUID");
        }
        if (negotiate.timestamp_ns < kEarliestPlausibleUnixTimestampNs) {
            return reject(sbe::NegotiationRejectCode::Unspecified, "Timestamp must be nanoseconds since the Unix epoch");
        }
        if (negotiate.client_flow != sbe::FlowType::Idempotent) {
            return reject(sbe::NegotiationRejectCode::FlowTypeNotSupported, "client flow must be Idempotent");
        }
        const detail::CredentialParts credentials = detail::split_credentials(negotiate.credentials);
        const server::UserConfig* user =
            context.authenticate(credentials.username, credentials.password, settings_.authentication_protocol);
        FixpSbeUserState<ByteSink>* user_state = user == nullptr ? nullptr : user_states_->find(user->session_id);
        if (user_state == nullptr) return reject(sbe::NegotiationRejectCode::Credentials, "invalid credentials");
        if (user_state->has_attached_connection()) {
            return reject(sbe::NegotiationRejectCode::Unspecified, "user has an established session");
        }
        if (user_state->session_uuid_used_today(negotiate.session_id)) {
            return reject(sbe::NegotiationRejectCode::DuplicateId, "SessionId already used");
        }
        if (user_state->remembered_session_uuid_count_ == kRememberedSessionUuidsPerUser) {
            return reject(sbe::NegotiationRejectCode::Unspecified, "daily logical session capacity reached");
        }
        user_state->begin_logical_session(negotiate.session_id);
        negotiated_user_ = user;
        session_uuid_ = negotiate.session_id;
        state_ = FixpSbeConnectionState::Negotiated;
        sbe::NegotiationResponse response;
        response.session_id = negotiate.session_id;
        response.request_timestamp_ns = negotiate.timestamp_ns;
        response.server_flow = sbe::FlowType::Recoverable;
        write_session_message(response, now);
    }

    // ---- Establish (FIXP 4.3) -----------------------------------------------

    void send_establishment_reject(const sbe::Establish& establish, sbe::EstablishmentRejectCode code,
                                   std::string_view reason, Timestamp now) noexcept {
        sbe::EstablishmentReject message;
        message.session_id = establish.session_id;
        message.request_timestamp_ns = establish.timestamp_ns;
        message.code = code;
        message.reason = reason;
        write_session_message(message, now);
    }

    template <class Context>
    void handle_establish(const sbe::Establish& establish, Context& context, Timestamp now) noexcept {
        const auto reject = [&](sbe::EstablishmentRejectCode code, std::string_view reason) noexcept {
            send_establishment_reject(establish, code, reason, now);
            close(FixpSbeCloseReason::EstablishmentRejected);
        };
        if (establish.timestamp_ns < kEarliestPlausibleUnixTimestampNs ||
            establish.next_seq_no != sbe::kNullSequenceNumber) {
            return reject(sbe::EstablishmentRejectCode::Unspecified, "invalid timestamp or idempotent NextSeqNo");
        }
        const server::UserConfig* user = negotiated_user_;
        if (!establish.credentials.empty()) {
            const detail::CredentialParts credentials = detail::split_credentials(establish.credentials);
            const server::UserConfig* authenticated =
                context.authenticate(credentials.username, credentials.password, settings_.authentication_protocol);
            if (authenticated == nullptr || (user != nullptr && authenticated->session_id != user->session_id)) {
                return reject(sbe::EstablishmentRejectCode::Credentials, "invalid credentials");
            }
            user = authenticated;
        } else if (user == nullptr) {
            return reject(sbe::EstablishmentRejectCode::Credentials, "credentials required on a new connection");
        }
        FixpSbeUserState<ByteSink>* user_state = user_states_->find(user->session_id);
        if (user_state == nullptr) return reject(sbe::EstablishmentRejectCode::Credentials, "invalid credentials");
        if (!user_state->has_logical_session() || user_state->logical_session_uuid() != establish.session_id) {
            return reject(sbe::EstablishmentRejectCode::Unnegotiated, "SessionId was not negotiated");
        }
        if (establish.keepalive_interval_ms < settings_.minimum_client_keepalive_interval_ms ||
            establish.keepalive_interval_ms > settings_.maximum_client_keepalive_interval_ms) {
            return reject(sbe::EstablishmentRejectCode::KeepaliveInterval, "KeepaliveInterval out of range");
        }
        if (user_state->has_attached_connection() || !context.bind(user->session_id, connection_id_)) {
            return reject(sbe::EstablishmentRejectCode::AlreadyEstablished, "session established on another connection");
        }
        user_state_ = user_state;
        user_state->attach_connection(this);
        negotiated_user_ = user;
        session_uuid_ = establish.session_id;
        client_keepalive_interval_ms_ = establish.keepalive_interval_ms;
        client_sequence_known_ = false;
        outbound_sequence_message_required_ = false;
        state_ = FixpSbeConnectionState::Established;
        // EstablishmentAck is sequence forming: the next server message is NextSeqNo.
        sbe::EstablishmentAck ack;
        ack.session_id = establish.session_id;
        ack.request_timestamp_ns = establish.timestamp_ns;
        ack.keepalive_interval_ms = settings_.server_keepalive_interval_ms;
        ack.next_seq_no = user_state->next_server_sequence_number();
        write_session_message(ack, now);
    }

    // ---- recoverable server flow (FIXP 4.6) ---------------------------------

    void retransmit(const sbe::RetransmitRequest& request, Timestamp now) noexcept {
        FixpSbeUserState<ByteSink>& user_state = *user_state_;
        const auto reject = [&](sbe::RetransmitRejectCode code, std::string_view reason) noexcept {
            sbe::RetransmitReject message;
            message.session_id = request.session_id;
            message.request_timestamp_ns = request.timestamp_ns;
            message.code = code;
            message.reason = reason;
            write_session_message(message, now);
        };
        if (request.session_id != session_uuid_) return reject(sbe::RetransmitRejectCode::InvalidSession, "unknown SessionId");
        if (request.from_seq_no == 0 || request.count == 0) {
            return reject(sbe::RetransmitRejectCode::OutOfRange, "FromSeqNo and Count must be positive");
        }
        const std::uint64_t last_sequence_number = user_state.last_server_sequence_number();
        if (request.from_seq_no > last_sequence_number) {
            // The client was told NextSeqNo, so it knew this was invalid (FIXP 4.6.2).
            send_terminate(sbe::TerminationCode::ReRequestOutOfBounds, "FromSeqNo beyond the last message sent", now);
            close(FixpSbeCloseReason::RetransmitOutOfBounds);
            return;
        }
        if (settings_.maximum_retransmission_batch == 0) {
            return reject(sbe::RetransmitRejectCode::RequestLimitExceeded, "retransmission disabled");
        }
        if (request.count > last_sequence_number - request.from_seq_no + 1) {
            return reject(sbe::RetransmitRejectCode::OutOfRange, "FromSeqNo + Count beyond the last message sent");
        }
        const std::uint32_t batch_count = std::min(request.count, settings_.maximum_retransmission_batch);

        sbe::Retransmission retransmission;
        retransmission.session_id = session_uuid_;
        retransmission.request_timestamp_ns = request.timestamp_ns;
        retransmission.next_seq_no = request.from_seq_no;
        retransmission.count = batch_count;
        sbe::Sequence realtime_resumes;  // FIXP 4.6.2.2: real-time flow resumes with Sequence
        realtime_resumes.next_seq_no = user_state.next_server_sequence_number();

        std::size_t total_size = sbe::encoded_frame_size(retransmission) + sbe::encoded_frame_size(realtime_resumes);
        for (std::uint32_t offset = 0; offset < batch_count; ++offset) {
            total_size += user_state.stored_server_message(request.from_seq_no + offset).bytes.size();
        }
        char* out = sink_.prepare(total_size);
        if (LOB_UNLIKELY(out == nullptr)) {
            close(FixpSbeCloseReason::SendBufferFull);
            return;
        }
        std::size_t written = sbe::encode(retransmission, out, total_size);
        for (std::uint32_t offset = 0; offset < batch_count; ++offset) {
            const std::span<const char> stored = user_state.stored_server_message(request.from_seq_no + offset).bytes;
            std::memcpy(out + written, stored.data(), stored.size());
            written += stored.size();
        }
        written += sbe::encode(realtime_resumes, out + written, total_size - written);
        assert(written == total_size);
        sink_.commit(written);
        last_outbound_ns_ = now;
        outbound_sequence_message_required_ = false;
        user_state.statistics_.retransmitted_messages += batch_count;
    }

    // Called by the user state for every live server flow message.
    void write_realtime_application_frame(const char* frame, std::size_t frame_size, std::uint64_t server_sequence_number,
                                          Timestamp now) noexcept {
        if (state_ != FixpSbeConnectionState::Established) return;
        if (outbound_sequence_message_required_) {
            sbe::Sequence sequence;
            sequence.next_seq_no = server_sequence_number;
            if (!write_session_message(sequence, now)) return;
            outbound_sequence_message_required_ = false;
        }
        write_frame(frame, frame_size, now);
    }

    // A drop-out fault skipped a message: announce the next one explicitly so
    // the client detects the gap (FIXP 4.5).
    void note_dropped_realtime_message() noexcept { outbound_sequence_message_required_ = true; }

    void terminate_after_store_overflow(Timestamp now) noexcept {
        if (state_ != FixpSbeConnectionState::Established) return;
        send_terminate(sbe::TerminationCode::UnspecifiedError, "outbound store full", now);
        close(FixpSbeCloseReason::StoreFull);
    }

    void write_sequence(Timestamp now) noexcept {
        sbe::Sequence sequence;
        sequence.next_seq_no = user_state_->next_server_sequence_number();
        if (write_session_message(sequence, now)) outbound_sequence_message_required_ = false;
    }

    // ---- output -------------------------------------------------------------

    void protocol_violation(std::string_view reason, Timestamp now) noexcept {
        if (state_ == FixpSbeConnectionState::Negotiated || state_ == FixpSbeConnectionState::Established) {
            send_terminate(sbe::TerminationCode::UnspecifiedError, reason, now);
        }
        close(FixpSbeCloseReason::ProtocolViolation);
    }

    void send_terminate(sbe::TerminationCode code, std::string_view reason, Timestamp now) noexcept {
        sbe::Terminate terminate;
        terminate.session_id = session_uuid_;
        terminate.code = code;
        terminate.reason = reason;
        write_session_message(terminate, now);
    }

    template <class Message>
    bool write_session_message(const Message& message, Timestamp now) noexcept {
        char frame[sbe::kMaxEncodedFrameSize];
        const std::size_t frame_size = sbe::encode(message, frame, sizeof frame);
        assert(frame_size != 0);
        return write_frame(frame, frame_size, now);
    }

    bool write_frame(const char* frame, std::size_t frame_size, Timestamp now) noexcept {
        if (state_ == FixpSbeConnectionState::Closed) return false;
        char* out = sink_.prepare(frame_size);
        if (LOB_UNLIKELY(out == nullptr)) {
            close(FixpSbeCloseReason::SendBufferFull);
            return false;
        }
        std::memcpy(out, frame, frame_size);
        sink_.commit(frame_size);
        last_outbound_ns_ = now;
        return true;
    }

    void close(FixpSbeCloseReason reason) noexcept {
        if (state_ == FixpSbeConnectionState::Closed) return;
        state_ = FixpSbeConnectionState::Closed;
        close_reason_ = reason;
    }

    template <class Context>
    void release_session(Context& context) noexcept {
        if (user_state_ == nullptr) return;
        FixpSbeUserState<ByteSink>* user_state = user_state_;
        user_state_ = nullptr;
        user_state->detach_connection(this);
        context.unbind(user_state->session_id(), connection_id_);
    }

    ByteSink sink_;
    const FixpSbeUserStateDirectory<ByteSink>* user_states_;
    server::ConnectionId connection_id_;
    FixpSbeSessionSettings settings_;
    FixpSbeConnectionState state_ = FixpSbeConnectionState::AwaitingNegotiate;
    FixpSbeCloseReason close_reason_ = FixpSbeCloseReason::None;
    Timestamp connected_at_ns_;
    Timestamp last_inbound_ns_;
    Timestamp last_outbound_ns_;

    const server::UserConfig* negotiated_user_ = nullptr;
    FixpSbeUserState<ByteSink>* user_state_ = nullptr;  // set while established (bound)
    sbe::SessionUuid session_uuid_{};
    std::uint32_t client_keepalive_interval_ms_ = 0;
    bool client_sequence_known_ = false;
    std::uint64_t next_implicit_client_sequence_number_ = 0;
    bool outbound_sequence_message_required_ = false;
};

}  // namespace exsim::gateway::fixp_sbe

#endif
