#ifndef EXSIM_GATEWAY_FIX_FIX_USER_STATE_HPP
#define EXSIM_GATEWAY_FIX_FIX_USER_STATE_HPP

// Day-lifetime state of one FIX user (server contract 2, gateway mapping 3):
// the ClOrdID token table, per-order records for OrderQty/AvgPx/OrdStatus
// through replace chains, the ExecID counter, the session day state (MsgSeqNum
// counters and resend store) and fault-injection counters.
//
// Inbound: FixConnection decodes and session-validates D/F/G/q into the
// request structs below and calls submit_*; business rejects that need no
// engine decision (token reuse, unknown symbol, unsupported values, injected
// rejects) are answered here and never reach the engine.
// Outbound: the server calls on_events once per batch that mentions this
// user's session; every application message is sequenced and stored, and
// transmitted when a connection is bound.

#include "command.hpp"
#include "event.hpp"
#include "gateway/fix/fix_common.hpp"
#include "gateway/fix/fix_session_day_state.hpp"
#include "gateway/token_table.hpp"
#include "instrument.hpp"
#include "server/config.hpp"
#include "server/core/policies.hpp"
#include <type_traits>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace exsim::gateway::fix {

__extension__ typedef unsigned __int128 FixFillNotional;

struct FixUserLimits {
    std::uint32_t max_tokens = 1u << 16;          // ClOrdIDs per day (orders, cancels, replaces, mass cancels)
    std::size_t store_max_messages = 1u << 16;    // outbound MsgSeqNums kept for resend
    std::size_t store_max_bytes = 16u << 20;
};

// Indexed by the ClOrdID token id. A replace copies the record to the new
// token, so AvgPx and CumQty continue across the chain; the previous token's
// record is then marked superseded.
struct FixOrderRecord {
    FixFillNotional fill_notional;  // sum of LastPx * LastQty in price units
    Price limit_price;
    OrderId order_id;
    Quantity order_qty;
    Quantity cum_qty;
    InstrumentId instrument;
    Side side;
    char order_status;            // OrdStatus(39): '0' '1' '2' '4' '8' 'C'
    char reserving_message_type;  // MsgType(35) that reserved this ClOrdID: 'D' 'F' 'G' 'q'
    bool market_order;            // OrdType(40)=1: ExecutionReports omit Price(44)
    bool superseded_by_replace;   // a later ClOrdID of the replace chain identifies the order now
};

// MassCancelRejectReason(532) values the gateway emits.
enum class MassCancelRejectReason : std::uint8_t {
    MassCancelNotSupported = 0,
    InvalidOrUnknownSecurity = 1,
    Other = 99,
};

// CxlRejReason(102) values the gateway emits.
enum class CancelRejectReason : std::uint8_t {
    TooLateToCancel = 0,
    UnknownOrder = 1,
    DuplicateClOrdId = 6,
    Other = 99,
};

struct FixNewOrderRequest {
    std::string_view client_order_id{};
    std::string_view symbol{};
    Side side = Side::BUY;
    char order_type_code = '2';  // OrdType(40) as received
    char time_in_force_code = '0';
    bool has_price = false;
    Price price = 0;
    std::uint64_t order_qty = 0;
    bool post_only = false;
    StpMode stp = StpMode::None;
};

struct FixCancelRequest {
    std::string_view original_client_order_id{};
    std::string_view client_order_id{};
};

struct FixReplaceRequest {
    std::string_view original_client_order_id{};
    std::string_view client_order_id{};
    char order_type_code = '2';
    bool has_price = false;
    Price price = 0;
    std::uint64_t order_qty = 0;
};

struct FixMassCancelRequest {
    std::string_view client_order_id{};
    char request_type = '7';  // MassCancelRequestType(530) '1'..'7'; '1' symbol and '7' all are supported
    std::string_view symbol{};
};

// OrdRejReason(103) for an engine NewOrder rejection (gateway mapping 3).
[[nodiscard]] constexpr std::uint64_t order_reject_reason_code(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::UnknownInstrument: return 1;
        case RejectReason::DuplicateClOrdId: return 6;
        case RejectReason::MarketClosed: return 2;
        default: return 99;
    }
}

template <FixByteSink Sink>
class FixUserDirectory;

template <FixByteSink Sink>
class FixUserState {
public:
    FixUserState(const server::UserConfig& user, std::string_view exchange_comp_id, const FixUserLimits& limits)
        : session_id_(user.session_id),
          owner_(user.owner),
          default_stp_(user.stp),
          cancel_on_disconnect_(user.cancel_on_disconnect),
          tokens_(limits.max_tokens),
          records_(new FixOrderRecord[std::size_t{limits.max_tokens} + 1]),
          day_(exchange_comp_id, user.username, limits.store_max_messages, limits.store_max_bytes) {}

    FixUserState(const FixUserState&) = delete;
    FixUserState& operator=(const FixUserState&) = delete;

    [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }
    [[nodiscard]] OwnerId owner() const noexcept { return owner_; }
    [[nodiscard]] StpMode default_stp() const noexcept { return default_stp_; }
    [[nodiscard]] bool cancel_on_disconnect() const noexcept { return cancel_on_disconnect_; }
    [[nodiscard]] std::string_view comp_id() const noexcept { return day_.peer_comp_id(); }
    [[nodiscard]] FixSessionDayState<Sink>& session_day_state() noexcept { return day_; }
    [[nodiscard]] const FixSessionDayState<Sink>& session_day_state() const noexcept { return day_; }
    [[nodiscard]] const TokenTable& tokens() const noexcept { return tokens_; }
    [[nodiscard]] std::uint64_t execution_ids_issued() const noexcept { return next_execution_id_ - 1; }

    // Record of a ClOrdID used today, or nullptr.
    [[nodiscard]] const FixOrderRecord* order_record(std::string_view client_order_id) const noexcept {
        const ClOrdId token_id = tokens_.find(client_order_id);
        return token_id == 0 ? nullptr : &records_[token_id];
    }

    // Fault injection (server contract 4).
    void inject_new_order_rejects(std::uint32_t count) noexcept { injected_new_order_rejects_ = count; }
    void drop_out_transmissions(std::uint32_t count) noexcept { day_.suppress_application_transmissions(count); }

    template <class Context>
    void submit_new_order(const FixNewOrderRequest& request, Context& ctx) noexcept {
        const FixTime now{ctx.now(), ctx.realtime_ns()};
        const TokenTable::InternResult interned = tokens_.intern(request.client_order_id);
        switch (interned.status) {
            case TokenTable::InternStatus::Invalid:
                return reject_new_order_at_gateway(request, 99, "ClOrdID longer than 20 characters", now, ctx);
            case TokenTable::InternStatus::Full:
                return reject_new_order_at_gateway(request, 99, "ClOrdID capacity exhausted", now, ctx);
            case TokenTable::InternStatus::Existing:
                return reject_new_order_at_gateway(request, 6, "duplicate ClOrdID", now, ctx);
            case TokenTable::InternStatus::Inserted:
                break;
        }
        FixOrderRecord& record = initialize_record(interned.id, 'D');
        record.market_order = request.order_type_code == '1';
        record.side = request.side;
        record.limit_price = request.has_price ? request.price : 0;
        record.order_qty = request.order_qty <= UINT32_MAX ? static_cast<Quantity>(request.order_qty) : 0;
        bool injected = false;
        if constexpr (requires { ctx.consume_injected_reject(session_id_); }) {
            injected = ctx.consume_injected_reject(session_id_);
        }
        if (injected_new_order_rejects_ > 0) {
            --injected_new_order_rejects_;
            injected = true;
        }
        if (injected) {
            return reject_new_order_at_gateway(request, 99, "injected", now, ctx);
        }
        const InstrumentId instrument =
            request.symbol.size() <= 8 ? ctx.find_instrument(Symbol(request.symbol)) : InstrumentId{0};
        if (instrument == 0) {
            return reject_new_order_at_gateway(request, 1, "unknown symbol", now, ctx);
        }
        record.instrument = instrument;
        if (request.order_type_code != '1' && request.order_type_code != '2') {
            return reject_new_order_at_gateway(request, 99, "unsupported OrdType", now, ctx);
        }
        TimeInForce time_in_force = TimeInForce::Day;
        if (!decode_time_in_force(request.time_in_force_code, time_in_force)) {
            return reject_new_order_at_gateway(request, 99, "unsupported TimeInForce", now, ctx);
        }
        if (request.order_qty > UINT32_MAX) {
            return reject_new_order_at_gateway(request, 99, "INVALID_QUANTITY", now, ctx);
        }
        const bool market = request.order_type_code == '1';
        const auto failure = submit_command(ctx, NewOrder{now.monotonic_ns, instrument, session_id_, owner_, interned.id, request.side,
                            market ? OrdType::Market : OrdType::Limit, time_in_force, request.stp,
                            request.post_only, market ? Price{0} : request.price,
                            static_cast<Quantity>(request.order_qty)});
        if (!failure.empty()) {
            reject_new_order_at_gateway(request, 99, failure, now, ctx);
        }
    }

    template <class Context>
    void submit_cancel(const FixCancelRequest& request, Context& ctx) noexcept {
        const FixTime now{ctx.now(), ctx.realtime_ns()};
        ClOrdId request_token_id = 0;
        if (!intern_request_token(request.client_order_id, request.original_client_order_id, '1', now,
                                  request_token_id, ctx)) {
            return;
        }
        const ClOrdId original_token_id = tokens_.find(request.original_client_order_id);
        if (original_token_id == 0 || original_token_id == request_token_id) {
            return send_cancel_reject(request.client_order_id, request.original_client_order_id, '1',
                                      CancelRejectReason::UnknownOrder, "unknown order", now, ctx);
        }
        const auto failure = submit_command(ctx, CancelOrder{now.monotonic_ns, session_id_, original_token_id, request_token_id, 0});
        if (!failure.empty()) {
            send_cancel_reject(request.client_order_id, request.original_client_order_id, '1',
                               CancelRejectReason::Other, failure, now, ctx);
        }
    }

    template <class Context>
    void submit_replace(const FixReplaceRequest& request, Context& ctx) noexcept {
        const FixTime now{ctx.now(), ctx.realtime_ns()};
        ClOrdId new_token_id = 0;
        if (!intern_request_token(request.client_order_id, request.original_client_order_id, '2', now,
                                  new_token_id, ctx)) {
            return;
        }
        const ClOrdId original_token_id = tokens_.find(request.original_client_order_id);
        if (original_token_id == 0 || original_token_id == new_token_id) {
            return send_cancel_reject(request.client_order_id, request.original_client_order_id, '2',
                                      CancelRejectReason::UnknownOrder, "unknown order", now, ctx);
        }
        if (request.order_type_code != '2') {
            return send_cancel_reject(request.client_order_id, request.original_client_order_id, '2',
                                      CancelRejectReason::Other, "replace supports OrdType 2 (limit) only", now, ctx);
        }
        if (request.order_qty > UINT32_MAX) {
            return send_cancel_reject(request.client_order_id, request.original_client_order_id, '2',
                                      CancelRejectReason::Other, "INVALID_QUANTITY", now, ctx);
        }
        const auto failure = submit_command(ctx, ReplaceOrder{now.monotonic_ns, session_id_, original_token_id, new_token_id, request.price,
                                static_cast<Quantity>(request.order_qty), false});
        if (!failure.empty()) {
            send_cancel_reject(request.client_order_id, request.original_client_order_id, '2',
                               CancelRejectReason::Other, failure, now, ctx);
        }
    }

    template <class Context>
    void submit_mass_cancel(const FixMassCancelRequest& request, Context& ctx) noexcept {
        const FixTime now{ctx.now(), ctx.realtime_ns()};
        const TokenTable::InternResult interned = tokens_.intern(request.client_order_id);
        if (interned.status != TokenTable::InternStatus::Inserted) {
            return send_mass_cancel_rejection(request.client_order_id, request.request_type,
                                              MassCancelRejectReason::Other, now, ctx);
        }
        initialize_record(interned.id, 'q');
        if (request.request_type != '1' && request.request_type != '7') {
            return send_mass_cancel_rejection(request.client_order_id, request.request_type,
                                              MassCancelRejectReason::MassCancelNotSupported, now, ctx);
        }
        InstrumentId instrument = 0;
        if (request.request_type == '1') {
            instrument = request.symbol.size() <= 8 ? ctx.find_instrument(Symbol(request.symbol)) : InstrumentId{0};
            if (instrument == 0) {
                return send_mass_cancel_rejection(request.client_order_id, request.request_type,
                                                  MassCancelRejectReason::InvalidOrUnknownSecurity, now, ctx);
            }
        }
        const auto failure = submit_command(ctx, MassCancel{now.monotonic_ns, session_id_, instrument, interned.id, false});
        if (!failure.empty()) {
            send_mass_cancel_rejection(request.client_order_id, request.request_type, MassCancelRejectReason::Other,
                                       now, ctx);
        }
    }

    // Encodes, in batch order, every event of the batch that concerns this
    // session. For a Fill where both legs are ours, the buy leg goes first.
    template <class Context>
    void on_events(std::span<const Event> batch, Context& ctx) noexcept {
        const FixTime now{ctx.now(), ctx.realtime_ns()};
        for (const Event& event : batch) {
            switch (event.type) {
                case EventType::Accepted:
                    if (event.accepted.session == session_id_) {
                        report_accepted(event, ctx, now);
                    }
                    break;
                case EventType::Rejected:
                    if (event.rejected.session == session_id_) {
                        report_rejected(event, ctx, now);
                    }
                    break;
                case EventType::Fill:
                    if (event.fill.buy.session == session_id_) {
                        report_fill_leg(event, event.fill.buy, ctx, now);
                    }
                    if (event.fill.sell.session == session_id_) {
                        report_fill_leg(event, event.fill.sell, ctx, now);
                    }
                    break;
                case EventType::Canceled:
                    if (event.canceled.session == session_id_) {
                        report_canceled(event, ctx, now);
                    }
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == session_id_) {
                        report_replaced(event, ctx, now);
                    }
                    break;
                case EventType::MassCancelDone:
                    if (event.mass_cancel_done.session == session_id_ &&
                        event.mass_cancel_done.request_cl_ord_id != 0) {
                        send_mass_cancel_report(tokens_.token(event.mass_cancel_done.request_cl_ord_id),
                                                event.instrument != 0 ? '1' : '7', event.instrument != 0 ? '1' : '7',
                                                event.mass_cancel_done.canceled_count, nullptr, now, ctx);
                    }
                    break;
                default:
                    break;
            }
        }
    }

private:
    template <class Context>
    static std::string_view submit_command(Context& ctx, const Command& command) noexcept {
        if constexpr (std::is_void_v<decltype(ctx.submit(command))>) {
            ctx.submit(command);
            return {};
        } else {
            const server::SubmitResult result = ctx.submit(command);
            return result == server::SubmitResult::Applied || result == server::SubmitResult::Deferred
                       ? std::string_view{} : server::to_string(result);
        }
    }

    template <class Context>
    void prepare_application_fault(Context& ctx) noexcept {
        if constexpr (requires { ctx.consume_drop_out(session_id_); }) {
            if (ctx.consume_drop_out(session_id_)) {
                day_.suppress_application_transmissions(1);
            }
        }
    }

    friend class FixUserDirectory<Sink>;

    static bool decode_time_in_force(char code, TimeInForce& time_in_force) noexcept {
        switch (code) {
            case '0': time_in_force = TimeInForce::Day; return true;
            case '1': time_in_force = TimeInForce::GTC; return true;
            case '3': time_in_force = TimeInForce::IOC; return true;
            case '4': time_in_force = TimeInForce::FOK; return true;
            default: return false;
        }
    }

    [[nodiscard]] static Price average_price(const FixOrderRecord& record) noexcept {
        if (record.cum_qty == 0) {
            return 0;
        }
        const FixFillNotional rounded = record.fill_notional + record.cum_qty / 2;
        return static_cast<Price>(rounded / record.cum_qty);
    }

    template <class Context>
    [[nodiscard]] static std::string_view symbol_of(const Context& ctx, InstrumentId instrument) noexcept {
        const InstrumentConfig* config = ctx.instrument(instrument);
        return config != nullptr ? config->symbol.view() : std::string_view("?");
    }

    FixOrderRecord& initialize_record(ClOrdId token_id, char reserving_message_type) noexcept {
        FixOrderRecord& record = records_[token_id];
        record = FixOrderRecord{0, 0, 0, 0, 0, 0, Side::BUY, '8', reserving_message_type, false, false};
        return record;
    }

    // OrdStatus(39) of an OrderCancelReject: the order's current status when
    // OrigClOrdID identifies it now, otherwise 8 (unknown or superseded ClOrdID).
    [[nodiscard]] static char cancel_reject_order_status(const FixOrderRecord* original) noexcept {
        return original != nullptr && original->order_id != 0 && !original->superseded_by_replace
                   ? original->order_status
                   : '8';
    }

    [[nodiscard]] FixOrderRecord* record_for(ClOrdId token_id) noexcept {
        return token_id != 0 && token_id <= tokens_.size() ? &records_[token_id] : nullptr;
    }

    // Reserves the request's own ClOrdID (cancel or replace); answers with
    // OrderCancelReject when it cannot be used. Returns true to continue.
    template <class Context>
    bool intern_request_token(std::string_view client_order_id, std::string_view original_client_order_id,
                              char response_to, FixTime now, ClOrdId& token_id, Context& ctx) noexcept {
        const TokenTable::InternResult interned = tokens_.intern(client_order_id);
        switch (interned.status) {
            case TokenTable::InternStatus::Inserted:
                initialize_record(interned.id, response_to == '1' ? 'F' : 'G');
                token_id = interned.id;
                return true;
            case TokenTable::InternStatus::Existing:
                send_cancel_reject(client_order_id, original_client_order_id, response_to,
                                   CancelRejectReason::DuplicateClOrdId, "duplicate ClOrdID", now, ctx);
                return false;
            case TokenTable::InternStatus::Invalid:
                send_cancel_reject(client_order_id, original_client_order_id, response_to, CancelRejectReason::Other,
                                   "ClOrdID longer than 20 characters", now, ctx);
                return false;
            case TokenTable::InternStatus::Full:
                send_cancel_reject(client_order_id, original_client_order_id, response_to, CancelRejectReason::Other,
                                   "ClOrdID capacity exhausted", now, ctx);
                return false;
        }
        return false;
    }

    template <class Context>
    void send_execution_report(fix44::ExecutionReport& report, FixTime now, Context& ctx) noexcept {
        const DecimalText execution_id(next_execution_id_++);
        report.execution_id.borrow(execution_id.view());
        report.transact_time = now.realtime_ns;
        prepare_application_fault(ctx);
        day_.send_application_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(report, header, buffer.data(), buffer.size());
            },
            now);
    }

    template <class Context>
    void reject_new_order_at_gateway(const FixNewOrderRequest& request, std::uint64_t reason_code,
                                     std::string_view text, FixTime now, Context& ctx) noexcept {
        fix44::ExecutionReport report;
        report.order_id.borrow("NONE");
        report.client_order_id.borrow(request.client_order_id);
        report.execution_type = '8';
        report.order_status = '8';
        report.symbol.borrow(request.symbol);
        report.side = request.side;
        report.quantity = request.order_qty <= UINT32_MAX ? static_cast<Quantity>(request.order_qty) : 0;
        report.has_price = request.has_price && request.order_type_code != '1';
        report.price = report.has_price ? request.price : 0;
        report.has_reject_reason = true;
        report.reject_reason = reason_code;
        report.has_text = true;
        report.text.borrow(text);
        send_execution_report(report, now, ctx);
    }

    // OrdStatus(39) follows the order the OrigClOrdID identifies now: an
    // unknown or superseded ClOrdID reports 8, never the status of a stale
    // record (a live replaced order is not reported Canceled).
    template <class Context>
    void send_cancel_reject(std::string_view client_order_id, std::string_view original_client_order_id,
                            char response_to, CancelRejectReason reason, std::string_view text,
                            FixTime now, Context& ctx) noexcept {
        const FixOrderRecord* original = order_record(original_client_order_id);
        const char order_status = cancel_reject_order_status(original);
        const bool order_known = order_status != '8';
        const DecimalText order_id(order_known ? original->order_id : 0);
        fix44::OrderCancelReject reject;
        reject.order_id.borrow(order_known ? order_id.view() : std::string_view("NONE"));
        reject.client_order_id.borrow(client_order_id);
        reject.original_client_order_id.borrow(original_client_order_id);
        reject.order_status = order_status;
        reject.response_to = response_to;
        reject.reason = static_cast<std::uint64_t>(reason);
        reject.has_text = !text.empty();
        reject.text.borrow(text);
        prepare_application_fault(ctx);
        day_.send_application_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(reject, header, buffer.data(), buffer.size());
            },
            now);
    }

    // MassCancelResponse(531)=0 with MassCancelRejectReason(532).
    template <class Context>
    void send_mass_cancel_rejection(std::string_view client_order_id, char request_type,
                                    MassCancelRejectReason reason, FixTime now, Context& ctx) noexcept {
        send_mass_cancel_report(client_order_id, request_type, '0', 0, &reason, now, ctx);
    }

    // `reject_reason` is null for an accepted request.
    template <class Context>
    void send_mass_cancel_report(std::string_view client_order_id, char request_type, char response,
                                 std::uint64_t affected_orders, const MassCancelRejectReason* reject_reason,
                                 FixTime now, Context& ctx) noexcept {
        const DecimalText report_id(next_execution_id_++);
        fix44::OrderMassCancelReport report;
        report.client_order_id.borrow(client_order_id);
        report.order_id.borrow(report_id.view());
        report.request_type = request_type;
        report.response = response;
        report.has_reject_reason = reject_reason != nullptr;
        report.reject_reason = reject_reason != nullptr ? static_cast<std::uint64_t>(*reject_reason) : 0;
        report.total_affected_orders = affected_orders;
        prepare_application_fault(ctx);
        day_.send_application_message(
            [&](const fix44::Header& header, std::span<char> buffer) {
                return fix44::encode(report, header, buffer.data(), buffer.size());
            },
            now);
    }

    template <class Context>
    void report_accepted(const Event& event, Context& ctx, FixTime now) noexcept {
        const AcceptedEvent& accepted = event.accepted;
        FixOrderRecord* record = record_for(accepted.cl_ord_id);
        if (record == nullptr) {
            return;
        }
        *record = FixOrderRecord{0, accepted.price, accepted.order_id, accepted.qty, 0, event.instrument,
                                 accepted.side, '0', 'D', accepted.ord_type == OrdType::Market, false};
        const DecimalText order_id(accepted.order_id);
        fix44::ExecutionReport report;
        report.order_id.borrow(order_id.view());
        report.client_order_id.borrow(tokens_.token(accepted.cl_ord_id));
        report.execution_type = '0';
        report.order_status = '0';
        report.symbol.borrow(symbol_of(ctx, event.instrument));
        report.side = accepted.side;
        report.quantity = accepted.qty;
        report.has_price = !record->market_order;
        report.price = accepted.price;
        report.leaves_quantity = accepted.qty;
        send_execution_report(report, now, ctx);
    }

    template <class Context>
    void report_rejected(const Event& event, Context& ctx, FixTime now) noexcept {
        const RejectedEvent& rejected = event.rejected;
        if (rejected.cmd == CommandType::NewOrder) {
            FixOrderRecord* record = record_for(rejected.cl_ord_id);
            if (record == nullptr) {
                return;
            }
            record->order_status = '8';
            fix44::ExecutionReport report;
            report.order_id.borrow("NONE");
            report.client_order_id.borrow(tokens_.token(rejected.cl_ord_id));
            report.execution_type = '8';
            report.order_status = '8';
            report.symbol.borrow(symbol_of(ctx, record->instrument));
            report.side = record->side;
            report.quantity = record->order_qty;
            report.has_price = !record->market_order;
            report.price = record->limit_price;
            report.has_reject_reason = true;
            report.reject_reason = order_reject_reason_code(rejected.reason);
            report.has_text = true;
            report.text.borrow(to_string(rejected.reason));
            send_execution_report(report, now, ctx);
            return;
        }
        if (rejected.cmd == CommandType::Cancel || rejected.cmd == CommandType::Replace) {
            send_cancel_reject(tokens_.token(rejected.cl_ord_id), tokens_.token(rejected.orig_cl_ord_id),
                               rejected.cmd == CommandType::Cancel ? '1' : '2',
                               cancel_reject_reason(rejected.reason, record_for(rejected.orig_cl_ord_id)),
                               to_string(rejected.reason), now, ctx);
            return;
        }
        if (rejected.cmd == CommandType::MassCancel) {
            send_mass_cancel_rejection(tokens_.token(rejected.cl_ord_id), event.instrument != 0 ? '1' : '7',
                                       rejected.reason == RejectReason::UnknownInstrument
                                           ? MassCancelRejectReason::InvalidOrUnknownSecurity
                                           : MassCancelRejectReason::Other,
                                       now, ctx);
        }
    }

    // The engine reports an order it no longer holds as UnknownOrder. When the
    // OrigClOrdID identifies a completed order (filled, canceled or expired)
    // the request was too late (102=0); otherwise the order is unknown (102=1).
    [[nodiscard]] static CancelRejectReason cancel_reject_reason(RejectReason reason,
                                                                 const FixOrderRecord* original) noexcept {
        if (reason != RejectReason::UnknownOrder) {
            return CancelRejectReason::Other;
        }
        const char order_status = cancel_reject_order_status(original);
        return order_status == '2' || order_status == '4' || order_status == 'C' ? CancelRejectReason::TooLateToCancel
                                                                                 : CancelRejectReason::UnknownOrder;
    }

    template <class Context>
    void report_fill_leg(const Event& event, const FillLeg& leg, Context& ctx, FixTime now) noexcept {
        FixOrderRecord* record = record_for(leg.cl_ord_id);
        if (record == nullptr) {
            return;
        }
        record->fill_notional += static_cast<FixFillNotional>(event.fill.price) * event.fill.qty;
        record->cum_qty = leg.cum_qty;
        record->order_status = leg.leaves_qty == 0 ? '2' : '1';
        const DecimalText order_id(leg.order_id);
        fix44::ExecutionReport report;
        report.order_id.borrow(order_id.view());
        report.client_order_id.borrow(tokens_.token(leg.cl_ord_id));
        report.execution_type = 'F';
        report.order_status = record->order_status;
        report.symbol.borrow(symbol_of(ctx, event.instrument));
        report.side = record->side;
        report.quantity = record->order_qty;
        report.has_price = !record->market_order;
        report.price = record->limit_price;
        report.last_quantity = event.fill.qty;
        report.last_price = event.fill.price;
        report.leaves_quantity = leg.leaves_qty;
        report.cumulative_quantity = leg.cum_qty;
        report.average_price = average_price(*record);
        send_execution_report(report, now, ctx);
    }

    template <class Context>
    void report_canceled(const Event& event, Context& ctx, FixTime now) noexcept {
        const CanceledEvent& canceled = event.canceled;
        FixOrderRecord* record = record_for(canceled.cl_ord_id);
        if (record == nullptr) {
            return;
        }
        FixOrderRecord* request = record_for(canceled.request_cl_ord_id);
        if (request != nullptr && request->reserving_message_type == 'G' && canceled.leaves_qty == 0 &&
            canceled.reason == CancelReason::UserRequested) {
            return report_replaced_to_filled(event, *record, *request, ctx, now);
        }
        const bool expired = canceled.reason == CancelReason::EndOfDay;
        record->cum_qty = canceled.cum_qty;
        if (canceled.leaves_qty == 0) {
            record->order_status = expired ? 'C' : '4';
        }
        const DecimalText order_id(canceled.order_id);
        fix44::ExecutionReport report;
        report.order_id.borrow(order_id.view());
        if (canceled.request_cl_ord_id != 0) {
            report.client_order_id.borrow(tokens_.token(canceled.request_cl_ord_id));
            report.has_original_client_order_id = true;
            report.original_client_order_id.borrow(tokens_.token(canceled.cl_ord_id));
        } else {
            report.client_order_id.borrow(tokens_.token(canceled.cl_ord_id));
        }
        report.execution_type = expired ? 'C' : '4';
        report.order_status = expired ? 'C' : '4';
        report.symbol.borrow(symbol_of(ctx, event.instrument));
        report.side = canceled.side;
        report.quantity = record->order_qty;
        report.has_price = !record->market_order;
        report.price = canceled.price;
        report.leaves_quantity = canceled.leaves_qty;
        report.cumulative_quantity = canceled.cum_qty;
        report.average_price = average_price(*record);
        if (canceled.reason != CancelReason::UserRequested) {
            report.has_text = true;
            report.text.borrow(to_string(canceled.reason));
        }
        send_execution_report(report, now, ctx);
    }

    // A replace to a quantity at or below CumQty: the engine cancels the
    // remainder and the order is done as filled. FIX 4.4 reports the accepted
    // replace, 150=5 39=2, with OrderQty(38) = CumQty and LeavesQty 0; the new
    // ClOrdID now identifies the (complete) order.
    template <class Context>
    void report_replaced_to_filled(const Event& event, FixOrderRecord& previous, FixOrderRecord& current,
                                   Context& ctx, FixTime now) noexcept {
        const CanceledEvent& canceled = event.canceled;
        current = previous;
        current.reserving_message_type = 'G';
        current.cum_qty = canceled.cum_qty;
        current.order_qty = canceled.cum_qty;
        current.order_status = '2';
        previous.order_status = '2';
        previous.superseded_by_replace = true;
        const DecimalText order_id(canceled.order_id);
        fix44::ExecutionReport report;
        report.order_id.borrow(order_id.view());
        report.client_order_id.borrow(tokens_.token(canceled.request_cl_ord_id));
        report.has_original_client_order_id = true;
        report.original_client_order_id.borrow(tokens_.token(canceled.cl_ord_id));
        report.execution_type = '5';
        report.order_status = '2';
        report.symbol.borrow(symbol_of(ctx, event.instrument));
        report.side = canceled.side;
        report.quantity = current.order_qty;
        report.has_price = !current.market_order;
        report.price = canceled.price;
        report.leaves_quantity = 0;
        report.cumulative_quantity = canceled.cum_qty;
        report.average_price = average_price(current);
        send_execution_report(report, now, ctx);
    }

    template <class Context>
    void report_replaced(const Event& event, Context& ctx, FixTime now) noexcept {
        const ReplacedEvent& replaced = event.replaced;
        FixOrderRecord* previous = record_for(replaced.orig_cl_ord_id);
        FixOrderRecord* current = record_for(replaced.cl_ord_id);
        if (previous == nullptr || current == nullptr) {
            return;
        }
        if (current != previous) {
            *current = *previous;
            current->reserving_message_type = 'G';
            current->market_order = false;  // replacements are limit orders
            previous->order_status = '4';
            previous->superseded_by_replace = true;
        }
        current->order_id = replaced.new_order_id;
        current->limit_price = replaced.price;
        current->order_qty = replaced.qty;
        current->cum_qty = replaced.cum_qty;
        current->order_status = replaced.cum_qty > 0 ? '1' : '0';
        const DecimalText order_id(replaced.new_order_id);
        fix44::ExecutionReport report;
        report.order_id.borrow(order_id.view());
        report.client_order_id.borrow(tokens_.token(replaced.cl_ord_id));
        report.has_original_client_order_id = true;
        report.original_client_order_id.borrow(tokens_.token(replaced.orig_cl_ord_id));
        report.execution_type = '5';
        report.order_status = current->order_status;
        report.symbol.borrow(symbol_of(ctx, event.instrument));
        report.side = replaced.side;
        report.quantity = replaced.qty;
        report.price = replaced.price;
        report.leaves_quantity = replaced.leaves_qty;
        report.cumulative_quantity = replaced.cum_qty;
        report.average_price = average_price(*current);
        send_execution_report(report, now, ctx);
    }

    SessionId session_id_;
    OwnerId owner_;
    StpMode default_stp_;
    bool cancel_on_disconnect_;
    TokenTable tokens_;
    std::unique_ptr<FixOrderRecord[]> records_;  // index = token id; written on first use
    FixSessionDayState<Sink> day_;
    std::uint64_t next_execution_id_ = 1;
    std::uint32_t injected_new_order_rejects_ = 0;
    std::uint64_t dispatch_generation_ = 0;
};

// All FIX users of the server, built at setup. find() maps a SessionId to its
// state in O(1); find_by_comp_id() serves Logon. on_events() is the
// server contract 2.3 dispatch for FIX users: each distinct session of the
// batch gets exactly one FixUserState::on_events call, in first-seen order.
template <FixByteSink Sink>
class FixUserDirectory {
public:
    FixUserDirectory(std::span<const server::UserConfig> users, std::string_view exchange_comp_id,
                     const FixUserLimits& limits)
        : index_by_session_(std::make_unique<std::uint16_t[]>(std::size_t{1} << 16)) {
        exchange_comp_id_.assign(exchange_comp_id.substr(0, kMaxCompIdLength));
        for (const server::UserConfig& user : users) {
            if (user.protocol != server::Protocol::Fix44 || user.session_id == 0 ||
                index_by_session_[user.session_id] != 0 || users_.size() >= UINT16_MAX) {
                continue;
            }
            users_.push_back(std::make_unique<FixUserState<Sink>>(user, exchange_comp_id_.view(), limits));
            index_by_session_[user.session_id] = static_cast<std::uint16_t>(users_.size());
        }
    }

    [[nodiscard]] std::string_view exchange_comp_id() const noexcept { return exchange_comp_id_.view(); }
    [[nodiscard]] std::size_t size() const noexcept { return users_.size(); }

    [[nodiscard]] FixUserState<Sink>* find(SessionId session) noexcept {
        const std::uint16_t position = index_by_session_[session];
        return position == 0 ? nullptr : users_[position - 1u].get();
    }
    [[nodiscard]] FixUserState<Sink>* find_by_comp_id(std::string_view comp_id) noexcept {
        for (const auto& user : users_) {
            if (user->comp_id() == comp_id) {
                return user.get();
            }
        }
        return nullptr;
    }

    template <class Context>
    void on_events(std::span<const Event> batch, Context& ctx) noexcept {
        ++dispatch_generation_;
        for (const Event& event : batch) {
            switch (event.type) {
                case EventType::Accepted: dispatch_once(event.accepted.session, batch, ctx); break;
                case EventType::Rejected: dispatch_once(event.rejected.session, batch, ctx); break;
                case EventType::Fill:
                    dispatch_once(event.fill.buy.session, batch, ctx);
                    dispatch_once(event.fill.sell.session, batch, ctx);
                    break;
                case EventType::Canceled: dispatch_once(event.canceled.session, batch, ctx); break;
                case EventType::Replaced: dispatch_once(event.replaced.session, batch, ctx); break;
                case EventType::MassCancelDone: dispatch_once(event.mass_cancel_done.session, batch, ctx); break;
                default: break;
            }
        }
    }

private:
    template <class Context>
    void dispatch_once(SessionId session, std::span<const Event> batch, Context& ctx) noexcept {
        FixUserState<Sink>* user = find(session);
        if (user == nullptr || user->dispatch_generation_ == dispatch_generation_) {
            return;
        }
        user->dispatch_generation_ = dispatch_generation_;
        user->on_events(batch, ctx);
    }

    FixedText<kMaxCompIdLength> exchange_comp_id_;
    std::vector<std::unique_ptr<FixUserState<Sink>>> users_;
    std::unique_ptr<std::uint16_t[]> index_by_session_;
    std::uint64_t dispatch_generation_ = 0;
};

}  // namespace exsim::gateway::fix

#endif
