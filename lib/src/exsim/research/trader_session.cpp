#include "exsim/research/trader_session.hpp"
#include "exsim/ipc/session_packets.hpp"
#include "exsim/gateway/ouch/ouch_client.hpp"
#include "exsim/gateway/fix/fix_client.hpp"
#include "exsim/gateway/fixp_sbe/fixp_sbe_client.hpp"
#include "exsim/md/book_builder.hpp"
#include "exsim/md/receiver.hpp"
#include <charconv>
#include <variant>

namespace exsim::research {
namespace {
namespace fix = protocol::fix44;
namespace ouch = protocol::ouch42;
namespace sbe = protocol::sbe_order_entry;
constexpr std::uint64_t kSessionEpoch = 1'783'036'800'000'000'000ull;
std::uint64_t number(std::string_view text) {
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    std::uint64_t result = 0; auto parsed = std::from_chars(text.data(),text.data()+text.size(),result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) throw std::runtime_error("invalid research session token");
    return result;
}
struct Decimal {
    std::array<char,24> storage{}; std::size_t size = 0;
    explicit Decimal(std::uint64_t value) { size = static_cast<std::size_t>(std::to_chars(storage.data(),storage.data()+storage.size(),value).ptr-storage.data()); }
    std::string_view view() const noexcept { return {storage.data(),size}; }
};
struct ClientHandler {
    struct Report {
        ReportKind kind{}; ClOrdId wire_token = 0; OrderId order = 0;
        Price price = 0; Quantity qty = 0, leaves = 0, cum = 0;
        bool has_price = false, has_qty = false, has_order = false, has_leaves = false;
    } report;
    std::unique_ptr<Report[]> queued = std::make_unique<Report[]>(65536);
    std::size_t first = 0, used = 0;
    std::array<bool,65536> ignored_requests{};
    bool has_report = false, failed = false;
    void record(Report value) noexcept {
        // A local pre-trade rejection has already returned to the caller. Its
        // protocol reply still advances sequencing when it reaches the client.
        if (value.wire_token < ignored_requests.size() && ignored_requests[value.wire_token]) return;
        if (!has_report) { report = value; has_report = true; }
        else if (used < 65536) { queued[(first+used)%65536] = value; ++used; }
        else failed = true;
    }
    void pop() noexcept {
        if (used) { report = queued[first]; first = (first+1)%65536; --used; }
        else has_report = false;
    }

    void on_order_accepted(const ouch::OrderAccepted& m,std::uint64_t) {
        record({ReportKind::Accepted,number({m.token.data(),m.token.size()}),m.order_reference,m.price,m.shares,0,0,true,true,true,false});
    }
    void on_order_executed(const ouch::OrderExecuted& m,std::uint64_t) {
        record({ReportKind::Fill,number({m.token.data(),m.token.size()}),0,m.price,m.shares,0,0,true,true,false,false});
    }
    void on_order_canceled(const ouch::OrderCanceled& m,std::uint64_t) {
        record({ReportKind::Canceled,number({m.token.data(),m.token.size()}),0,0,m.shares,0,0,false,true,false,false});
    }
    void on_order_replaced(const ouch::OrderReplaced& m,std::uint64_t) {
        record({ReportKind::Replaced,number({m.replacement_token.data(),m.replacement_token.size()}),m.order_reference,m.price,0,m.shares,0,true,false,true,false});
    }
    void on_order_rejected(const ouch::OrderRejected& m,std::uint64_t) { record({ReportKind::Rejected,number({m.token.data(),m.token.size()})}); }
    void on_login_rejected(char) noexcept { failed = true; }
    void on_server_timeout() noexcept { failed = true; }
    void on_logon(const gateway::fix::FixInboundMessageInfo&) noexcept {}
    void on_execution_report(const fix::ExecutionReport& m,const gateway::fix::FixInboundMessageInfo&) {
        Report r;
        r.kind = m.execution_type == '0' ? ReportKind::Accepted : m.execution_type == 'F' ? ReportKind::Fill : m.execution_type == '5' ? ReportKind::Replaced : m.execution_type == '8' ? ReportKind::Rejected : ReportKind::Canceled;
        r.wire_token = number(m.client_order_id.view());
        if (r.kind != ReportKind::Rejected) { r.order = number(m.order_id.view()); r.has_order = true; }
        r.price = r.kind == ReportKind::Fill ? m.last_price : m.price; r.has_price = r.kind != ReportKind::Rejected && (m.has_price || r.kind == ReportKind::Fill);
        r.qty = r.kind == ReportKind::Fill ? m.last_quantity : m.quantity;
        r.has_qty = r.kind != ReportKind::Canceled && r.kind != ReportKind::Rejected;
        r.leaves = m.leaves_quantity; r.cum = m.cumulative_quantity; r.has_leaves = r.kind != ReportKind::Rejected;
        record(r);
    }
    void on_order_cancel_reject(const fix::OrderCancelReject& m,const gateway::fix::FixInboundMessageInfo&) { record({ReportKind::Rejected,number(m.client_order_id.view())}); }
    void on_order_mass_cancel_report(const fix::OrderMassCancelReport&,const gateway::fix::FixInboundMessageInfo&) noexcept { failed = true; }
    void on_session_reject(const fix::Reject&,const gateway::fix::FixInboundMessageInfo&) noexcept { failed = true; }
    void on_logout(std::string_view) noexcept { failed = true; }
    template<class Message>
    void on_application_message(const gateway::fixp_sbe::FixpSbeReceivedMessageInfo&,const Message& m) noexcept {
        Report r;
        if constexpr (std::is_same_v<Message,sbe::ExecutionReportNew> || std::is_same_v<Message,sbe::ExecutionReportTrade> ||
                      std::is_same_v<Message,sbe::ExecutionReportCancel> || std::is_same_v<Message,sbe::ExecutionReportReplace>) {
            r.wire_token = m.cl_ord_id; r.order = m.order_id; r.has_order = true;
            r.leaves = m.leaves_qty; r.cum = m.cum_qty; r.has_leaves = true;
            if constexpr (std::is_same_v<Message,sbe::ExecutionReportNew>) { r.kind = ReportKind::Accepted; r.price = m.price_mantissa; r.qty = m.order_qty; r.has_price = r.has_qty = true; }
            if constexpr (std::is_same_v<Message,sbe::ExecutionReportTrade>) { r.kind = ReportKind::Fill; r.price = m.last_px_mantissa; r.qty = m.last_qty; r.has_price = r.has_qty = true; }
            if constexpr (std::is_same_v<Message,sbe::ExecutionReportCancel>) { r.kind = ReportKind::Canceled; r.qty = m.canceled_qty; r.has_qty = true; }
            if constexpr (std::is_same_v<Message,sbe::ExecutionReportReplace>) { r.kind = ReportKind::Replaced; r.price = m.price_mantissa; r.qty = m.order_qty; r.has_price = r.has_qty = true; }
        } else if constexpr (std::is_same_v<Message,sbe::ExecutionReportReject> || std::is_same_v<Message,sbe::OrderCancelReject>) {
            r.kind = ReportKind::Rejected; r.wire_token = m.cl_ord_id;
        } else { failed = true; return; }
        record(r);
    }
};
using OuchClient = gateway::ouch::OuchClient<ipc::SessionSink,ClientHandler>;
using FixClient = gateway::fix::FixClient<ClientHandler,ipc::SessionSink>;
using SbeClient = gateway::fixp_sbe::FixpSbeClient<ipc::SessionSink,ClientHandler>;
using Client = std::variant<std::unique_ptr<OuchClient>,std::unique_ptr<FixClient>,std::unique_ptr<SbeClient>>;
struct RecoverySink {
    ipc::SessionBuffer* requests;
    void operator()(std::span<const char> request) const { requests->append(request); }
};
}
struct TraderSession::Impl {
    ExperimentConfig config;
    ipc::SessionBuffer outgoing, incoming, recovery;
    ipc::SessionSink sink{&outgoing};
    ClientHandler handler;
    Client client;
    md::ItchBookBuilder book;
    md::MoldUdp64Receiver<md::ItchBookBuilder,RecoverySink> receiver;
    std::unique_ptr<ClOrdId[]> stable_by_wire;
    lob::FlatHashMap<ClOrdId> wire_by_stable{65536};
    std::size_t order_capacity = 16;
    ClOrdId next_wire = 1, pending_wire = 0;
    Timestamp now = 0;
    bool reopening = false;
    explicit Impl(const ExperimentConfig& value)
        : config(value), book(md::BookBuilderConfig{1,value.engine.max_orders,value.engine.max_orders}),
          receiver(md::ReceiverConfig{"RESEARCH",1,16384,64,1024,100'000'000,10},book,RecoverySink{&recovery}),
          stable_by_wire(std::make_unique<ClOrdId[]>(65536)) {
        while (order_capacity < 2 * static_cast<std::size_t>(value.risk.max_open_orders)) order_capacity *= 2;
        wire_by_stable.set_allow_growth(false);
        switch (config.order_entry_protocol) {
            case server::Protocol::Ouch42: client.emplace<0>(std::make_unique<OuchClient>(sink,handler)); break;
            case server::Protocol::Fix44:
                client.emplace<1>(std::make_unique<FixClient>(handler,gateway::fix::FixClientSettings{"TRADER","EXSIM","research",30,65536,8u<<20,{}})); break;
            case server::Protocol::FixpSbe: client.emplace<2>(std::make_unique<SbeClient>(sink,handler,gateway::fixp_sbe::FixpSbeClientSettings{"TRADER:research"})); break;
        }
    }
    gateway::fix::FixTime time() const noexcept { return {now,kSessionEpoch + now}; }
    void on_bytes(std::span<const char> bytes) {
        std::visit([&](auto& peer) {
            using Type = std::remove_reference_t<decltype(*peer)>;
            if constexpr (std::is_same_v<Type,OuchClient>) {
                peer->on_data(bytes,now);
                if (peer->state() == gateway::ouch::OuchClientState::ProtocolError)
                    throw std::runtime_error("research OUCH session protocol error");
            }
            else if constexpr (std::is_same_v<Type,FixClient>) {
                if (peer->on_data(bytes,time()) != bytes.size() || peer->wants_disconnect()) throw std::runtime_error("invalid or incomplete research FIX response");
            } else {
                peer->on_data(bytes,kSessionEpoch + now);
                if (peer->failure() != gateway::fixp_sbe::FixpSbeClientFailure::None)
                    throw std::runtime_error("research FIXP session failed");
                if (peer->state() == gateway::fixp_sbe::FixpSbeClientState::Negotiated && !peer->establish(kSessionEpoch + now)) throw std::runtime_error("cannot establish research FIXP session");
            }
        },client);
        if (handler.failed || outgoing.failed()) throw std::runtime_error("research trader session failed");
    }
    void on_packet(std::span<const char> packet) {
        if (receiver.on_packet(packet,now) != md::PacketStatus::Accepted || book.rejected_message_count() || receiver.stats().messages_lost)
            throw std::runtime_error("research ITCH feed is invalid or stale");
    }
};
TraderSession::TraderSession(const ExperimentConfig& config) : impl_(std::make_unique<Impl>(config)) {}
TraderSession::~TraderSession() = default;
void TraderSession::connect() {
    auto& self = *impl_;
    bool ok = false;
    std::visit([&](auto& peer) {
        using Type = std::remove_reference_t<decltype(*peer)>;
        if constexpr (std::is_same_v<Type,OuchClient>) ok = peer->login({"TRADER","research","EXSIM"},1,0);
        else if constexpr (std::is_same_v<Type,FixClient>) ok = peer->connect(self.sink,false,self.time());
        else ok = peer->negotiate(gateway::fixp_sbe::version4_uuid_from_random_bits(1,2),kSessionEpoch);
    },self.client);
    if (!ok) throw std::runtime_error("cannot start research order-entry session");
}
void TraderSession::reconnect(Timestamp now) {
    auto& self = *impl_; self.now = now; self.reopening = true;
    self.outgoing.clear();
    bool ok = false;
    std::visit([&](auto& peer) {
        using Type = std::remove_reference_t<decltype(*peer)>;
        if constexpr (std::is_same_v<Type,OuchClient>) {
            const auto expected = peer->next_expected_sequence(); peer->on_transport_closed();
            ok = peer->login({"TRADER","research","EXSIM"},expected,now);
        } else if constexpr (std::is_same_v<Type,FixClient>) {
            peer->on_disconnected(); ok = peer->connect(self.sink,false,self.time());
        } else { peer->reset_transport(self.sink); ok = peer->establish(kSessionEpoch+now); }
    },self.client);
    if (!ok) throw std::runtime_error("cannot reconnect research order-entry session");
}
void TraderSession::tick(Timestamp now) {
    auto& self = *impl_; self.now = now;
    std::visit([&](auto& peer) {
        using Type = std::remove_reference_t<decltype(*peer)>;
        if constexpr (std::is_same_v<Type,OuchClient>) peer->on_timer(now);
        else if constexpr (std::is_same_v<Type,FixClient>) peer->on_timer(self.time());
        else peer->on_timer(kSessionEpoch+now);
    },self.client);
    self.receiver.poll(now);
    if (!active() || self.handler.failed || self.receiver.stats().messages_lost) throw std::runtime_error("research session timer or feed recovery failed");
}
bool TraderSession::active() const {
    return std::visit([](const auto& peer) {
        using Type = std::remove_reference_t<decltype(*peer)>;
        if constexpr (std::is_same_v<Type,OuchClient>) return peer->state() == gateway::ouch::OuchClientState::LoggedIn;
        else if constexpr (std::is_same_v<Type,FixClient>) return peer->active();
        else return peer->state() == gateway::fixp_sbe::FixpSbeClientState::Established;
    },impl_->client);
}
bool TraderSession::order_action(const StrategyAction& action,const StrategyVisibleState& visible) {
    auto& self = *impl_; self.now = visible.now;
    if (!active() || self.next_wire >= 65536) throw std::runtime_error("research session unavailable or token capacity exhausted");
    self.pending_wire = self.next_wire++;
    const auto* wire_token = self.wire_by_stable.find(action.token);
    const auto original_wire = wire_token ? *wire_token : 0;
    const auto& candidate = visible.orders[action.token & (visible.order_capacity-1)];
    const auto* original = candidate.live && candidate.cl_ord_id == action.token ? &candidate : nullptr;
    const Side side = action.kind == StrategyActionKind::NewOrder ? action.side : original ? original->side : Side::BUY;
    const Decimal request(self.pending_wire), previous(original_wire);
    bool sent = false;
    std::visit([&](auto& peer) {
        using Type = std::remove_reference_t<decltype(*peer)>;
        if constexpr (std::is_same_v<Type,OuchClient>) {
            if (action.kind == StrategyActionKind::NewOrder) {
                if (action.tif == TimeInForce::FOK) throw std::runtime_error("OUCH does not support FOK research orders");
                ouch::EnterOrder message; protocol::wire::alpha(message.token.data(),message.token.size(),request.view());
                message.side = side == Side::BUY ? 'B' : 'S'; message.stock = self.config.instrument.symbol;
                message.price = action.price; message.shares = action.qty;
                message.time_in_force = action.tif == TimeInForce::IOC ? 0 : action.tif == TimeInForce::GTC ? 99999 : 99998;
                message.display = action.post_only ? 'P' : 'Y'; message.capacity = 'A'; message.cross_type = 'N'; message.ise = 'N'; message.customer_type = 'N';
                sent = peer->enter_order(message,self.now);
            } else if (action.kind == StrategyActionKind::Cancel) {
                ouch::CancelOrder message; protocol::wire::alpha(message.token.data(),message.token.size(),previous.view()); sent = peer->cancel_order(message,self.now);
            } else {
                ouch::ReplaceOrder message; protocol::wire::alpha(message.existing_token.data(),message.existing_token.size(),previous.view());
                protocol::wire::alpha(message.replacement_token.data(),message.replacement_token.size(),request.view());
                message.price = action.price; message.shares = action.qty;
                message.time_in_force = original && original->tif == TimeInForce::GTC ? 99999 : 99998;
                message.display = self.config.strategy.quoting.post_only ? 'P' : 'Y'; message.ise = 'N'; sent = peer->replace_order(message,self.now);
            }
        } else if constexpr (std::is_same_v<Type,FixClient>) {
            if (action.kind == StrategyActionKind::NewOrder) {
                fix::NewOrderSingle message; message.client_order_id.borrow(request.view()); message.symbol.borrow(self.config.instrument.symbol.view());
                message.side = side; message.quantity = action.qty; message.order_type = OrdType::Limit;
                message.price = action.price; message.has_price = true; message.time_in_force = action.tif; message.transact_time = kSessionEpoch+self.now;
                message.stp_mode = self.config.strategy.stp; message.has_exec_inst = action.post_only; message.exec_inst.borrow("6");
                sent = peer->send_new_order_single(message,self.time());
            } else if (action.kind == StrategyActionKind::Cancel) {
                fix::OrderCancelRequest message; message.client_order_id.borrow(request.view()); message.original_client_order_id.borrow(previous.view());
                message.symbol.borrow(self.config.instrument.symbol.view()); message.side = side; message.transact_time = kSessionEpoch+self.now;
                sent = peer->send_order_cancel_request(message,self.time());
            } else {
                fix::OrderCancelReplaceRequest message; message.client_order_id.borrow(request.view()); message.original_client_order_id.borrow(previous.view());
                message.symbol.borrow(self.config.instrument.symbol.view()); message.side = side; message.transact_time = kSessionEpoch+self.now;
                message.quantity = action.qty; message.price = action.price; message.has_price = true; message.order_type = OrdType::Limit;
                sent = peer->send_order_cancel_replace_request(message,self.time());
            }
        } else {
            if (action.kind == StrategyActionKind::NewOrder) {
                sbe::NewOrderSingle message; message.cl_ord_id = self.pending_wire; message.price_mantissa = action.price; message.order_qty = action.qty;
                message.symbol = sbe::wire_symbol_from(self.config.instrument.symbol); message.side = side == Side::BUY ? sbe::SideCode::Buy : sbe::SideCode::Sell;
                message.time_in_force = action.tif == TimeInForce::IOC ? sbe::TimeInForceCode::ImmediateOrCancel : action.tif == TimeInForce::FOK ? sbe::TimeInForceCode::FillOrKill : action.tif == TimeInForce::GTC ? sbe::TimeInForceCode::GoodTillCancel : sbe::TimeInForceCode::Day;
                message.exec_inst = action.post_only ? sbe::kExecInstParticipateDontInitiate : 0;
                sent = peer->send(message,kSessionEpoch+self.now) != 0;
            } else if (action.kind == StrategyActionKind::Cancel) {
                sbe::OrderCancelRequest message; message.cl_ord_id = self.pending_wire; message.orig_cl_ord_id = original_wire; sent = peer->send(message,kSessionEpoch+self.now) != 0;
            } else {
                sbe::OrderCancelReplaceRequest message; message.cl_ord_id = self.pending_wire; message.orig_cl_ord_id = original_wire;
                message.price_mantissa = action.price; message.order_qty = action.qty; sent = peer->send(message,kSessionEpoch+self.now) != 0;
            }
        }
    },self.client);
    if (!sent) throw std::runtime_error("cannot send research order-entry message");
    // Map cancel/replace request identifiers before any synchronous rejection.
    if (action.kind != StrategyActionKind::NewOrder) self.stable_by_wire[self.pending_wire] = action.token;
    return true;
}
void TraderSession::accept_result(const StrategyAction& action,std::uint64_t result) {
    auto& self = *impl_;
    if (!result) self.handler.ignored_requests[self.pending_wire] = true;
    if (action.kind == StrategyActionKind::NewOrder && result) {
        self.stable_by_wire[self.pending_wire] = result;
        if (!self.wire_by_stable.insert(result,self.pending_wire)) throw std::runtime_error("research token map capacity exhausted");
    } else if (action.kind == StrategyActionKind::Replace && result) {
        auto* token = self.wire_by_stable.find(action.token);
        if (!token) throw std::runtime_error("unknown research replacement handle");
        *token = self.pending_wire;
    }
}
bool TraderSession::has_output() const { return !impl_->outgoing.bytes().empty() || !impl_->recovery.bytes().empty(); }
void TraderSession::send_request(int socket) {
    auto& self = *impl_; ipc::Frame header; std::uint8_t channel = self.reopening ? 2 : self.outgoing.bytes().empty() ? 1 : 0; self.reopening = false; header.field(channel); ipc::send_frame(socket,header);
    auto& buffer = channel == 1 ? self.recovery : self.outgoing;
    ipc::send_session_bytes(socket,buffer.bytes()); buffer.clear();
}
void TraderSession::receive_control(int socket,Timestamp now) {
    auto& self = *impl_; self.now = now;
    ipc::receive_session_bytes(socket,self.incoming); self.on_bytes(self.incoming.bytes());
    ipc::receive_session_packets(socket,self.incoming,[&](auto packet) { self.on_packet(packet); });
    ipc::receive_session_packets(socket,self.incoming,[&](auto packet) { self.on_packet(packet); });
}
void TraderSession::receive_report(int socket,ExecutionReport& expected,Timestamp now) {
    auto& self = *impl_; self.now = now;
    ipc::receive_session_packets(socket,self.incoming,[&](auto bytes) { self.on_bytes(bytes); });
    if (expected.kind != ReportKind::Rested && !self.handler.has_report) reconnect(now);
}
void TraderSession::finish_report(ExecutionReport& expected) {
    auto& self = *impl_;
    const auto& observed = self.handler.report;
    if (expected.kind == ReportKind::Rested) return;
    if (!self.handler.has_report || observed.wire_token >= 65536 || self.stable_by_wire[observed.wire_token] != expected.cl_ord_id || observed.kind != expected.kind ||
        (observed.has_price && observed.price != expected.price) || (observed.has_qty && observed.qty != expected.qty) ||
        (observed.has_order && observed.order != expected.order_id) ||
        (observed.has_leaves && (observed.leaves != expected.leaves_qty || observed.cum != expected.cum_qty)))
        throw std::runtime_error("wire execution report disagrees with research accounting: kind="+std::to_string(static_cast<unsigned>(observed.kind))+"/"+std::to_string(static_cast<unsigned>(expected.kind))+" token="+std::to_string(observed.wire_token)+" stable="+std::to_string(observed.wire_token < 65536 ? self.stable_by_wire[observed.wire_token] : 0)+"/"+std::to_string(expected.cl_ord_id)+" price="+std::to_string(observed.price)+"/"+std::to_string(expected.price)+" qty="+std::to_string(observed.qty)+"/"+std::to_string(expected.qty)+" order="+std::to_string(observed.order)+"/"+std::to_string(expected.order_id));
    if (self.config.order_entry_protocol == server::Protocol::Ouch42 && expected.kind == ReportKind::Replaced) {
        if (observed.leaves != expected.leaves_qty) throw std::runtime_error("OUCH replacement leaves disagree with research accounting");
        expected.leaves_qty = observed.leaves;
    }
    expected.cl_ord_id = self.stable_by_wire[observed.wire_token];
    if (observed.has_price) expected.price = observed.price;
    if (observed.has_qty) expected.qty = observed.qty;
    if (observed.has_order) expected.order_id = observed.order;
    if (observed.has_leaves) { expected.leaves_qty = observed.leaves; expected.cum_qty = observed.cum; }
    self.handler.pop();
}
void TraderSession::receive_market(int socket,MarketDataUpdate& update,Timestamp now) {
    auto& self = *impl_; self.now = now;
    ipc::receive_session_packets(socket,self.incoming,[&](auto packet) { self.on_packet(packet); });
    (void)update;
}
void TraderSession::finish_market(MarketDataUpdate& update) {
    auto& self = *impl_;
    TopOfBook top; LevelInfo level;
    if (self.book.depth(1,Side::BUY,&level,1)) { top.has_bid = true; top.bid_price = level.price; top.bid_qty = level.qty; }
    if (self.book.depth(1,Side::SELL,&level,1)) { top.has_ask = true; top.ask_price = level.price; top.ask_qty = level.qty; }
    if (!(top == update.top)) throw std::runtime_error("delivered ITCH book disagrees with research feed");
    update.top = top;
}
} // namespace exsim::research
