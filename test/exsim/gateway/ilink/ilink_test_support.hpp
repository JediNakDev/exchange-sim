#pragma once
#include "server/server_runtime.hpp"
#include "test/framework/test.hpp"

namespace ilink_test {
using namespace exsim;
namespace wire = protocol::ilink3;
namespace schema = wire::schema;
struct Memory {
    std::vector<char> bytes = std::vector<char>(2 * 1024 * 1024);
    std::size_t used = 0;
    void clear() { used = 0; }
};
struct Sink {
    Memory* memory = nullptr;
    char* prepare(std::size_t size) noexcept { return size <= memory->bytes.size() - memory->used ? memory->bytes.data() + memory->used : nullptr; }
    void commit(std::size_t size) noexcept { memory->used += size; }
};
struct Clock {
    Timestamp day = 36000000000000;
    Timestamp day_ns() const noexcept { return day; }
    std::uint64_t realtime_ns() const noexcept { return 1700000000000000000ull + day; }
    std::uint64_t epoch_ns(Timestamp value) const noexcept { return 1700000000000000000ull + value; }
};
struct MarketData { void on_events(std::span<const Event>) noexcept {} void flush() noexcept {} };
using Gateway = server::runtime::BasicIlinkGateway<Sink>;
using Sessions = server::runtime::GatewaySessions<Gateway>;
using Core = server::ExchangeCore<Sessions, MarketData, Clock>;

inline server::ServerConfig config() {
    server::ServerConfig result;
    result.engine.engine = {2, 64};
    result.engine.max_tokens_per_session = 256;
    result.engine.store_messages_per_session = 512;
    result.engine.store_bytes_per_session = 512 * 1024;
    InstrumentConfig instrument;
    instrument.symbol = Symbol("ES"); instrument.cme_security_id = 800001;
    instrument.min_price = 100; instrument.max_price = 2000000; instrument.reference_price = 1500000;
    result.instruments.push_back(instrument);
    for (std::uint16_t id = 1; id <= 2; ++id) {
        server::UserConfig user;
        user.username = id == 1 ? "ALICE" : "BOB"; user.protocol = server::Protocol::Ilink3;
        user.session_id = id; user.owner = id; user.cancel_on_disconnect = false;
        user.ilink_access_key = id == 1 ? "ACCESS-1" : "ACCESS-2";
        user.ilink_session = id == 1 ? "AAA" : "BBB"; user.ilink_firm = "EXSIM";
        user.ilink_secret_key = std::string(43, 'A') + "=";
        user.ilink_party_id = 1000 + id; user.ilink_account = "SIM-ACCOUNT";
        result.users.push_back(user);
    }
    return result;
}
struct Exchange {
    server::ServerConfig configuration;
    Sessions sessions;
    MarketData md;
    Clock clock;
    Core core;
    std::uint64_t auth_counter = 0;
    explicit Exchange(server::ServerConfig value = config()) : configuration(std::move(value)), sessions(configuration), core(configuration, sessions, md, clock) {}
    auto& user(SessionId id) {
        for (const auto& user : sessions.user_directory<Gateway>().users()) {
            if (user->session_id() == id) return *user;
        }
        FAIL("no iLink user with that session");
    }
    std::uint64_t auth_time() { return clock.realtime_ns() + ++auth_counter; }
};
struct Frame {
    std::vector<char> bytes;
    protocol::cme::MessageView view() const {
        protocol::cme::MessageView result;
        REQUIRE_EQ(protocol::cme::decode_ilink(bytes, result), protocol::Status::ok); return result;
    }
    std::uint16_t id() const { return view().header.template_id; }
};
inline std::vector<Frame> frames(const Memory& memory) {
    std::vector<Frame> result;
    auto bytes = std::span<const char>(memory.bytes.data(), memory.used);
    while (!bytes.empty()) {
        protocol::cme::MessageView message;
        REQUIRE_EQ(protocol::cme::decode_ilink(bytes, message), protocol::Status::ok);
        result.push_back({{bytes.begin(), bytes.begin() + message.frame_size}});
        bytes = bytes.subspan(message.frame_size);
    }
    return result;
}
template<class M> inline std::vector<char> encode(const wire::Root<M>& root) {
    std::vector<char> bytes(2048); const auto size = root.encode(bytes); REQUIRE(size); bytes.resize(size); return bytes;
}
template<class M> void sign(wire::Root<M>& root, const server::UserConfig& user) {
    const auto bytes = encode(root);
    protocol::cme::MessageView message;
    REQUIRE_EQ(protocol::cme::decode_ilink(bytes, message), protocol::Status::ok);
    wire::SigningText text;
    text.append(wire::get(message, M::RequestTimestamp)); text.append(wire::get(message, M::UUID));
    text.append(wire::get(message, M::Session)); text.append(wire::get(message, M::Firm));
    if constexpr (M::id == 503) {
        text.append(wire::get(message, M::TradingSystemName)); text.append(wire::get(message, M::TradingSystemVersion));
        text.append(wire::get(message, M::TradingSystemVendor)); text.append(wire::get(message, M::NextSeqNo));
        text.append(wire::get(message, M::KeepAliveInterval));
    }
    std::array<char, 32> key{}; REQUIRE(wire::decode_secret(user.ilink_secret_key, key));
    const auto signature = wire::hmac_sha256(key, text.view());
    REQUIRE(root.set(M::HMACSignature, std::string_view(signature.data(), signature.size())));
}
inline wire::Root<schema::Negotiate500> negotiation(const server::UserConfig& user, std::uint64_t uuid, std::uint64_t timestamp) {
    using M = schema::Negotiate500; wire::Root<M> root;
    root.set(M::AccessKeyID, user.ilink_access_key); root.set(M::Session, user.ilink_session); root.set(M::Firm, user.ilink_firm);
    root.set(M::UUID, uuid); root.set(M::RequestTimestamp, timestamp); sign(root, user); return root;
}
inline wire::Root<schema::Establish503> establishment(const server::UserConfig& user, std::uint64_t uuid,
                                                    std::uint64_t timestamp, std::uint32_t sequence) {
    using M = schema::Establish503; wire::Root<M> root;
    root.set(M::AccessKeyID, user.ilink_access_key); root.set(M::Session, user.ilink_session); root.set(M::Firm, user.ilink_firm);
    root.set(M::TradingSystemName, "exsim-test"); root.set(M::TradingSystemVersion, "1"); root.set(M::TradingSystemVendor, "test");
    root.set(M::UUID, uuid); root.set(M::RequestTimestamp, timestamp); root.set(M::NextSeqNo, sequence);
    root.set(M::KeepAliveInterval, 5000); sign(root, user); return root;
}
template<class M> void request_fields(wire::Root<M>& root, const server::UserConfig& user, std::uint32_t sequence, std::uint64_t request) {
    root.set(M::SeqNum, sequence); root.set(M::PartyDetailsListReqID, user.ilink_party_id);
    root.set(M::OrderRequestID, request); root.set(M::SenderID, "TRADER"); root.set(M::Location, "US");
    root.set(M::ManualOrderIndicator, 0); root.set(M::SendingTimeEpoch, 1700000000000000000ull);
}
inline wire::Root<schema::NewOrderSingle514> order(const server::UserConfig& user, std::uint32_t sequence,
        std::uint64_t request, std::string_view client, std::uint8_t side = 1, std::uint32_t quantity = 10, std::int64_t price = 150000000000) {
    using M = schema::NewOrderSingle514; wire::Root<M> root;
    request_fields(root, user, sequence, request);
    root.set(M::ClOrdID, client); root.set(M::SecurityID, 800001); root.set(M::Side, side);
    root.set(M::OrderQty, quantity); root.set(M::Price, price); root.set(M::OrdType, '2'); root.set(M::TimeInForce, 0);
    return root;
}
struct Client {
    Exchange& exchange; SessionId session; Memory memory;
    gateway::ilink::IlinkConnection<Sink> connection;
    std::uint64_t uuid;
    Client(Exchange& value, SessionId id, server::ConnectionId connection_id = 0)
        : exchange(value), session(id), connection(exchange.sessions.user_directory<Gateway>()), uuid(1700000000000000ull + id) {
        connection.open(Sink{&memory}, connection_id ? connection_id : id, exchange.core);
    }
    const auto& user_config() const { return exchange.configuration.users[session - 1]; }
    void feed(std::span<const char> bytes) { ++exchange.clock.day; REQUIRE_EQ(connection.on_data(bytes, exchange.core), bytes.size()); }
    template<class M> void send(const wire::Root<M>& root) { const auto bytes = encode(root); feed(bytes); }
    void login(bool negotiate = true, std::uint32_t next = 1) {
        if (negotiate) {
            send(negotiation(user_config(), uuid, exchange.auth_time()));
            auto responses = frames(memory); REQUIRE_EQ(responses.size(), 1u); REQUIRE_EQ(responses[0].id(), 501); memory.clear();
        }
        send(establishment(user_config(), uuid, exchange.auth_time(), next));
        auto responses = frames(memory); REQUIRE_EQ(responses.size(), 1u); REQUIRE_EQ(responses[0].id(), 504); memory.clear();
    }
};
}
