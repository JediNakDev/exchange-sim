#include "runtime.hpp"
#include "exsim/api/worker_process.hpp"
#include "exsim/api/research_service.hpp"
#include "exsim/api/api_json.hpp"
#include <csignal>
#include <iostream>
#include <unistd.h>

namespace exsim::api {
namespace {
std::string text(const research::JsonValue& object, const char* name) {
    const auto* value = object.find(name);
    if (!value || value->kind != research::JsonValue::Kind::String) throw std::runtime_error("invalid worker bootstrap or request");
    return value->text;
}
void reply(int socket, ResearchService& service, const HttpResponse& response) {
    const auto [record, view] = service.worker_snapshot();
    ApiJsonWriter json;
    json.begin_object().key("status").integer_number(response.status).key("body").string(response.body)
        .key("record").raw(record).key("runView").raw(view).end_object();
    write_worker_frame(socket, json.take());
}
void reply(int socket, const ResearchService::WorkerResponse& response) {
    ApiJsonWriter json;
    json.begin_object().key("status").integer_number(response.response.status).key("body").string(response.response.body)
        .key("record").raw(response.record).key("runView").raw(response.run_view).end_object();
    write_worker_frame(socket, json.take());
}
}

int run_worker_process(int socket, const std::string& executable) {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        const auto message = read_worker_frame(socket);
        const auto bootstrap = research::parse_json(message);
        ResearchServiceOptions options;
        options.executable_path = text(bootstrap, "apiExecutable");
        if (std::filesystem::canonical(std::filesystem::path(options.executable_path).parent_path() / "exsim_exchanged") !=
            std::filesystem::canonical(executable))
            throw std::runtime_error("exchange worker does not belong to the API release directory");
        options.data_directory = text(bootstrap, "root");
        if (bootstrap.find("query")) options.worker_query_json = message;
        else {
            options.worker_run_id = text(bootstrap, "runId");
            options.worker_config_json = serialize_json_value(*bootstrap.find("config"));
            options.worker_record_json = serialize_json_value(*bootstrap.find("record"));
            options.source_revision = text(bootstrap, "sourceRevision");
            options.release_id = text(bootstrap, "releaseId");
        }
        ResearchService service(options);
        try {
            reply(socket, service, HttpResponse{});
            for (;;) {
                const auto request_json = research::parse_json(read_worker_frame(socket, true));
                if (request_json.find("shutdown")) {
                    service.shutdown();
                    reply(socket, service, HttpResponse{});
                    break;
                }
                const auto method = text(request_json, "method");
                const auto target = text(request_json, "target");
                const auto body = text(request_json, "body");
                const auto raw = method + " " + target + " HTTP/1.1\r\nHost: worker\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body;
                HttpRequest request;
                if (parse_http_request(raw, HttpLimits{}, request).status != HttpParseStatus::Complete)
                    throw std::runtime_error("invalid worker HTTP request");
                reply(socket, service.handle_worker_request(request));
            }
        } catch (...) {
            // Parent death/timeout closes the private socket. Stop locally;
            // simulation steps never inspect or wait on that socket.
            service.shutdown();
        }
    } catch (const std::exception& error) {
        std::cerr << "exsim_worker: " << error.what() << "\n";
        ::close(socket);
        return 1;
    }
    ::close(socket);
    return 0;
}

} // namespace exsim::api
