#include "api/runtime_snapshot.hpp"
#include "api/api_json.hpp"
namespace exsim::api {
std::string runtime_snapshot_json(server::ServerRuntime& runtime) {
    const auto& market_data = runtime.config().market_data;
    const bool mdp = market_data.protocol == server::MarketDataProtocol::Mdp3;
    ApiJsonWriter json;
    json.begin_object()
        .key("state").string(runtime.core().shutdown_requested() ? "stopping" : "running")
        .key("liveOrders").unsigned_text(runtime.core().engine().live_order_count())
        .key("commandsApplied").unsigned_text(runtime.core().counters().commands_applied)
        .key("connections").begin_object()
        .key("address").string(runtime.config().exchange.bind_address)
        .key("orderEntryProtocol").string("ouch42_soupbintcp")
        .key("orderEntryPort").integer_number(runtime.listen_endpoint<server::runtime::OuchGateway>().port)
        .key("fixOrderEntryPort").integer_number(runtime.listen_endpoint<server::runtime::FixGateway>().port)
        .key("ilinkOrderEntryPort").integer_number(runtime.listen_endpoint<server::runtime::IlinkGateway>().port)
        .key("marketDataProtocol").string(mdp ? "mdp3_sbe" : "itch50_moldudp64")
        .key("marketDataAddress").string(market_data.address)
        .key("marketDataPort").integer_number(market_data.port)
        .key("recoveryPort").integer_number(runtime.retransmission_endpoint().port)
        .key("recoveryTransport").string(mdp ? "tcp" : "udp")
        .key("snapshotPort").integer_number(market_data.snapshot_port)
        .key("instrumentReplayPort").integer_number(market_data.instrument_port)
        .key("backupMarketDataPort").integer_number(market_data.backup_port)
        .key("channelId").integer_number(market_data.channel_id)
        .key("session").string(market_data.session)
        .end_object()
        .end_object();
    return json.take();
}
}
