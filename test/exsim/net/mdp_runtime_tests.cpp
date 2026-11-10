#include "api/api_json.hpp"
#include "api/exchange_service.hpp"
#include "server/server_runtime.hpp"
#include "test/exsim/gateway/fix/fix_test_support.hpp"
#include "test/exsim/gateway/ilink/ilink_test_support.hpp"
#include "test/framework/test.hpp"

#include <arpa/inet.h>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <poll.h>
#include <unistd.h>

namespace {
using namespace exsim;
namespace cme = protocol::cme;
using Pump = std::function<void()>;
struct Feeds {
    net::UdpReceiver primary, backup, snapshot, instrument;
    net::Ipv4Endpoint p{}, b{}, s{}, i{};
    Feeds() {
        for (auto* socket : {&primary, &backup, &snapshot, &instrument}) REQUIRE_EQ(socket->open(net::loopback_endpoint(0)), 0);
        REQUIRE_EQ(net::local_endpoint(primary.fd(), p), 0);
        REQUIRE_EQ(net::local_endpoint(backup.fd(), b), 0);
        REQUIRE_EQ(net::local_endpoint(snapshot.fd(), s), 0);
        REQUIRE_EQ(net::local_endpoint(instrument.fd(), i), 0);
    }
    std::string config_text() const {
        std::ifstream file("test/fixtures/mdp3.conf");
        REQUIRE(file.good());
        std::string text((std::istreambuf_iterator<char>(file)), {});
        for (const auto& [from, to] : {std::pair{19001, p.port}, {19002, b.port}, {19003, s.port}, {19004, i.port}}) {
            const auto key = std::to_string(from);
            const auto pos = text.find(key);
            REQUIRE(pos != std::string::npos);
            text.replace(pos, key.size(), std::to_string(to));
        }
        const auto interval = text.find("snapshot_interval_ms = 1000");
        text.replace(interval, std::string_view("snapshot_interval_ms = 1000").size(), "snapshot_interval_ms = 20");
        return text;
    }
};
void wait_readable(int fd, const Pump& pump) {
    if (pump) pump();
    pollfd event{fd, POLLIN, 0};
    (void)::poll(&event, 1, 1);
}
std::vector<char> datagram(net::UdpReceiver& socket, const Pump& pump, unsigned template_id) {
    std::array<char, 65507> bytes{};
    const auto deadline = net::monotonic_ns() + 2000000000;
    while (net::monotonic_ns() < deadline) {
        wait_readable(socket.fd(), pump);
        net::Ipv4Endpoint peer{};
        const auto size = socket.receive_from(bytes, peer);
        if (size < 0) { REQUIRE(net::is_would_block(errno)); continue; }
        cme::MdpPacketView packet; cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_packet(std::span(bytes).first(static_cast<std::size_t>(size)), packet), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
        if (message.header.template_id == template_id) return {bytes.begin(), bytes.begin() + size};
    }
    FAIL("MDP datagram timeout"); return {};
}
net::Socket connect_port(std::uint16_t port) {
    net::Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(socket.is_open());
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    REQUIRE_EQ(::connect(socket.fd(), reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    REQUIRE_EQ(net::set_non_blocking(socket.fd()), 0);
    REQUIRE_EQ(net::set_no_sigpipe(socket.fd()), 0);
    return socket;
}
void send_text(int fd, std::string_view text) {
    REQUIRE_EQ(net::send_some(fd, text.data(), text.size()), static_cast<ssize_t>(text.size()));
}
std::vector<char> read_exact(int fd, std::size_t size, const Pump& pump) {
    std::vector<char> bytes(size);
    std::size_t used = 0;
    const auto deadline = net::monotonic_ns() + 2000000000;
    while (used < size && net::monotonic_ns() < deadline) {
        wait_readable(fd, pump);
        const auto count = ::recv(fd, bytes.data() + used, size - used, 0);
        if (count > 0) used += static_cast<std::size_t>(count);
        else if (count < 0) REQUIRE(net::is_would_block(errno));
        else FAIL("unexpected replay EOF");
    }
    REQUIRE_EQ(used, size); return bytes;
}
std::vector<char> replay_packet(int fd, const Pump& pump) {
    const auto header = read_exact(fd, 2, pump);
    return read_exact(fd, cme::load_le<std::uint16_t>(header.data()), pump);
}
void expect_template(const std::vector<char>& bytes, unsigned id, unsigned schema) {
    cme::MdpPacketView packet; cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(bytes, packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    CHECK_EQ(message.header.template_id, id); CHECK_EQ(message.header.schema_id, schema);
}
void fix_exchange(int fd, std::string_view message, const Pump& pump) {
    send_text(fd, message);
    std::string response;
    const auto deadline = net::monotonic_ns() + 2000000000;
    std::array<char, 8192> bytes{};
    while (net::monotonic_ns() < deadline) {
        wait_readable(fd, pump);
        const auto count = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (count > 0) response.append(bytes.data(), static_cast<std::size_t>(count));
        else if (count < 0) REQUIRE(net::is_would_block(errno));
        std::size_t size = 0;
        if (protocol::fix44::frame_size(std::span(response), size) == protocol::Status::ok && size <= response.size()) {
            REQUIRE(response.find("35=3\1") == std::string::npos);
            REQUIRE(response.find("35=5\1") == std::string::npos);
            return;
        }
    }
    FAIL("FIX response timeout");
}
struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() { char text[] = "/tmp/exsim-mdp-XXXXXX"; const char* result = ::mkdtemp(text); REQUIRE(result); path = result; }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
api::HttpResponse api_request(api::ExchangeService& service, std::string method, std::string path, std::string body = {}) {
    api::HttpRequest request; request.method = std::move(method); request.path = std::move(path); request.body = std::move(body);
    return service.handle(request);
}
std::string config_json(const std::string& text) {
    api::ApiJsonWriter writer; return writer.begin_object().key("config").string(text).end_object().take();
}
std::string ilink_user_text() {
    return "\n[user ILINK]\nprotocol=ilink3\nsession_id=3\nowner=3\ncancel_on_disconnect=true\n"
           "ilink_access_key=ACCESS-1\nilink_secret_key=" + std::string(43, 'A') + "=\n"
           "ilink_session=AAA\nilink_firm=EXSIM\nilink_party_id=1001\nilink_account=SIM-ACCOUNT\n";
}
ilink_test::Frame ilink_packet(int fd, const Pump& pump) {
    auto bytes = read_exact(fd, 4, pump);
    const auto size = cme::load_le<std::uint16_t>(bytes.data());
    REQUIRE(size >= 12);
    const auto body = read_exact(fd, size - 4, pump);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return {std::move(bytes)};
}
template<class M> void ilink_send(int fd, const protocol::ilink3::Root<M>& root) {
    const auto bytes = ilink_test::encode(root);
    REQUIRE_EQ(net::send_some(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
}
void ilink_logon(int fd, const server::UserConfig& user, std::uint64_t uuid, const Pump& pump) {
    ilink_send(fd, ilink_test::negotiation(user, uuid, net::realtime_ns()));
    REQUIRE_EQ(ilink_packet(fd, pump).id(), 501);
    ilink_send(fd, ilink_test::establishment(user, uuid, net::realtime_ns(), 1));
    REQUIRE_EQ(ilink_packet(fd, pump).id(), 504);
}
}

TEST_CASE(mdp_runtime, fix_orders_udp_redundancy_snapshots_and_tcp_recovery) {
    Feeds feeds;
    server::ServerConfig config; std::string error;
    REQUIRE(server::parse_config(feeds.config_text(), config, error));
    server::ServerRuntimeOptions options; options.log = nullptr; options.day_time_at_start = fix_test::kSessionStartNs;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    const Pump pump = [&] { runtime.poll_once(); };
    const auto primary = datagram(feeds.primary, pump, 4);
    CHECK_EQ(datagram(feeds.backup, pump, 4), primary);
    expect_template(datagram(feeds.instrument, pump, 54), 54, 1);
    expect_template(datagram(feeds.snapshot, pump, 52), 52, 1);
    auto alice = connect_port(runtime.listen_endpoint<server::runtime::FixGateway>().port);
    auto bob = connect_port(runtime.listen_endpoint<server::runtime::FixGateway>().port);
    fix_exchange(alice.fd(), fix_test::client_message("A", 1, "98=0|108=30|554=alice|", "ALICE"), pump);
    fix_exchange(bob.fd(), fix_test::client_message("A", 1, "98=0|108=30|554=bob|", "BOB"), pump);
    fix_exchange(alice.fd(), fix_test::client_message("D", 2, fix_test::new_order_fields("BUY", "1", 10, "150", "ES"), "ALICE"), pump);
    const auto book = datagram(feeds.primary, pump, 46);
    CHECK_EQ(datagram(feeds.backup, pump, 46), book);
    CHECK_EQ(runtime.core().engine().live_order_count(), 1u);
    fix_exchange(bob.fd(), fix_test::client_message("D", 2, fix_test::new_order_fields("SELL", "2", 4, "150", "ES"), "BOB"), pump);
    expect_template(datagram(feeds.primary, pump, 48), 48, 1);
    auto replay = connect_port(runtime.retransmission_endpoint().port);
    send_text(replay.fd(), fix_test::client_message("A", 1, "553=CME|554=exsim-replay|"));
    expect_template(replay_packet(replay.fd(), pump), 408, 9);
    const auto sequence = cme::load_le<std::uint32_t>(book.data());
    send_text(replay.fd(), fix_test::client_message("V", 2,
        "262=RECOVERY|1180=310|1182=" + std::to_string(sequence) + "|1183=" + std::to_string(sequence) + "|"));
    CHECK_EQ(replay_packet(replay.fd(), pump), book);
    expect_template(replay_packet(replay.fd(), pump), 409, 9);
    send_text(replay.fd(), fix_test::client_message("5", 3, ""));
    // Snapshot stream eventually includes the same remaining quantity as the engine.
    bool recovered = false;
    for (unsigned attempt = 0; attempt < 10 && !recovered; ++attempt) {
        const auto bytes = datagram(feeds.snapshot, pump, 52);
        cme::MdpPacketView packet; cme::MessageView message;
        REQUIRE_EQ(cme::decode_mdp_packet(bytes, packet), protocol::Status::ok);
        REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
        protocol::mdp3::Snapshot snapshot; cme::GroupView entries;
        REQUIRE_EQ(protocol::mdp3::decode_snapshot(message, snapshot, entries), protocol::Status::ok);
        if (entries.count == 1 && cme::load_le<std::int32_t>(entries.entry(0).data() + 8) == 6) recovered = true;
    }
    CHECK(recovered);
    runtime.request_shutdown();
    const auto deadline = net::monotonic_ns() + 4000000000;
    while (!runtime.finished() && net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK(runtime.finished());
}

TEST_CASE(mdp_runtime, api_managed_exchange_readiness_redaction_conflicts_and_restart) {
    Feeds feeds;
    TempDirectory directory;
    const char* build = std::getenv("EXSIM_BUILD");
    api::ExchangeService service(directory.path, std::filesystem::path(build ? build : "build/release") / "exsim_exchanged");
    const auto text = feeds.config_text() + ilink_user_text();
    const auto created = api_request(service, "POST", "/v1/exchanges", config_json(text));
    REQUIRE_EQ(created.status, 201);
    REQUIRE(created.body.find("exsim-replay") == std::string::npos);
    REQUIRE(created.body.find("[REDACTED]") != std::string::npos);
    REQUIRE(created.body.find(std::string(43, 'A')) == std::string::npos);
    const auto json = api::parse_json(created.body);
    const auto path = "/v1/exchanges/" + json.find("id")->text;
    const auto started = api_request(service, "POST", path + "/start");
    REQUIRE_EQ(started.status, 200);
    const auto running = api::parse_json(started.body);
    const auto* connections = running.find("runtime")->find("connections");
    REQUIRE(connections);
    CHECK_EQ(connections->find("marketDataProtocol")->text, "mdp3_sbe");
    CHECK_EQ(connections->find("recoveryTransport")->text, "tcp");
    expect_template(datagram(feeds.primary, {}, 4), 4, 1);
    auto alice = connect_port(static_cast<std::uint16_t>(std::stoul(connections->find("ilinkOrderEntryPort")->text)));
    server::ServerConfig config; std::string error;
    REQUIRE(server::parse_config(text, config, error));
    ilink_logon(alice.fd(), config.users.back(), 1700000000000001ull, {});
    ilink_send(alice.fd(), ilink_test::order(config.users.back(), 1, 1, "MANAGED", 1, 7));
    CHECK_EQ(ilink_packet(alice.fd(), {}).id(), 522);
    expect_template(datagram(feeds.primary, {}, 46), 46, 1);
    // Cross-protocol destination conflicts include snapshot and definition feeds.
    auto other_text = text;
    auto position = other_text.find("port = " + std::to_string(feeds.p.port));
    other_text.replace(position, std::string("port = " + std::to_string(feeds.p.port)).size(), "port = " + std::to_string(feeds.s.port));
    position = other_text.find("snapshot_port = " + std::to_string(feeds.s.port));
    other_text.replace(position, std::string("snapshot_port = " + std::to_string(feeds.s.port)).size(), "snapshot_port = " + std::to_string(feeds.p.port));
    const auto other = api_request(service, "POST", "/v1/exchanges", config_json(other_text));
    REQUIRE_EQ(other.status, 201);
    const auto other_json = api::parse_json(other.body);
    CHECK_EQ(api_request(service, "POST", "/v1/exchanges/" + other_json.find("id")->text + "/start").status, 409);
    alice.close();
    REQUIRE_EQ(api_request(service, "POST", path + "/stop").status, 200);
    const auto restart = api_request(service, "POST", path + "/start");
    REQUIRE_EQ(restart.status, 200);
    const auto restarted = api::parse_json(restart.body);
    CHECK_EQ(restarted.find("runtime")->find("liveOrders")->text, "0");
    REQUIRE_EQ(api_request(service, "POST", path + "/stop").status, 200);
}

TEST_CASE(ilink_runtime, private_fills_correlate_with_mdp_and_replay_over_real_sockets) {
    namespace ilink = protocol::ilink3;
    namespace schema = ilink::schema;
    Feeds feeds;
    server::ServerConfig config; std::string error;
    REQUIRE(server::parse_config(feeds.config_text() + ilink_user_text(), config, error));
    server::ServerRuntimeOptions options; options.log = nullptr; options.day_time_at_start = fix_test::kSessionStartNs;
    server::ServerRuntime runtime(config, options);
    REQUIRE(runtime.start().empty());
    const Pump pump = [&] { runtime.poll_once(); };
    auto trader = connect_port(runtime.listen_endpoint<server::runtime::IlinkGateway>().port);
    const auto& user = config.users.back();
    constexpr std::uint64_t uuid = 1700000000000001ull;
    ilink_logon(trader.fd(), user, uuid, pump);
    ilink_send(trader.fd(), ilink_test::order(user, 1, 1, "CME-BUY", 1, 10));
    const auto accepted = ilink_packet(trader.fd(), pump);
    REQUIRE_EQ(accepted.id(), 522);
    const auto order_id = ilink::get(accepted.view(), schema::ExecutionReportNew522::OrderID);
    expect_template(datagram(feeds.primary, pump, 46), 46, 1);
    auto bob = connect_port(runtime.listen_endpoint<server::runtime::FixGateway>().port);
    fix_exchange(bob.fd(), fix_test::client_message("A", 1, "98=0|108=30|554=bob|", "BOB"), pump);
    fix_exchange(bob.fd(), fix_test::client_message("D", 2, fix_test::new_order_fields("SELL", "2", 4, "150", "ES"), "BOB"), pump);
    const auto fill = ilink_packet(trader.fd(), pump); REQUIRE_EQ(fill.id(), 525);
    const auto public_bytes = datagram(feeds.primary, pump, 48);
    cme::MdpPacketView packet; cme::MessageView message;
    REQUIRE_EQ(cme::decode_mdp_packet(public_bytes, packet), protocol::Status::ok);
    REQUIRE_EQ(cme::decode_mdp_message(packet.messages, message), protocol::Status::ok);
    protocol::mdp3::IncrementalView public_trade;
    REQUIRE_EQ(protocol::mdp3::decode_incremental(message, public_trade), protocol::Status::ok);
    const auto entry = public_trade.entries.entry(0);
    CHECK_EQ(ilink::get(fill.view(), schema::ExecutionReportTradeOutright525::MDTradeEntryID), cme::load_le<std::uint32_t>(entry.data() + 26));
    CHECK_EQ(ilink::get(fill.view(), schema::ExecutionReportTradeOutright525::LastPx), cme::load_le<std::int64_t>(entry.data()));
    CHECK_EQ(ilink::get(fill.view(), schema::ExecutionReportTradeOutright525::TransactTime), public_trade.transact_time);
    CHECK_EQ(cme::load_le<std::uint64_t>(public_trade.orders.entry(0).data()), order_id);
    CHECK_EQ(ilink::get(fill.view(), schema::ExecutionReportTradeOutright525::LeavesQty), 6u);
    ilink::Root<schema::RetransmitRequest508> replay;
    replay.set(schema::RetransmitRequest508::UUID, uuid); replay.set(schema::RetransmitRequest508::FromSeqNo, 2);
    replay.set(schema::RetransmitRequest508::MsgCount, 1); replay.set(schema::RetransmitRequest508::RequestTimestamp, net::realtime_ns());
    ilink_send(trader.fd(), replay);
    REQUIRE_EQ(ilink_packet(trader.fd(), pump).id(), 509);
    const auto recovered = ilink_packet(trader.fd(), pump); REQUIRE_EQ(recovered.id(), 525);
    CHECK_EQ(ilink::get(recovered.view(), schema::ExecutionReportTradeOutright525::SeqNum), 2u);
    CHECK_EQ(ilink::get(recovered.view(), schema::ExecutionReportTradeOutright525::PossRetransFlag), 1);
    trader.close();
    const auto deadline = net::monotonic_ns() + 2000000000;
    while (runtime.core().engine().live_order_count() && net::monotonic_ns() < deadline) runtime.poll_once();
    CHECK_EQ(runtime.core().engine().live_order_count(), 0u);
    bob.close(); runtime.request_shutdown();
    const auto stopped = net::monotonic_ns() + 4000000000;
    while (!runtime.finished() && net::monotonic_ns() < stopped) runtime.poll_once();
    CHECK(runtime.finished());
}
