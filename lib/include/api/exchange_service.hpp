#ifndef EXSIM_API_EXCHANGE_SERVICE_HPP
#define EXSIM_API_EXCHANGE_SERVICE_HPP
#include "api/exchange_process.hpp"
#include "api/http_message.hpp"
#include "net/socket.hpp"
#include "server/config.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
namespace exsim::api {
HttpResponse exchange_error(int status, std::string_view code, std::string_view message);
// Handles management requests from concurrent HTTP connection threads.
// mutex_ guards the exchange map and feed claims and is held only briefly.
// Each exchange has its own mutex, held for a whole request on that exchange
// (including start and stop waits), so a slow exchange delays only requests
// about itself. Listing never waits for a busy exchange: it shows the view
// published when that exchange's last request finished.
class ExchangeService {
public:
    ExchangeService(std::filesystem::path directory, std::filesystem::path executable);
    ~ExchangeService();
    HttpResponse handle(const HttpRequest& request);
private:
    struct Exchange {
        std::string id, config_text;
        server::ServerConfig config;
        ExchangeProcess process;
        std::mutex mutex;
        bool deleted = false;      // guarded by mutex
        std::string listed_view;   // guarded by the service mutex_
    };
    HttpResponse list_exchanges();
    HttpResponse create_exchange(const HttpRequest& request);
    HttpResponse handle_exchange_request(Exchange& exchange, const HttpRequest& request, const std::string& action);
    HttpResponse replace_exchange(Exchange& exchange, const HttpRequest& request);
    HttpResponse delete_exchange(Exchange& exchange);
    HttpResponse start_exchange(Exchange& exchange);
    HttpResponse read_exchange_status(Exchange& exchange, const std::string& action);
    // Called with exchange.mutex held at the end of every request on it.
    void release_feed_and_publish_view(Exchange& exchange);
    void save(const std::string& id, std::string_view config_text);
    std::string view(const Exchange& exchange) const;
    std::filesystem::path directory_, executable_;
    net::Socket lock_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Exchange>> exchanges_;
    // Market data destinations of exchanges whose process may be running.
    std::map<std::string, server::MarketDataSection> claimed_feeds_;
};
}
#endif
