#ifndef EXSIM_TESTS_GATEWAY_FIXP_SBE_TEST_SUPPORT_HPP
#define EXSIM_TESTS_GATEWAY_FIXP_SBE_TEST_SUPPORT_HPP

// Shared fixtures for the FIXP/SBE gateway tests: an in-memory byte sink, a
// server Context backed by the real MatchingEngine (SERVER.md 2.1 and 2.3),
// output frame parsing and a harness that drives one server connection with
// raw client frames.

#include <tests/framework/test.hpp>

#include "exsim/command.hpp"
#include "exsim/event.hpp"
#include "exsim/gateway/fixp_sbe/fixp_sbe_client.hpp"
#include "exsim/gateway/fixp_sbe/fixp_sbe_server.hpp"
#include "exsim/instrument.hpp"
#include "exsim/matching_engine.hpp"
#include "exsim/protocol/sbe_order_entry.hpp"
#include "exsim/server/config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fixp_sbe_test {

namespace fixp = exsim::gateway::fixp_sbe;
namespace sbe = exsim::protocol::sbe_order_entry;

// ---------------------------------------------------------------------------
// Byte sink over a fixed buffer; never allocates after construction.

class MemoryBuffer {
public:
    explicit MemoryBuffer(std::size_t capacity = std::size_t{1} << 20)
        : storage_(std::make_unique<char[]>(capacity)), capacity_(capacity) {}

    [[nodiscard]] char* prepare(std::size_t byte_count) noexcept {
        if (refuse_writes || byte_count > capacity_ - used_) return nullptr;
        return storage_.get() + used_;
    }
    void commit(std::size_t byte_count) noexcept { used_ += byte_count; }

    [[nodiscard]] std::span<const char> bytes() const noexcept { return {storage_.get(), used_}; }
    [[nodiscard]] std::size_t size() const noexcept { return used_; }
    [[nodiscard]] bool empty() const noexcept { return used_ == 0; }
    void clear() noexcept { used_ = 0; }
    // Drops the first byte_count bytes (consumed by a reader).
    void consume(std::size_t byte_count) noexcept {
        std::memmove(storage_.get(), storage_.get() + byte_count, used_ - byte_count);
        used_ -= byte_count;
    }
    void append(std::span<const char> bytes_to_append) noexcept {
        char* out = prepare(bytes_to_append.size());
        if (out == nullptr) return;
        std::memcpy(out, bytes_to_append.data(), bytes_to_append.size());
        commit(bytes_to_append.size());
    }

    bool refuse_writes = false;

private:
    std::unique_ptr<char[]> storage_;
    std::size_t capacity_;
    std::size_t used_ = 0;
};

class MemorySink {
public:
    explicit MemorySink(MemoryBuffer* buffer) noexcept : buffer_(buffer) {}
    [[nodiscard]] char* prepare(std::size_t byte_count) noexcept { return buffer_->prepare(byte_count); }
    void commit(std::size_t byte_count) noexcept { buffer_->commit(byte_count); }
    [[nodiscard]] MemoryBuffer& buffer() const noexcept { return *buffer_; }

private:
    MemoryBuffer* buffer_;
};

static_assert(fixp::FixpSbeByteSink<MemorySink>);

using UserState = fixp::FixpSbeUserState<MemorySink>;
using Connection = fixp::FixpSbeConnection<MemorySink>;

// ---------------------------------------------------------------------------
// Frames

template <class Message>
[[nodiscard]] std::vector<char> encode_frame(const Message& message) {
    std::vector<char> frame(sbe::encoded_frame_size(message));
    const std::size_t written = sbe::encode(message, frame.data(), frame.size());
    if (written != frame.size()) FAIL("encode failed");
    return frame;
}

// One owned copy of an output frame.
struct ParsedFrame {
    std::uint16_t template_id = 0;
    std::vector<char> bytes;

    [[nodiscard]] bool is(sbe::TemplateId expected) const noexcept {
        return template_id == sbe::template_id_value(expected);
    }
    // The returned message may hold string_views into this frame.
    template <class Message>
    [[nodiscard]] Message as() const {
        sbe::FrameView view;
        if (sbe::decode_frame(bytes.data(), bytes.size(), view) != sbe::DecodeStatus::Ok) FAIL("bad frame");
        Message message;
        if (sbe::decode(view, message) != sbe::DecodeStatus::Ok) FAIL("frame is not the expected message");
        return message;
    }
};

[[nodiscard]] inline std::vector<ParsedFrame> parse_frames(std::span<const char> bytes) {
    std::vector<ParsedFrame> frames;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        sbe::FrameView view;
        if (sbe::decode_frame(bytes.data() + offset, bytes.size() - offset, view) != sbe::DecodeStatus::Ok) {
            FAIL("server output is not a sequence of whole frames");
        }
        frames.push_back(ParsedFrame{view.header.template_id,
                                     std::vector<char>(bytes.data() + offset, bytes.data() + offset + view.frame_size)});
        offset += view.frame_size;
    }
    return frames;
}

[[nodiscard]] inline std::vector<ParsedFrame> take_frames(MemoryBuffer& buffer) {
    std::vector<ParsedFrame> frames = parse_frames(buffer.bytes());
    buffer.clear();
    return frames;
}

[[nodiscard]] inline std::size_t count_frames(const std::vector<ParsedFrame>& frames, sbe::TemplateId template_id) {
    std::size_t count = 0;
    for (const ParsedFrame& frame : frames) count += frame.is(template_id) ? 1 : 0;
    return count;
}

[[nodiscard]] inline sbe::WireSymbol wire_symbol(std::string_view text) {
    return sbe::wire_symbol_from(exsim::Symbol(text));
}

// ---------------------------------------------------------------------------
// Server Context over the real MatchingEngine.

class EngineTestContext {
public:
    static constexpr std::uint64_t kRealtimeOriginUnixNs = 1'762'214'400'000'000'000ull;  // 2025-11-04T00:00:00Z

    explicit EngineTestContext(std::size_t max_orders = 4096)
        : engine_config_{4, max_orders}, engine_(engine_config_), events_(exsim::required_event_capacity(engine_config_)) {}

    exsim::InstrumentId add_instrument(std::string_view symbol_text) {
        exsim::InstrumentConfig config;
        config.symbol = exsim::Symbol(symbol_text);
        const exsim::InstrumentId instrument_id = engine_.add_instrument(config);
        REQUIRE(instrument_id != 0);
        instruments_.push_back({instrument_id, config});
        return instrument_id;
    }

    [[nodiscard]] static fixp::FixpSbeUserStateSettings small_user_state_settings() {
        fixp::FixpSbeUserStateSettings settings;
        settings.max_tokens = 4096;
        settings.store_messages = 8192;
        settings.store_bytes = 8192 * 128;
        settings.store_reserve_messages = 16;
        return settings;
    }

    UserState& add_user(std::string username, std::string password, exsim::SessionId session_id,
                        exsim::OwnerId owner = 0, exsim::StpMode default_stp = exsim::StpMode::None,
                        bool cancel_on_disconnect = true,
                        const fixp::FixpSbeUserStateSettings& settings = small_user_state_settings()) {
        auto entry = std::make_unique<UserEntry>();
        entry->config.username = std::move(username);
        entry->config.password = std::move(password);
        entry->config.protocol = fixp::kProposedFixpSbeProtocol;
        entry->config.session_id = session_id;
        entry->config.owner = owner;
        entry->config.stp = default_stp;
        entry->config.cancel_on_disconnect = cancel_on_disconnect;
        entry->state = std::make_unique<UserState>(entry->config, settings);
        users_.push_back(std::move(entry));
        return *users_.back()->state;
    }

    // ---- Context (SERVER.md 2.1) --------------------------------------------

    const exsim::server::UserConfig* authenticate(std::string_view username, std::string_view password,
                                                  exsim::server::Protocol protocol) noexcept {
        for (const auto& entry : users_) {
            if (entry->config.username == username && entry->config.password == password &&
                entry->config.protocol == protocol) {
                return &entry->config;
            }
        }
        return nullptr;
    }

    bool bind(exsim::SessionId session_id, exsim::net::ConnectionId connection_id) noexcept {
        UserEntry* entry = find_user(session_id);
        if (entry == nullptr || entry->bound) return false;
        entry->bound = true;
        entry->connection_id = connection_id;
        ++bind_calls;
        return true;
    }

    void unbind(exsim::SessionId session_id) noexcept {
        UserEntry* entry = find_user(session_id);
        if (entry == nullptr || !entry->bound) return;
        entry->bound = false;
        ++unbind_calls;
        if (entry->config.cancel_on_disconnect) {
            exsim::MassCancel cancel_on_disconnect;
            cancel_on_disconnect.ts = now_ns;
            cancel_on_disconnect.session = session_id;
            cancel_on_disconnect.cancel_on_disconnect = true;
            submit(exsim::Command(cancel_on_disconnect));
        }
    }

    exsim::InstrumentId find_instrument(const exsim::Symbol& symbol) const noexcept {
        for (const InstrumentEntry& entry : instruments_) {
            if (entry.config.symbol == symbol) return entry.id;
        }
        return 0;
    }

    const exsim::InstrumentConfig* instrument(exsim::InstrumentId instrument_id) const noexcept {
        for (const InstrumentEntry& entry : instruments_) {
            if (entry.id == instrument_id) return &entry.config;
        }
        return nullptr;
    }

    // Synchronous like the server (SERVER.md 1): handle, then dispatch the
    // batch once to every distinct session that has a FIXP/SBE user state.
    void submit(const exsim::Command& command) noexcept {
        ++submitted_commands;
        events_.clear();
        if (engine_.handle(command, events_) != exsim::HandleResult::Applied) {
            ++insufficient_output_results;
            return;
        }
        const std::span<const exsim::Event> batch(events_.begin(), events_.size());
        std::array<exsim::SessionId, 16> dispatched_sessions{};
        std::size_t dispatched_count = 0;
        const auto dispatch_once = [&](exsim::SessionId session_id) noexcept {
            if (session_id == 0) return;
            for (std::size_t index = 0; index < dispatched_count; ++index) {
                if (dispatched_sessions[index] == session_id) return;
            }
            if (dispatched_count < dispatched_sessions.size()) dispatched_sessions[dispatched_count++] = session_id;
            if (UserState* user_state = fixp_sbe_user_state(session_id)) user_state->on_events(batch, *this);
        };
        for (const exsim::Event& event : batch) {
            switch (event.type) {
                case exsim::EventType::Accepted: dispatch_once(event.accepted.session); break;
                case exsim::EventType::Rejected: dispatch_once(event.rejected.session); break;
                case exsim::EventType::Rested: dispatch_once(event.rested.session); break;
                case exsim::EventType::Fill:
                    dispatch_once(event.fill.buy.session);
                    dispatch_once(event.fill.sell.session);
                    break;
                case exsim::EventType::Canceled: dispatch_once(event.canceled.session); break;
                case exsim::EventType::Replaced: dispatch_once(event.replaced.session); break;
                case exsim::EventType::MassCancelDone: dispatch_once(event.mass_cancel_done.session); break;
                case exsim::EventType::PhaseChanged:
                case exsim::EventType::AuctionSummary: break;
            }
        }
    }

    exsim::Timestamp now() const noexcept { return now_ns; }
    std::uint64_t realtime_ns() const noexcept { return kRealtimeOriginUnixNs + now_ns; }

    UserState* fixp_sbe_user_state(exsim::SessionId session_id) noexcept {
        UserEntry* entry = find_user(session_id);
        return entry == nullptr ? nullptr : entry->state.get();
    }

    // Fault hooks (SERVER.md 4), consumed one message at a time.
    bool consume_injected_reject(exsim::SessionId session_id) noexcept {
        UserEntry* entry = find_user(session_id);
        if (entry == nullptr || entry->injected_rejects_remaining == 0) return false;
        --entry->injected_rejects_remaining;
        return true;
    }
    bool consume_drop_out(exsim::SessionId session_id) noexcept {
        UserEntry* entry = find_user(session_id);
        if (entry == nullptr || entry->drop_outs_remaining == 0) return false;
        --entry->drop_outs_remaining;
        return true;
    }

    // ---- test controls --------------------------------------------------------

    void inject_rejects(exsim::SessionId session_id, unsigned count) { require_user(session_id).injected_rejects_remaining = count; }
    void inject_drop_outs(exsim::SessionId session_id, unsigned count) { require_user(session_id).drop_outs_remaining = count; }
    [[nodiscard]] bool is_bound(exsim::SessionId session_id) { return require_user(session_id).bound; }
    void advance_ms(std::uint64_t milliseconds) noexcept { now_ns += milliseconds * 1'000'000ull; }

    // Liquidity from a session without a FIXP/SBE user state.
    void submit_resting_order(exsim::InstrumentId instrument_id, exsim::SessionId session_id, exsim::ClOrdId cl_ord_id,
                              exsim::Side side, exsim::Price price, exsim::Quantity quantity) noexcept {
        exsim::NewOrder order;
        order.ts = now_ns;
        order.instrument = instrument_id;
        order.session = session_id;
        order.owner = session_id;
        order.cl_ord_id = cl_ord_id;
        order.side = side;
        order.price = price;
        order.qty = quantity;
        submit(exsim::Command(order));
    }

    [[nodiscard]] exsim::MatchingEngine& engine() noexcept { return engine_; }

    exsim::Timestamp now_ns = 34'200'000'000'000ull;  // 09:30 day-relative
    std::uint64_t submitted_commands = 0;
    std::uint64_t insufficient_output_results = 0;
    std::uint64_t bind_calls = 0;
    std::uint64_t unbind_calls = 0;

private:
    struct UserEntry {
        exsim::server::UserConfig config;
        std::unique_ptr<UserState> state;
        bool bound = false;
        exsim::net::ConnectionId connection_id{};
        unsigned injected_rejects_remaining = 0;
        unsigned drop_outs_remaining = 0;
    };
    struct InstrumentEntry {
        exsim::InstrumentId id;
        exsim::InstrumentConfig config;
    };

    UserEntry* find_user(exsim::SessionId session_id) noexcept {
        for (const auto& entry : users_) {
            if (entry->config.session_id == session_id) return entry.get();
        }
        return nullptr;
    }
    UserEntry& require_user(exsim::SessionId session_id) {
        UserEntry* entry = find_user(session_id);
        REQUIRE(entry != nullptr);
        return *entry;
    }

    exsim::EngineConfig engine_config_;
    exsim::MatchingEngine engine_;
    exsim::EventBuffer events_;
    std::vector<std::unique_ptr<UserEntry>> users_;
    std::vector<InstrumentEntry> instruments_;
};

static_assert(fixp::FixpSbeServerContext<EngineTestContext, MemorySink>);

// ---------------------------------------------------------------------------
// One server connection driven with raw client frames.

inline const sbe::SessionUuid kFirstSessionUuid =
    fixp::version4_uuid_from_random_bits(0x0123'4567'89AB'CDEFull, 0xFEDC'BA98'7654'3210ull);
inline const sbe::SessionUuid kSecondSessionUuid =
    fixp::version4_uuid_from_random_bits(0x1111'2222'3333'4444ull, 0x5555'6666'7777'8888ull);

class ServerConnectionHarness {
public:
    explicit ServerConnectionHarness(EngineTestContext& context, std::uint32_t connection_slot = 1,
                                     const fixp::FixpSbeSessionSettings& settings = fixp::FixpSbeSessionSettings{})
        : context_(context),
          connection_(MemorySink(&output_), exsim::net::ConnectionId{connection_slot, 1}, settings, context.now()) {}

    ~ServerConnectionHarness() { connection_.on_disconnect(context_); }

    ServerConnectionHarness(const ServerConnectionHarness&) = delete;
    ServerConnectionHarness& operator=(const ServerConnectionHarness&) = delete;

    // Returns the bytes the connection consumed.
    std::size_t send_bytes(std::span<const char> bytes) { return connection_.on_data(bytes, context_); }

    template <class Message>
    std::size_t send(const Message& message) {
        const std::vector<char> frame = encode_frame(message);
        return send_bytes(frame);
    }

    void negotiate(const sbe::SessionUuid& session_uuid, std::string_view credentials = "alice:secret") {
        sbe::Negotiate negotiate;
        negotiate.session_id = session_uuid;
        negotiate.timestamp_ns = context_.realtime_ns();
        negotiate.client_flow = sbe::FlowType::Idempotent;
        negotiate.credentials = credentials;
        send(negotiate);
    }

    void establish(const sbe::SessionUuid& session_uuid, std::uint32_t keepalive_interval_ms = 1000,
                   std::string_view credentials = {}) {
        sbe::Establish establish;
        establish.session_id = session_uuid;
        establish.timestamp_ns = context_.realtime_ns();
        establish.keepalive_interval_ms = keepalive_interval_ms;
        establish.credentials = credentials;
        send(establish);
    }

    void sequence(std::uint64_t next_client_sequence_number) {
        sbe::Sequence sequence;
        sequence.next_seq_no = next_client_sequence_number;
        send(sequence);
    }

    // Negotiate + Establish + Sequence(next_client_sequence_number); output cleared.
    void log_in(const sbe::SessionUuid& session_uuid = kFirstSessionUuid, std::string_view credentials = "alice:secret",
                std::uint64_t next_client_sequence_number = 1) {
        negotiate(session_uuid, credentials);
        establish(session_uuid);
        REQUIRE(connection_.state() == fixp::FixpSbeConnectionState::Established);
        sequence(next_client_sequence_number);
        output_.clear();
    }

    [[nodiscard]] std::vector<ParsedFrame> take_output() { return take_frames(output_); }
    [[nodiscard]] Connection& connection() noexcept { return connection_; }
    [[nodiscard]] MemoryBuffer& output() noexcept { return output_; }

private:
    EngineTestContext& context_;
    MemoryBuffer output_;
    Connection connection_;
};

// ---------------------------------------------------------------------------
// Request builders.

[[nodiscard]] inline sbe::NewOrderSingle limit_order(std::uint64_t cl_ord_id, sbe::SideCode side, std::int64_t price,
                                                     std::uint32_t quantity, std::string_view symbol = "AAPL") {
    sbe::NewOrderSingle order;
    order.cl_ord_id = cl_ord_id;
    order.price_mantissa = price;
    order.order_qty = quantity;
    order.symbol = wire_symbol(symbol);
    order.side = side;
    order.ord_type = sbe::OrdTypeCode::Limit;
    order.time_in_force = sbe::TimeInForceCode::Day;
    order.exec_inst = 0;
    order.self_trade_prevention = sbe::SelfTradePreventionCode::UseUserDefault;
    return order;
}

[[nodiscard]] inline sbe::OrderCancelRequest cancel_request(std::uint64_t cl_ord_id, std::uint64_t orig_cl_ord_id,
                                                            std::uint32_t reduce_to_total_qty = 0) {
    sbe::OrderCancelRequest request;
    request.cl_ord_id = cl_ord_id;
    request.orig_cl_ord_id = orig_cl_ord_id;
    request.reduce_to_total_qty = reduce_to_total_qty;
    return request;
}

[[nodiscard]] inline sbe::OrderCancelReplaceRequest replace_request(std::uint64_t cl_ord_id, std::uint64_t orig_cl_ord_id,
                                                                    std::int64_t price, std::uint32_t quantity,
                                                                    bool force_priority_loss = false) {
    sbe::OrderCancelReplaceRequest request;
    request.cl_ord_id = cl_ord_id;
    request.orig_cl_ord_id = orig_cl_ord_id;
    request.price_mantissa = price;
    request.order_qty = quantity;
    request.force_priority_loss = force_priority_loss ? sbe::BooleanCode::True : sbe::BooleanCode::False;
    return request;
}

}  // namespace fixp_sbe_test

#endif
