#include "research/process_simulation.hpp"
#include <bit>

namespace exsim::research {
StrategyIdentity builtin_strategy_identity(StrategyKind kind) {
    switch (kind) {
        case StrategyKind::FixedSpreadMarketMaker: return {"fixed_spread_market_maker",1};
        case StrategyKind::InventoryAwareMarketMaker: return {"inventory_aware_market_maker",1};
        case StrategyKind::TwapExecutor: return {"twap_executor",1};
        case StrategyKind::ImmediateSweepExecutor: return {"immediate_sweep_executor",1};
    }
    throw std::invalid_argument("unknown built-in strategy kind");
}
ProcessSimulation::ProcessSimulation(const ExperimentConfig& config, std::uint64_t seed) : SimulationCore(config,seed), session_(*this) {
    ipc::Frame bootstrap; std::uint32_t version = 2;
    bootstrap.fields(version,seed);
    bootstrap.field(const_cast<StrategyConfig&>(config.strategy));
    bootstrap.field(const_cast<RiskLimits&>(config.risk));
    bootstrap.field(const_cast<InstrumentConfig&>(config.instrument));
    auto protocol = config.order_entry_protocol; bootstrap.field(protocol);
    auto max_orders = static_cast<std::uint64_t>(config.engine.max_orders); bootstrap.field(max_orders);
    ipc::send_frame(trader_.socket(),bootstrap);
    // Complete real protocol login/negotiation before the strategy can run.
    for (unsigned attempt = 0;; ++attempt) {
        if (attempt > 32) throw std::runtime_error("research session handshake did not settle");
        auto request = ipc::receive_frame(trader_.socket());
        StrategyAction action; ipc::action_fields(request,action); request.finish();
        if (action.kind == StrategyActionKind::Done) break;
        if (action.kind != StrategyActionKind::Session) throw std::runtime_error("order before research session login");
        const auto result = session_.receive_request(trader_.socket());
        ipc::Frame response; auto value = result; response.field(value); ipc::send_frame(trader_.socket(),response);
        session_.send_control(trader_.socket()); ipc::send_visible(trader_.socket(),strategy_view());
    }
    auto ready = ipc::receive_frame(trader_.socket()); std::uint32_t reply = 0; ready.field(reply); ready.finish();
    if (reply != version) throw std::runtime_error("trader protocol version mismatch");
}
RunStop ProcessSimulation::run(const RunBounds& bounds) {
    begin_run();
    std::uint64_t steps = 0;
    for (;;) {
        const auto stop = prepare_step(bounds,steps);
        if (stop != RunStop::Continue) return stop;
        ++steps;
        if (next_step_is_observation()) { run_observation_step(); continue; }
        SimulationEvent event;
        if (!pop_simulation_event(event)) continue;
        if (event.kind == SimulationEventKind::StrategyWakeup && !accept_wakeup(event)) { after_strategy_callback(); continue; }
        if (event.kind == SimulationEventKind::MarketDataDelivery) deliver_market_data(event.data.market_data);
        if (event.kind == SimulationEventKind::ExecutionReportDelivery) deliver_execution_report(event.data.report);
        callback(event);
        after_strategy_callback();
    }
}
void ProcessSimulation::callback(const SimulationEvent& event) {
    const int socket = trader_.socket();
    ipc::Frame delivery; auto kind = event.kind; auto callback_time = now(); delivery.fields(kind,callback_time,reconnect_); reconnect_ = false;
    if (kind == SimulationEventKind::MarketDataDelivery) delivery.field(const_cast<MarketDataUpdate&>(event.data.market_data));
    if (kind == SimulationEventKind::ExecutionReportDelivery) delivery.field(const_cast<ExecutionReport&>(event.data.report));
    if (kind == SimulationEventKind::SessionTimer) session_.tick();
    ipc::send_frame(socket,delivery);
    session_.send_control(socket);
    if (kind == SimulationEventKind::MarketDataDelivery) session_.send_market(socket,event.data.market_data.sequence);
    if (kind == SimulationEventKind::ExecutionReportDelivery) session_.send_report(socket,event.protocol_sequence);
    ipc::send_visible(socket,strategy_view());
    auto& context = strategy_context();
    // Each action is acknowledged before the next action. This preserves
    // risk/order-view semantics and the scheduler's existing enqueue order.
    for (;;) {
        auto frame = ipc::receive_frame(socket); StrategyAction action;
        ipc::action_fields(frame,action); frame.finish();
        std::uint64_t result = 0;
        switch (action.kind) {
            case StrategyActionKind::Done: return;
            case StrategyActionKind::Headroom: result = std::bit_cast<std::uint64_t>(context.position_headroom(action.side,action.token)); break;
            case StrategyActionKind::Session: result = session_.receive_request(socket); break;
            case StrategyActionKind::NewOrder:
            case StrategyActionKind::Cancel:
            case StrategyActionKind::Replace: throw std::runtime_error("research order bypassed exchange protocol");
            case StrategyActionKind::Wake: context.wake_at(action.time); break;
            case StrategyActionKind::Parent: context.declare_parent_order(action.side,action.qty); break;
            case StrategyActionKind::Decision: context.record_execution_decision(action.decision); break;
        }
        ipc::Frame response; response.field(result); ipc::send_frame(socket,response); session_.send_control(socket); ipc::send_visible(socket,strategy_view());
    }
}
} // namespace exsim::research
