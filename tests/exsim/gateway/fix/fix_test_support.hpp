#ifndef TESTS_EXSIM_GATEWAY_FIX_FIX_TEST_SUPPORT_HPP
#define TESTS_EXSIM_GATEWAY_FIX_FIX_TEST_SUPPORT_HPP

// In-memory harness for the FIX gateway tests:
//   * TestByteSink: fixed-capacity prepare/commit sink (no allocation after setup)
//   * FixTestExchange: a docs/SERVER.md 2.1 Context over a real MatchingEngine
//     with an EventBuffer of required_event_capacity, a bind registry,
//     cancel-on-disconnect on unbind and per-batch dispatch (SERVER.md 2.3)
//   * wire helpers: build frames from "tag=value|" text with computed
//     BodyLength and CheckSum, split output into parsed messages
//   * FixTestConnection: one exchange-side FixConnection fed with raw frames

#include "exsim/gateway/fix/fix_gateway.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/server/config.hpp"
#include "tests/framework/test.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fix_test {

namespace fix = exsim::gateway::fix;
namespace fix44 = exsim::protocol::fix44;

// 2026-11-03 00:00:00 UTC; the exchange clock starts at 09:30:00.000.
inline constexpr std::uint64_t kMidnightRealtimeNs =
    static_cast<std::uint64_t>(20'760) * 86'400 * fix::kNanosecondsPerSecond;
inline constexpr exsim::Timestamp kSessionStartNs = (9 * 3600 + 30 * 60) * fix::kNanosecondsPerSecond;
inline constexpr std::string_view kStartTimestamp = "20261103-09:30:00.000";
inline constexpr exsim::Price kDollar = exsim::kPriceScale;

class TestByteSink {
public:
    explicit TestByteSink(std::size_t capacity = 1u << 20)
        : storage_(std::make_unique<char[]>(capacity)), capacity_(capacity) {}

    char* prepare(std::size_t byte_count) noexcept {
        if (refuse_prepare_ || byte_count > capacity_ - used_) {
            return nullptr;
        }
        return storage_.get() + used_;
    }
    void commit(std::size_t byte_count) noexcept { used_ += byte_count; }

    [[nodiscard]] std::string_view bytes() const noexcept { return {storage_.get(), used_}; }
    void clear() noexcept { used_ = 0; }
    // Drops the first byte_count bytes (a transport that delivered them).
    void consume(std::size_t byte_count) noexcept {
        std::memmove(storage_.get(), storage_.get() + byte_count, used_ - byte_count);
        used_ -= byte_count;
    }
    void set_refuse_prepare(bool refuse) noexcept { refuse_prepare_ = refuse; }

private:
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_;
    std::size_t used_ = 0;
    bool refuse_prepare_ = false;
};

[[nodiscard]] inline exsim::server::UserConfig make_user(std::string username, std::string password,
                                                         exsim::server::Protocol protocol, exsim::SessionId session,
                                                         exsim::OwnerId owner, exsim::StpMode stp,
                                                         bool cancel_on_disconnect) {
    exsim::server::UserConfig user;
    user.username = std::move(username);
    user.password = std::move(password);
    user.protocol = protocol;
    user.session_id = session;
    user.owner = owner;
    user.stp = stp;
    user.cancel_on_disconnect = cancel_on_disconnect;
    return user;
}

// CLIENT1 (session 1, owner 7, password "secret1"), CLIENT2 (session 2,
// owner 8), CLIENT3 (session 3, owner 7, STP CancelResting, no
// cancel-on-disconnect), OUCH01 (an OUCH user, session 9).
[[nodiscard]] inline std::vector<exsim::server::UserConfig> default_users() {
    using exsim::server::Protocol;
    std::vector<exsim::server::UserConfig> users;
    users.push_back(make_user("CLIENT1", "secret1", Protocol::Fix44, 1, 7, exsim::StpMode::None, true));
    users.push_back(make_user("CLIENT2", "", Protocol::Fix44, 2, 8, exsim::StpMode::None, true));
    users.push_back(make_user("CLIENT3", "", Protocol::Fix44, 3, 7, exsim::StpMode::CancelResting, false));
    users.push_back(make_user("OUCH01", "", Protocol::Ouch42, 9, 9, exsim::StpMode::None, true));
    return users;
}

[[nodiscard]] inline fix::FixUserLimits small_limits() {
    fix::FixUserLimits limits;
    limits.max_tokens = 4096;
    limits.store_max_messages = 8192;
    limits.store_max_bytes = 4u << 20;
    return limits;
}

class FixTestExchange {
public:
    explicit FixTestExchange(fix::FixUserLimits limits = small_limits(),
                             std::vector<exsim::server::UserConfig> users = default_users(),
                             exsim::Phase initial_phase = exsim::Phase::Continuous)
        : engine_config_{8, 4096},
          engine_(engine_config_),
          events_(exsim::required_event_capacity(engine_config_)),
          users_(std::move(users)),
          directory_(users_, "EXSIM", limits),
          bound_sessions_(std::size_t{1} << 16, false) {
        for (std::string_view symbol : {std::string_view("AAPL"), std::string_view("MSFT")}) {
            exsim::InstrumentConfig instrument;
            instrument.symbol = exsim::Symbol(symbol);
            instrument.initial_phase = initial_phase;
            REQUIRE(engine_.add_instrument(instrument) != 0);
        }
        submitted_commands.reserve(1024);
        dispatched_events.reserve(4096);
    }

    // docs/SERVER.md 2.1 Context.
    const exsim::server::UserConfig* authenticate(std::string_view user, std::string_view password,
                                                  exsim::server::Protocol protocol) noexcept {
        for (const auto& config : users_) {
            if (config.username == user && config.protocol == protocol &&
                (config.password.empty() || config.password == password)) {
                return &config;
            }
        }
        return nullptr;
    }
    bool bind(exsim::SessionId session, exsim::server::ConnectionId) noexcept {
        if (bound_sessions_[session]) {
            return false;
        }
        bound_sessions_[session] = true;
        return true;
    }
    void unbind(exsim::SessionId session) noexcept {
        if (!bound_sessions_[session]) {
            return;
        }
        bound_sessions_[session] = false;
        const auto* user = directory_.find(session);
        if (user != nullptr && user->cancel_on_disconnect()) {
            submit(exsim::MassCancel{now(), session, 0, 0, true});
        }
    }
    exsim::InstrumentId find_instrument(const exsim::Symbol& symbol) const noexcept {
        return engine_.find_instrument(symbol);
    }
    const exsim::InstrumentConfig* instrument(exsim::InstrumentId id) const noexcept {
        return engine_.instrument(id);
    }
    void submit(const exsim::Command& command) noexcept {
        events_.clear();
        if (engine_.handle(command, events_) != exsim::HandleResult::Applied) {
            ++insufficient_output_results;
            return;
        }
        if (record_history) {
            submitted_commands.push_back(command);
            dispatched_events.insert(dispatched_events.end(), events_.begin(), events_.end());
        }
        directory_.on_events(std::span<const exsim::Event>(events_.begin(), events_.size()), *this);
    }
    exsim::Timestamp now() const noexcept { return now_ns_; }
    std::uint64_t realtime_ns() const noexcept { return kMidnightRealtimeNs + now_ns_; }

    void advance(exsim::Timestamp nanoseconds) noexcept { now_ns_ += nanoseconds; }
    [[nodiscard]] fix::FixTime fix_time() const noexcept { return {now(), realtime_ns()}; }
    [[nodiscard]] bool is_bound(exsim::SessionId session) const noexcept { return bound_sessions_[session]; }
    [[nodiscard]] fix::FixUserDirectory<TestByteSink>& directory() noexcept { return directory_; }
    [[nodiscard]] fix::FixUserState<TestByteSink>& user(exsim::SessionId session) {
        auto* state = directory_.find(session);
        REQUIRE(state != nullptr);
        return *state;
    }
    [[nodiscard]] exsim::MatchingEngine& engine() noexcept { return engine_; }

    // Commands of one session, excluding cancel-on-disconnect mass cancels.
    [[nodiscard]] std::vector<exsim::Command> commands_of(exsim::SessionId session) const {
        std::vector<exsim::Command> result;
        for (const auto& command : submitted_commands) {
            switch (command.type) {
                case exsim::CommandType::NewOrder:
                    if (command.new_order.session == session) result.push_back(command);
                    break;
                case exsim::CommandType::Cancel:
                    if (command.cancel.session == session) result.push_back(command);
                    break;
                case exsim::CommandType::Replace:
                    if (command.replace.session == session) result.push_back(command);
                    break;
                case exsim::CommandType::MassCancel:
                    if (command.mass_cancel.session == session && !command.mass_cancel.cancel_on_disconnect)
                        result.push_back(command);
                    break;
                default:
                    break;
            }
        }
        return result;
    }

    bool record_history = true;
    std::vector<exsim::Command> submitted_commands;
    std::vector<exsim::Event> dispatched_events;
    std::uint64_t insufficient_output_results = 0;

private:
    exsim::EngineConfig engine_config_;
    exsim::MatchingEngine engine_;
    exsim::EventBuffer events_;
    std::vector<exsim::server::UserConfig> users_;
    fix::FixUserDirectory<TestByteSink> directory_;
    std::vector<bool> bound_sessions_;
    exsim::Timestamp now_ns_ = kSessionStartNs;
};

// "35=A|49=X|...|" (every field terminated by '|') to a framed message with
// BodyLength(9) and CheckSum(10) computed here, independently of the gateway.
[[nodiscard]] inline std::string fix_frame(std::string_view fields) {
    std::string body(fields);
    for (char& character : body) {
        if (character == '|') character = '\001';
    }
    std::string frame = "8=FIX.4.4\001" "9=" + std::to_string(body.size()) + "\001" + body;
    unsigned checksum = 0;
    for (char character : frame) checksum += static_cast<unsigned char>(character);
    char trailer[8];
    std::snprintf(trailer, sizeof trailer, "10=%03u\001", checksum % 256);
    return frame + trailer;
}

[[nodiscard]] inline std::string with_pipes(std::string_view frame) {
    std::string text(frame);
    for (char& character : text) {
        if (character == '\001') character = '|';
    }
    return text;
}

struct ParsedFixMessage {
    std::string raw;
    std::vector<std::pair<unsigned, std::string>> fields;

    [[nodiscard]] bool has(unsigned tag) const {
        for (const auto& field : fields) {
            if (field.first == tag) return true;
        }
        return false;
    }
    [[nodiscard]] std::string get(unsigned tag) const {
        for (const auto& field : fields) {
            if (field.first == tag) return field.second;
        }
        return {};
    }
    [[nodiscard]] std::string type() const { return get(35); }
    [[nodiscard]] std::uint64_t sequence() const { return std::stoull(get(34)); }
    [[nodiscard]] std::string pipes() const { return with_pipes(raw); }
};

// Splits a byte stream of complete, valid frames.
[[nodiscard]] inline std::vector<ParsedFixMessage> split_frames(std::string_view bytes) {
    std::vector<ParsedFixMessage> messages;
    std::size_t position = 0;
    while (position < bytes.size()) {
        std::size_t size = 0;
        const auto status = fix44::frame_size({bytes.data() + position, bytes.size() - position}, size);
        REQUIRE(status == exsim::protocol::Status::ok);
        fix44::Parser<> parser;
        REQUIRE(parser.parse(bytes.data() + position, size) == exsim::protocol::Status::ok);
        ParsedFixMessage message;
        message.raw.assign(bytes.data() + position, size);
        std::size_t field_begin = 0;
        while (field_begin < message.raw.size()) {
            const std::size_t equals = message.raw.find('=', field_begin);
            const std::size_t end = message.raw.find('\001', equals);
            message.fields.emplace_back(static_cast<unsigned>(std::stoul(message.raw.substr(field_begin, equals))),
                                        message.raw.substr(equals + 1, end - equals - 1));
            field_begin = end + 1;
        }
        messages.push_back(std::move(message));
        position += size;
    }
    return messages;
}

// A client-to-exchange message with the standard header.
[[nodiscard]] inline std::string client_message(std::string_view type, std::uint64_t sequence, std::string_view fields,
                                                std::string_view sender = "CLIENT1",
                                                std::string_view extra_header = {}) {
    std::string text = "35=" + std::string(type) + "|49=" + std::string(sender) + "|56=EXSIM|34=" +
                       std::to_string(sequence) + "|" + std::string(extra_header) + "52=" +
                       std::string(kStartTimestamp) + "|" + std::string(fields);
    return fix_frame(text);
}

[[nodiscard]] inline std::string new_order_fields(std::string_view client_order_id, std::string_view side,
                                                  std::uint64_t quantity, std::string_view price,
                                                  std::string_view symbol = "AAPL", std::string_view extra = {}) {
    return "11=" + std::string(client_order_id) + "|55=" + std::string(symbol) + "|54=" + std::string(side) +
           "|38=" + std::to_string(quantity) + "|40=2|44=" + std::string(price) + "|59=0|" + std::string(extra) +
           "60=" + std::string(kStartTimestamp) + "|";
}

using ExchangeConnection = fix::FixConnection<FixTestExchange, TestByteSink>;

// One exchange-side connection driven by raw frames, as a client would send.
class FixTestConnection {
public:
    FixTestConnection(FixTestExchange& exchange, std::string sender, fix::FixSessionSettings settings = {})
        : exchange_(exchange), sender_(std::move(sender)), connection_(exchange.directory(), settings) {
        connection_.start(sink_, ++generation_, exchange_);
    }
    // A bound FixConnection must be released before it is destroyed.
    ~FixTestConnection() { connection_.on_disconnected(exchange_); }
    FixTestConnection(const FixTestConnection&) = delete;
    FixTestConnection& operator=(const FixTestConnection&) = delete;

    std::size_t feed(std::string_view bytes) { return connection_.on_data({bytes.data(), bytes.size()}, exchange_); }

    // Sends with the next client MsgSeqNum.
    void send(std::string_view type, std::string_view fields) {
        feed(client_message(type, next_sequence++, fields, sender_));
    }
    void send_at(std::string_view type, std::uint64_t sequence, std::string_view fields, bool possible_duplicate = false) {
        feed(client_message(type, sequence, fields, sender_,
                            possible_duplicate ? std::string_view("43=Y|122=20261103-09:29:59.000|")
                                               : std::string_view{}));
    }

    std::vector<ParsedFixMessage> drain() {
        auto messages = split_frames(sink_.bytes());
        sink_.clear();
        return messages;
    }

    // Logs on and returns the exchange's Logon response.
    ParsedFixMessage logon(std::uint64_t heartbeat_seconds = 30, bool reset = false,
                           std::string_view password = "secret1") {
        std::string fields = "98=0|108=" + std::to_string(heartbeat_seconds) + "|";
        if (reset) {
            fields += "141=Y|";
            next_sequence = 1;
        }
        if (!password.empty()) fields += "554=" + std::string(password) + "|";
        send("A", fields);
        auto messages = drain();
        REQUIRE(!messages.empty());
        REQUIRE_EQ(messages.front().type(), std::string("A"));
        return messages.front();
    }

    void tick(exsim::Timestamp nanoseconds) {
        exchange_.advance(nanoseconds);
        connection_.on_timer(exchange_);
    }

    void disconnect() { connection_.on_disconnected(exchange_); }
    void reconnect() {
        disconnect();
        sink_.clear();
        connection_.start(sink_, ++generation_, exchange_);
    }

    [[nodiscard]] ExchangeConnection& connection() noexcept { return connection_; }
    [[nodiscard]] TestByteSink& sink() noexcept { return sink_; }

    std::uint64_t next_sequence = 1;

private:
    FixTestExchange& exchange_;
    std::string sender_;
    TestByteSink sink_;
    ExchangeConnection connection_;
    std::uint32_t generation_ = 0;
};

// A FixClient wired to an exchange-side FixConnection through two in-memory
// sinks on the exchange clock. pump() delivers queued bytes both ways until
// neither side produces more; it does not allocate.
template <class Handler>
class FixClientWire {
public:
    using Client = fix::FixClient<Handler, TestByteSink>;

    FixClientWire(FixTestExchange& exchange, Handler& handler, const fix::FixClientSettings& client_settings,
                  const fix::FixSessionSettings& exchange_settings = {})
        : exchange_(exchange), connection_(exchange.directory(), exchange_settings), client_(handler, client_settings) {}
    // A bound FixConnection must be released before it is destroyed.
    ~FixClientWire() { connection_.on_disconnected(exchange_); }
    FixClientWire(const FixClientWire&) = delete;
    FixClientWire& operator=(const FixClientWire&) = delete;

    void connect(bool reset_sequence_numbers) {
        client_to_exchange_.clear();
        exchange_to_client_.clear();
        connection_.start(exchange_to_client_, ++generation_, exchange_);
        client_.connect(client_to_exchange_, reset_sequence_numbers, exchange_.fix_time());
    }

    void pump() noexcept {
        for (int round = 0; round < 10'000; ++round) {
            std::size_t delivered = 0;
            const std::string_view to_exchange = client_to_exchange_.bytes();
            if (!to_exchange.empty()) {
                const std::size_t consumed = connection_.on_data({to_exchange.data(), to_exchange.size()}, exchange_);
                client_to_exchange_.consume(consumed);
                delivered += consumed;
            }
            const std::string_view to_client = exchange_to_client_.bytes();
            if (!to_client.empty()) {
                const std::size_t consumed = client_.on_data({to_client.data(), to_client.size()}, exchange_.fix_time());
                exchange_to_client_.consume(consumed);
                delivered += consumed;
            }
            if (delivered == 0) {
                return;
            }
        }
    }

    void tick(exsim::Timestamp nanoseconds) {
        exchange_.advance(nanoseconds);
        connection_.on_timer(exchange_);
        client_.on_timer(exchange_.fix_time());
        pump();
    }

    void disconnect() {
        connection_.on_disconnected(exchange_);
        client_.on_disconnected();
        client_to_exchange_.clear();
        exchange_to_client_.clear();
    }

    [[nodiscard]] bool wants_disconnect() const noexcept {
        return connection_.wants_disconnect() || client_.wants_disconnect();
    }

    [[nodiscard]] Client& client() noexcept { return client_; }
    [[nodiscard]] ExchangeConnection& connection() noexcept { return connection_; }
    [[nodiscard]] TestByteSink& client_to_exchange() noexcept { return client_to_exchange_; }
    [[nodiscard]] TestByteSink& exchange_to_client() noexcept { return exchange_to_client_; }

private:
    FixTestExchange& exchange_;
    TestByteSink client_to_exchange_;
    TestByteSink exchange_to_client_;
    ExchangeConnection connection_;
    Client client_;
    std::uint32_t generation_ = 1000;
};

[[nodiscard]] inline fix::FixClientSettings client_settings(std::string_view sender, std::string_view password,
                                                            std::uint64_t heartbeat_seconds = 30) {
    fix::FixClientSettings settings;
    settings.sender_comp_id = sender;
    settings.password = password;
    settings.heartbeat_interval_seconds = heartbeat_seconds;
    settings.store_max_messages = 8192;
    settings.store_max_bytes = 4u << 20;
    return settings;
}

[[nodiscard]] inline fix44::NewOrderSingle make_new_order_single(std::string_view client_order_id, exsim::Side side,
                                                                 exsim::Quantity quantity, exsim::Price price,
                                                                 std::string_view symbol = "AAPL") {
    fix44::NewOrderSingle order;
    order.client_order_id.assign(client_order_id);
    order.symbol.assign(symbol);
    order.side = side;
    order.quantity = quantity;
    order.order_type = exsim::OrdType::Limit;
    order.price = price;
    order.has_price = true;
    order.time_in_force = exsim::TimeInForce::Day;
    order.transact_time = kMidnightRealtimeNs + kSessionStartNs;
    order.stp_mode = exsim::StpMode::None;
    return order;
}

[[nodiscard]] inline fix44::OrderCancelRequest make_cancel_request(std::string_view original,
                                                                   std::string_view client_order_id,
                                                                   exsim::Side side, std::string_view symbol = "AAPL") {
    fix44::OrderCancelRequest request;
    request.original_client_order_id.assign(original);
    request.client_order_id.assign(client_order_id);
    request.symbol.assign(symbol);
    request.side = side;
    return request;
}

[[nodiscard]] inline fix44::OrderCancelReplaceRequest make_replace_request(std::string_view original,
                                                                           std::string_view client_order_id,
                                                                           exsim::Side side, exsim::Quantity quantity,
                                                                           exsim::Price price,
                                                                           std::string_view symbol = "AAPL") {
    fix44::OrderCancelReplaceRequest request;
    request.original_client_order_id.assign(original);
    request.client_order_id.assign(client_order_id);
    request.symbol.assign(symbol);
    request.side = side;
    request.quantity = quantity;
    request.order_type = exsim::OrdType::Limit;
    request.price = price;
    request.has_price = true;
    return request;
}

[[nodiscard]] inline fix44::OrderMassCancelRequest make_mass_cancel_request(std::string_view client_order_id,
                                                                            std::string_view symbol = {}) {
    fix44::OrderMassCancelRequest request;
    request.client_order_id.assign(client_order_id);
    request.request_type = symbol.empty() ? '7' : '1';
    request.has_symbol = !symbol.empty();
    request.symbol.assign(symbol);
    return request;
}

// The engine commands of one session as "D:<ClOrdID>", "F:<request>",
// "G:<new token>", "q:<request>" (cancel-on-disconnect excluded).
[[nodiscard]] inline std::vector<std::string> engine_command_tokens(FixTestExchange& exchange,
                                                                    exsim::SessionId session) {
    const auto& tokens = exchange.user(session).tokens();
    std::vector<std::string> result;
    for (const auto& command : exchange.commands_of(session)) {
        switch (command.type) {
            case exsim::CommandType::NewOrder:
                result.push_back("D:" + std::string(tokens.token(command.new_order.cl_ord_id)));
                break;
            case exsim::CommandType::Cancel:
                result.push_back("F:" + std::string(tokens.token(command.cancel.cl_ord_id)));
                break;
            case exsim::CommandType::Replace:
                result.push_back("G:" + std::string(tokens.token(command.replace.cl_ord_id)));
                break;
            case exsim::CommandType::MassCancel:
                result.push_back("q:" + std::string(tokens.token(command.mass_cancel.cl_ord_id)));
                break;
            default:
                break;
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> message_types(const std::vector<ParsedFixMessage>& messages) {
    std::vector<std::string> types;
    for (const auto& message : messages) types.push_back(message.type());
    return types;
}

// Expected exchange-to-client frame: header with SendingTime at the session start.
[[nodiscard]] inline std::string exchange_message(std::string_view type, std::uint64_t sequence, std::string_view fields,
                                                  std::string_view target = "CLIENT1") {
    return fix_frame("35=" + std::string(type) + "|49=EXSIM|56=" + std::string(target) + "|34=" +
                     std::to_string(sequence) + "|52=" + std::string(kStartTimestamp) + "|" + std::string(fields));
}

}  // namespace fix_test

#endif
