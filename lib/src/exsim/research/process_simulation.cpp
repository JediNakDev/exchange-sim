#include "exsim/research/process_simulation.hpp"
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
ProcessSimulation::ProcessSimulation(const ExperimentConfig& config, std::uint64_t seed) : SimulationCore(config,seed) {
    ipc::Frame bootstrap; std::uint32_t version = 1;
    bootstrap.fields(version,seed);
    bootstrap.field(const_cast<StrategyConfig&>(config.strategy));
    bootstrap.field(const_cast<RiskLimits&>(config.risk)); ipc::send_frame(trader_.socket(),bootstrap);
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
    ipc::Frame delivery; auto kind = event.kind; delivery.field(kind);
    if (kind == SimulationEventKind::MarketDataDelivery) delivery.field(const_cast<MarketDataUpdate&>(event.data.market_data));
    if (kind == SimulationEventKind::ExecutionReportDelivery) delivery.field(const_cast<ExecutionReport&>(event.data.report));
    ipc::send_frame(socket,delivery); ipc::send_visible(socket,strategy_view());
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
            case StrategyActionKind::NewOrder: result = context.submit_limit_order(action.side,action.price,action.qty,action.tif,action.post_only); break;
            case StrategyActionKind::Cancel: result = context.cancel_order(action.token); break;
            case StrategyActionKind::Replace: result = context.replace_order(action.token,action.price,action.qty); break;
            case StrategyActionKind::Wake: context.wake_at(action.time); break;
            case StrategyActionKind::Parent: context.declare_parent_order(action.side,action.qty); break;
            case StrategyActionKind::Decision: context.record_execution_decision(action.decision); break;
        }
        ipc::Frame response; response.field(result); ipc::send_frame(socket,response); ipc::send_visible(socket,strategy_view());
    }
}
} // namespace exsim::research
