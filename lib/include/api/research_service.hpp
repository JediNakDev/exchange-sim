#ifndef EXSIM_API_RESEARCH_SERVICE_HPP
#define EXSIM_API_RESEARCH_SERVICE_HPP

// The research HTTP API (HTTP API contract, config/api/openapi.yaml): routing,
// experiments, idempotency, the run executor and comparisons.
//
// The CLI isolates each active run in its own process. The API supervises
// control/status exchanges and remains the sole metadata writer. Each worker's
// local executor advances scheduler slices independently of IPC. Library tests
// may instead drive in-process runs explicitly. Controls acknowledge their
// event-boundary effects synchronously. See historical design notes.

#include "api/http_message.hpp"
#include "api/research_run.hpp"
#include "api/postgres_store.hpp"
#include "api/worker_process.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace exsim::api {

inline constexpr std::string_view kApiSchemaVersion = "exsim.api/1";

struct ResearchServiceOptions {
    std::filesystem::path data_directory = "exsim-api-data";
    std::size_t max_live_simulations = 8;
    // Includes reservations for mutations interrupted before their response was saved.
    std::size_t max_idempotency_keys = 100'000;
    std::uint64_t executor_slice_steps = 2048;
    std::string service_build_id = "exsim_api/1";
    std::string executable_path;
    std::string source_revision; // supplied build metadata; empty means unavailable
    std::string release_id;
    bool start_executor = true;  // tests drive the executor by hand when false
    std::string database_url; // empty selects the legacy file store
    bool import_file_metadata = false; // explicit, one-time import into an empty database
    bool isolate_runs = false; // CLI defaults to isolation; library tests may drive in-process runs
    // Internal inherited-socket worker bootstrap, never exposed as an HTTP input.
    std::string worker_run_id;
    std::string worker_config_json;
    std::string worker_record_json;
    std::string worker_query_json;
};

class ResearchService {
public:
    explicit ResearchService(ResearchServiceOptions options);
    ~ResearchService();
    ResearchService(const ResearchService&) = delete;
    ResearchService& operator=(const ResearchService&) = delete;

    // Thread safe. Mutations reserve a persisted key before execution. An
    // uncertain outcome returns 503 and keeps the key reserved across restarts.
    // Successful responses replay verbatim; legacy JSONL responses remain readable.
    [[nodiscard]] HttpResponse handle(const HttpRequest& request);
    // Error response for a request the HTTP layer rejected before routing.
    [[nodiscard]] HttpResponse rejection_response(int http_status, std::string_view code, std::string_view message);

    // Stops the executor; live runs fail with a recorded reason.
    void shutdown();
    // One executor pass (activations plus one slice per running run).
    // Returns true when some run is still running. Used by tests.
    bool run_executor_pass();
    std::pair<std::string, std::string> worker_snapshot() const;
    struct WorkerResponse {
        HttpResponse response;
        std::string record;
        std::string run_view;
    };
    WorkerResponse handle_worker_request(const HttpRequest& request);

private:
    struct StoredExperiment {
        std::string experiment_id;
        std::string name;
        std::string description;
        std::int64_t created_at_unix_ms = 0;
        std::string derived_from_baseline_id;
        std::optional<std::string> parent_experiment_id;
        std::string config_json;
        std::shared_ptr<const TranslatedExperimentConfig> translated;
    };
    struct StoredIdempotentResponse {
        std::string request_fingerprint;
        int status = 0;  // zero reserves a key whose mutation outcome is uncertain
        std::string body;
    };

    HttpResponse route(const HttpRequest& request, const std::string& request_id);
    std::string route_get(const HttpRequest& request);
    HttpResponse route_post(const HttpRequest& request);
    std::pair<int, std::string> execute_mutation(const HttpRequest& request, const research::JsonValue* body);

    std::string render_service_info() const;
    std::string render_build_provenance() const;
    std::string render_baselines() const;
    std::string render_preview(const research::JsonValue& body);
    std::string create_experiment(const research::JsonValue& body);
    std::string create_run(const research::JsonValue& body);
    std::string render_experiment(const StoredExperiment& experiment) const;
    std::string list_experiments(const HttpRequest& request);
    std::string list_runs(const HttpRequest& request);
    std::string render_comparison(const HttpRequest& request);
    std::string render_connections() const;
    std::string route_run_get(const HttpRequest& request, const std::string& run_id, const std::string& view,
                              const std::string& order_segment);

    std::shared_ptr<ResearchRun> find_run(const std::string& run_id) const;
    std::shared_ptr<const StoredExperiment> find_experiment(const std::string& experiment_id) const;
    void persist_experiment(const StoredExperiment& experiment) const;
    std::string render_experiment_record(const StoredExperiment& experiment) const;
    void persist_idempotency_record(const std::string& key, const StoredIdempotentResponse& response) const;
    void remove_idempotency_reservation(const std::string& key) const;
    void load_data_directory();
    void import_file_metadata();
    std::vector<std::pair<std::filesystem::path, research::JsonValue>> metadata_records(
        PostgresStore::Records kind, const char* directory) const;
    void executor_loop();
    bool supervise_workers();
    std::shared_ptr<WorkerProcess> find_worker(const std::string& run_id) const;
    void publish_worker_reply(const std::string& run_id, const WorkerReply& reply);
    void fail_worker(const std::string& run_id, const std::string& reason);
    std::string isolated_query(const HttpRequest& request);
    std::string new_identifier(std::string_view prefix);

    ResearchServiceOptions options_;
    std::shared_ptr<PostgresStore> postgres_;
    std::atomic<bool> postgres_requires_restart_{false};
    std::optional<std::string> executable_sha256_;
    std::optional<std::string> exchange_sha256_;
    std::optional<std::string> trader_sha256_;
    mutable std::mutex state_mutex_;  // experiments, runs, idempotency maps
    std::mutex mutation_mutex_;       // serializes mutating requests (idempotency)
    std::mutex query_worker_mutex_;   // at most one cold replay/comparison worker
    std::unique_ptr<WorkerRootLock> worker_owner_lock_;
    std::map<std::string, std::shared_ptr<const StoredExperiment>> experiments_;
    std::vector<std::shared_ptr<ResearchRun>> runs_in_creation_order_;
    std::unordered_map<std::string, std::shared_ptr<ResearchRun>> runs_by_id_;
    std::unordered_map<std::string, std::shared_ptr<WorkerProcess>> workers_;
    std::unordered_map<std::string, std::string> worker_run_views_;
    std::unordered_map<std::string, StoredIdempotentResponse> idempotent_responses_;
    std::atomic<std::uint64_t> next_request_number_{1};
    std::uint64_t identifier_counter_ = 0;

    std::mutex executor_mutex_;
    std::condition_variable executor_wakeup_;
    bool executor_stop_requested_ = false;
    std::thread executor_thread_;
};

}  // namespace exsim::api

#endif
