#include "api/exchange_service.hpp"
#include "api/api_json.hpp"
#include "ipc/exchange_control.hpp"
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <random>
#include <spawn.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
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
void reap(pid_t pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
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
        Exchange exchange;
        exchange.id = entry.path().stem().string();
        exchange.config_text = read_file(entry.path());
        exchange.config = validate(exchange.config_text);
        exchanges_.emplace(exchange.id, std::move(exchange));
    }
}
ExchangeService::~ExchangeService() {
    // Let every child drain concurrently before waiting for individual exits.
    for (auto& [id, exchange] : exchanges_) { (void)id; if (exchange.pid > 0) ::kill(exchange.pid, SIGTERM); }
    for (auto& [id, exchange] : exchanges_) { (void)id; stop(exchange); }
}
void ExchangeService::save(const Exchange& exchange) {
    const auto final = directory_ / (exchange.id + ".conf");
    const auto temporary = directory_ / (exchange.id + ".tmp");
    net::Socket file(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!file.is_open()) throw std::runtime_error("cannot create config file");
    try {
        std::size_t offset = 0;
        while (offset < exchange.config_text.size()) {
            const auto written = ::write(file.fd(), exchange.config_text.data() + offset, exchange.config_text.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("cannot write config file");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file.fd()) != 0) throw std::runtime_error("cannot flush config file");
        std::filesystem::rename(temporary, final);
    } catch (...) { std::filesystem::remove(temporary); throw; }
}
void ExchangeService::refresh(Exchange& exchange) {
    if (exchange.pid <= 0) return;
    int status = 0;
    const auto result = ::waitpid(exchange.pid, &status, WNOHANG);
    if (result == exchange.pid || (result < 0 && errno == ECHILD)) {
        exchange.pid = -1;
        exchange.control.close();
        exchange.state = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "stopped" : "failed";
        exchange.error = exchange.state == "failed" ? "exchange process exited unexpectedly" : "";
        exchange.snapshot.clear();
    }
}
void ExchangeService::stop(Exchange& exchange) noexcept {
    if (exchange.pid <= 0) return;
    ::kill(exchange.pid, SIGTERM);
    for (int i = 0; i < 450; ++i) {
        int status = 0;
        const auto result = ::waitpid(exchange.pid, &status, WNOHANG);
        if (result == exchange.pid || (result < 0 && errno == ECHILD)) {
            exchange.pid = -1;
            exchange.control.close();
            exchange.state = "stopped";
            exchange.snapshot.clear();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(exchange.pid, SIGKILL);
    reap(exchange.pid);
    exchange.pid = -1;
    exchange.control.close();
    exchange.state = "stopped";
    exchange.snapshot.clear();
}
std::string ExchangeService::view(const Exchange& exchange) const {
    ApiJsonWriter json;
    json.begin_object().key("id").string(exchange.id).key("name").string(exchange.config.exchange.name)
        .key("state").string(exchange.state).key("pid");
    if (exchange.pid > 0) json.integer_number(exchange.pid); else json.null();
    json.key("error").string(exchange.error).key("config").string(redacted(exchange.config_text))
        .key("runtime");
    if (exchange.snapshot.empty()) json.null(); else json.raw(exchange.snapshot);
    return json.end_object().take();
}
HttpResponse ExchangeService::handle(const HttpRequest& request) {
    std::lock_guard guard(mutex_);
    try {
        for (auto& [id, exchange] : exchanges_) { (void)id; refresh(exchange); }
        if (request.path == "/v1/service" && request.method == "GET")
            return response(200, R"({"product":"exsim","apiVersion":"v1","profile":"ouch42_soupbintcp_itch50_moldudp64","orderEntryProtocols":["ouch42_soupbintcp","fix44","ilink3"],"marketDataProtocols":["itch50_moldudp64","mdp3_sbe"]})");
        const std::string root = "/v1/exchanges";
        if (request.path == root) {
            if (request.method == "GET") {
                ApiJsonWriter json; json.begin_array();
                for (const auto& [id, exchange] : exchanges_) { (void)id; json.raw(view(exchange)); }
                return response(200, json.end_array().take());
            }
            if (request.method == "POST") {
                if (exchanges_.size() >= 32) return exchange_error(409, "capacity", "at most 32 exchange definitions");
                Exchange exchange;
                exchange.config_text = config_body(request);
                exchange.config = validate(exchange.config_text);
                std::random_device random;
                do {
                    std::ostringstream id; id << "exchange-" << std::hex << random() << random();
                    exchange.id = id.str();
                } while (exchanges_.contains(exchange.id));
                save(exchange);
                const auto body = view(exchange);
                exchanges_.emplace(exchange.id, std::move(exchange));
                return response(201, body);
            }
            return exchange_error(405, "method_not_allowed", "use GET or POST");
        }
        if (!request.path.starts_with(root + "/")) return exchange_error(404, "not_found", "unknown route");
        const auto tail = request.path.substr(root.size() + 1);
        const auto slash = tail.find('/');
        const auto id = tail.substr(0, slash);
        const auto action = slash == std::string::npos ? "" : tail.substr(slash + 1);
        auto found = exchanges_.find(id);
        if (found == exchanges_.end()) return exchange_error(404, "not_found", "unknown exchange");
        auto& exchange = found->second;
        if (action.empty()) {
            if (request.method == "GET") return response(200, view(exchange));
            if (request.method != "PUT" && request.method != "DELETE") return exchange_error(405, "method_not_allowed", "use GET, PUT or DELETE");
            if (exchange.pid > 0) return exchange_error(409, "exchange_running", "stop the exchange before updating or deleting it");
            if (request.method == "DELETE") {
                std::filesystem::remove(directory_ / (id + ".conf"));
                exchanges_.erase(found);
                return response(204, "");
            }
            const auto text = config_body(request);
            auto config = validate(text);
            Exchange replacement;
            replacement.id = id; replacement.config_text = text; replacement.config = std::move(config);
            save(replacement);
            exchange = std::move(replacement);
            return response(200, view(exchange));
        }
        if (action == "stop" && request.method == "POST") { stop(exchange); return response(200, view(exchange)); }
        if (action == "start" && request.method == "POST") {
            if (exchange.pid > 0) return response(200, view(exchange));
            for (const auto& [other_id, other] : exchanges_) {
                if (other_id == id || other.pid <= 0 || other.config.market_data.address != exchange.config.market_data.address) continue;
                for (const auto port : feed_ports(exchange.config.market_data))
                    for (const auto other_port : feed_ports(other.config.market_data))
                        if (port && port == other_port)
                            return exchange_error(409, "feed_conflict", "another running exchange uses this market data destination");
            }
            int descriptors[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) throw std::runtime_error("cannot create control socket");
            net::Socket parent(descriptors[0]), child(descriptors[1]);
            (void)net::set_no_sigpipe(parent.fd());
            (void)net::set_non_blocking(parent.fd());
            ::fcntl(parent.fd(), F_SETFD, FD_CLOEXEC);
            ::fcntl(child.fd(), F_SETFD, FD_CLOEXEC);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, child.fd(), 3);
#ifdef __linux__
            posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#endif
            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
            sigset_t mask; sigemptyset(&mask);
            posix_spawnattr_setsigmask(&attributes, &mask);
            short flags = POSIX_SPAWN_SETSIGMASK;
#ifdef __APPLE__
            flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
            posix_spawnattr_setflags(&attributes, flags);
            const std::string binary = executable_.string(), config_path = (directory_ / (id + ".conf")).string();
            char* arguments[] = {const_cast<char*>(binary.c_str()), const_cast<char*>("--config"), const_cast<char*>(config_path.c_str()),
                                 const_cast<char*>("--control-fd"), const_cast<char*>("3"), nullptr};
            const int result = ::posix_spawn(&exchange.pid, binary.c_str(), &actions, &attributes, arguments, environ);
            posix_spawnattr_destroy(&attributes);
            posix_spawn_file_actions_destroy(&actions);
            child.close();
            if (result != 0) { exchange.pid = -1; exchange.state = "failed"; exchange.error = std::strerror(result); return exchange_error(503, "start_failed", exchange.error); }
            exchange.control = std::move(parent);
            try {
                exchange.snapshot = ipc::receive_control(exchange.control.fd(), 10000);
                const auto ready = parse_json(exchange.snapshot);
                if (ready.find("error")) throw std::runtime_error(ready.find("error")->text);
                if (!ready.find("state") || ready.find("state")->text != "running") throw std::runtime_error("exchange did not report readiness");
                exchange.state = "running"; exchange.error.clear();
            } catch (const std::exception& error) {
                const std::string message = error.what();
                stop(exchange); exchange.state = "failed"; exchange.error = message;
                return exchange_error(409, "start_failed", message);
            }
            return response(200, view(exchange));
        }
        if ((action == "status" || action == "connections") && request.method == "GET") {
            if (exchange.pid <= 0) return action == "status" ? response(200, view(exchange)) : exchange_error(409, "exchange_stopped", "start the exchange first");
            try {
                ipc::send_control(exchange.control.fd(), "status");
                exchange.snapshot = ipc::receive_control(exchange.control.fd());
                const auto snapshot = parse_json(exchange.snapshot);
                if (action == "connections") return response(200, serialize_json_value(*snapshot.find("connections")));
            } catch (const std::exception&) {
                stop(exchange); exchange.state = "failed"; exchange.error = "exchange control failed";
                return exchange_error(503, "exchange_unavailable", exchange.error);
            }
            return response(200, view(exchange));
        }
        return exchange_error(404, "not_found", "unknown exchange route or method");
    } catch (const JsonError& error) { return exchange_error(400, "invalid_json", error.what()); }
      catch (const std::invalid_argument& error) { return exchange_error(400, "invalid_config", error.what()); }
      catch (const std::exception&) { return exchange_error(500, "internal_error", "exchange management operation failed"); }
}
}
