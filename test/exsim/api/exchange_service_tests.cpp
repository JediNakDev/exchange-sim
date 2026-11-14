#include "api/api_json.hpp"
#include "api/exchange_service.hpp"
#include "api/json.hpp"
#include "../../framework/test.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>

using namespace exsim;

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char text[] = "/tmp/exsim-service-XXXXXX";
        const char* result = ::mkdtemp(text);
        REQUIRE(result);
        path = result;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

// Stand-in for exsim_exchanged, selected by the exchange name in its config:
// SLOWSTART exits after 1.5 s without reporting readiness; SLOWSTOP reports
// readiness at once and takes 1.5 s to exit after SIGTERM.
constexpr const char* kSlowExchangeScript = R"(#!/bin/sh
if grep -q 'name = SLOWSTART' "$2"; then sleep 1.5; exit 1; fi
trap 'sleep 1.5; exit 0' TERM
printf '\000\000\000\044{"state":"running","connections":{}}' >&3
while :; do sleep 0.05; done
)";

std::filesystem::path write_slow_exchange_script(const std::filesystem::path& directory) {
    const auto path = directory / "slow_exchanged";
    std::ofstream(path) << kSlowExchangeScript;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}

std::string exchange_config_json(const std::string& name, int market_data_port) {
    const std::string text = "[exchange]\nname = " + name + "\n[market_data]\nport = " + std::to_string(market_data_port) +
                             "\n[instrument AAPL]\ntick_size = 0.01\nmin_price = 0.01\nmax_price = 1000\nreference_price = 150\n"
                             "[user ALICE]\npassword = pw\nsession_id = 1\n";
    api::ApiJsonWriter json;
    return json.begin_object().key("config").string(text).end_object().take();
}

api::HttpResponse api_request(api::ExchangeService& service, std::string method, std::string path, std::string body = {}) {
    api::HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.body = std::move(body);
    return service.handle(request);
}

std::string create_exchange(api::ExchangeService& service, const std::string& name, int market_data_port) {
    const auto created = api_request(service, "POST", "/v1/exchanges", exchange_config_json(name, market_data_port));
    REQUIRE_EQ(created.status, 201);
    return "/v1/exchanges/" + api::parse_json(created.body).find("id")->text;
}

// Requests about other exchanges must not wait for a slow lifecycle call.
void check_other_requests_are_prompt(api::ExchangeService& service, const std::string& other_path) {
    using namespace std::chrono;
    for (const auto& path : {other_path + "/status", other_path, std::string("/v1/exchanges")}) {
        const auto started = steady_clock::now();
        const auto reply = api_request(service, "GET", path);
        const auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - started).count();
        CHECK_EQ(reply.status, 200);
        CHECK_LT(elapsed_ms, 500);
    }
}
}  // namespace

TEST_CASE(api_exchange_service, other_exchange_requests_do_not_wait_for_start_or_stop) {
    using namespace std::chrono;
    TemporaryDirectory scripts, data;
    api::ExchangeService service(data.path, write_slow_exchange_script(scripts.path));
    const auto slow_start = create_exchange(service, "SLOWSTART", 21001);
    const auto slow_stop = create_exchange(service, "SLOWSTOP", 21002);
    const auto idle = create_exchange(service, "IDLE", 21003);

    auto started_at = steady_clock::now();
    auto start = std::async(std::launch::async, [&] { return api_request(service, "POST", slow_start + "/start"); });
    std::this_thread::sleep_for(milliseconds(300));
    check_other_requests_are_prompt(service, idle);
    const auto listing = api::parse_json(api_request(service, "GET", "/v1/exchanges").body);
    CHECK_EQ(listing.items.size(), 3u);
    const auto failed_start = start.get();
    CHECK_GE(duration_cast<milliseconds>(steady_clock::now() - started_at).count(), 1000);
    CHECK_EQ(failed_start.status, 409);
    CHECK_EQ(api::parse_json(api_request(service, "GET", slow_start).body).find("state")->text, "failed");

    const auto running = api_request(service, "POST", slow_stop + "/start");
    REQUIRE_EQ(running.status, 200);
    CHECK_EQ(api::parse_json(running.body).find("state")->text, "running");
    started_at = steady_clock::now();
    auto stop = std::async(std::launch::async, [&] { return api_request(service, "POST", slow_stop + "/stop"); });
    std::this_thread::sleep_for(milliseconds(300));
    check_other_requests_are_prompt(service, idle);
    const auto stopped = stop.get();
    CHECK_GE(duration_cast<milliseconds>(steady_clock::now() - started_at).count(), 1000);
    CHECK_EQ(stopped.status, 200);
    const auto stopped_view = api::parse_json(stopped.body);
    CHECK_EQ(stopped_view.find("state")->text, "stopped");
    CHECK_EQ(stopped_view.find("pid")->kind, api::JsonValue::Kind::Null);
}
