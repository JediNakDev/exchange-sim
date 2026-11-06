#ifndef EXSIM_API_RESEARCH_SERVICE_HPP
#define EXSIM_API_RESEARCH_SERVICE_HPP

// The research HTTP API (docs/API.md, docs/api/openapi.yaml): routing,
// experiments, idempotency, the run executor and comparisons.
//
// Execution model: one executor thread advances every running run in
// round-robin slices of scheduler steps, at most `max_live_simulations`
// simulations held at once (running or paused). A slice holds only that
// run's mutex, so reads of other runs, and of this run between slices, are
// never blocked for long. step and cancel run synchronously in the request
// that asks for them, at an event boundary, so their response already shows
// the new state.

#include "exsim/api/http_message.hpp"
#include "exsim/api/research_run.hpp"

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
    bool start_executor = true;  // tests drive the executor by hand when false
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
    void persist_idempotency_record(const std::string& key, const StoredIdempotentResponse& response) const;
    void remove_idempotency_reservation(const std::string& key) const;
    void load_data_directory();
    void executor_loop();
    std::string new_identifier(std::string_view prefix);

    ResearchServiceOptions options_;
    mutable std::mutex state_mutex_;  // experiments, runs, idempotency maps
    std::mutex mutation_mutex_;       // serializes mutating requests (idempotency)
    std::map<std::string, std::shared_ptr<const StoredExperiment>> experiments_;
    std::vector<std::shared_ptr<ResearchRun>> runs_in_creation_order_;
    std::unordered_map<std::string, std::shared_ptr<ResearchRun>> runs_by_id_;
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
