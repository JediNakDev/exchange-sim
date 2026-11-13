#include "api/exchange_service.hpp"
#include "api/api_json.hpp"
#include "api/json.hpp"
#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace exsim::api {
namespace {
std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read exchange configuration");
    std::string value((std::istreambuf_iterator<char>(file)), {});
    if (value.size() > 1024 * 1024) throw std::runtime_error("exchange configuration too large");
    return value;
}
server::ServerConfig validate(std::string_view text) {
    server::ServerConfig config;
    std::string error;
    if (!server::parse_config(text, config, error)) throw std::invalid_argument(error);
    if (config.users.empty()) throw std::invalid_argument("configure at least one user");
    std::uint32_t address = 0;
    if (!net::parse_ipv4_address(config.exchange.bind_address, address) || address == 0 || net::is_ipv4_multicast_address(address))
        throw std::invalid_argument("bind_address must be a specific local unicast IPv4 address");
    if (!net::parse_ipv4_address(config.market_data.address, address) || !config.market_data.port)
        throw std::invalid_argument("market data requires an IPv4 destination and nonzero port");
    if (!config.market_data.interface_address.empty() && !net::parse_ipv4_address(config.market_data.interface_address, address))
        throw std::invalid_argument("invalid market data interface IPv4 address");
    if (!config.journal_path.empty()) throw std::invalid_argument("managed exchange journaling is not enabled yet");
    if (text.find("[REDACTED]") != std::string_view::npos) throw std::invalid_argument("supply full credentials when replacing configuration");
    return config;
}
std::string config_body(const HttpRequest& request) {
    const auto json = parse_json(request.body);
    if (json.kind != JsonValue::Kind::Object || json.members.size() != 1 || !json.find("config") || json.find("config")->kind != JsonValue::Kind::String)
        throw std::invalid_argument("expected an object containing only a config string (INI format)");
    return json.find("config")->text;
}
std::string redacted(std::string_view config) {
    std::istringstream input{std::string(config)};
    std::string line, output;
    while (std::getline(input, line)) {
        auto key = line.substr(0, line.find('='));
        key.erase(0, key.find_first_not_of(" \t"));
        const auto last = key.find_last_not_of(" \t");
        if (last != std::string::npos) key.resize(last + 1);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "password" || key == "replay_password" || key == "ilink_secret_key") line = key + " = [REDACTED]";
        output += line + '\n';
    }
    return output;
}
HttpResponse response(int status, std::string body) {
    HttpResponse result; result.status = status; result.body = std::move(body);
    result.set_header("Content-Type", "application/json");
    return result;
}
std::array<std::uint16_t, 4> feed_ports(const server::MarketDataSection& md) {
    if (md.protocol == server::MarketDataProtocol::Mdp3) return {md.port, md.backup_port, md.snapshot_port, md.instrument_port};
    return {md.port, 0, 0, 0};
}
bool feeds_overlap(const server::MarketDataSection& first, const server::MarketDataSection& second) {
    if (first.address != second.address) return false;
    for (const auto port : feed_ports(first))
        for (const auto other_port : feed_ports(second))
            if (port && port == other_port) return true;
    return false;
}
}
HttpResponse exchange_error(int status, std::string_view code, std::string_view message) {
    ApiJsonWriter json;
    json.begin_object().key("error").begin_object().key("code").string(code).key("message").string(message).end_object().end_object();
    return response(status, json.take());
}
ExchangeService::ExchangeService(std::filesystem::path directory, std::filesystem::path executable)
    : directory_(std::move(directory)), executable_(std::filesystem::absolute(executable)) {
    std::filesystem::create_directories(directory_);
    ::chmod(directory_.c_str(), 0700);
    lock_ = net::Socket(::open((directory_ / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
    if (!lock_.is_open() || ::flock(lock_.fd(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("exchange directory is locked by another manager");
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (entry.path().extension() != ".conf") continue;
        auto exchange = std::make_shared<Exchange>();
        exchange->id = entry.path().stem().string();
        exchange->config_text = read_file(entry.path());
        exchange->config = validate(exchange->config_text);
        exchange->listed_view = view(*exchange);
        exchanges_.emplace(exchange->id, std::move(exchange));
    }
}
ExchangeService::~ExchangeService() {
    // Let every child drain concurrently before waiting for individual exits.
    for (auto& [id, exchange] : exchanges_) { (void)id; exchange->process.request_stop(); }
    for (auto& [id, exchange] : exchanges_) { (void)id; exchange->process.stop(); }
}
void ExchangeService::save(const std::string& id, std::string_view config_text) {
    const auto final = directory_ / (id + ".conf");
    const auto temporary = directory_ / (id + ".tmp");
    net::Socket file(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!file.is_open()) throw std::runtime_error("cannot create config file");
    try {
        std::size_t offset = 0;
        while (offset < config_text.size()) {
            const auto written = ::write(file.fd(), config_text.data() + offset, config_text.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("cannot write config file");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file.fd()) != 0) throw std::runtime_error("cannot flush config file");
        std::filesystem::rename(temporary, final);
    } catch (...) { std::filesystem::remove(temporary); throw; }
}
std::string ExchangeService::view(const Exchange& exchange) const {
    const auto& process = exchange.process;
    ApiJsonWriter json;
    json.begin_object().key("id").string(exchange.id).key("name").string(exchange.config.exchange.name)
        .key("state").string(to_string(process.state())).key("pid");
    if (process.pid() > 0) json.integer_number(process.pid()); else json.null();
    json.key("error").string(process.error()).key("config").string(redacted(exchange.config_text))
        .key("runtime");
    if (process.snapshot().empty()) json.null(); else json.raw(process.snapshot());
    return json.end_object().take();
}
void ExchangeService::release_feed_and_publish_view(Exchange& exchange) {
    auto listed_view = exchange.deleted ? std::string() : view(exchange);
    std::lock_guard guard(mutex_);
    if (exchange.process.pid() <= 0) claimed_feeds_.erase(exchange.id);
    if (!exchange.deleted) exchange.listed_view = std::move(listed_view);
}
HttpResponse ExchangeService::handle(const HttpRequest& request) {
    try {
        if (request.path == "/v1/service" && request.method == "GET")
            return response(200, R"({"product":"exsim","apiVersion":"v1","profile":"ouch42_soupbintcp_itch50_moldudp64","orderEntryProtocols":["ouch42_soupbintcp","fix44","ilink3"],"marketDataProtocols":["itch50_moldudp64","mdp3_sbe"]})");
        const std::string root = "/v1/exchanges";
        if (request.path == root) {
            if (request.method == "GET") return list_exchanges();
            if (request.method == "POST") return create_exchange(request);
            return exchange_error(405, "method_not_allowed", "use GET or POST");
        }
        if (!request.path.starts_with(root + "/")) return exchange_error(404, "not_found", "unknown route");
        const auto tail = request.path.substr(root.size() + 1);
        const auto slash = tail.find('/');
        const auto id = tail.substr(0, slash);
        const auto action = slash == std::string::npos ? "" : tail.substr(slash + 1);
        std::shared_ptr<Exchange> exchange;
        {
            std::lock_guard guard(mutex_);
            const auto found = exchanges_.find(id);
            if (found == exchanges_.end()) return exchange_error(404, "not_found", "unknown exchange");
            exchange = found->second;
        }
        std::lock_guard exchange_guard(exchange->mutex);
        if (exchange->deleted) return exchange_error(404, "not_found", "unknown exchange");
        HttpResponse result;
        try {
            exchange->process.reap_if_exited();
            result = handle_exchange_request(*exchange, request, action);
        } catch (...) {
            release_feed_and_publish_view(*exchange);
            throw;
        }
        release_feed_and_publish_view(*exchange);
        return result;
    } catch (const JsonError& error) { return exchange_error(400, "invalid_json", error.what()); }
      catch (const std::invalid_argument& error) { return exchange_error(400, "invalid_config", error.what()); }
      catch (const std::exception&) { return exchange_error(500, "internal_error", "exchange management operation failed"); }
}
HttpResponse ExchangeService::list_exchanges() {
    std::vector<std::shared_ptr<Exchange>> exchanges;
    {
        std::lock_guard guard(mutex_);
        for (const auto& [id, exchange] : exchanges_) { (void)id; exchanges.push_back(exchange); }
    }
    ApiJsonWriter json;
    json.begin_array();
    for (const auto& exchange : exchanges) {
        // A busy exchange is starting, stopping or answering; show its last published view.
        std::unique_lock exchange_guard(exchange->mutex, std::try_to_lock);
        if (exchange_guard.owns_lock()) {
            if (exchange->deleted) continue;
            exchange->process.reap_if_exited();
            release_feed_and_publish_view(*exchange);
        }
        std::lock_guard guard(mutex_);
        if (!exchange_guard.owns_lock() && !exchanges_.contains(exchange->id)) continue;
        json.raw(exchange->listed_view);
    }
    return response(200, json.end_array().take());
}
HttpResponse ExchangeService::create_exchange(const HttpRequest& request) {
    std::lock_guard guard(mutex_);
    if (exchanges_.size() >= 32) return exchange_error(409, "capacity", "at most 32 exchange definitions");
    auto exchange = std::make_shared<Exchange>();
    exchange->config_text = config_body(request);
    exchange->config = validate(exchange->config_text);
    std::random_device random;
    do {
        std::ostringstream id; id << "exchange-" << std::hex << random() << random();
        exchange->id = id.str();
    } while (exchanges_.contains(exchange->id));
    save(exchange->id, exchange->config_text);
    exchange->listed_view = view(*exchange);
    auto body = exchange->listed_view;
    exchanges_.emplace(exchange->id, std::move(exchange));
    return response(201, std::move(body));
}
HttpResponse ExchangeService::handle_exchange_request(Exchange& exchange, const HttpRequest& request, const std::string& action) {
    if (action.empty()) {
        if (request.method == "GET") return response(200, view(exchange));
        if (request.method != "PUT" && request.method != "DELETE") return exchange_error(405, "method_not_allowed", "use GET, PUT or DELETE");
        if (exchange.process.pid() > 0) return exchange_error(409, "exchange_running", "stop the exchange before updating or deleting it");
        return request.method == "DELETE" ? delete_exchange(exchange) : replace_exchange(exchange, request);
    }
    if (action == "stop" && request.method == "POST") {
        exchange.process.stop();
        return response(200, view(exchange));
    }
    if (action == "start" && request.method == "POST") return start_exchange(exchange);
    if ((action == "status" || action == "connections") && request.method == "GET") return read_exchange_status(exchange, action);
    return exchange_error(404, "not_found", "unknown exchange route or method");
}
HttpResponse ExchangeService::replace_exchange(Exchange& exchange, const HttpRequest& request) {
    auto text = config_body(request);
    auto config = validate(text);
    save(exchange.id, text);
    exchange.config_text = std::move(text);
    exchange.config = std::move(config);
    exchange.process.clear_failure();
    return response(200, view(exchange));
}
HttpResponse ExchangeService::delete_exchange(Exchange& exchange) {
    std::filesystem::remove(directory_ / (exchange.id + ".conf"));
    std::lock_guard guard(mutex_);
    exchange.deleted = true;
    exchanges_.erase(exchange.id);
    return response(204, "");
}
HttpResponse ExchangeService::start_exchange(Exchange& exchange) {
    if (exchange.process.pid() > 0) return response(200, view(exchange));
    {
        std::lock_guard guard(mutex_);
        for (const auto& [other_id, other_feed] : claimed_feeds_) {
            // An exited child keeps its claim until a request on it reaps it.
            if (other_id == exchange.id || exchanges_.find(other_id)->second->process.has_exited()) continue;
            if (feeds_overlap(exchange.config.market_data, other_feed))
                return exchange_error(409, "feed_conflict", "another running exchange uses this market data destination");
        }
        claimed_feeds_[exchange.id] = exchange.config.market_data;
    }
    if (!exchange.process.spawn(executable_, directory_ / (exchange.id + ".conf")))
        return exchange_error(503, "start_failed", exchange.process.error());
    if (!exchange.process.await_readiness()) return exchange_error(409, "start_failed", exchange.process.error());
    return response(200, view(exchange));
}
HttpResponse ExchangeService::read_exchange_status(Exchange& exchange, const std::string& action) {
    if (exchange.process.pid() <= 0)
        return action == "status" ? response(200, view(exchange)) : exchange_error(409, "exchange_stopped", "start the exchange first");
    if (!exchange.process.read_status()) return exchange_error(503, "exchange_unavailable", exchange.process.error());
    if (action == "connections") return response(200, serialize_json_value(*parse_json(exchange.process.snapshot()).find("connections")));
    return response(200, view(exchange));
}
}
