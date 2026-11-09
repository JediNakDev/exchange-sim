#ifndef TESTS_EXSIM_SERVER_E2E_E2E_SUPPORT_HPP
#define TESTS_EXSIM_SERVER_E2E_E2E_SUPPORT_HPP

// End-to-end support: a real ServerRuntime on its own thread with OS-picked
// loopback ports, real OUCH / FIX / FIXP-SBE / MoldUDP64 clients and the admin console
// in the test thread. Every wait is bounded (wait_until) and polls with a
// 1 ms idle step.

#include "exsim/client/exchange_client_sessions.hpp"
#include "exsim/md/book_builder.hpp"
#include "exsim/server/config.hpp"
#include "exsim/server/server_runtime.hpp"
#include "test/framework/alloc_counter.hpp"
#include "test/framework/test.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace exsim::e2e {

namespace fix44 = exsim::protocol::fix44;
namespace ouch42 = exsim::protocol::ouch42;
namespace sbe = exsim::protocol::sbe_order_entry;
using gateway::fix::FixInboundMessageInfo;
using gateway::fixp_sbe::FixpSbeReceivedMessageInfo;
using server::ServerRuntime;
using server::runtime::FixGateway;
using server::runtime::FixpSbeGateway;
using server::runtime::OuchGateway;

inline constexpr Price kDollar = kPriceScale;
inline constexpr Timestamp kTenAm = Timestamp{10} * 3600 * 1'000'000'000;
inline constexpr int kWaitMs = 5000;

// AAPL continuous, MSFT pre-open; OUCH users ALICE and BOB (cancel on
// disconnect), OUCH user KEEP (no cancel on disconnect), FIX users CAROL and
// DAVE, SBE over FIXP users ERIN and FRANK; bot users MM1 and NT1.
inline const char* const kE2eBaseConfig = R"(
[exchange]
ouch_port = 0
fix_port = 0
fixp_sbe_port = 0
admin_port = 0
heartbeat_interval_ms = 1000
max_connections = 16

[market_data]
address = 127.0.0.1
retransmit_port = 0
noii_interval_ms = 1000
retransmit_buffer = 1048576

[engine]
max_instruments = 8
max_orders = 65536
max_tokens_per_session = 65536
store_messages_per_session = 262144
store_bytes_per_session = 33554432

[journal]
flush_interval_ms = 0

[instrument AAPL]
tick_size = 0.01
min_price = 0.01
max_price = 1000
reference_price = 150.00
initial_phase = continuous

[instrument MSFT]
tick_size = 0.01
min_price = 0.01
max_price = 2000
reference_price = 400.00
initial_phase = pre_open

[user ALICE]
password = alice
protocol = ouch42
session_id = 1
owner = 1

[user BOB]
password = bob
protocol = ouch42
session_id = 2
owner = 2

[user KEEP]
password = keep
protocol = ouch42
session_id = 5
owner = 5
cancel_on_disconnect = false

[user CAROL]
password = carol
protocol = fix44
session_id = 3
owner = 3

[user DAVE]
password = dave
protocol = fix44
session_id = 4
owner = 4

[user ERIN]
password = erin
protocol = fixp_sbe
session_id = 6
owner = 6

[user FRANK]
password = frank
protocol = fixp_sbe
session_id = 7
owner = 7

[user MM1]
password = mm1
protocol = ouch42
session_id = 100
owner = 100

[user NT1]
password = nt1
protocol = ouch42
session_id = 101
owner = 101
)";

inline const char* const kE2eBots = R"(
[bot maker]
type = market_maker
user = MM1
instrument = AAPL
seed = 7
levels = 2
size = 100
spread_ticks = 2
requote_ms = 20
fair_value_step_ms = 100

[bot taker]
type = noise_taker
user = NT1
instrument = AAPL
seed = 11
mean_interval_ms = 10
min_size = 10
max_size = 50
limit_ppm = 0
)";

inline server::ServerConfig make_e2e_config(bool with_bots, const std::string& journal_path = {}) {
    server::ServerConfig config;
    std::string error;
    const bool parsed =
        server::parse_config(std::string(kE2eBaseConfig) + (with_bots ? kE2eBots : ""), config, error);
    if (!parsed) testfw::report_failure(__FILE__, __LINE__, "config: " + error);
    config.journal_path = journal_path;
    return config;
}

// Temporary journal file, removed by the destructor.
struct TemporaryJournalPath {
    std::string path;
    TemporaryJournalPath() {
        char pattern[] = "/tmp/exsim-e2e-journal-XXXXXX";
        const int descriptor = ::mkstemp(pattern);
        if (descriptor >= 0) ::close(descriptor);
        path = pattern;
    }
    ~TemporaryJournalPath() { std::remove(path.c_str()); }
};

// ---- the server under test --------------------------------------------------------------

class ServerUnderTest {
public:
    // feed_port: where the server sends market data (a MarketDataFeed's port).
    ServerUnderTest(server::ServerConfig config, std::uint16_t feed_port, bool bots_enabled = true,
                    bool busy_poll = false,
                    std::size_t connection_send_buffer_bytes =
                        server::ServerRuntimeOptions{}.connection_send_buffer_bytes) {
        config.market_data.port = feed_port;
        server::ServerRuntimeOptions options;
        options.log = nullptr;
        options.connection_send_buffer_bytes = connection_send_buffer_bytes;
        options.day_time_at_start = kTenAm;
        options.market_data.heartbeat_interval_ns = 20'000'000;
        options.end_of_day_drain_timeout_ns = 2'000'000'000;
        options.bots_enabled_at_start = bots_enabled;
        options.busy_poll = busy_poll;
        runtime_ = std::make_unique<ServerRuntime>(config, options);
        start_failure_ = runtime_->start();
        if (!start_failure_.empty()) {
            testfw::report_failure(__FILE__, __LINE__, "server start: " + start_failure_);
            return;
        }
        ouch_ = runtime_->listen_endpoint<OuchGateway>();
        fix_ = runtime_->listen_endpoint<FixGateway>();
        fixp_sbe_ = runtime_->listen_endpoint<FixpSbeGateway>();
        admin_ = runtime_->admin_endpoint();
        retransmission_ = runtime_->retransmission_endpoint();
        thread_ = std::thread([this] { run(); });
    }

    ~ServerUnderTest() { stop(); }

    ServerUnderTest(const ServerUnderTest&) = delete;
    ServerUnderTest& operator=(const ServerUnderTest&) = delete;

    [[nodiscard]] bool started() const noexcept { return start_failure_.empty(); }
    [[nodiscard]] net::Ipv4Endpoint ouch() const noexcept { return ouch_; }
    [[nodiscard]] net::Ipv4Endpoint fix() const noexcept { return fix_; }
    [[nodiscard]] net::Ipv4Endpoint fixp_sbe() const noexcept { return fixp_sbe_; }
    [[nodiscard]] net::Ipv4Endpoint admin() const noexcept { return admin_; }
    [[nodiscard]] net::Ipv4Endpoint retransmission() const noexcept { return retransmission_; }

    // Waits (bounded) for the day to end on its own (admin shutdown); false on timeout.
    bool wait_finished(int timeout_ms = kWaitMs) {
        const std::uint64_t deadline = net::monotonic_ns() + std::uint64_t(timeout_ms) * 1'000'000;
        while (!finished_.load(std::memory_order_acquire)) {
            if (net::monotonic_ns() >= deadline) return false;
            ::poll(nullptr, 0, 1);
        }
        if (thread_.joinable()) thread_.join();
        return true;
    }

    // Ends the day (if still running) and joins the server thread.
    void stop() {
        if (!thread_.joinable()) return;
        runtime_->request_shutdown();
        thread_.join();
    }

    // Only after stop() / wait_finished(): the server thread is gone.
    [[nodiscard]] ServerRuntime& runtime_after_stop() noexcept { return *runtime_; }

    // Allocation guard on the server thread: start counting at the next loop
    // iteration, stop and report.
    void begin_allocation_count() {
        allocation_phase_.store(1, std::memory_order_release);
        while (allocation_phase_.load(std::memory_order_acquire) != 2) ::poll(nullptr, 0, 1);
    }
    std::size_t end_allocation_count() {
        allocation_phase_.store(3, std::memory_order_release);
        while (allocation_phase_.load(std::memory_order_acquire) != 4) ::poll(nullptr, 0, 1);
        return allocations_counted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t loop_iterations() const noexcept {
        return loop_iterations_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        std::optional<testfw::AllocationScope> allocation_scope;
        while (!runtime_->finished()) {
            const int phase = allocation_phase_.load(std::memory_order_acquire);
            if (phase == 1) {
                allocation_scope.emplace();
                allocation_phase_.store(2, std::memory_order_release);
            } else if (phase == 3) {
                allocations_counted_.store(allocation_scope ? allocation_scope->count() : 0,
                                           std::memory_order_release);
                allocation_scope.reset();
                allocation_phase_.store(4, std::memory_order_release);
            }
            runtime_->poll_once();
            loop_iterations_.fetch_add(1, std::memory_order_relaxed);
        }
        finished_.store(true, std::memory_order_release);
    }

    std::unique_ptr<ServerRuntime> runtime_;
    std::string start_failure_;
    net::Ipv4Endpoint ouch_{}, fix_{}, fixp_sbe_{}, admin_{}, retransmission_{};
    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::atomic<int> allocation_phase_{0};
    std::atomic<std::size_t> allocations_counted_{0};
    std::atomic<std::uint64_t> loop_iterations_{0};
};

// Pumps until `condition` holds or timeout_ms passes.
template <class Condition, class Pump>
bool wait_until(Condition&& condition, Pump&& pump, int timeout_ms = kWaitMs) {
    const std::uint64_t deadline = net::monotonic_ns() + std::uint64_t(timeout_ms) * 1'000'000;
    for (;;) {
        pump();
        if (condition()) return true;
        if (net::monotonic_ns() >= deadline) return false;
        ::poll(nullptr, 0, 1);
    }
}

// ---- OUCH client recorder ---------------------------------------------------------------------

inline std::string token_text(const std::array<char, 14>& token) {
    std::string text(token.data(), token.size());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

inline std::array<char, 14> make_token(std::string_view text) {
    std::array<char, 14> token;
    token.fill(' ');
    std::memcpy(token.data(), text.data(), std::min<std::size_t>(text.size(), token.size()));
    return token;
}

struct OuchRecorder {
    struct Accepted { std::string token; Quantity shares; Price price; std::uint64_t sequence; };
    struct Executed { std::string token; Quantity shares; Price price; std::uint64_t sequence; };
    struct Canceled { std::string token; Quantity shares; char reason; std::uint64_t sequence; };
    struct Replaced { std::string token; Quantity shares; Price price; std::uint64_t sequence; };
    struct Rejected { std::string token; char reason; std::uint64_t sequence; };

    std::vector<Accepted> accepted;
    std::vector<Executed> executed;
    std::vector<Canceled> canceled;
    std::vector<Replaced> replaced;
    std::vector<Rejected> rejected;
    std::vector<char> system_events;
    std::vector<std::uint64_t> sequences;  // every sequenced message, in arrival order
    int logins_accepted = 0;
    int logins_rejected = 0;
    bool end_of_session = false;

    void on_login_accepted(const protocol::soupbintcp::LoginAccepted&) { ++logins_accepted; }
    void on_login_rejected(char) { ++logins_rejected; }
    void on_end_of_session() { end_of_session = true; }
    void on_system_event(const ouch42::SystemEvent& message, std::uint64_t sequence) {
        system_events.push_back(message.event);
        sequences.push_back(sequence);
    }
    void on_order_accepted(const ouch42::OrderAccepted& message, std::uint64_t sequence) {
        accepted.push_back({token_text(message.token), message.shares, message.price, sequence});
        sequences.push_back(sequence);
    }
    void on_order_executed(const ouch42::OrderExecuted& message, std::uint64_t sequence) {
        executed.push_back({token_text(message.token), message.shares, message.price, sequence});
        sequences.push_back(sequence);
    }
    void on_order_canceled(const ouch42::OrderCanceled& message, std::uint64_t sequence) {
        canceled.push_back({token_text(message.token), message.shares, message.reason, sequence});
        sequences.push_back(sequence);
    }
    void on_order_replaced(const ouch42::OrderReplaced& message, std::uint64_t sequence) {
        replaced.push_back({token_text(message.replacement_token), message.shares, message.price, sequence});
        sequences.push_back(sequence);
    }
    void on_order_rejected(const ouch42::OrderRejected& message, std::uint64_t sequence) {
        rejected.push_back({token_text(message.token), message.reason, sequence});
        sequences.push_back(sequence);
    }

    [[nodiscard]] Quantity executed_shares(std::string_view token) const {
        Quantity total = 0;
        for (const Executed& fill : executed) total += fill.token == token ? fill.shares : 0;
        return total;
    }
    [[nodiscard]] bool has_accepted(std::string_view token) const {
        for (const Accepted& message : accepted) if (message.token == token) return true;
        return false;
    }
    [[nodiscard]] bool has_canceled(std::string_view token) const {
        for (const Canceled& message : canceled) if (message.token == token) return true;
        return false;
    }
    [[nodiscard]] bool has_rejected(std::string_view token) const {
        for (const Rejected& message : rejected) if (message.token == token) return true;
        return false;
    }
};

using OuchSession = client::OuchTcpSession<OuchRecorder>;

inline ouch42::EnterOrder ouch_limit_order(std::string_view token, Side side, Quantity shares, Price price,
                                           std::string_view symbol = "AAPL") {
    ouch42::EnterOrder order;
    order.token = make_token(token);
    order.side = side == Side::BUY ? 'B' : 'S';
    order.shares = shares;
    order.stock = Symbol(symbol);
    order.price = price;
    order.time_in_force = 99999;  // day
    order.display = 'Y';
    order.capacity = 'A';
    order.ise = 'N';
    order.minimum_quantity = 0;
    order.cross_type = 'N';
    order.customer_type = 'R';
    return order;
}

inline ouch42::ReplaceOrder ouch_replace(std::string_view existing, std::string_view replacement, Quantity shares,
                                         Price price) {
    ouch42::ReplaceOrder replace;
    replace.existing_token = make_token(existing);
    replace.replacement_token = make_token(replacement);
    replace.shares = shares;
    replace.price = price;
    replace.time_in_force = 99999;
    replace.display = 'Y';
    replace.ise = 'N';
    replace.minimum_quantity = 0;
    return replace;
}

inline ouch42::CancelOrder ouch_cancel(std::string_view token, Quantity remaining_shares = 0) {
    ouch42::CancelOrder cancel;
    cancel.token = make_token(token);
    cancel.shares = remaining_shares;
    return cancel;
}

// ---- FIX client recorder -------------------------------------------------------------------------

struct FixRecorder {
    struct Report {
        std::string client_order_id;
        char execution_type;
        char order_status;
        Quantity last_quantity;
        Quantity leaves_quantity;
        std::uint64_t sequence;
        bool possible_duplicate;
    };
    std::vector<Report> reports;
    int logons = 0;
    int cancel_rejects = 0;
    int session_rejects = 0;
    std::vector<std::string> logout_texts;

    void on_logon(const FixInboundMessageInfo&) { ++logons; }
    void on_execution_report(const fix44::ExecutionReport& report, const FixInboundMessageInfo& info) {
        reports.push_back({std::string(report.client_order_id.view()), report.execution_type, report.order_status,
                           report.last_quantity, report.leaves_quantity, info.sequence, info.possible_duplicate});
    }
    void on_order_cancel_reject(const fix44::OrderCancelReject&, const FixInboundMessageInfo&) { ++cancel_rejects; }
    void on_order_mass_cancel_report(const fix44::OrderMassCancelReport&, const FixInboundMessageInfo&) {}
    void on_session_reject(const fix44::Reject&, const FixInboundMessageInfo&) { ++session_rejects; }
    void on_logout(std::string_view text) { logout_texts.emplace_back(text); }

    [[nodiscard]] std::size_t count(std::string_view client_order_id, char execution_type) const {
        std::size_t matches = 0;
        for (const Report& report : reports) {
            matches += report.client_order_id == client_order_id && report.execution_type == execution_type ? 1 : 0;
        }
        return matches;
    }
    [[nodiscard]] Quantity filled(std::string_view client_order_id) const {
        Quantity total = 0;
        for (const Report& report : reports) {
            if (report.client_order_id == client_order_id && report.execution_type == 'F') total += report.last_quantity;
        }
        return total;
    }
};

using FixSession = client::FixTcpSession<FixRecorder>;

inline gateway::fix::FixClientSettings fix_settings(std::string_view sender, std::string_view password) {
    gateway::fix::FixClientSettings settings;
    settings.sender_comp_id = sender;
    settings.target_comp_id = "EXSIM";
    settings.password = password;
    settings.heartbeat_interval_seconds = 30;
    settings.store_max_messages = 1u << 16;
    settings.store_max_bytes = 16u << 20;
    return settings;
}

inline fix44::NewOrderSingle fix_limit_order(std::string_view client_order_id, Side side, Quantity quantity,
                                             Price price, std::string_view symbol = "AAPL") {
    fix44::NewOrderSingle order;
    order.client_order_id.assign(client_order_id);
    order.symbol.assign(symbol);
    order.side = side;
    order.quantity = quantity;
    order.order_type = OrdType::Limit;
    order.price = price;
    order.has_price = true;
    order.time_in_force = TimeInForce::Day;
    order.transact_time = net::realtime_ns();
    order.stp_mode = StpMode::None;
    return order;
}

inline fix44::OrderCancelReplaceRequest fix_replace(std::string_view original, std::string_view client_order_id,
                                                    Side side, Quantity quantity, Price price,
                                                    std::string_view symbol = "AAPL") {
    fix44::OrderCancelReplaceRequest request;
    request.original_client_order_id.assign(original);
    request.client_order_id.assign(client_order_id);
    request.symbol.assign(symbol);
    request.side = side;
    request.quantity = quantity;
    request.order_type = OrdType::Limit;
    request.price = price;
    request.has_price = true;
    return request;
}

inline fix44::OrderCancelRequest fix_cancel(std::string_view original, std::string_view client_order_id, Side side,
                                            std::string_view symbol = "AAPL") {
    fix44::OrderCancelRequest request;
    request.original_client_order_id.assign(original);
    request.client_order_id.assign(client_order_id);
    request.symbol.assign(symbol);
    request.side = side;
    return request;
}

// ---- SBE over FIXP client recorder ---------------------------------------------------------------

struct FixpSbeRecorder {
    struct Report {
        std::uint16_t template_id;
        std::uint64_t cl_ord_id;
        std::uint64_t server_sequence_number;
        bool retransmitted;
        Quantity last_quantity;  // trades only
        Quantity leaves_quantity;
        std::uint8_t reason;     // ExsimRejectReasonCode or ExsimCancelReasonCode
    };
    std::vector<Report> reports;  // execution and mass cancel reports, in delivery order
    int negotiation_responses = 0;
    int establishment_acks = 0;
    std::uint64_t last_establishment_next_sequence_number = 0;
    int negotiation_rejects = 0;
    int establishment_rejects = 0;
    int cancel_rejects = 0;
    int business_message_rejects = 0;
    int not_applied = 0;
    std::vector<sbe::TerminationCode> terminate_codes;

    void on_negotiation_response(const sbe::NegotiationResponse&) { ++negotiation_responses; }
    void on_negotiation_reject(const sbe::NegotiationReject&) { ++negotiation_rejects; }
    void on_establishment_ack(const sbe::EstablishmentAck& ack) {
        ++establishment_acks;
        last_establishment_next_sequence_number = ack.next_seq_no;
    }
    void on_establishment_reject(const sbe::EstablishmentReject&) { ++establishment_rejects; }
    void on_terminate(const sbe::Terminate& terminate) { terminate_codes.push_back(terminate.code); }

    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::ExecutionReportNew& report) {
        record(sbe::TemplateId::ExecutionReportNew, report.cl_ord_id, info, 0, report.leaves_qty, 0);
    }
    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::ExecutionReportTrade& report) {
        record(sbe::TemplateId::ExecutionReportTrade, report.cl_ord_id, info, report.last_qty, report.leaves_qty, 0);
    }
    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::ExecutionReportCancel& report) {
        record(sbe::TemplateId::ExecutionReportCancel, report.cl_ord_id, info, 0, report.leaves_qty,
               static_cast<std::uint8_t>(report.cancel_reason));
    }
    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::ExecutionReportReplace& report) {
        record(sbe::TemplateId::ExecutionReportReplace, report.cl_ord_id, info, 0, report.leaves_qty, 0);
    }
    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::ExecutionReportReject& report) {
        record(sbe::TemplateId::ExecutionReportReject, report.cl_ord_id, info, 0, 0,
               static_cast<std::uint8_t>(report.exsim_reject_reason));
    }
    void on_application_message(const FixpSbeReceivedMessageInfo&, const sbe::OrderCancelReject&) { ++cancel_rejects; }
    void on_application_message(const FixpSbeReceivedMessageInfo& info, const sbe::OrderMassCancelReport& report) {
        record(sbe::TemplateId::OrderMassCancelReport, report.cl_ord_id, info, 0, 0, 0);
    }
    void on_application_message(const FixpSbeReceivedMessageInfo&, const sbe::BusinessMessageReject&) {
        ++business_message_rejects;
    }
    void on_application_message(const FixpSbeReceivedMessageInfo&, const sbe::NotApplied&) { ++not_applied; }

    [[nodiscard]] const Report* find(sbe::TemplateId template_id, std::uint64_t cl_ord_id) const {
        for (const Report& report : reports) {
            if (report.template_id == sbe::template_id_value(template_id) && report.cl_ord_id == cl_ord_id) {
                return &report;
            }
        }
        return nullptr;
    }
    [[nodiscard]] bool has(sbe::TemplateId template_id, std::uint64_t cl_ord_id) const {
        return find(template_id, cl_ord_id) != nullptr;
    }
    [[nodiscard]] Quantity filled(std::uint64_t cl_ord_id) const {
        Quantity total = 0;
        for (const Report& report : reports) {
            if (report.template_id == sbe::template_id_value(sbe::TemplateId::ExecutionReportTrade) &&
                report.cl_ord_id == cl_ord_id) {
                total += report.last_quantity;
            }
        }
        return total;
    }

private:
    void record(sbe::TemplateId template_id, std::uint64_t cl_ord_id, const FixpSbeReceivedMessageInfo& info,
                Quantity last_quantity, Quantity leaves_quantity, std::uint8_t reason) {
        reports.push_back({sbe::template_id_value(template_id), cl_ord_id, info.server_sequence_number,
                           info.retransmitted, last_quantity, leaves_quantity, reason});
    }
};

using FixpSbeSession = client::FixpSbeTcpSession<FixpSbeRecorder>;

// The storage behind `credentials` must outlive the session.
inline gateway::fixp_sbe::FixpSbeClientSettings fixp_sbe_settings(std::string_view credentials) {
    gateway::fixp_sbe::FixpSbeClientSettings settings;
    settings.credentials = credentials;
    settings.keepalive_interval_ms = 1000;
    return settings;
}

inline sbe::NewOrderSingle sbe_limit_order(std::uint64_t cl_ord_id, Side side, Quantity quantity, Price price,
                                           std::string_view symbol = "AAPL") {
    sbe::NewOrderSingle order;
    order.cl_ord_id = cl_ord_id;
    order.price_mantissa = price;
    order.order_qty = static_cast<std::uint32_t>(quantity);
    order.symbol = sbe::wire_symbol_from(Symbol(symbol));
    order.side = side == Side::BUY ? sbe::SideCode::Buy : sbe::SideCode::Sell;
    order.ord_type = sbe::OrdTypeCode::Limit;
    order.time_in_force = sbe::TimeInForceCode::Day;
    return order;
}

inline sbe::OrderCancelReplaceRequest sbe_replace(std::uint64_t original, std::uint64_t cl_ord_id, Quantity quantity,
                                                  Price price) {
    sbe::OrderCancelReplaceRequest request;
    request.cl_ord_id = cl_ord_id;
    request.orig_cl_ord_id = original;
    request.price_mantissa = price;
    request.order_qty = static_cast<std::uint32_t>(quantity);
    return request;
}

inline sbe::OrderCancelRequest sbe_cancel(std::uint64_t original, std::uint64_t cl_ord_id) {
    sbe::OrderCancelRequest request;
    request.cl_ord_id = cl_ord_id;
    request.orig_cl_ord_id = original;
    return request;
}

template <class Message>
bool send_sbe(FixpSbeSession& session, const Message& message) {
    return session.client().send(message, net::realtime_ns()) != 0 && session.flush();
}

// ---- market data -----------------------------------------------------------------------------------

struct BookFeedHandler {
    md::ItchBookBuilder builder{md::BookBuilderConfig{8, 1u << 16, 4096}};
    std::uint64_t messages = 0;
    std::vector<char> system_events;
    bool end_of_session = false;

    void on_message(std::uint64_t sequence, std::span<const char> message) {
        ++messages;
        if (!message.empty() && message[0] == 'S' && message.size() >= 12) system_events.push_back(message[11]);
        builder.on_message(sequence, message);
    }
    void on_end_of_session(std::uint64_t) { end_of_session = true; }
};

using Feed = client::MarketDataFeed<BookFeedHandler>;

inline md::ReceiverConfig feed_receiver_config() {
    md::ReceiverConfig config;
    config.session = "EXSIM";
    config.first_sequence = 1;
    config.request_timeout_ns = 20'000'000;
    config.max_request_attempts = 200;
    return config;
}

// The builder's book in the admin `book` format (asks deepest first, bids best first).
inline std::string builder_book_text(const md::ItchBookBuilder& builder, InstrumentId instrument,
                                     std::string_view symbol, std::string_view phase, std::size_t levels) {
    std::vector<LevelInfo> asks(levels), bids(levels);
    const std::size_t ask_count = builder.depth(instrument, Side::SELL, asks.data(), levels);
    const std::size_t bid_count = builder.depth(instrument, Side::BUY, bids.data(), levels);
    std::string text = "book " + std::string(symbol) + " phase=" + std::string(phase) + "\n";
    if (ask_count == 0 && bid_count == 0) text += "(empty)\n";
    const auto level_line = [](std::string_view side, const LevelInfo& level) {
        return std::string(side) + " " + server::format_price(level.price) + " qty=" + std::to_string(level.qty) +
               " orders=" + std::to_string(level.order_count) + "\n";
    };
    for (std::size_t index = ask_count; index > 0; --index) text += level_line("ask", asks[index - 1]);
    for (std::size_t index = 0; index < bid_count; ++index) text += level_line("bid", bids[index]);
    return text + "OK\n";
}

// Exact comparison of the builder and engine depth (every level, both sides).
inline bool builder_matches_engine(const md::ItchBookBuilder& builder, const MatchingEngine& engine,
                                   InstrumentId instrument, std::string& difference) {
    constexpr std::size_t kLevels = 4096;
    std::vector<LevelInfo> from_feed(kLevels), from_engine(kLevels);
    for (Side side : {Side::BUY, Side::SELL}) {
        const std::size_t feed_count = builder.depth(instrument, side, from_feed.data(), kLevels);
        const std::size_t engine_count = engine.depth(instrument, side, from_engine.data(), kLevels);
        if (feed_count != engine_count) {
            difference = "level count " + std::to_string(feed_count) + " vs " + std::to_string(engine_count);
            return false;
        }
        for (std::size_t level = 0; level < feed_count; ++level) {
            const LevelInfo& a = from_feed[level];
            const LevelInfo& b = from_engine[level];
            if (a.price != b.price || a.qty != b.qty || a.order_count != b.order_count) {
                difference = "level " + std::to_string(level) + " differs";
                return false;
            }
        }
    }
    return true;
}

// "md next_sequence=N" from a `status` reply; 0 if absent.
inline std::uint64_t md_next_sequence_from_status(const std::string& status) {
    const std::size_t at = status.find("md next_sequence=");
    if (at == std::string::npos) return 0;
    return std::strtoull(status.c_str() + at + std::strlen("md next_sequence="), nullptr, 10);
}

inline bool reply_ok(const std::string& reply) {
    return reply.size() >= 3 && reply.compare(reply.size() - 3, 3, "OK\n") == 0;
}

// Compares the feed-built book with the engine's, read through the admin
// `book` command at a moment when the feed has every message the server
// had published (the MD sequence is read before and after the book and must
// not move). Retries while the exchange is still active.
template <class Pump>
bool feed_book_matches_admin_book(client::AdminLineClient& admin, Feed& feed, const BookFeedHandler& handler,
                                  std::string_view symbol, InstrumentId instrument, Pump&& pump,
                                  std::string& mismatch) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::uint64_t published_before = md_next_sequence_from_status(admin.request("status"));
        if (published_before == 0) {
            mismatch = "status unavailable";
            return false;
        }
        if (!wait_until([&] { return feed.next_sequence() >= published_before; }, pump)) {
            mismatch = "feed stuck at " + std::to_string(feed.next_sequence()) + " of " +
                       std::to_string(published_before);
            return false;
        }
        const std::string engine_book = admin.request("book " + std::string(symbol) + " 1000");
        const std::uint64_t published_after = md_next_sequence_from_status(admin.request("status"));
        if (published_after != published_before || feed.next_sequence() != published_before) continue;
        const std::size_t phase_at = engine_book.find("phase=");
        const std::size_t phase_end = engine_book.find('\n');
        if (phase_at == std::string::npos || phase_end == std::string::npos) {
            mismatch = "bad book reply: " + engine_book;
            return false;
        }
        const std::string phase = engine_book.substr(phase_at + 6, phase_end - phase_at - 6);
        const std::string feed_book = builder_book_text(handler.builder, instrument, symbol, phase, 1000);
        if (feed_book == engine_book) return true;
        mismatch = "engine:\n" + engine_book + "feed:\n" + feed_book;
        return false;
    }
    mismatch = "market data never settled";
    return false;
}

}  // namespace exsim::e2e

#endif
