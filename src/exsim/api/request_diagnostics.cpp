#include "exsim/api/request_diagnostics.hpp"
#include "exsim/api/api_json.hpp"
#include <map>
#include <algorithm>
#include <deque>
namespace exsim::api {
void write_request_diagnostics(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events) {
    json.begin_array();
    for (const auto request : {CommandType::NewOrder, CommandType::Cancel, CommandType::Replace}) {
        std::uint64_t submitted = 0, rejected = 0;
        std::map<std::string, std::uint64_t> reasons;
        std::map<ClOrdId, std::deque<Timestamp>> pending;
        research::DurationStatistic transit;
        for (const auto& event : events) {
            if (event.actor != EventActor::ResearchStrategy && !event.has(kIsOwnOrder)) continue;
            if ((request == CommandType::NewOrder && event.kind == CanonicalEventKind::StrategyOrderSubmitted) ||
                (request == CommandType::Cancel && event.kind == CanonicalEventKind::StrategyCancelSubmitted) ||
                (request == CommandType::Replace && event.kind == CanonicalEventKind::StrategyReplaceSubmitted)) { ++submitted; pending[event.client_order_id].push_back(event.simulation_time_ns); }
            const bool arrived = (request == CommandType::NewOrder && event.kind == CanonicalEventKind::OrderArrivedAtExchange) ||
                (request == CommandType::Cancel && event.kind == CanonicalEventKind::CancelArrivedAtExchange) ||
                (request == CommandType::Replace && event.kind == CanonicalEventKind::ReplaceArrivedAtExchange);
            if (arrived) {
                auto found = pending.find(event.client_order_id);
                if (found != pending.end() && !found->second.empty()) {
                    transit.add(event.simulation_time_ns - found->second.front()); found->second.pop_front();
                }
            }
            if (event.kind == CanonicalEventKind::OrderRejected && event.rejected_command == request) { ++rejected; ++reasons[to_api_string(event.reject_reason)]; }
        }
        json.begin_object().key("requestKind").string(to_api_string(request));
        json.key("submittedCount").unsigned_text(submitted).key("exchangeRejectedCount").unsigned_text(rejected);
        json.key("arrivalTransit").begin_object().key("sampleCount").unsigned_text(transit.count);
        json.key("minNs"); if (transit.count) json.unsigned_text(transit.min); else json.null();
        json.key("maxNs"); if (transit.count) json.unsigned_text(transit.max); else json.null();
        json.key("meanNs"); if (transit.count) json.string(format_derived_decimal(static_cast<double>(transit.sum) / static_cast<double>(transit.count))); else json.null();
        json.end_object();
        json.key("rejectReasons").begin_array();
        for (const auto& [reason, count] : reasons) json.begin_object().key("reason").string(reason).key("count").unsigned_text(count).end_object();
        json.end_array().end_object();
    }
    json.end_array();
}
void write_replace_transits(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events, std::span<const std::uint64_t> positions) {
    json.begin_array();
    std::uint64_t submit = kNoEventPosition;
    for (const auto position : positions) {
        const auto& event = events[position];
        if (event.kind == CanonicalEventKind::StrategyReplaceSubmitted) submit = position;
        if (event.kind != CanonicalEventKind::ReplaceArrivedAtExchange || submit == kNoEventPosition) continue;
        std::uint64_t fill = kNoEventPosition;
        for (const auto prior : positions) if (prior > submit && prior < position && events[prior].kind == CanonicalEventKind::OrderFilled &&
            (events[prior].order_id == event.order_id || events[prior].buy_leg_order_id == event.order_id || events[prior].sell_leg_order_id == event.order_id)) fill = prior;
        json.begin_object().key("submitEventPosition").unsigned_text(submit).key("arrivalEventPosition").unsigned_text(position);
        json.key("transitNs").unsigned_text(event.simulation_time_ns - events[submit].simulation_time_ns);
        json.key("fillBeforeArrivalEventPosition"); if (fill == kNoEventPosition) json.null(); else json.unsigned_text(fill);
        json.end_object();
        submit = kNoEventPosition;
    }
    json.end_array();
}
bool write_fifo_evidence(ApiJsonWriter& json, std::span<const CanonicalEventRecord> events, std::span<const OrderId> ids, std::uint64_t last) {
    constexpr std::size_t limit = 500;
    std::size_t count = 0;
    ExchangeTruthBook book;
    std::map<OrderId, CanonicalEventRecord> replacements;
    json.begin_array();
    const auto emit = [&](std::uint64_t position, const char* reason, OrderId id, const ExchangeTruthBook::QueuePosition& queue) {
        if (count++ >= limit) return;
        const auto replacement = replacements.find(id);
        json.begin_object().key("eventPosition").unsigned_text(position).key("simulationTimeNs").unsigned_text(events[position].simulation_time_ns);
        json.key("reason").string(reason).key("source").string("exact_simulated_truth");
        json.key("orderId").unsigned_text(id).key("replacedOldOrderId");
        if (replacement != replacements.end()) json.unsigned_text(replacement->second.replaced_old_order_id); else json.null();
        json.key("priorityKept"); if (replacement != replacements.end()) json.boolean(replacement->second.has(kPriorityKept)); else json.null();
        json.key("price").string(format_price_units(queue.price));
        json.key("queuePositionAvailable").boolean(queue.found);
        json.key("sharesAhead"); if (queue.found) json.unsigned_text(queue.shares_ahead); else json.null();
        json.key("ordersAhead"); if (queue.found) json.unsigned_text(queue.orders_ahead); else json.null();
        json.key("levelTotalShares"); if (queue.found) json.unsigned_text(queue.level_shares); else json.null();
        json.end_object();
    };
    for (std::uint64_t position = 0; position < events.size() && position <= last; ++position) {
        const auto& event = events[position];
        OrderId tracked = 0;
        ExchangeTruthBook::QueuePosition before;
        for (const auto id : ids) { before = book.queue_position(id); if (before.found) { tracked = id; break; } }
        const bool own = std::find(ids.begin(), ids.end(), event.order_id) != ids.end();
        const bool touches = tracked && (event.order_id == tracked || event.buy_leg_order_id == tracked || event.sell_leg_order_id == tracked);
        if (touches && event.kind == CanonicalEventKind::OrderFilled) emit(position, "immediately_before_fill", tracked, before);
        if (touches && event.kind == CanonicalEventKind::OrderCanceled) emit(position, "immediately_before_cancel", tracked, before);
        if (own && event.kind == CanonicalEventKind::OrderReplaced) replacements[event.order_id] = event;
        book.apply(event);
        if (own && (event.kind == CanonicalEventKind::OrderRested || event.kind == CanonicalEventKind::OrderReplaced)) {
            const auto after = book.queue_position(event.order_id);
            if (after.found) emit(position, event.kind == CanonicalEventKind::OrderRested ? "rested" : event.has(kPriorityKept) ? "replace_priority_kept" : "replace_priority_reset", event.order_id, after);
            else if (event.kind == CanonicalEventKind::OrderReplaced) {
                // Priority has been removed; matching/re-rest has not happened yet.
                ExchangeTruthBook::QueuePosition absent; absent.price = event.price;
                emit(position, "replace_priority_reset", event.order_id, absent);
            }
        }
        if (tracked && !touches) {
            const auto after = book.queue_position(tracked);
            if (after.found && after.shares_ahead < before.shares_ahead)
                emit(position, event.kind == CanonicalEventKind::OrderFilled ? "queue_advanced_by_fill" : "queue_advanced_by_cancel", tracked, after);
        }
    }
    json.end_array();
    return count > limit;
}

}
