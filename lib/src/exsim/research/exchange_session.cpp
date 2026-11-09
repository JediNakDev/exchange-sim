#include "research/exchange_session.hpp"
#include "ipc/session_packets.hpp"
#include "server/runtime/order_entry_gateways.hpp"
#include "md/publisher.hpp"
#include "md/retransmission_server.hpp"
#include <variant>
#include <exchange/flat_hash_map.hpp>

namespace exsim::research {
namespace {
using ipc::SessionSink;
using Ouch = server::runtime::BasicOuchGateway<SessionSink>;
using Fix = server::runtime::BasicFixGateway<SessionSink>;
using Sbe = server::runtime::BasicFixpSbeGateway<SessionSink>;
// A fixed deterministic epoch is used only for FIX/FIXP timestamps. Timers
// and latency use injected simulated time, never the wall clock.
constexpr std::uint64_t kSessionEpoch = 1'783'036'800'000'000'000ull;
struct ResearchContext {
    SimulationCore& simulation;
    server::ServerConfig configuration;
    struct Token { ClOrdId stable = 0, current = 0, pending = 0, pending_cancel = 0; };
    std::unique_ptr<ClOrdId[]> stable_by_gateway;
    lob::FlatHashMap<Token> tokens{65536};
    std::size_t token_capacity, order_capacity;
    server::ConnectionId bound = 0;
    std::uint64_t result = 0, decoded_orders = 0;
    bool failed = false;
    explicit ResearchContext(SimulationCore& core)
        : simulation(core), token_capacity(65536), order_capacity(core.strategy_view().order_capacity) {
        configuration.engine.engine = research_engine_config(core.config());
        configuration.engine.max_tokens_per_session = static_cast<std::uint32_t>(token_capacity - 1);
        configuration.engine.store_messages_per_session = 65536;
        configuration.engine.store_bytes_per_session = 8u << 20;
        configuration.instruments.push_back(core.config().instrument);
        configuration.users.push_back({"TRADER","research",core.config().order_entry_protocol,kStrategySessionId,kStrategyOwnerId,core.config().strategy.stp,false});
        stable_by_gateway = std::make_unique<ClOrdId[]>(token_capacity);
        tokens.set_allow_growth(false);
    }
    const server::ServerConfig& config() const noexcept { return configuration; }
    Timestamp now() const noexcept { return simulation.now(); }
    std::uint64_t realtime_ns() const noexcept { return kSessionEpoch + now(); }
    const server::UserConfig* authenticate(std::string_view username,std::string_view password,server::Protocol protocol) const noexcept {
        const auto& user = configuration.users.front();
        return user.username == username && user.password == password && user.protocol == protocol ? &user : nullptr;
    }
    bool bind(SessionId session,server::ConnectionId connection) noexcept {
        if (session != kStrategySessionId || bound) return false;
        bound = connection; return true;
    }
    void unbind(SessionId session,server::ConnectionId connection) noexcept {
        if (session == kStrategySessionId && bound == connection) bound = 0;
    }
    InstrumentId find_instrument(const Symbol& symbol) const noexcept { return simulation.engine().find_instrument(symbol); }
    const InstrumentConfig* instrument(InstrumentId id) const noexcept { return simulation.engine().instrument(id); }
    bool consume_injected_reject(SessionId) noexcept { return false; }
    bool consume_drop_out(SessionId) noexcept { return false; }
    bool has_pending_drop_out(SessionId) const noexcept { return false; }
    Token* token(ClOrdId stable) noexcept {
        return tokens.find(stable);
    }
    ClOrdId stable(ClOrdId gateway) const noexcept { return gateway < token_capacity ? stable_by_gateway[gateway] : 0; }
    ClOrdId gateway(ClOrdId stable_id) noexcept { const auto* slot = token(stable_id); return slot ? slot->current : 0; }
    server::SubmitResult submit(const Command& command) noexcept {
        ++decoded_orders;
        if (command.type == CommandType::NewOrder) {
            const auto& order = command.new_order;
            if (!order.cl_ord_id || order.cl_ord_id >= token_capacity) { failed = true; return server::SubmitResult::DeferredQueueFull; }
            result = simulation.strategy_submit_limit_order(order.side,order.price,order.qty,order.tif,order.post_only);
            if (result) {
                stable_by_gateway[order.cl_ord_id] = result;
                if (!tokens.insert(result,Token{result,order.cl_ord_id,0})) failed = true;
            }
        } else if (command.type == CommandType::Cancel) {
            const auto id = stable(command.cancel.orig_cl_ord_id);
            result = id && simulation.strategy_cancel_order(id);
            if (result && token(id)) token(id)->pending_cancel = command.cancel.cl_ord_id;
        } else if (command.type == CommandType::Replace) {
            const auto& replace = command.replace;
            const auto id = stable(replace.orig_cl_ord_id);
            result = id && simulation.strategy_replace_order(id,replace.price,replace.qty,replace.force_priority_loss);
            if (result) {
                if (replace.cl_ord_id >= token_capacity || !token(id)) { failed = true; return server::SubmitResult::DeferredQueueFull; }
                stable_by_gateway[replace.cl_ord_id] = id;
                token(id)->pending = replace.cl_ord_id;
            }
        } else { failed = true; return server::SubmitResult::DeferredQueueFull; }
        // A failed pre-trade check is returned to the protocol as a rejection;
        // it is never silently scheduled under different order parameters.
        return result ? server::SubmitResult::Deferred : server::SubmitResult::OutputCapacityExceeded;
    }
    Event translate(const Event& source) noexcept {
        Event event = source;
        switch (event.type) {
            case EventType::Accepted: if (event.accepted.session == kStrategySessionId) event.accepted.cl_ord_id = gateway(event.accepted.cl_ord_id); break;
            case EventType::Rested: if (event.rested.session == kStrategySessionId) event.rested.cl_ord_id = gateway(event.rested.cl_ord_id); break;
            case EventType::Fill:
                if (event.fill.buy.session == kStrategySessionId) event.fill.buy.cl_ord_id = gateway(event.fill.buy.cl_ord_id);
                if (event.fill.sell.session == kStrategySessionId) event.fill.sell.cl_ord_id = gateway(event.fill.sell.cl_ord_id);
                break;
            case EventType::Canceled:
                if (event.canceled.session == kStrategySessionId) {
                    auto* slot = token(event.canceled.cl_ord_id);
                    event.canceled.cl_ord_id = slot ? slot->current : 0;
                    if (slot && event.canceled.reason == CancelReason::UserRequested) {
                        event.canceled.request_cl_ord_id = slot->pending_cancel;
                        slot->pending_cancel = 0;
                    }
                }
                break;
            case EventType::Replaced:
                if (event.replaced.session == kStrategySessionId) {
                    auto* slot = token(event.replaced.orig_cl_ord_id);
                    if (!slot || !slot->pending) { failed = true; break; }
                    event.replaced.orig_cl_ord_id = slot->current;
                    event.replaced.cl_ord_id = slot->pending;
                    slot->current = slot->pending; slot->pending = 0;
                }
                break;
            case EventType::Rejected:
                if (event.rejected.session == kStrategySessionId) {
                    auto* slot = token(event.rejected.orig_cl_ord_id ? event.rejected.orig_cl_ord_id : event.rejected.cl_ord_id);
                    event.rejected.orig_cl_ord_id = slot ? slot->current : 0;
                    event.rejected.cl_ord_id = slot ? (event.rejected.cmd == CommandType::Cancel ? slot->pending_cancel : slot->pending ? slot->pending : slot->current) : 0;
                    if (slot && event.rejected.cmd == CommandType::Cancel) slot->pending_cancel = 0;
                    if (slot && event.rejected.cmd == CommandType::Replace) slot->pending = 0;
                }
                break;
            default: break;
        }
        return event;
    }
};
// User directories are deliberately noncopyable; retain their factory ownership.
template<class Gateway>
struct GatewayState {
    std::unique_ptr<typename Gateway::UserDirectory> users;
    typename Gateway::template Connection<ResearchContext> connection;
    GatewayState(ResearchContext& context,ipc::SessionBuffer& buffer)
        : users(Gateway::make_user_directory(context.config())), connection(*users) { connection.open(SessionSink{&buffer},1,context); }
};
using State = std::variant<std::unique_ptr<GatewayState<Ouch>>,std::unique_ptr<GatewayState<Fix>>,std::unique_ptr<GatewayState<Sbe>>>;
struct MarketSink {
    ipc::SessionPackets* packets;
    std::uint64_t* key;
    void operator()(std::span<const char> packet) const noexcept { (void)packets->append(*key,packet); }
};
}
struct ExchangeSession::Impl {
    ResearchContext context;
    ipc::SessionBuffer outgoing, incoming;
    ipc::SessionPackets reports, market, recovery;
    std::uint64_t market_key = 0;
    md::MarketDataPublisher<MarketSink> publisher;
    md::RetransmissionServer retransmission;
    State state;
    bool failed = false;
    bool drop_report = false, drop_market = false;
    std::uint64_t report_bytes = 0, reports_dropped = 0, packets_dropped = 0;
    server::ConnectionId connection_generation = 1;
    explicit Impl(SimulationCore& simulation)
        : context(simulation), reports(simulation.config().limits.simulation_queue_capacity), market(simulation.config().limits.simulation_queue_capacity), recovery(65536),
          publisher(md::PublisherConfig{"RESEARCH",768,65536,1,0,1'000'000'000,0},MarketSink{&market,&market_key}),
          retransmission(publisher.ring(),md::RetransmissionConfig{"RESEARCH",768,1024}) {
        switch (simulation.config().order_entry_protocol) {
            case server::Protocol::Ouch42: state.emplace<0>(std::make_unique<GatewayState<Ouch>>(context,outgoing)); break;
            case server::Protocol::Fix44: state.emplace<1>(std::make_unique<GatewayState<Fix>>(context,outgoing)); break;
            case server::Protocol::FixpSbe: state.emplace<2>(std::make_unique<GatewayState<Sbe>>(context,outgoing)); break;
        }
        if (publisher.add_instruments_from(simulation.engine()) != md::PublishStatus::Ok || publisher.start_session(0) != md::PublishStatus::Ok)
            throw std::runtime_error("cannot start research ITCH session");
        market_key = 1;
        simulation.attach_session_hooks({this,
            [](void* owner,const Event& event,Phase phase) noexcept { static_cast<Impl*>(owner)->event(event,phase); },
            [](void* owner,std::uint64_t key) noexcept {
                auto& self = *static_cast<Impl*>(owner); self.report_bytes += self.outgoing.bytes().size(); self.reports.stream(key,self.outgoing.bytes()); self.outgoing.clear();
            },
            [](void* owner,std::uint64_t key) noexcept { static_cast<Impl*>(owner)->market_key = key + 1; },
            [](void* owner,std::span<const Event> events) noexcept {
                auto& self = *static_cast<Impl*>(owner);
                // Preserve the real publisher's whole-command lookahead (U
                // versus D/A replaces, auction cross type and packet flush).
                if (self.publisher.on_events(events) != md::PublishStatus::Ok) self.failed = true;
            }});
    }
    ~Impl() { std::visit([&](auto& peer) { if (peer) peer->connection.on_closed(context); },state); }
    void event(const Event& source,Phase auction_phase) noexcept {
        const auto translated = context.translate(source);
        std::array<Event,2> batch{translated,Event{EventType::PhaseChanged,source.instrument,source.ts,{.phase_changed={}}}};
        batch[1].phase_changed.old_phase = auction_phase;
        batch[1].phase_changed.new_phase = context.simulation.engine().phase(source.instrument);
        const std::size_t count = source.type == EventType::Fill && source.fill.aggressor == Aggressor::None ? 2 : 1;
        std::visit([&](auto& peer) {
            using Gateway = std::conditional_t<std::is_same_v<std::remove_reference_t<decltype(*peer)>,GatewayState<Ouch>>,Ouch,
                std::conditional_t<std::is_same_v<std::remove_reference_t<decltype(*peer)>,GatewayState<Fix>>,Fix,Sbe>>;
            Gateway::dispatch(*peer->users,kStrategySessionId,{batch.data(),count},context);
        },state);
    }
};
ExchangeSession::ExchangeSession(SimulationCore& simulation) : impl_(std::make_unique<Impl>(simulation)) {}
ExchangeSession::~ExchangeSession() = default;
SessionStatistics ExchangeSession::statistics() const noexcept {
    const auto& self = *impl_;
    return {self.context.bound != 0,self.context.decoded_orders,self.connection_generation-1,
        self.report_bytes,self.publisher.stats().messages_sequenced,self.retransmission.stats().requests,
        self.reports_dropped,self.packets_dropped};
}
void ExchangeSession::tick() {
    std::visit([&](auto& peer) { peer->connection.on_timer(impl_->context); },impl_->state);
    check();
}
void ExchangeSession::check() const {
    const auto& self = *impl_;
    if (self.failed || self.context.failed || self.outgoing.failed() || self.reports.failed() || self.market.failed()) throw std::runtime_error("research exchange session failed");
    std::visit([](const auto& peer) { if (peer->connection.wants_close()) throw std::runtime_error("research exchange session closed"); },self.state);
}
std::uint64_t ExchangeSession::receive_request(int socket) {
    auto& self = *impl_; self.context.result = 0;
    auto header = ipc::receive_frame(socket); std::uint8_t channel = 0; header.field(channel); header.finish();
    ipc::receive_session_bytes(socket,self.incoming);
    if (channel == 0 || channel == 2) {
        if (channel == 2) {
            self.outgoing.clear(); self.reports.clear(); ++self.connection_generation;
            // Bytes from the old stream are abandoned. Session stores replay
            // them; the trader holds recovered future reports until their
            // ORIGINAL scheduler delivery, so reconnect cannot reveal truth early.
            std::visit([&](auto& peer) {
                peer->connection.on_closed(self.context);
                peer->connection.open(SessionSink{&self.outgoing},self.connection_generation,self.context);
            },self.state);
        }
        std::visit([&](auto& peer) {
            if (peer->connection.on_data(self.incoming.bytes(),self.context) != self.incoming.bytes().size()) self.failed = true;
        },self.state);
    } else if (channel == 1) {
        // The receiver's gap request is served from the publisher's real ring.
        (void)self.retransmission.answer(self.incoming.bytes(),[&](std::span<const char> packet) { (void)self.recovery.append(0,packet); });
    } else throw std::runtime_error("invalid research session channel");
    check(); return self.context.result;
}
void ExchangeSession::send_control(int socket) {
    // Session traffic shares the order-entry stream. A later heartbeat or
    // gateway rejection must not overtake earlier reports still in flight.
    // Reconnect abandons the old queue before replaying its session store.
    if (!impl_->reports.empty() && !impl_->outgoing.bytes().empty()) {
        impl_->report_bytes += impl_->outgoing.bytes().size();
        impl_->reports.stream(impl_->reports.last_key(),impl_->outgoing.bytes());
        impl_->outgoing.clear();
    }
    check(); ipc::send_session_bytes(socket,impl_->outgoing.bytes()); impl_->outgoing.clear();
    impl_->market.send(socket,0);
    impl_->recovery.send(socket,0);
}
void ExchangeSession::drop_next_report() noexcept { impl_->drop_report = true; }
void ExchangeSession::drop_next_market_packet() noexcept { impl_->drop_market = true; }
void ExchangeSession::send_report(int socket,std::uint64_t sequence) {
    check(); if (impl_->reports.send(socket,sequence,impl_->drop_report)) { impl_->drop_report = false; ++impl_->reports_dropped; }
}
void ExchangeSession::send_market(int socket,std::uint64_t sequence) {
    check(); if (impl_->market.send(socket,sequence,impl_->drop_market,true)) { impl_->drop_market = false; ++impl_->packets_dropped; }
}
} // namespace exsim::research
