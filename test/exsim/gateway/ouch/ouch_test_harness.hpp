#ifndef EXSIM_TESTS_GATEWAY_OUCH_OUCH_TEST_HARNESS_HPP
#define EXSIM_TESTS_GATEWAY_OUCH_OUCH_TEST_HARNESS_HPP

// In-memory server for OUCH gateway tests: an OuchServerContext that owns a
// real exsim::MatchingEngine and an EventBuffer sized with
// required_event_capacity, a session registry, the per-user reject and
// drop_out fault counters, an OuchUserStateDirectory and per-batch dispatch
// (each distinct session of a batch gets on_events exactly once). Plus a memory
// byte sink, client/server links, frame builders and decoders.
// (ouch_exchange_core_test.cpp runs the gateway on the real ExchangeCore.)
//
// Hot-path members (submit, dispatch, sinks) do not allocate after setup, so
// steady-state allocation tests can wrap them.

#include "test/exsim/support/clients/ouch_client.hpp"
#include "gateway/ouch/ouch_server.hpp"
#include "engine/matching_engine.hpp"
#include "server/core/session_dispatch_set.hpp"
#include "../../../framework/test.hpp"

#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::gateway::ouch::testing {

namespace soup = protocol::soupbintcp;
namespace ouch42 = protocol::ouch42;
namespace wire = protocol::wire;

inline constexpr Timestamp kNineThirty = 34'200'000'000'000;  // 09:30:00 in ns since midnight
inline constexpr Timestamp kOneSecond = 1'000'000'000;

// ---------------------------------------------------------------------------
// Byte sink
// ---------------------------------------------------------------------------

class MemoryByteSink {
public:
    explicit MemoryByteSink(std::size_t capacity = std::size_t{1} << 20)
        : storage_(std::make_unique<char[]>(capacity == 0 ? 1 : capacity)), capacity_(capacity) {}

    char* prepare(std::size_t byte_count) noexcept {
        return byte_count <= capacity_ - used_ ? storage_.get() + used_ : nullptr;
    }
    void commit(std::size_t byte_count) noexcept { used_ += byte_count; }

    [[nodiscard]] std::span<const char> bytes() const noexcept { return {storage_.get(), used_}; }
    [[nodiscard]] std::string_view view() const noexcept { return {storage_.get(), used_}; }
    [[nodiscard]] std::size_t size() const noexcept { return used_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    void clear() noexcept { used_ = 0; }

private:
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_;
    std::size_t used_ = 0;
};

using TestUserState = OuchUserState<MemoryByteSink>;
using TestUserStateDirectory = OuchUserStateDirectory<MemoryByteSink>;
using TestConnection = OuchConnection<MemoryByteSink>;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

inline server::UserConfig make_user(std::string username, std::string password, SessionId session, OwnerId owner,
                                    StpMode stp = StpMode::None, bool cancel_on_disconnect = true,
                                    server::Protocol protocol = server::Protocol::Ouch42) {
    server::UserConfig user;
    user.username = std::move(username);
    user.password = std::move(password);
    user.protocol = protocol;
    user.session_id = session;
    user.owner = owner;
    user.stp = stp;
    user.cancel_on_disconnect = cancel_on_disconnect;
    return user;
}

inline InstrumentConfig make_instrument(std::string_view symbol, Phase initial_phase = Phase::Continuous) {
    InstrumentConfig instrument;
    instrument.symbol = Symbol(symbol);
    instrument.tick_size = 100;
    instrument.lot_size = 1;
    instrument.max_order_qty = 1'000'000;
    instrument.min_price = 100;
    instrument.max_price = 2'000'000;  // $200
    instrument.reference_price = 1'000'000;
    instrument.initial_phase = initial_phase;
    return instrument;
}

struct TestExchangeConfig {
    EngineConfig engine{4, 4096};
    std::vector<InstrumentConfig> instruments;
    std::vector<server::UserConfig> users;
    OuchUserStateLimits limits;
    OuchSessionSettings session_settings;

    // AAPL (1), MSFT (2). alice: session 1, owner 1, cancel-on-disconnect.
    // bob: session 2, owner 2, no cancel-on-disconnect. carol: FIX user.
    // dave: session 4, owner 1 (alice's firm), STP CancelResting.
    static TestExchangeConfig standard() {
        TestExchangeConfig config;
        config.instruments = {make_instrument("AAPL"), make_instrument("MSFT")};
        config.users = {make_user("alice", "alicepw", 1, 1, StpMode::None, true),
                        make_user("bob", "bobpw", 2, 2, StpMode::None, false),
                        make_user("carol", "carolpw", 3, 3, StpMode::None, true, server::Protocol::Fix44),
                        make_user("dave", "davepw", 4, 1, StpMode::CancelResting, true)};
        config.limits.max_tokens = 4096;
        config.limits.store_messages = 16384;
        config.limits.store_bytes = std::size_t{2} << 20;
        config.limits.store_reserve_messages = 0;
        wire::alpha(config.session_settings.soup_session.data(), 10, "EXSIM");
        config.session_settings.server_heartbeat_interval_ns = kOneSecond;
        config.session_settings.client_idle_timeout_ns = 15 * kOneSecond;
        return config;
    }

    // The part of a ServerConfig the user state directory reads.
    [[nodiscard]] server::ServerConfig server_config() const {
        server::ServerConfig config;
        config.users = users;
        return config;
    }
};

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

class TestExchange {
public:
    explicit TestExchange(TestExchangeConfig config = TestExchangeConfig::standard())
        : config_(std::move(config)),
          engine_(config_.engine),
          events_(required_event_capacity(config_.engine)),
          user_states_(config_.server_config(), config_.limits),
          binding_by_session_(65536),
          injected_rejects_by_session_(65536, 0),
          outbound_drops_by_session_(65536, 0),
          session_dispatch_mark_(65536, 0),
          phase_left_by_instrument_(std::size_t{std::numeric_limits<InstrumentId>::max()} + 1) {
        for (const InstrumentConfig& instrument : config_.instruments) {
            REQUIRE(engine_.add_instrument(instrument) != 0);
        }
        dispatch_sessions_.reserve(65536);
    }

    // ---- OuchServerContext -------------------------------------------------

    const server::UserConfig* authenticate(std::string_view username, std::string_view password,
                                           server::Protocol protocol) noexcept {
        for (const server::UserConfig& user : config_.users) {
            if (user.username == username) {
                return user.password == password && user.protocol == protocol ? &user : nullptr;
            }
        }
        return nullptr;
    }

    bool bind(SessionId session, server::ConnectionId connection) noexcept {
        if (binding_by_session_[session].has_value()) return false;
        binding_by_session_[session] = connection;
        return true;
    }

    void unbind(SessionId session, server::ConnectionId connection) noexcept {
        if (binding_by_session_[session] != connection) return;  // not bound, or bound to another connection
        binding_by_session_[session].reset();
        ++unbind_count_;
        const server::UserConfig* user = find_user(session);
        if (user != nullptr && user->cancel_on_disconnect) {
            MassCancel cancel;
            cancel.session = session;
            cancel.cancel_on_disconnect = true;
            submit(Command{cancel});
        }
    }

    InstrumentId find_instrument(const Symbol& symbol) const noexcept { return engine_.find_instrument(symbol); }
    const InstrumentConfig* instrument(InstrumentId id) const noexcept { return engine_.instrument(id); }

    // Stamps, runs the engine, dispatches to every distinct session once.
    void submit(const Command& command) noexcept {
        Command stamped = command;
        stamp(stamped, now_);
        events_.clear();
        if (engine_.handle(stamped, events_) != HandleResult::Applied) {
            ++insufficient_output_count_;
            return;
        }
        ++submitted_command_count_;
        last_command_ = stamped;
        dispatch(std::span<const Event>(events_.begin(), events_.size()));
    }

    Timestamp now() const noexcept { return now_; }
    std::uint64_t realtime_ns() const noexcept { return now_; }

    // The phase the batch being dispatched moved `instrument` out of;
    // Halted when it changes no phase of it.
    Phase phase_left_by_batch(InstrumentId instrument) const noexcept {
        const BatchPhaseChange& change = phase_left_by_instrument_[instrument];
        return change.dispatch_generation == dispatch_generation_ ? change.old_phase : Phase::Halted;
    }

    // Fault queries: each true consumes one count.
    bool consume_injected_reject(SessionId session) noexcept { return consume_one(injected_rejects_by_session_[session]); }
    bool consume_drop_out(SessionId session) noexcept { return consume_one(outbound_drops_by_session_[session]); }
    bool has_pending_drop_out(SessionId session) const noexcept { return outbound_drops_by_session_[session] != 0; }

    // ---- Test controls ------------------------------------------------------

    // `fault user X reject n` and `fault user X drop_out n`.
    void inject_gateway_rejects(SessionId session, std::uint32_t count) noexcept {
        injected_rejects_by_session_[session] = count;
    }
    void drop_next_outbound_messages(SessionId session, std::uint32_t count) noexcept {
        outbound_drops_by_session_[session] = count;
    }

    void set_now(Timestamp now) noexcept { now_ = now; }
    void advance(Timestamp duration) noexcept { now_ += duration; }
    void set_phase(InstrumentId instrument, Phase phase) noexcept {
        SetPhase command;
        command.instrument = instrument;
        command.phase = phase;
        submit(Command{command});
    }

    MatchingEngine& engine() noexcept { return engine_; }
    TestUserState& user_state(SessionId session) noexcept { return *user_states_.find(session); }
    TestUserStateDirectory& user_states() noexcept { return user_states_; }
    const OuchSessionSettings& session_settings() const noexcept { return config_.session_settings; }
    bool is_bound(SessionId session) const noexcept { return binding_by_session_[session].has_value(); }
    std::uint64_t submitted_command_count() const noexcept { return submitted_command_count_; }
    std::uint64_t insufficient_output_count() const noexcept { return insufficient_output_count_; }
    std::uint64_t unbind_count() const noexcept { return unbind_count_; }
    const Command& last_command() const noexcept { return last_command_; }
    const EventBuffer& last_batch() const noexcept { return events_; }
    server::ConnectionId next_connection_id() noexcept { return next_connection_number_++; }

    // Live order of `session` with the given OUCH token, if any.
    std::optional<OrderInfo> find_live_order(SessionId session, std::string_view token14) const noexcept {
        const ClOrdId id = user_states_.find(session)->find_cl_ord_id(token14);
        if (id == 0) return std::nullopt;
        return engine_.find_order(session, id);
    }

private:
    static void stamp(Command& command, Timestamp now) noexcept {
        switch (command.type) {
            case CommandType::NewOrder: command.new_order.ts = now; break;
            case CommandType::Cancel: command.cancel.ts = now; break;
            case CommandType::Replace: command.replace.ts = now; break;
            case CommandType::MassCancel: command.mass_cancel.ts = now; break;
            case CommandType::SetPhase: command.set_phase.ts = now; break;
        }
    }

    static bool consume_one(std::uint32_t& remaining) noexcept {
        if (remaining == 0) return false;
        --remaining;
        return true;
    }

    void dispatch(std::span<const Event> batch) noexcept {
        ++dispatch_generation_;
        dispatch_sessions_.clear();
        for (const Event& event : batch) {
            if (event.type == EventType::PhaseChanged) {
                phase_left_by_instrument_[event.instrument] = {dispatch_generation_, event.phase_changed.old_phase};
            }
            server::visit_event_sessions(event, [&](SessionId session) {
                if (session == 0 || session_dispatch_mark_[session] == dispatch_generation_) return;
                session_dispatch_mark_[session] = dispatch_generation_;
                dispatch_sessions_.push_back(session);
            });
        }
        for (const SessionId session : dispatch_sessions_) {
            if (TestUserState* user = user_states_.find(session)) user->on_events(batch, *this);
        }
    }

    const server::UserConfig* find_user(SessionId session) const noexcept {
        for (const server::UserConfig& user : config_.users) {
            if (user.session_id == session) return &user;
        }
        return nullptr;
    }

    TestExchangeConfig config_;
    MatchingEngine engine_;
    EventBuffer events_;
    TestUserStateDirectory user_states_;
    std::vector<std::optional<server::ConnectionId>> binding_by_session_;
    std::vector<std::uint32_t> injected_rejects_by_session_;
    std::vector<std::uint32_t> outbound_drops_by_session_;
    std::vector<std::uint64_t> session_dispatch_mark_;
    std::vector<SessionId> dispatch_sessions_;
    std::uint64_t dispatch_generation_ = 0;
    struct BatchPhaseChange {
        std::uint64_t dispatch_generation = 0;
        Phase old_phase = Phase::Halted;
    };
    std::vector<BatchPhaseChange> phase_left_by_instrument_;
    Timestamp now_ = kNineThirty;
    std::uint64_t submitted_command_count_ = 0;
    std::uint64_t insufficient_output_count_ = 0;
    std::uint64_t unbind_count_ = 0;
    server::ConnectionId next_connection_number_ = 1;
    Command last_command_{};
};

static_assert(OuchServerContext<TestExchange>);

// ---------------------------------------------------------------------------
// Frame builders (client -> server) using the codecs
// ---------------------------------------------------------------------------

inline std::array<char, 14> token14(std::string_view text) {
    std::array<char, 14> token = wire::spaces<14>();
    REQUIRE(wire::alpha(token.data(), token.size(), text));
    return token;
}
inline std::string padded_token(std::string_view text) {
    const auto token = token14(text);
    return {token.data(), token.size()};
}

inline ouch42::EnterOrder enter_order(std::string_view token, char side, Quantity shares, std::string_view stock,
                                      Price price, std::uint32_t time_in_force = kOuchTimeInForceSystemHours,
                                      char display = 'Y') {
    ouch42::EnterOrder order;
    order.token = token14(token);
    order.side = side;
    order.shares = shares;
    order.stock = Symbol(stock);
    order.price = price;
    order.time_in_force = time_in_force;
    wire::alpha(order.firm.data(), order.firm.size(), "FRM1");
    order.display = display;
    order.capacity = 'A';
    order.ise = 'N';
    order.minimum_quantity = 0;
    order.cross_type = 'N';
    order.customer_type = ' ';
    return order;
}

inline ouch42::ReplaceOrder replace_order(std::string_view existing, std::string_view replacement, Quantity shares,
                                          Price price, std::uint32_t time_in_force = kOuchTimeInForceSystemHours,
                                          char display = 'Y') {
    ouch42::ReplaceOrder order;
    order.existing_token = token14(existing);
    order.replacement_token = token14(replacement);
    order.shares = shares;
    order.price = price;
    order.time_in_force = time_in_force;
    order.display = display;
    order.ise = 'N';
    order.minimum_quantity = 0;
    return order;
}

inline ouch42::CancelOrder cancel_order(std::string_view token, Quantity shares) {
    ouch42::CancelOrder order;
    order.token = token14(token);
    order.shares = shares;
    return order;
}

template <class Message>
std::string unsequenced_frame(const Message& message) {
    char payload[64];
    const std::size_t payload_size = ouch42::encode(message, payload, sizeof payload);
    REQUIRE(payload_size > 0);
    char frame[96];
    const std::size_t frame_size = soup::encode(soup::UnsequencedData{{payload, payload_size}}, frame, sizeof frame);
    REQUIRE(frame_size > 0);
    return {frame, frame_size};
}

inline std::string login_frame(std::string_view username, std::string_view password, std::string_view session,
                               std::uint64_t sequence) {
    soup::LoginRequest request;
    REQUIRE(wire::alpha(request.username.data(), 6, username));
    REQUIRE(wire::alpha(request.password.data(), 10, password));
    REQUIRE(wire::alpha(request.session.data(), 10, session));
    request.sequence = sequence;
    char frame[64];
    const std::size_t size = soup::encode(request, frame, sizeof frame);
    REQUIRE(size == 49);
    return {frame, size};
}

template <char Type>
std::string control_frame() {
    char frame[8];
    const std::size_t size = soup::encode(soup::ControlPacket<Type>{}, frame, sizeof frame);
    REQUIRE(size == 3);
    return {frame, size};
}

// ---------------------------------------------------------------------------
// Golden byte builder: explicit big-endian layout, independent of the codecs.
// ---------------------------------------------------------------------------

class Golden {
public:
    Golden& byte(char value) {
        bytes_.push_back(value);
        return *this;
    }
    Golden& u16(std::uint64_t value) { return big_endian(value, 2); }
    Golden& u32(std::uint64_t value) { return big_endian(value, 4); }
    Golden& u64(std::uint64_t value) { return big_endian(value, 8); }
    Golden& alpha(std::string_view text, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) bytes_.push_back(i < text.size() ? text[i] : ' ');
        return *this;
    }
    Golden& numeric(std::uint64_t value, std::size_t width) {  // right-justified, space padded
        std::string digits = std::to_string(value);
        bytes_.append(width - digits.size(), ' ');
        bytes_ += digits;
        return *this;
    }
    // SoupBinTCP frame: big-endian length (type + payload), type, payload.
    [[nodiscard]] std::string soup(char type) const {
        std::string frame;
        const std::size_t length = bytes_.size() + 1;
        frame.push_back(static_cast<char>((length >> 8) & 0xFF));
        frame.push_back(static_cast<char>(length & 0xFF));
        frame.push_back(type);
        frame += bytes_;
        return frame;
    }
    [[nodiscard]] const std::string& payload() const { return bytes_; }

private:
    Golden& big_endian(std::uint64_t value, unsigned width) {
        for (unsigned i = width; i > 0; --i) bytes_.push_back(static_cast<char>((value >> (8 * (i - 1))) & 0xFF));
        return *this;
    }
    std::string bytes_;
};

// ---------------------------------------------------------------------------
// Server output decoding
// ---------------------------------------------------------------------------

// Splits a server byte stream into SoupBinTCP frames (whole frames, header included).
inline std::vector<std::string> split_frames(std::string_view stream) {
    std::vector<std::string> frames;
    while (stream.size() >= 2) {
        const std::size_t length = (static_cast<unsigned char>(stream[0]) << 8) | static_cast<unsigned char>(stream[1]);
        REQUIRE(stream.size() >= length + 2);
        frames.emplace_back(stream.substr(0, length + 2));
        stream.remove_prefix(length + 2);
    }
    CHECK(stream.empty());
    return frames;
}

inline char frame_type(const std::string& frame) { return frame.size() > 2 ? frame[2] : '\0'; }
inline std::string_view frame_payload(const std::string& frame) { return std::string_view(frame).substr(3); }
// OUCH message type of a Sequenced Data frame.
inline char ouch_type(const std::string& frame) {
    return frame_type(frame) == 'S' && frame.size() > 3 ? frame[3] : '\0';
}

// Concatenated OUCH message types of the Sequenced Data frames, e.g. "AEC".
inline std::string sequenced_types(const std::vector<std::string>& frames) {
    std::string types;
    for (const std::string& frame : frames) {
        if (frame_type(frame) == 'S') types.push_back(ouch_type(frame));
    }
    return types;
}

template <class Message>
Message decode_ouch(const std::string& frame) {
    Message message;
    const std::string_view payload = frame_payload(frame);
    REQUIRE(ouch42::decode(payload.data(), payload.size(), message) == protocol::Status::ok);
    return message;
}

inline std::string_view token_text(const std::array<char, 14>& token) { return wire::alpha(token.data(), token.size()); }

// ---------------------------------------------------------------------------
// Raw server session: drive one connection with raw client bytes.
// ---------------------------------------------------------------------------

class RawSession {
public:
    explicit RawSession(TestExchange& exchange, std::size_t sink_capacity = std::size_t{1} << 20)
        : exchange_(exchange) {
        connection_ = std::make_unique<TestConnection>(MemoryByteSink(sink_capacity), exchange.user_states(),
                                                       exchange.next_connection_id(), exchange.session_settings(),
                                                       exchange.now());
    }
    ~RawSession() { disconnect(); }

    void send(std::string_view bytes) { connection_->on_data(std::span<const char>(bytes.data(), bytes.size()), exchange_); }
    template <class Message>
    void send_message(const Message& message) {
        send(unsequenced_frame(message));
    }
    bool login(std::string_view username, std::string_view password, std::uint64_t sequence = 0,
               std::string_view session = "") {
        send(login_frame(username, password, session, sequence));
        return connection_->state() == OuchConnectionState::LoggedIn;
    }

    // Frames written by the server since the last take; clears the sink.
    std::vector<std::string> take_frames() {
        std::vector<std::string> frames = split_frames(connection_->sink().view());
        connection_->sink().clear();
        return frames;
    }
    // OUCH types of the Sequenced Data frames since the last take.
    std::string take_types() { return sequenced_types(take_frames()); }

    void timer() { connection_->on_timer(exchange_); }
    void disconnect() {
        if (connection_ != nullptr) {
            connection_->on_disconnect(exchange_);
            connection_.reset();
        }
    }
    TestConnection& connection() { return *connection_; }

private:
    TestExchange& exchange_;
    std::unique_ptr<TestConnection> connection_;
};

// ---------------------------------------------------------------------------
// Client handler that records every callback (allocates: functional tests only).
// ---------------------------------------------------------------------------

struct ReceivedMessage {
    char type = 0;
    std::uint64_t sequence = 0;
    ouch42::SystemEvent system_event{};
    ouch42::OrderAccepted accepted{};
    ouch42::OrderReplaced replaced{};
    ouch42::OrderCanceled canceled{};
    ouch42::OrderExecuted executed{};
    ouch42::OrderRejected rejected{};
};

struct RecordingHandler {
    std::vector<ReceivedMessage> messages;
    std::vector<char> login_rejections;
    std::vector<std::uint64_t> login_accepted_sequences;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sequence_mismatches;
    int server_heartbeats = 0;
    int end_of_sessions = 0;
    int server_timeouts = 0;
    int unknown_messages = 0;

    void on_login_accepted(const soup::LoginAccepted& accepted) { login_accepted_sequences.push_back(accepted.sequence); }
    void on_login_rejected(char reason) { login_rejections.push_back(reason); }
    void on_sequence_mismatch(std::uint64_t requested, std::uint64_t server_next) {
        sequence_mismatches.emplace_back(requested, server_next);
    }
    void on_server_heartbeat() { ++server_heartbeats; }
    void on_end_of_session() { ++end_of_sessions; }
    void on_server_timeout() { ++server_timeouts; }
    void on_unknown_sequenced_message(std::string_view, std::uint64_t) { ++unknown_messages; }
    void on_system_event(const ouch42::SystemEvent& m, std::uint64_t s) { add('S', s).system_event = m; }
    void on_order_accepted(const ouch42::OrderAccepted& m, std::uint64_t s) { add('A', s).accepted = m; }
    void on_order_replaced(const ouch42::OrderReplaced& m, std::uint64_t s) { add('U', s).replaced = m; }
    void on_order_canceled(const ouch42::OrderCanceled& m, std::uint64_t s) { add('C', s).canceled = m; }
    void on_order_executed(const ouch42::OrderExecuted& m, std::uint64_t s) { add('E', s).executed = m; }
    void on_order_rejected(const ouch42::OrderRejected& m, std::uint64_t s) { add('J', s).rejected = m; }

    std::string types() const {
        std::string result;
        for (const ReceivedMessage& message : messages) result.push_back(message.type);
        return result;
    }

private:
    ReceivedMessage& add(char type, std::uint64_t sequence) {
        messages.push_back({});
        messages.back().type = type;
        messages.back().sequence = sequence;
        return messages.back();
    }
};

// ---------------------------------------------------------------------------
// Client <-> server link in memory
// ---------------------------------------------------------------------------

template <class Handler>
class ClientLink {
public:
    using Client = OuchClient<MemoryByteSink, Handler>;

    ClientLink(TestExchange& exchange, Handler& handler, std::size_t server_sink_capacity = std::size_t{1} << 20,
               const OuchClientSettings& client_settings = {})
        : exchange_(exchange),
          server_sink_capacity_(server_sink_capacity),
          client_(MemoryByteSink(std::size_t{1} << 16), handler, client_settings) {}
    ~ClientLink() { drop_transport(); }

    // New TCP connection (server side object); the client must then log in.
    void connect() {
        drop_transport();
        connection_ = std::make_unique<TestConnection>(MemoryByteSink(server_sink_capacity_), exchange_.user_states(),
                                                       exchange_.next_connection_id(), exchange_.session_settings(),
                                                       exchange_.now());
    }

    bool login(std::string_view username, std::string_view password, std::uint64_t requested_sequence,
               std::string_view session = "") {
        if (connection_ == nullptr) connect();
        if (!client_.login({username, password, session}, requested_sequence, exchange_.now())) return false;
        pump();
        return client_.state() == OuchClientState::LoggedIn;
    }

    // Moves bytes both ways until neither side has anything pending.
    void pump() noexcept {
        for (int round = 0; round < 64 && connection_ != nullptr; ++round) {
            const bool client_had_bytes = client_.transport().size() > 0;
            const bool server_had_bytes = connection_->sink().size() > 0;
            if (!client_had_bytes && !server_had_bytes) break;
            if (client_had_bytes) {
                connection_->on_data(client_.transport().bytes(), exchange_);
                client_.transport().clear();
            }
            if (connection_->sink().size() > 0) {
                client_.on_data(connection_->sink().bytes(), exchange_.now());
                connection_->sink().clear();
                connection_->on_writable(exchange_);
            }
        }
    }

    // TCP loss: both sides notice; the server unbinds (cancel-on-disconnect).
    void drop_transport() noexcept {
        if (connection_ != nullptr) {
            connection_->on_disconnect(exchange_);
            connection_.reset();
        }
        client_.on_transport_closed();
        client_.transport().clear();
    }

    void timers() noexcept {
        if (connection_ != nullptr) connection_->on_timer(exchange_);
        client_.on_timer(exchange_.now());
    }

    Client& client() noexcept { return client_; }
    TestConnection& connection() noexcept { return *connection_; }
    bool connected() const noexcept { return connection_ != nullptr; }

private:
    TestExchange& exchange_;
    std::size_t server_sink_capacity_;
    Client client_;
    std::unique_ptr<TestConnection> connection_;
};

}  // namespace exsim::gateway::ouch::testing

#endif
