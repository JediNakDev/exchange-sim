#include "runtime.hpp"
#include "exsim/ipc/strategy_wire.hpp"
#include "exsim/research/trader_session.hpp"
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
    TraderSession* session = nullptr;
    static std::uint64_t request(void* host, const StrategyAction& action) noexcept {
        auto& self = *static_cast<TraderHost*>(host);
        if (self.failed) return 0;
        try {
            StrategyAction control = action;
            const bool order = action.kind == StrategyActionKind::NewOrder || action.kind == StrategyActionKind::Cancel || action.kind == StrategyActionKind::Replace;
            if (order) { self.session->order_action(action,self.visible); control.kind = StrategyActionKind::Session; }
            std::uint64_t result = 0;
            for (unsigned round = 0;; ++round) {
                if (round > 32) throw std::runtime_error("research session recovery did not settle");
                Frame frame; action_fields(frame,control); send_frame(self.socket,frame);
                if (control.kind == StrategyActionKind::Session) self.session->send_request(self.socket);
                auto reply = receive_frame(self.socket); std::uint64_t value = 0; reply.field(value); reply.finish();
                if (round == 0) {
                    result = value;
                    if (order) self.session->accept_result(action,result);
                }
                self.session->receive_control(self.socket,self.visible.now);
                receive_visible(self.socket,self.visible);
                if (!self.session->has_output()) break;
                control = {}; control.kind = StrategyActionKind::Session;
            }
            return result;
        } catch (...) { self.failed = true; return 0; }
    }
};
template<class Strategy>
int run_strategy(int socket, const ExperimentConfig& config, std::uint64_t seed) {
    TraderSession session(config);
    TraderHost host{socket,{},false,&session};
    host.visible.order_capacity = 16;
    while (host.visible.order_capacity < 2 * static_cast<std::size_t>(config.risk.max_open_orders)) host.visible.order_capacity *= 2;
    host.visible.orders = std::make_unique<StrategyOrderView[]>(host.visible.order_capacity);
    sim::Rng random(derive_stream_seed(seed,"strategy"));
    StrategyContext context(StrategyRemote{&host,&TraderHost::request,&random},host.visible);
    Strategy strategy(config.strategy);
    session.connect();
    StrategyAction session_action; session_action.kind = StrategyActionKind::Session;
    for (unsigned round = 0; session.has_output(); ++round) {
        if (round > 32) throw std::runtime_error("research session login did not settle");
        (void)TraderHost::request(&host,session_action);
        if (host.failed) throw std::runtime_error("research protocol login failed");
    }
    if (!session.active()) throw std::runtime_error("research protocol login was not acknowledged");
    StrategyAction login_done; Frame login_complete; action_fields(login_complete,login_done); send_frame(socket,login_complete);
    Frame ready; std::uint32_t version = 2; ready.field(version); send_frame(socket,ready);
    for (;;) {
        auto frame = receive_frame(socket,true);
        SimulationEventKind kind = SimulationEventKind::StrategyStart; Timestamp callback_time = 0; bool reconnect = false; frame.fields(kind,callback_time,reconnect);
        MarketDataUpdate update; ExecutionReport report;
        if (kind == SimulationEventKind::MarketDataDelivery) frame.field(update);
        else if (kind == SimulationEventKind::ExecutionReportDelivery) frame.field(report);
        frame.finish();
        session.receive_control(socket,callback_time);
        if (kind == SimulationEventKind::MarketDataDelivery) session.receive_market(socket,update,callback_time);
        if (kind == SimulationEventKind::ExecutionReportDelivery) session.receive_report(socket,report,callback_time);
        receive_visible(socket,host.visible);
        if (reconnect) session.reconnect(callback_time);
        if (kind == SimulationEventKind::SessionTimer) session.tick(callback_time);
        // A gap detected by the real client/receiver is recovered through its
        // own protocol before the strategy consumes the delivered observation.
        if (session.has_output()) {
            (void)TraderHost::request(&host,session_action);
            if (host.failed) throw std::runtime_error("research session recovery failed");
        }
        if (kind == SimulationEventKind::ExecutionReportDelivery && report.kind == ReportKind::Rested) {
            const auto& view = host.visible.orders[report.cl_ord_id & (host.visible.order_capacity-1)];
            if (view.live && view.cl_ord_id == report.cl_ord_id && view.cancel_after_ack && view.pending == PendingRequest::None) {
                StrategyAction retry; retry.kind = StrategyActionKind::Cancel; retry.token = report.cl_ord_id;
                (void)TraderHost::request(&host,retry);
            }
        }
        if (kind == SimulationEventKind::ExecutionReportDelivery) session.finish_report(report);
        if (kind == SimulationEventKind::MarketDataDelivery) session.finish_market(update);
        switch (kind) {
            case SimulationEventKind::SessionTimer: break;
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
        std::uint64_t max_orders = 0;
        bootstrap.fields(version,seed,config.strategy,config.risk,config.instrument,config.order_entry_protocol,max_orders); bootstrap.finish();
        if (max_orders == 0 || max_orders > (1u << 20)) throw std::runtime_error("invalid research book capacity");
        config.engine.max_orders = static_cast<std::size_t>(max_orders);
        if (config.order_entry_protocol > server::Protocol::FixpSbe) throw std::runtime_error("invalid research order-entry protocol");
        if (version != 2) throw std::runtime_error("unsupported strategy wire version");
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
