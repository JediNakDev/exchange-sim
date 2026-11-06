// Real temporary-directory restarts and filesystem failures through public APIs.
#include "exsim/api/api_json.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/api/research_service.hpp"
#include "exsim/api/sha256.hpp"
#include "exsim/research/run_artifacts.hpp"
#include "tests/framework/test.hpp"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <filesystem>
#include <fstream>

namespace {
using namespace exsim::api;
using exsim::research::parse_json;

struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "exsim-durability-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

ResearchServiceOptions options(const Directory& directory, std::size_t capacity = 100'000) {
    ResearchServiceOptions result;
    result.data_directory = directory.path;
    result.start_executor = false;
    result.max_idempotency_keys = capacity;
    return result;
}

HttpResponse send(ResearchService& service, std::string_view method, std::string_view path,
                  std::string_view body = {}, std::string_view key = "durability-key") {
    const std::string raw = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: test\r\nIdempotency-Key: " +
        std::string(key) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
    HttpRequest request;
    if (parse_http_request(raw, HttpLimits{}, request).status != HttpParseStatus::Complete)
        throw std::runtime_error("bad test request");
    return service.handle(request);
}

std::string experiment_body() {
    const auto& baseline = baseline_configurations().front();
    return R"({"name":"durability","description":"","derivedFromBaselineId":")" + baseline.baseline_id +
           R"(","config":)" + baseline.config_json + "}";
}

std::size_t experiment_count(ResearchService& service) {
    return parse_json(send(service, "GET", "/v1/experiments").body).find("items")->items.size();
}

void write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    stream.close();
    if (!stream) throw std::runtime_error("test write failed");
}

// Isolate the old loader's null dereference so corruption cannot kill the suite.
bool startup_rejects(const Directory& directory, std::size_t capacity = 100'000) {
    const pid_t child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        try { ResearchService service(options(directory, capacity)); }
        catch (const std::exception&) { ::_exit(0); }
        ::_exit(2);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child) throw std::runtime_error("waitpid failed");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}  // namespace

TEST_CASE(api_durability, unavailable_store_rejects_before_creation_and_retry_after_restart_creates_once) {
    Directory directory;
    const std::string body = experiment_body();
    {
        ResearchService service(options(directory));
        // Obstruct both the legacy and new store. No permission assumptions.
        std::filesystem::remove_all(directory.path / "idempotency");
        write(directory.path / "idempotency", "blocked");
        std::filesystem::create_directory(directory.path / "idempotency.jsonl");
        CHECK_EQ(send(service, "POST", "/v1/experiments", body).status, 503);
        CHECK_EQ(experiment_count(service), 0u);
        std::filesystem::remove(directory.path / "idempotency");
        std::filesystem::remove(directory.path / "idempotency.jsonl");
    }
    ResearchService restarted(options(directory));
    const auto created = send(restarted, "POST", "/v1/experiments", body);
    CHECK_EQ(created.status, 201);
    CHECK_EQ(experiment_count(restarted), 1u);
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", body).body, created.body);
}

TEST_CASE(api_durability, response_write_failure_after_creation_blocks_duplicate_after_abrupt_restart) {
    Directory probe_directory;
    std::uintmax_t resource_size = 0;
    {
        ResearchService probe(options(probe_directory));
        REQUIRE_EQ(send(probe, "POST", "/v1/experiments", experiment_body()).status, 201);
        resource_size = std::filesystem::file_size(std::filesystem::directory_iterator(probe_directory.path / "experiments")->path());
    }
    Directory directory;
    int pipe_fds[2];
    REQUIRE_EQ(::pipe(pipe_fds), 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::close(pipe_fds[0]);
        ResearchService service(options(directory));
        ::signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit {};
        if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(3);
        limit.rlim_cur = static_cast<rlim_t>(resource_size + 64);
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(4);
        const int status = send(service, "POST", "/v1/experiments", experiment_body()).status;
        (void)::write(pipe_fds[1], &status, sizeof(status));
        ::_exit(0);  // abrupt process loss, no service destructor
    }
    ::close(pipe_fds[1]);
    int response_status = 0;
    const auto received = ::read(pipe_fds[0], &response_status, sizeof(response_status));
    ::close(pipe_fds[0]);
    int status = 0;
    REQUIRE_EQ(::waitpid(child, &status, 0), child);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    REQUIRE_EQ(received, static_cast<ssize_t>(sizeof(response_status)));
    CHECK_EQ(response_status, 503);
    // fork copied the identifier generator. Advance the parent's copy so an
    // unfixed retry cannot hide duplication by overwriting the child's same ID.
    {
        ResearchService probe(options(probe_directory));
        REQUIRE_EQ(send(probe, "POST", "/v1/experiments", experiment_body(), "advance-parent-key").status, 201);
    }
    ResearchService restarted(options(directory, 1));
    CHECK_EQ(experiment_count(restarted), 1u); // resource committed before injected failure
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body()).status, 503);
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", "{}").status, 409);
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body(), "another-key").status, 503);
    CHECK_EQ(experiment_count(restarted), 1u);
}

TEST_CASE(api_durability, mutation_persistence_failure_retains_reservation_across_restart) {
    Directory directory;
    {
        ResearchService service(options(directory));
        std::filesystem::remove(directory.path / "experiments");
        write(directory.path / "experiments", "blocked");
        CHECK_EQ(send(service, "POST", "/v1/experiments", experiment_body()).status, 503);
        std::filesystem::remove(directory.path / "experiments");
    }
    ResearchService restarted(options(directory));
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body()).status, 503);
    CHECK_EQ(experiment_count(restarted), 0u);
}

TEST_CASE(api_durability, successful_response_replays_verbatim_after_restart) {
    Directory directory;
    std::string response;
    {
        ResearchService service(options(directory));
        const auto created = send(service, "POST", "/v1/experiments", experiment_body());
        REQUIRE_EQ(created.status, 201);
        response = created.body;
    }
    ResearchService restarted(options(directory, 1));
    const auto replay = send(restarted, "POST", "/v1/experiments", experiment_body());
    CHECK_EQ(replay.status, 201);
    CHECK_EQ(replay.body, response);
    CHECK_EQ(experiment_count(restarted), 1u);
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body(), "another-key").status, 503);
}

TEST_CASE(api_durability, legacy_successes_replay_without_rewriting_user_data) {
    Directory directory;
    std::string response;
    {
        ResearchService service(options(directory));
        response = send(service, "POST", "/v1/experiments", experiment_body()).body;
    }
    std::filesystem::remove_all(directory.path / "idempotency");
    ApiJsonWriter record;
    record.begin_object().key("key").string("durability-key").key("fingerprint")
        .string(sha256_hex("/v1/experiments\n" + stable_serialize_json_value(parse_json(experiment_body()))))
        .key("status").integer_number(201).key("body").string(response).end_object();
    const std::string legacy = record.take() + "\n";
    write(directory.path / "idempotency.jsonl", legacy);
    ResearchService restarted(options(directory, 1));
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body()).body, response);
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", "{}").status, 409);
    CHECK_EQ(exsim::research::read_text_file(directory.path / "idempotency.jsonl"), legacy);
}

TEST_CASE(api_durability, invalid_requests_release_reservations) {
    Directory directory;
    {
        ResearchService service(options(directory, 1));
        CHECK_EQ(send(service, "POST", "/v1/experiments", "{}").status, 422);
    }
    ResearchService restarted(options(directory, 1));
    CHECK_EQ(send(restarted, "POST", "/v1/experiments", experiment_body()).status, 201);
}

TEST_CASE(api_durability, malformed_experiment_is_rejected_without_changing_bytes) {
    Directory directory;
    { ResearchService service(options(directory)); }
    const auto path = directory.path / "experiments/exp_bad.json";
    write(path, "{}");
    CHECK(startup_rejects(directory));
    CHECK_EQ(exsim::research::read_text_file(path), std::string("{}"));
}

TEST_CASE(api_durability, malformed_run_is_rejected_without_changing_bytes) {
    Directory directory;
    { ResearchService service(options(directory)); }
    std::filesystem::create_directory(directory.path / "runs/run_bad");
    const auto path = directory.path / "runs/run_bad/run.json";
    write(path, "{}");
    CHECK(startup_rejects(directory));
    CHECK_EQ(exsim::research::read_text_file(path), std::string("{}"));
}

TEST_CASE(api_durability, corrupt_legacy_records_including_torn_tail_fail_closed) {
    for (const std::string_view corrupt : {"not JSON\n", "{}\n", "{\"key\":5}\n", "{\"key\":\"torn"}) {
        Directory directory;
        { ResearchService service(options(directory)); }
        const auto path = directory.path / "idempotency.jsonl";
        write(path, corrupt);
        CHECK(startup_rejects(directory));
        CHECK_EQ(exsim::research::read_text_file(path), std::string(corrupt));
    }
}

TEST_CASE(api_durability, restart_does_not_silently_exceed_configured_key_capacity) {
    Directory directory;
    {
        ResearchService service(options(directory));
        REQUIRE_EQ(send(service, "POST", "/v1/experiments", experiment_body()).status, 201);
        REQUIRE_EQ(send(service, "POST", "/v1/experiments", experiment_body(), "another-key").status, 201);
    }
    CHECK(startup_rejects(directory, 1));
    ResearchService restarted(options(directory, 2));
    CHECK_EQ(experiment_count(restarted), 2u);
}

TEST_CASE(api_durability, corrupt_or_misnamed_new_records_fail_closed) {
    for (const bool corrupt_body : {false, true}) {
        Directory directory;
        {
            ResearchService service(options(directory));
            REQUIRE_EQ(send(service, "POST", "/v1/experiments", experiment_body()).status, 201);
        }
        const auto path = directory.path / "idempotency" / (sha256_hex("durability-key") + ".json");
        // On the pre-fix source only JSONL exists. Create the new-format corrupt
        // record explicitly so that this also demonstrates ignored state there.
        std::filesystem::create_directories(path.parent_path());
        if (corrupt_body) write(path, "{}");
        else {
            ApiJsonWriter record;
            record.begin_object().key("schema").string("exsim.api.idempotency-record/1")
                .key("key").string("different-key").key("fingerprint").string(std::string(64, 'a'))
                .key("status").integer_number(0).key("body").string("").end_object();
            write(path, record.text());
        }
        const auto before = exsim::research::read_text_file(path);
        CHECK(startup_rejects(directory));
        CHECK_EQ(exsim::research::read_text_file(path), before);
    }
}

TEST_CASE(api_durability, successful_run_creation_replays_without_creating_another_run_after_restart) {
    Directory directory;
    std::string request;
    std::string original;
    {
        ResearchService service(options(directory));
        const auto experiment = parse_json(send(service, "POST", "/v1/experiments", experiment_body()).body);
        request = R"({"experimentId":")" + experiment.find("experimentId")->text + R"(","masterSeed":"1","startPaused":true})";
        const auto created = send(service, "POST", "/v1/runs", request, "run-create-key");
        REQUIRE_EQ(created.status, 201);
        original = created.body;
    }
    ResearchService restarted(options(directory));
    const auto replay = send(restarted, "POST", "/v1/runs", request, "run-create-key");
    CHECK_EQ(replay.status, 201);
    CHECK_EQ(replay.body, original);
    CHECK_EQ(parse_json(send(restarted, "GET", "/v1/runs").body).find("items")->items.size(), 1u);
}

TEST_CASE(api_durability, missing_terminal_cache_fails_startup_without_rewriting_completed_record) {
    Directory directory;
    std::filesystem::path run_directory;
    {
        ResearchService service(options(directory));
        std::string body = experiment_body();
        const auto at = body.find(R"("endSimulationTimeNs":"60000000000")");
        REQUIRE(at != std::string::npos);
        body.replace(at, std::string(R"("endSimulationTimeNs":"60000000000")").size(), R"("endSimulationTimeNs":"200000000")");
        const auto experiment = parse_json(send(service, "POST", "/v1/experiments", body).body);
        const std::string request = R"({"experimentId":")" + experiment.find("experimentId")->text + R"(","masterSeed":"1"})";
        const auto created = send(service, "POST", "/v1/runs", request, "run-create-key");
        REQUIRE_EQ(created.status, 201);
        const auto run_id = parse_json(created.body).find("runId")->text;
        run_directory = directory.path / "runs" / run_id;
        for (int pass = 0; pass < 1000 && service.run_executor_pass(); ++pass) {}
        REQUIRE_EQ(parse_json(send(service, "GET", "/v1/runs/" + run_id).body).find("state")->text, std::string("completed"));
    }
    const auto path = run_directory / "run.json";
    const auto before = exsim::research::read_text_file(path);
    REQUIRE(std::filesystem::remove(run_directory / "events.bin"));
    CHECK(startup_rejects(directory));
    CHECK_EQ(exsim::research::read_text_file(path), before);
}

TEST_CASE(api_durability, successful_response_with_missing_resource_fails_startup) {
    Directory directory;
    {
        ResearchService service(options(directory));
        REQUIRE_EQ(send(service, "POST", "/v1/experiments", experiment_body()).status, 201);
    }
    std::filesystem::remove_all(directory.path / "experiments");
    CHECK(startup_rejects(directory));
}

TEST_CASE(api_durability, swallowed_step_persistence_failure_is_not_saved_as_a_success) {
    Directory directory;
    std::string step_path;
    {
        ResearchService service(options(directory));
        const auto experiment = parse_json(send(service, "POST", "/v1/experiments", experiment_body()).body);
        const std::string request = R"({"experimentId":")" + experiment.find("experimentId")->text + R"(","masterSeed":"1","startPaused":true})";
        const auto created = send(service, "POST", "/v1/runs", request, "run-create-key");
        REQUIRE_EQ(created.status, 201);
        const auto run_id = parse_json(created.body).find("runId")->text;
        service.run_executor_pass();
        step_path = "/v1/runs/" + run_id + "/step";
        const auto temporary = directory.path / "runs" / run_id / "run.json.tmp";
        std::filesystem::create_directory(temporary);
        const auto first = send(service, "POST", step_path, R"({"canonicalEventCount":10})", "step-request-key");
        CHECK_EQ(first.status, 503);
        const auto progress = send(service, "GET", "/v1/runs/" + run_id).body;
        CHECK_EQ(send(service, "POST", step_path, R"({"canonicalEventCount":10})", "step-request-key").status, 503);
        CHECK_EQ(send(service, "GET", "/v1/runs/" + run_id).body, progress);
        std::filesystem::remove(temporary);
    }
    ResearchService restarted(options(directory));
    CHECK_EQ(send(restarted, "POST", step_path, R"({"canonicalEventCount":10})", "step-request-key").status, 503);
}

TEST_CASE(api_durability, terminal_write_failure_is_explicit_and_keeps_diagnostics_across_restarts) {
    Directory directory;
    std::filesystem::path run_directory;
    std::string cancel_path;
    std::string failed_record;
    std::string diagnostics;
    {
        ResearchService service(options(directory));
        const auto experiment = parse_json(send(service, "POST", "/v1/experiments", experiment_body()).body);
        const std::string request = R"({"experimentId":")" + experiment.find("experimentId")->text + R"(","masterSeed":"1","startPaused":true})";
        const auto created = send(service, "POST", "/v1/runs", request, "run-create-key");
        REQUIRE_EQ(created.status, 201);
        const auto run_id = parse_json(created.body).find("runId")->text;
        run_directory = directory.path / "runs" / run_id;
        service.run_executor_pass();
        std::filesystem::create_directory(run_directory / "events.bin.tmp");
        cancel_path = "/v1/runs/" + run_id + "/cancel";
        CHECK_EQ(send(service, "POST", cancel_path, "{}", "cancel-request-key").status, 503);
        const auto run = parse_json(send(service, "GET", "/v1/runs/" + run_id).body);
        CHECK_EQ(run.find("state")->text, std::string("failed"));
        REQUIRE(run.find("terminalDiagnostics") != nullptr);
        diagnostics = serialize_json_value(*run.find("terminalDiagnostics"));
        CHECK(diagnostics.find("could not be persisted") != std::string::npos);
        failed_record = exsim::research::read_text_file(run_directory / "run.json");
        std::filesystem::remove(run_directory / "events.bin.tmp");
    }
    for (int restart = 0; restart < 2; ++restart) {
        ResearchService restarted(options(directory));
        CHECK_EQ(send(restarted, "POST", cancel_path, "{}", "cancel-request-key").status, 503);
        const auto run = parse_json(send(restarted, "GET", cancel_path.substr(0, cancel_path.size() - 7)).body);
        CHECK_EQ(run.find("state")->text, std::string("failed"));
        CHECK_EQ(serialize_json_value(*run.find("terminalDiagnostics")), diagnostics);
        CHECK_EQ(exsim::research::read_text_file(run_directory / "run.json"), failed_record);
    }
}

TEST_CASE(api_durability, executor_terminal_write_failure_does_not_claim_completion) {
    Directory directory;
    ResearchService service(options(directory));
    std::string body = experiment_body();
    const auto at = body.find(R"("endSimulationTimeNs":"60000000000")");
    REQUIRE(at != std::string::npos);
    body.replace(at, std::string(R"("endSimulationTimeNs":"60000000000")").size(), R"("endSimulationTimeNs":"200000000")");
    const auto experiment = parse_json(send(service, "POST", "/v1/experiments", body).body);
    const std::string request = R"({"experimentId":")" + experiment.find("experimentId")->text + R"(","masterSeed":"1"})";
    const auto created = send(service, "POST", "/v1/runs", request, "run-create-key");
    REQUIRE_EQ(created.status, 201);
    const auto run_id = parse_json(created.body).find("runId")->text;
    const auto temporary = directory.path / "runs" / run_id / "events.bin.tmp";
    std::filesystem::create_directory(temporary);
    bool reported = false;
    try {
        for (int pass = 0; pass < 1000 && service.run_executor_pass(); ++pass) {}
    } catch (const exsim::research::ArtifactError&) { reported = true; }
    CHECK(reported);
    const auto run = parse_json(send(service, "GET", "/v1/runs/" + run_id).body);
    CHECK_EQ(run.find("state")->text, std::string("failed"));
    CHECK_EQ(run.find("terminalDiagnostics")->find("outcomeCode")->text, std::string("failed_internal_error"));
    CHECK(run.find("terminalDiagnostics")->find("message")->text.find("could not be persisted") != std::string::npos);
    std::filesystem::remove(temporary);
}
