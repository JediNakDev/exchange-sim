#ifndef TESTS_EXSIM_SIM_BOTS_FAKE_EXCHANGE_HPP
#define TESTS_EXSIM_SIM_BOTS_FAKE_EXCHANGE_HPP
#include "exsim/sim/bots/bot_set.hpp"
#include <array>
#include <vector>
#include <cassert>

namespace bot_tests {
using namespace exsim;
using namespace exsim::sim::bots;
struct FakeExchange {
    struct Order { NewOrder command{}; OrderId id = 0; Quantity leaves = 0, cum = 0; };
    InstrumentConfig config{};
    Phase phase = Phase::Continuous;
    Timestamp time = 0;
    std::array<Order, 512> book{};
    std::vector<Command> commands;
    std::array<Event, 4096> pending{};
    std::size_t event_count = 0;
    OrderId next_id = 1;
    MatchId next_match = 1;
    void* listener = nullptr;
    void (*deliver)(void*, std::span<const Event>) = nullptr;
    bool deferred = false;
    bool reject_next = false;
    FakeExchange() {
        config.tick_size = 10; config.min_price = 10; config.max_price = 2000;
        config.reference_price = 1000; config.lot_size = 5; config.max_order_qty = 1000;
        commands.reserve(200000);
    }
    template <typename Bot> void bind(Bot& bot) {
        listener = &bot;
        deliver = [](void* target, std::span<const Event> events) { static_cast<Bot*>(target)->on_events(events); };
    }
    Timestamp now() const { return time; }
    const InstrumentConfig* instrument(InstrumentId id) const { return id == 1 ? &config : nullptr; }
    void emit(Event event) { assert(event_count < pending.size()); pending[event_count++] = event; }
    Event event(EventType type) const { Event result{}; result.type = type; result.instrument = 1; result.ts = time; return result; }
    void flush() {
        if (deliver && event_count) deliver(listener, {pending.data(), event_count});
        event_count = 0;
    }
    Order* find(SessionId session, ClOrdId id) {
        for (auto& order : book) if (order.id && order.command.session == session && order.command.cl_ord_id == id) return &order;
        return nullptr;
    }
    std::size_t count(SessionId session) const {
        std::size_t result = 0;
        for (const auto& order : book) result += order.id && order.command.session == session;
        return result;
    }
    void reject(const Command& command, RejectReason reason) {
        auto result = event(EventType::Rejected);
        result.rejected.cmd = command.type; result.rejected.reason = reason;
        if (command.type == CommandType::NewOrder) {
            result.rejected.session = command.new_order.session; result.rejected.cl_ord_id = command.new_order.cl_ord_id;
        } else if (command.type == CommandType::Replace) {
            result.rejected.session = command.replace.session; result.rejected.orig_cl_ord_id = command.replace.orig_cl_ord_id;
            result.rejected.cl_ord_id = command.replace.cl_ord_id;
        } else {
            result.rejected.session = command.cancel.session; result.rejected.orig_cl_ord_id = command.cancel.orig_cl_ord_id;
            result.rejected.cl_ord_id = command.cancel.cl_ord_id;
        }
        emit(result);
    }
    void cancel(Order& order, CancelReason reason, bool resting = true, ClOrdId request = 0) {
        auto result = event(EventType::Canceled);
        result.canceled = {order.id, order.command.cl_ord_id, request, order.command.session, order.command.side,
                           order.command.price, order.leaves, order.cum, reason, resting};
        emit(result); order = {};
    }
    static FillLeg leg(const Order& order) {
        return {order.id, order.command.cl_ord_id, order.command.session, order.command.owner, order.leaves, order.cum};
    }
    void finish(Order& incoming, bool already_resting = false) {
        if (phase == Phase::Continuous && !already_resting) {
            while (incoming.id && incoming.leaves) {
                Order* best = nullptr;
                for (auto& resting : book) {
                    if (!resting.id || resting.command.side == incoming.command.side) continue;
                    const bool buy = incoming.command.side == Side::BUY;
                    if (buy ? incoming.command.price < resting.command.price : incoming.command.price > resting.command.price) continue;
                    if (!best || (resting.command.price == best->command.price ? resting.id < best->id :
                        buy ? resting.command.price < best->command.price : resting.command.price > best->command.price)) best = &resting;
                }
                if (!best) break;
                if (incoming.command.owner && incoming.command.owner == best->command.owner && incoming.command.stp != StpMode::None) {
                    const auto mode = incoming.command.stp;
                    if (mode != StpMode::CancelAggressor) cancel(*best, CancelReason::SelfTradePrevention);
                    if (mode != StpMode::CancelResting) { cancel(incoming, CancelReason::SelfTradePrevention, false); return; }
                    continue;
                }
                const auto qty = std::min(incoming.leaves, best->leaves);
                incoming.leaves -= qty; incoming.cum += qty; best->leaves -= qty; best->cum += qty;
                auto result = event(EventType::Fill);
                result.fill = {next_match++, best->command.price, qty,
                    incoming.command.side == Side::BUY ? Aggressor::Buy : Aggressor::Sell,
                    incoming.command.side == Side::BUY ? leg(incoming) : leg(*best),
                    incoming.command.side == Side::SELL ? leg(incoming) : leg(*best)};
                emit(result);
                if (!best->leaves) *best = {};
            }
        }
        if (!incoming.leaves) return;
        if (incoming.command.tif == TimeInForce::IOC) { cancel(incoming, CancelReason::ImmediateOrCancel, false); return; }
        if (!already_resting) {
            auto result = event(EventType::Rested);
            result.rested = {incoming.id, incoming.command.cl_ord_id, incoming.command.session,
                             incoming.command.side, incoming.command.price, incoming.leaves};
            emit(result);
        }
        for (auto& slot : book) if (!slot.id) { slot = incoming; return; }
        assert(false);
    }
    bool would_cross(const NewOrder& incoming) const {
        for (const auto& order : book) {
            if (order.id && order.command.side != incoming.side && (incoming.side == Side::BUY
                ? incoming.price >= order.command.price : incoming.price <= order.command.price)) return true;
        }
        return false;
    }
    void submit(const Command& command) {
        commands.push_back(command); time = command.ts();
        if (reject_next) { reject_next = false; reject(command, RejectReason::CapacityExceeded); }
        else if (command.type == CommandType::NewOrder) {
            const auto& request = command.new_order;
            if (!phase_accepts_limit_orders(phase)) reject(command, RejectReason::MarketClosed);
            else if (request.tif == TimeInForce::IOC && phase != Phase::Continuous) reject(command, RejectReason::NotAllowedInPhase);
            else if (!request.qty || request.qty > config.max_order_qty || request.qty % config.lot_size) reject(command, RejectReason::InvalidQuantity);
            else if (request.post_only && phase == Phase::Continuous && would_cross(request)) reject(command, RejectReason::PostOnlyWouldCross);
            else {
                Order incoming{request, next_id++, request.qty, 0};
                auto result = event(EventType::Accepted);
                result.accepted = {incoming.id, request.cl_ord_id, request.session, request.owner, request.side,
                    request.type, request.tif, request.stp, request.post_only, request.price, request.qty};
                emit(result); finish(incoming);
            }
        } else if (command.type == CommandType::Cancel) {
            if (auto* order = find(command.cancel.session, command.cancel.orig_cl_ord_id)) cancel(*order, CancelReason::UserRequested, true, command.cancel.cl_ord_id);
            else reject(command, RejectReason::UnknownOrder);
        } else if (command.type == CommandType::Replace) {
            const auto& request = command.replace;
            auto* order = find(request.session, request.orig_cl_ord_id);
            if (!order) reject(command, RejectReason::UnknownOrder);
            else if (request.qty > config.max_order_qty) reject(command, RejectReason::InvalidQuantity);
            else if (request.qty <= order->cum) cancel(*order, CancelReason::UserRequested);
            else {
                const auto old = *order;
                const bool kept = request.price == old.command.price && request.qty - old.cum <= old.leaves;
                Order updated = old; updated.id = kept ? old.id : next_id++;
                updated.command.price = request.price; updated.command.qty = request.qty; updated.leaves = request.qty - updated.cum;
                *order = {};
                auto result = event(EventType::Replaced);
                result.replaced = {old.id, updated.id, old.command.cl_ord_id, updated.command.cl_ord_id, request.session,
                    updated.command.side, old.command.price, request.price, old.leaves, request.qty, updated.leaves, updated.cum, kept};
                emit(result); finish(updated, kept);
            }
        }
        if (!deferred) flush();
    }
    void change_phase(Phase value) {
        if (value == Phase::Closed) for (auto& order : book) if (order.id) cancel(order, CancelReason::EndOfDay);
        auto result = event(EventType::PhaseChanged); result.phase_changed = {phase, value}; phase = value;
        emit(result); flush();
    }
};
inline MarketMakerConfig maker_config() {
    MarketMakerConfig config; config.session_id = 1; config.instrument = 1; config.owner = 1;
    config.fair_value_step_ms = 0; config.size = 100; return config;
}
template <typename Bot> void tick(Bot& bot, FakeExchange& exchange, Timestamp now) {
    exchange.time = now; bot.on_timer(now, exchange);
}
} // namespace bot_tests
#endif
