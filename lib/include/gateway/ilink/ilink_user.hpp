#ifndef EXSIM_GATEWAY_ILINK_USER_HPP
#define EXSIM_GATEWAY_ILINK_USER_HPP
#include "gateway/sequenced_store.hpp"
#include "gateway/token_table.hpp"
#include "protocol/ilink3.hpp"
#include "server/config.hpp"
#include "server/core/policies.hpp"

#include <optional>

namespace exsim::gateway::ilink {
namespace wire = protocol::ilink3;
namespace schema = wire::schema;

struct OrderMetadata {
    std::array<char, 20> sender{}, client{};
    std::array<char, 5> location{};
    std::uint64_t request = 0, party = 0;
    InstrumentId instrument = 0;
    OrderId order_id = 0;
    Price price = 0;
    Quantity quantity = 0;
    Side side = Side::BUY;
    std::uint8_t tif = 0, manual = 0, liquidity = UINT8_MAX;
    [[nodiscard]] static std::string_view text(std::span<const char> bytes) noexcept {
        std::size_t size = 0; while (size < bytes.size() && bytes[size]) ++size;
        return {bytes.data(), size};
    }
};

template<class Sink> class IlinkUser {
public:
    IlinkUser(const server::UserConfig& configuration, const server::EngineSection& limits)
        : config(configuration), store(limits.store_messages_per_session, limits.store_bytes_per_session),
          previous_store(limits.store_messages_per_session, limits.store_bytes_per_session),
          tokens_(limits.max_tokens_per_session), requests_(limits.max_tokens_per_session),
          metadata_(std::make_unique<OrderMetadata[]>(std::size_t{limits.max_tokens_per_session} + 1)),
          affected_(limits.engine.max_orders) {
        if (!wire::decode_secret(configuration.ilink_secret_key, key)) throw std::invalid_argument("invalid iLink HMAC key");
    }
    void negotiate(std::uint64_t next_uuid) noexcept {
        previous_uuid = uuid;
        std::swap(store, previous_store); store.clear();
        uuid = next_uuid; expected_sequence = 1; gap_pending = false;
    }
    template<class Core> void application(const protocol::cme::MessageView& message, Core& core) noexcept {
        last_template_ = message.header.template_id;
        switch (message.header.template_id) {
            case 514: new_order(message, core); break;
            case 515: replace_order(message, core); break;
            case 516: cancel_order(message, core); break;
            case 529: mass_cancel(message, core); break;
            default: business_reject("unsupported application template", 0, core); break;
        }
    }
    template<class Core> void on_events(std::span<const Event> events, Core& core) noexcept {
        for (const auto& event : events) {
            switch (event.type) {
                case EventType::Accepted:
                    if (event.accepted.session == config.session_id) {
                        auto& metadata = metadata_[event.accepted.cl_ord_id];
                        metadata.order_id = event.accepted.order_id;
                        auto report = report_root<schema::ExecutionReportNew522>(metadata, event.ts, core);
                        send(report, core);
                    }
                    break;
                case EventType::Rejected:
                    if (event.rejected.session == config.session_id) {
                        request_failed_ = true;
                        const auto reason = to_string(event.rejected.reason);
                        if (event.rejected.cmd == CommandType::Cancel) reject_request<schema::OrderCancelReject535>(reason, event.ts, core);
                        else if (event.rejected.cmd == CommandType::Replace) reject_request<schema::OrderCancelReplaceReject536>(reason, event.ts, core);
                        else {
                            auto report = report_root<schema::ExecutionReportReject523>(pending_, event.ts, core);
                            report.set(schema::ExecutionReportReject523::Text, reason);
                            report.set(schema::ExecutionReportReject523::OrdRejReason, 99);
                            send(report, core);
                        }
                    }
                    break;
                case EventType::Fill:
                    if (event.fill.buy.session == config.session_id) report_fill(event, event.fill.buy, core);
                    if (event.fill.sell.session == config.session_id) report_fill(event, event.fill.sell, core);
                    break;
                case EventType::Canceled:
                    if (event.canceled.session == config.session_id) {
                        const auto& canceled = event.canceled;
                        const auto& metadata = pending_command_ == CommandType::Cancel && canceled.request_cl_ord_id == pending_token_
                            ? pending_ : metadata_[canceled.cl_ord_id];
                        if (canceled.reason == CancelReason::ImmediateOrCancel || canceled.reason == CancelReason::FillOrKill ||
                            canceled.reason == CancelReason::EndOfDay) {
                            auto report = report_root<schema::ExecutionReportElimination524>(metadata, event.ts, core);
                            report.set(schema::ExecutionReportElimination524::CumQty, canceled.cum_qty);
                            send(report, core);
                            break;
                        }
                        if (canceled.request_cl_ord_id == kMaxClOrdId && affected_count_ < affected_.size()) {
                            auto& entry = affected_[affected_count_++]; entry.fill(0);
                            std::copy(metadata.client.begin(), metadata.client.end(), entry.begin());
                            protocol::cme::store_le(entry.data() + 20, canceled.order_id);
                            protocol::cme::store_le(entry.data() + 28, canceled.canceled_qty);
                        }
                        auto report = report_root<schema::ExecutionReportCancel534>(metadata, event.ts, core);
                        report.set(schema::ExecutionReportCancel534::CumQty, canceled.cum_qty);
                        report.set(schema::ExecutionReportCancel534::ExecRestatementReason,
                            canceled.reason == CancelReason::CancelOnDisconnect ? 100 :
                            canceled.reason == CancelReason::SelfTradePrevention ? 103 : 8);
                        send(report, core);
                    }
                    break;
                case EventType::Replaced:
                    if (event.replaced.session == config.session_id) {
                        auto& metadata = metadata_[event.replaced.cl_ord_id];
                        metadata.order_id = event.replaced.new_order_id;
                        auto report = report_root<schema::ExecutionReportModify531>(metadata, event.ts, core);
                        report.set(schema::ExecutionReportModify531::CumQty, event.replaced.cum_qty);
                        report.set(schema::ExecutionReportModify531::LeavesQty, event.replaced.leaves_qty);
                        report.set(schema::ExecutionReportModify531::PriorityIndicator, event.replaced.priority_kept ? 0 : 1);
                        send(report, core);
                    }
                    break;
                case EventType::MassCancelDone:
                    if (event.mass_cancel_done.session == config.session_id && event.mass_cancel_done.request_cl_ord_id == kMaxClOrdId) {
                        using M = schema::OrderMassActionReport562;
                        const auto report_id = store.next_seq();
                        std::size_t offset = 0;
                        do {
                            const auto count = std::min(std::size_t{32}, affected_count_ - offset);
                            auto report = report_root<M>(pending_, event.ts, core);
                            report.set(M::MassActionReportID, report_id); report.set(M::MassActionResponse, 1);
                            report.set(M::MassActionScope, pending_scope_);
                            if (pending_scope_ == 9) report.set(M::MarketSegmentID, core.config().market_data.market_segment_id);
                            report.set(M::TotalAffectedOrders, event.mass_cancel_done.canceled_count);
                            report.set(M::LastFragment, offset + count == affected_count_ ? 1 : 0);
                            std::array<char, 1027> tail{};
                            protocol::cme::store_le(tail.data(), std::uint16_t{32}); tail[2] = static_cast<char>(count);
                            for (std::size_t i = 0; i < count; ++i)
                                std::copy(affected_[offset + i].begin(), affected_[offset + i].end(), tail.begin() + 3 + i * 32);
                            send(report, core, std::span(tail).first(3 + count * 32)); offset += count;
                        } while (offset < affected_count_);
                    }
                    break;
                default: break;
            }
        }
    }
    template<class Core> void business_reject(std::string_view text, std::uint64_t request, Core& core) noexcept {
        using M = schema::BusinessReject521;
        wire::Root<M> report;
        report.set(M::SeqNum, static_cast<std::uint32_t>(store.next_seq())); report.set(M::UUID, uuid);
        report.set(M::Text, text); report.set(M::BusinessRejectRefID, request);
        report.set(M::SendingTimeEpoch, core.realtime_ns()); report.set(M::RefSeqNum, expected_sequence - 1);
        report.set(M::BusinessRejectReason, 5);
        report.set(M::RefMsgType, last_template_ == 515 ? "G" : last_template_ == 516 ? "F" : last_template_ == 529 ? "CA" : "D");
        report.set(M::PossRetransFlag, 0);
        send(report, core);
    }
    server::UserConfig config;
    std::array<char, 32> key{};
    SequencedStore store, previous_store;
    std::uint64_t uuid = 0, previous_uuid = 0, last_auth_timestamp = 0;
    std::uint32_t expected_sequence = 1;
    Timestamp last_send_time = 0;
    server::ConnectionId reserved_by = 0;
    std::optional<Sink> sink;
    bool output_failed = false, gap_pending = false;

private:
    template<class M, class Core> wire::Root<M> report_root(const OrderMetadata& metadata, Timestamp timestamp, Core& core) noexcept {
        wire::Root<M> report;
        if constexpr (requires { M::SeqNum; }) report.set(M::SeqNum, static_cast<std::uint32_t>(store.next_seq()));
        if constexpr (requires { M::UUID; }) report.set(M::UUID, uuid);
        if constexpr (requires { M::ExecID; }) {
            std::array<char, 40> digits{};
            auto end = std::to_chars(digits.data(), digits.data() + 20, uuid).ptr;
            *end++ = ':';
            end = std::to_chars(end, digits.data() + digits.size(), static_cast<std::uint32_t>(store.next_seq())).ptr;
            report.set(M::ExecID, std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
        }
        if constexpr (requires { M::SenderID; }) report.set(M::SenderID, OrderMetadata::text(metadata.sender));
        if constexpr (requires { M::ClOrdID; }) report.set(M::ClOrdID, OrderMetadata::text(metadata.client));
        if constexpr (requires { M::Location; }) report.set(M::Location, OrderMetadata::text(metadata.location));
        if constexpr (requires { M::OrderRequestID; }) report.set(M::OrderRequestID, metadata.request);
        if constexpr (requires { M::PartyDetailsListReqID; }) report.set(M::PartyDetailsListReqID, metadata.party);
        if constexpr (requires { M::OrderID; }) report.set(M::OrderID, metadata.order_id);
        if constexpr (requires { M::Price; }) {
            std::int64_t price = 0; (void)protocol::cme::price_to_price9(metadata.price, price); report.set(M::Price, price);
        }
        if constexpr (requires { M::OrderQty; }) report.set(M::OrderQty, metadata.quantity);
        if constexpr (requires { M::SecurityID; }) {
            const auto* instrument = core.instrument(metadata.instrument);
            if constexpr (M::id == 562) { if (instrument) report.set(M::SecurityID, instrument->cme_security_id); }
            else report.set(M::SecurityID, instrument ? instrument->cme_security_id : 0);
        }
        if constexpr (M::id != 562) {
            if constexpr (requires { M::OrdType; }) report.set(M::OrdType, '2');
            if constexpr (requires { M::Side; }) report.set(M::Side, metadata.side == Side::BUY ? 1 : 2);
            if constexpr (requires { M::TimeInForce; }) report.set(M::TimeInForce, metadata.tif);
        }
        if constexpr (requires { M::ManualOrderIndicator; }) report.set(M::ManualOrderIndicator, metadata.manual);
        if constexpr (requires { M::LiquidityFlag; }) report.set(M::LiquidityFlag, metadata.liquidity);
        if constexpr (requires { M::PossRetransFlag; }) report.set(M::PossRetransFlag, 0);
        if constexpr (requires { M::SendingTimeEpoch; }) report.set(M::SendingTimeEpoch, core.realtime_ns());
        if constexpr (requires { M::TransactTime; }) {
            report.set(M::TransactTime, core.epoch_ns(timestamp));
        }
        return report;
    }
    template<class M, class Core> void send(const wire::Root<M>& report, Core& core,
                                           std::span<const char> tail = M::empty_tail) noexcept {
        if (output_failed) return;
        std::array<char, 2048> bytes{};
        const auto size = report.encode(bytes, tail);
        if (store.next_seq() > 999999999 || !size || !store.append(std::span(bytes).first(size))) { output_failed = true; return; }
        if (!sink || core.consume_drop_out(config.session_id)) return;
        char* output = sink->prepare(size);
        if (!output) { output_failed = true; return; }
        std::memcpy(output, bytes.data(), size); sink->commit(size);
        last_send_time = core.now();
    }
    template<class Core> void report_fill(const Event& event, const FillLeg& leg, Core& core) noexcept {
        using M = schema::ExecutionReportTradeOutright525;
        const auto& metadata = metadata_[leg.cl_ord_id];
        auto report = report_root<M>(metadata, event.ts, core);
        std::int64_t price = 0; (void)protocol::cme::price_to_price9(event.fill.price, price);
        report.set(M::LastPx, price); report.set(M::LastQty, event.fill.qty);
        report.set(M::CumQty, leg.cum_qty); report.set(M::LeavesQty, leg.leaves_qty);
        report.set(M::OrdStatus, leg.leaves_qty ? 1 : 2); report.set(M::SecExecID, event.fill.match_id);
        const auto id = event.fill.match_id < UINT32_MAX ? static_cast<std::uint32_t>(event.fill.match_id) : UINT32_MAX;
        report.set(M::MDTradeEntryID, id); report.set(M::SideTradeID, id);
        report.set(M::AggressorIndicator, (event.fill.aggressor == Aggressor::Buy && metadata.side == Side::BUY) ||
                                        (event.fill.aggressor == Aggressor::Sell && metadata.side == Side::SELL) ? 1 : 0);
        report.set(M::Ownership, 0);
        report.set(M::TradeDate, static_cast<std::uint16_t>(core.realtime_ns() / 86400000000000ull));
        std::array<char, 62> tail{};
        protocol::cme::store_le(tail.data(), std::uint16_t{15}); tail[2] = 1;
        std::copy(M::NoFills::defaults.begin(), M::NoFills::defaults.end(), tail.begin() + 3);
        protocol::cme::store_le(tail.data() + 3, price); protocol::cme::store_le(tail.data() + 11, event.fill.qty);
        tail[15] = '0'; tail[16] = '1'; tail[17] = 1;  // FIFO fill
        protocol::cme::store_le(tail.data() + 18, std::uint16_t{41}); tail[20] = 1;
        std::copy(M::NoOrderEvents::defaults.begin(), M::NoOrderEvents::defaults.end(), tail.begin() + 21);
        protocol::cme::store_le(tail.data() + 21, price);
        protocol::cme::store_le(tail.data() + 34, id); protocol::cme::store_le(tail.data() + 38, event.fill.qty);
        tail[42] = 4; tail[43] = 100;
        send(report, core, tail);
    }
    template<class M, class Core> void reject_request(std::string_view reason, Timestamp timestamp, Core& core) noexcept {
        auto report = report_root<M>(pending_, timestamp, core);
        report.set(M::Text, reason); report.set(M::CxlRejReason, 99); send(report, core);
    }
    template<class M, class Core> bool metadata(const protocol::cme::MessageView& message, OrderMetadata& out, Core& core) noexcept {
        const auto request = wire::get(message, M::OrderRequestID);
        bool liquidity_valid = true;
        if constexpr (requires { M::LiquidityFlag; }) {
            const auto liquidity = wire::get(message, M::LiquidityFlag);
            liquidity_valid = liquidity == 0 || liquidity == 1 || liquidity == UINT8_MAX;
        }
        if (request == UINT64_MAX || wire::get(message, M::PartyDetailsListReqID) != config.ilink_party_id ||
            !wire::valid_text(message, M::SenderID) || !wire::valid_text(message, M::Location) ||
            wire::get(message, M::Location).size() < 2 || wire::get(message, M::ManualOrderIndicator) > 1 || !liquidity_valid) {
            business_reject("invalid request, party, operator or location", request, core); return false;
        }
        std::array<char, 20> digits{};
        const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), request).ptr;
        const auto intern = requests_.intern({digits.data(), static_cast<std::size_t>(end - digits.data())});
        if (intern.status != TokenTable::InternStatus::Inserted) { business_reject("duplicate request ID or request capacity", request, core); return false; }
        out.request = request; out.party = config.ilink_party_id;
        const auto sender = wire::get(message, M::SenderID), location = wire::get(message, M::Location);
        out.sender.fill(0); out.location.fill(0);
        std::copy(sender.begin(), sender.end(), out.sender.begin()); std::copy(location.begin(), location.end(), out.location.begin());
        out.manual = wire::get(message, M::ManualOrderIndicator);
        if constexpr (requires { M::LiquidityFlag; }) out.liquidity = wire::get(message, M::LiquidityFlag);
        return true;
    }
    template<class Core> InstrumentId instrument_id(std::int32_t security, const Core& core) const noexcept {
        const auto& instruments = core.config().instruments;
        for (std::size_t i = 0; i < instruments.size(); ++i) if (instruments[i].cme_security_id == security) return static_cast<InstrumentId>(i + 1);
        return 0;
    }
    template<class M, class Core> bool order_fields(const protocol::cme::MessageView& message, OrderMetadata& out, Core& core) noexcept {
        const auto client = wire::get(message, M::ClOrdID);
        const auto side = wire::get(message, M::Side), tif = wire::get(message, M::TimeInForce);
        out.instrument = instrument_id(wire::get(message, M::SecurityID), core);
        const auto optional_qty = [](std::uint32_t value) { return value == 0 || value == UINT32_MAX; };
        bool valid = out.instrument && wire::valid_text(message, M::ClOrdID) && (side == 1 || side == 2) &&
            (tif == 0 || tif == 3) && wire::get(message, M::OrdType) == '2' &&
            wire::get(message, M::StopPx) == protocol::cme::kNullPrice9 &&
            optional_qty(wire::get(message, M::MinQty)) && optional_qty(wire::get(message, M::DisplayQty)) &&
            wire::get(message, M::ExpireDate) == UINT16_MAX && wire::get(message, M::ExecInst) == 0 &&
            (wire::get(message, M::ExecutionMode) == 0 || wire::get(message, M::ExecutionMode) == 'A') &&
            (wire::get(message, M::ManagedOrder) == UINT8_MAX || wire::get(message, M::ManagedOrder) == 0) &&
            wire::get(message, M::ShortSaleType) == UINT8_MAX &&
            wire::get(message, M::DiscretionPrice) == protocol::cme::kNullPrice9 &&
            protocol::cme::price_from_price9(wire::get(message, M::Price), out.price);
        if constexpr (requires { M::ReservationPrice; }) valid = valid && wire::get(message, M::ReservationPrice) == protocol::cme::kNullPrice9;
        out.quantity = wire::get(message, M::OrderQty);
        valid = valid && out.quantity > 0 && out.quantity <= 99999;
        if (!valid) { business_reject("unsupported or invalid order fields", out.request, core); return false; }
        out.client.fill(0); std::copy(client.begin(), client.end(), out.client.begin());
        out.side = side == 1 ? Side::BUY : Side::SELL; out.tif = tif;
        return true;
    }
    template<class Core> void submit(const Command& command, Core& core) noexcept {
        request_failed_ = false;
        const auto result = core.submit(command);
        if (result != server::SubmitResult::Applied && result != server::SubmitResult::Deferred) {
            request_failed_ = true; business_reject(server::to_string(result), pending_.request, core);
        }
    }
    template<class Core> void new_order(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::NewOrderSingle514;
        OrderMetadata value;
        if (!metadata<M>(message, value, core) || !order_fields<M>(message, value, core)) return;
        const auto token = tokens_.intern(OrderMetadata::text(value.client));
        if (token.status != TokenTable::InternStatus::Inserted) { business_reject("duplicate client ID or token capacity", value.request, core); return; }
        metadata_[token.id] = value; pending_ = value; pending_token_ = token.id; pending_command_ = CommandType::NewOrder;
        NewOrder order;
        order.session = config.session_id; order.owner = config.owner; order.stp = config.stp;
        order.cl_ord_id = token.id; order.instrument = value.instrument; order.side = value.side;
        order.price = value.price; order.qty = value.quantity; order.tif = value.tif == 3 ? TimeInForce::IOC : TimeInForce::Day;
        if (core.consume_injected_reject(config.session_id)) { business_reject("injected rejection", value.request, core); return; }
        submit(Command{order}, core);
    }
    template<class M, class Core> std::optional<OrderInfo> find_order(const protocol::cme::MessageView& message, Core& core) noexcept {
        const auto id = wire::get(message, M::OrderID);
        if (id == 0) return {};
        auto order = id != UINT64_MAX ? core.engine().find_order(id) :
                     core.engine().find_order(config.session_id, tokens_.find(wire::get(message, M::ClOrdID)));
        if (order && order->session != config.session_id) order.reset();
        return order;
    }
    template<class Core> void cancel_order(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::OrderCancelRequest516;
        const auto order = find_order<M>(message, core);
        OrderMetadata value = order ? metadata_[order->cl_ord_id] : OrderMetadata{};
        if (!metadata<M>(message, value, core)) return;
        pending_ = value; pending_token_ = order ? order->cl_ord_id : 0; pending_command_ = CommandType::Cancel;
        const auto client = wire::get(message, M::ClOrdID);
        pending_.client.fill(0); std::copy(client.begin(), client.end(), pending_.client.begin());
        if (!wire::valid_text(message, M::ClOrdID) || !order || wire::get(message, M::Side) != (order->side == Side::BUY ? 1 : 2) ||
            instrument_id(wire::get(message, M::SecurityID), core) != order->instrument || !wire::get(message, M::OrigOrderUser).empty()) {
            reject_request<schema::OrderCancelReject535>("unknown order or mismatched instrument/side", core.now(), core); return;
        }
        CancelOrder cancel; cancel.session = config.session_id; cancel.orig_cl_ord_id = order->cl_ord_id; cancel.cl_ord_id = order->cl_ord_id;
        submit(Command{cancel}, core);
        pending_command_ = CommandType::NewOrder;
    }
    template<class Core> void replace_order(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::OrderCancelReplaceRequest515;
        const auto order = find_order<M>(message, core);
        OrderMetadata value = order ? metadata_[order->cl_ord_id] : OrderMetadata{};
        if (!metadata<M>(message, value, core) || !order_fields<M>(message, value, core)) return;
        pending_ = value; pending_command_ = CommandType::Replace;
        if (!order || order->instrument != value.instrument || order->side != value.side || wire::get(message, M::OFMOverride) != 0 ||
            value.tif != metadata_[order->cl_ord_id].tif) {
            reject_request<schema::OrderCancelReplaceReject536>("unknown order or unsupported replace fields", core.now(), core); return;
        }
        const auto token = tokens_.intern(OrderMetadata::text(value.client));
        if (!token.id || (token.status == TokenTable::InternStatus::Existing && token.id != order->cl_ord_id)) {
            reject_request<schema::OrderCancelReplaceReject536>("duplicate client ID or token capacity", core.now(), core); return;
        }
        const auto old = metadata_[token.id];
        value.order_id = order->order_id; metadata_[token.id] = value; pending_ = value;
        ReplaceOrder replace; replace.session = config.session_id; replace.orig_cl_ord_id = order->cl_ord_id;
        replace.cl_ord_id = token.id; replace.price = value.price; replace.qty = value.quantity;
        submit(Command{replace}, core);
        if (request_failed_) metadata_[token.id] = old;
    }
    template<class Core> void mass_cancel(const protocol::cme::MessageView& message, Core& core) noexcept {
        using M = schema::OrderMassActionRequest529;
        OrderMetadata value;
        if (!metadata<M>(message, value, core)) return;
        const auto scope = wire::get(message, M::MassActionScope);
        if ((scope != 1 && scope != 7 && scope != 9) ||
            (scope == 9 && wire::get(message, M::MarketSegmentID) != core.config().market_data.market_segment_id) ||
            wire::get(message, M::Side) != UINT8_MAX || wire::get(message, M::OrdType) != 0 ||
            wire::get(message, M::TimeInForce) != UINT8_MAX || wire::get(message, M::MassCancelRequestType) != UINT8_MAX ||
            !wire::get(message, M::OrigOrderUser).empty() || !wire::get(message, M::SecurityGroup).empty()) {
            business_reject("unsupported mass cancel scope or filter", value.request, core); return;
        }
        if (scope == 1) value.instrument = instrument_id(wire::get(message, M::SecurityID), core);
        if (scope == 1 && !value.instrument) { business_reject("unknown instrument", value.request, core); return; }
        pending_ = value; pending_scope_ = scope; pending_command_ = CommandType::MassCancel;
        affected_count_ = 0;
        MassCancel cancel; cancel.session = config.session_id; cancel.instrument = value.instrument; cancel.cl_ord_id = kMaxClOrdId;
        submit(Command{cancel}, core);
        pending_command_ = CommandType::NewOrder;
    }
    TokenTable tokens_, requests_;
    std::unique_ptr<OrderMetadata[]> metadata_;
    std::vector<std::array<char, 32>> affected_;
    std::size_t affected_count_ = 0;
    OrderMetadata pending_;
    ClOrdId pending_token_ = 0;
    CommandType pending_command_ = CommandType::NewOrder;
    std::uint8_t pending_scope_ = 7;
    bool request_failed_ = false;
    std::uint16_t last_template_ = 514;
};

template<class Sink> class IlinkUsers {
public:
    explicit IlinkUsers(const server::ServerConfig& config) {
        for (const auto& user : config.users) if (user.protocol == server::Protocol::Ilink3)
            users_.push_back(std::make_unique<IlinkUser<Sink>>(user, config.engine));
    }
    IlinkUser<Sink>* find(SessionId id) noexcept {
        for (auto& user : users_) if (user->config.session_id == id) return user.get();
        return nullptr;
    }
    IlinkUser<Sink>* credentials(std::string_view access, std::string_view session, std::string_view firm) noexcept {
        for (auto& user : users_)
            if (user->config.ilink_access_key == access && user->config.ilink_session == session && user->config.ilink_firm == firm) return user.get();
        return nullptr;
    }
private:
    std::vector<std::unique_ptr<IlinkUser<Sink>>> users_;
};
}  // namespace exsim::gateway::ilink
#endif
