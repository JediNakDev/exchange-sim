#include "runtime.hpp"
#include "exsim/ipc/strategy_wire.hpp"
#include "exsim/research/seed_derivation.hpp"
#include "exsim/research/strategies/fixed_spread_market_maker.hpp"
#include "exsim/research/strategies/inventory_aware_market_maker.hpp"
#include "exsim/research/strategies/twap_executor.hpp"
#include "exsim/research/strategies/immediate_sweep_executor.hpp"
#include <iostream>

namespace exsim::trader {
using namespace ipc;
namespace {
using namespace research;
struct TraderHost {
    int socket;
    StrategyVisibleState visible;
    bool failed = false;
    static std::uint64_t request(void* host, const StrategyAction& action) noexcept {
        auto& self = *static_cast<TraderHost*>(host);
        if (self.failed) return 0;
        try {
            Frame frame; action_fields(frame,const_cast<StrategyAction&>(action)); send_frame(self.socket,frame);
            auto reply = receive_frame(self.socket); std::uint64_t result = 0; reply.field(result); reply.finish();
            receive_visible(self.socket,self.visible);
            return result;
        } catch (...) { self.failed = true; return 0; }
    }
};
template<class Strategy>
int run_strategy(int socket, const ExperimentConfig& config, std::uint64_t seed) {
    TraderHost host{socket,{},false};
    host.visible.order_capacity = 16;
    while (host.visible.order_capacity < 2 * static_cast<std::size_t>(config.risk.max_open_orders)) host.visible.order_capacity *= 2;
    host.visible.orders = std::make_unique<StrategyOrderView[]>(host.visible.order_capacity);
    sim::Rng random(derive_stream_seed(seed,"strategy"));
    StrategyContext context(StrategyRemote{&host,&TraderHost::request,&random},host.visible);
    Strategy strategy(config.strategy);
    Frame ready; std::uint32_t version = 1; ready.field(version); send_frame(socket,ready);
    for (;;) {
        auto frame = receive_frame(socket,true);
        SimulationEventKind kind = SimulationEventKind::StrategyStart; frame.field(kind);
        MarketDataUpdate update; ExecutionReport report;
        if (kind == SimulationEventKind::MarketDataDelivery) frame.field(update);
        else if (kind == SimulationEventKind::ExecutionReportDelivery) frame.field(report);
        frame.finish();
        receive_visible(socket,host.visible);
        switch (kind) {
            case SimulationEventKind::StrategyStart: strategy.on_start(context); break;
            case SimulationEventKind::StrategyWakeup: strategy.on_wakeup(context); break;
            case SimulationEventKind::MarketDataDelivery: strategy.on_market_data(update,context); break;
            case SimulationEventKind::ExecutionReportDelivery: strategy.on_execution_report(report,context); break;
            default: throw std::runtime_error("invalid trader callback");
        }
        if (host.failed) throw std::runtime_error("strategy action transport failed");
        StrategyAction done; Frame response; action_fields(response,done); send_frame(socket,response);
    }
}
}
int run(int socket) {
    try {
        auto bootstrap = receive_frame(socket);
        std::uint32_t version = 0; std::uint64_t seed = 0;
        ExperimentConfig config;
        bootstrap.fields(version,seed,config.strategy,config.risk); bootstrap.finish();
        if (version != 1) throw std::runtime_error("unsupported strategy wire version");
        if (config.risk.max_open_orders == 0 || config.risk.max_open_orders > 65536 ||
            config.strategy.parent.slice_interval_ns == 0 || config.strategy.inventory.skew_unit == 0)
            throw std::runtime_error("invalid trader configuration");
        switch (config.strategy.kind) {
            case StrategyKind::FixedSpreadMarketMaker: return run_strategy<strategies::FixedSpreadMarketMaker>(socket,config,seed);
            case StrategyKind::InventoryAwareMarketMaker: return run_strategy<strategies::InventoryAwareMarketMaker>(socket,config,seed);
            case StrategyKind::TwapExecutor: return run_strategy<strategies::TwapExecutor>(socket,config,seed);
            case StrategyKind::ImmediateSweepExecutor: return run_strategy<strategies::ImmediateSweepExecutor>(socket,config,seed);
        }
    } catch (const std::exception& error) { std::cerr << "exsim_traderd: " << error.what() << '\n'; }
    return 1;
}
} // namespace exsim::trader
