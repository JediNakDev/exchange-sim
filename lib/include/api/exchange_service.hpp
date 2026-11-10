#ifndef EXSIM_API_EXCHANGE_SERVICE_HPP
#define EXSIM_API_EXCHANGE_SERVICE_HPP
#include "api/http_message.hpp"
#include "net/socket.hpp"
#include "server/config.hpp"
#include <filesystem>
#include <map>
#include <mutex>
#include <sys/types.h>
namespace exsim::api {
HttpResponse exchange_error(int status, std::string_view code, std::string_view message);
class ExchangeService {
public:
    ExchangeService(std::filesystem::path directory, std::filesystem::path executable);
    ~ExchangeService();
    HttpResponse handle(const HttpRequest& request);
private:
    struct Exchange {
        std::string id, config_text, state = "stopped", error, snapshot;
        server::ServerConfig config;
        pid_t pid = -1;
        net::Socket control;
    };
    void refresh(Exchange& exchange);
    void stop(Exchange& exchange) noexcept;
    void save(const Exchange& exchange);
    std::string view(const Exchange& exchange) const;
    std::filesystem::path directory_, executable_;
    net::Socket lock_;
    std::map<std::string, Exchange> exchanges_;
    std::mutex mutex_;
};
}
#endif
