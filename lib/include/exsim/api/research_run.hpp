#ifndef EXSIM_API_RESEARCH_RUN_HPP
#define EXSIM_API_RESEARCH_RUN_HPP

// One API run: a deterministic research simulation for one master seed,
// driven step by step through the runner library, with the API's state
// machine, canonical event log and read views (HTTP API contract).
//
// Threading: every public method takes the run's mutex. The service's
// executor thread advances running runs in bounded slices; HTTP handlers
// read views and apply controls between slices, which are always scheduler
// event boundaries.

#include "exsim/api/canonical_event_log.hpp"
#include "exsim/api/experiment_config_translation.hpp"
#include "exsim/research/run_records.hpp"
#include "exsim/research/simulation_core.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace exsim::api {

class PostgresStore;

// An error response in the contract's envelope (HTTP API contract, Errors).
class ApiFailure : public std::runtime_error {
public:
    ApiFailure(int status_value, std::string error_code, const std::string& message)
        : std::runtime_error(message), http_status(status_value), code(std::move(error_code)) {}
    int http_status;
    std::string code;
    std::optional<std::string> current_run_state;
    std::vector<FieldIssue> field_issues;
    bool retryable = false;
};

enum class ApiRunState : std::uint8_t { Queued = 0, Running, Paused, Completed, Cancelled, Failed };

[[nodiscard]] const char* to_api_string(ApiRunState state) noexcept;
[[nodiscard]] bool is_terminal(ApiRunState state) noexcept;
[[nodiscard]] std::optional<ApiRunState> parse_api_run_state(std::string_view text) noexcept;

// Preserve incomplete cache publications from an interrupted writer separately
// from the committed inspection snapshot. Called only after ownership is held.
void preserve_interrupted_caches(const std::filesystem::path& directory);

struct RunStateTransitionRecord {
    std::optional<ApiRunState> from_state;
    ApiRunState to_state = ApiRunState::Queued;
    std::int64_t transitioned_at_unix_ms = 0;
    std::uint64_t at_event_position = 0;
    Timestamp at_simulation_time_ns = 0;
    std::string reason;
};

struct TerminalDiagnosticsRecord {
    std::string outcome_code;
    std::string message;
    std::int64_t residual_position_shares = 0;
    std::uint64_t residual_open_order_count = 0;
};

// The strategy's view of one of its orders, aggregated from the canonical log
// by client order id (the runner's order identity). Exchange order ids are
// aliases: a replace that loses priority assigns a new one.
struct OwnOrderRecord {
    ClOrdId client_order_id = 0;
    OrderId first_order_id = 0;
    OrderId current_order_id = 0;
    Side side = Side::BUY;
    Price price = 0;
    Quantity quantity = 0;
    Quantity cumulative_quantity = 0;
    Timestamp submit_time_ns = 0;
    std::uint64_t submit_position = kNoEventPosition;
    std::uint64_t arrival_position = kNoEventPosition;
    std::uint64_t accepted_position = kNoEventPosition;
    std::uint64_t acknowledgment_position = kNoEventPosition;
    std::uint64_t last_position = kNoEventPosition;
    std::uint32_t fill_count = 0;
    bool canceled = false;
    bool rejected = false;
    bool done = false;                          // filled, canceled or rejected at the exchange
    std::vector<std::uint64_t> event_positions;  // every event that references the order, ascending
};

// Everything the read views use. Appended while the run executes, persisted
// as flat binary files once it is terminal.
struct RunArtifacts {
    std::vector<CanonicalEventRecord> events;
    std::vector<research::FillRecord> fills;
    std::vector<std::uint64_t> fill_event_positions;  // parallel to fills
    std::vector<research::MarkoutRecord> markouts;
    std::vector<research::SeriesSample> samples;

    std::vector<OwnOrderRecord> orders;  // in submit order
    std::unordered_map<ClOrdId, std::size_t> order_by_client_order_id;
    std::unordered_map<OrderId, std::size_t> order_by_exchange_order_id;
    std::unordered_map<MatchId, std::size_t> fill_by_match_id;
    // markouts row at fill_index * kMaxMarkoutHorizons + horizon_index, or
    // kNoMarkoutRow; a later row for the same pair replaces an earlier one.
    std::vector<std::size_t> markout_row_by_fill_horizon;

    static constexpr std::size_t kNoMarkoutRow = ~std::size_t{0};

    void index_event(std::uint64_t position);
    void index_markout(std::size_t row);
    [[nodiscard]] std::size_t markout_row(std::uint64_t fill_index, std::size_t horizon_index) const noexcept;
    void rebuild_indexes();
};

class SimulationDriver;

struct RunCreateParameters {
    std::string run_id;
    std::string experiment_id;
    std::shared_ptr<const TranslatedExperimentConfig> config;
    std::uint64_t master_seed = 0;
    bool start_paused = false;
    std::int64_t created_at_unix_ms = 0;
    std::filesystem::path run_directory;  // <data>/runs/<runId>
    std::uint64_t creation_ordinal = 0; // zero for legacy records
    std::string service_build_id;
    std::string executable_path;
    std::string provenance_json = "null";
    std::shared_ptr<PostgresStore> metadata_store; // control-plane I/O only
    bool worker_artifacts = false; // durable local artifacts; no database access
};

// Query parameters already validated by the router.
struct EventPageQuery {
    std::uint64_t start_position = 0;  // from the cursor or fromEventPosition
    std::uint64_t limit = 100;
    std::optional<std::uint64_t> to_position;  // inclusive
    std::vector<CanonicalEventKind> kinds;
    std::optional<EventVisibility> visibility;
    std::optional<EventActor> actor;
    std::optional<OrderId> order_id;
};

struct SeriesQuery {
    std::vector<std::string> kinds;
    std::optional<Timestamp> from_time_ns;
    std::optional<Timestamp> to_time_ns;
    std::uint32_t max_points = 500;
};

struct PageQuery {
    std::uint64_t start_index = 0;
    std::uint64_t limit = 100;
};

// Values a comparison reads from one terminal run.
struct RunComparisonInputs {
    std::string run_id;
    std::uint64_t master_seed = 0;
    // metric key + horizon -> (value text or empty when unavailable, unit, exactness)
    struct Metric {
        std::string metric_key;
        std::optional<Timestamp> horizon_ns;
        std::optional<std::string> value;
        std::string unit;
        std::string exactness;
    };
    std::vector<Metric> metrics;
    struct SamplePoint {
        Timestamp time_ns = 0;
        bool has_mark = false;
        std::int64_t equity_times_two = 0;
        std::int64_t position = 0;
    };
    std::vector<SamplePoint> samples;
    Timestamp end_time_ns = 0;
    bool execution_eligible = true;
    std::string observation_status = "not_applicable";
    Timestamp window_end_ns = 0;
    std::vector<research::ExecutionDecision> decisions;
    std::uint64_t decision_count = 0;
    std::string request_diagnostics_json;
    std::string markouts_json;
};

class ResearchRun {
public:
    explicit ResearchRun(RunCreateParameters parameters);
    // Recovery fails interrupted active runs; PostgreSQL queued runs may resume
    // with matching provenance. Snapshot mode loads acknowledged metadata without
    // invoking recovery. Only an actual worker sets worker_artifacts to write
    // diagnostic records rather than authoritative legacy run.json.
    static std::unique_ptr<ResearchRun> load(const std::filesystem::path& run_directory,
                                             std::shared_ptr<const TranslatedExperimentConfig> config,
                                             std::shared_ptr<PostgresStore> metadata_store = {},
                                             std::string record_json = {}, std::string executable_path = {},
                                             std::string current_provenance = "null", bool snapshot = false,
                                             bool worker_artifacts = false);
    ~ResearchRun();
    ResearchRun(const ResearchRun&) = delete;
    ResearchRun& operator=(const ResearchRun&) = delete;

    [[nodiscard]] const std::string& run_id() const noexcept { return parameters_.run_id; }
    [[nodiscard]] const std::string& experiment_id() const noexcept { return parameters_.experiment_id; }
    [[nodiscard]] std::uint64_t master_seed() const noexcept { return parameters_.master_seed; }
    [[nodiscard]] std::int64_t created_at_unix_ms() const noexcept { return parameters_.created_at_unix_ms; }
    [[nodiscard]] std::uint64_t creation_ordinal() const noexcept { return parameters_.creation_ordinal; }
    [[nodiscard]] ApiRunState state() const;
    // A control-plane snapshot, serialized under one lock between scheduler slices.
    [[nodiscard]] std::pair<std::string, std::string> worker_snapshot() const;
    std::string render_audit_export_json(std::optional<std::uint64_t> artifact, std::uint64_t offset) const;
    std::string render_execution_trace_json(const PageQuery& page) const;
    // True while the run holds a live simulation (running or paused).
    [[nodiscard]] bool holds_simulation() const;

    // Executor: queued -> running (or paused for startPaused). False when the
    // run is not queued.
    bool activate();
    // Executor: advances a running run by at most `max_steps` scheduler steps.
    // Returns true when the run is still running afterwards.
    bool advance_running_slice(std::uint64_t max_steps);

    // Controls throw ApiFailure invalid_state_transition when not allowed.
    // Persistence errors propagate after in-memory effects; callers must retain
    // their idempotency reservation rather than execute the request again.
    void pause(const std::string& reason);
    void resume(const std::string& reason);
    void step(std::uint64_t canonical_event_count, const std::string& reason);
    void cancel(const std::string& reason);
    void cancel_before_execution(const std::string& reason);
    void reserve_worker_launch();
    // Service shutdown: a live run fails with a recorded reason.
    void abandon(const std::string& reason);

    // Views: complete JSON bodies (throw ApiFailure).
    [[nodiscard]] std::string render_run_json() const;
    [[nodiscard]] std::string render_manifest_json() const;
    [[nodiscard]] std::string render_summary_json() const;
    [[nodiscard]] std::string render_series_json(const SeriesQuery& query) const;
    [[nodiscard]] std::string render_events_json(const EventPageQuery& query, const std::string& filter_fingerprint) const;
    [[nodiscard]] std::string render_orders_json(const PageQuery& page, const std::optional<std::string>& final_status,
                                                 const std::string& filter_fingerprint) const;
    [[nodiscard]] std::string render_order_lifecycle_json(OrderId order_id) const;
    [[nodiscard]] std::string render_fills_json(const PageQuery& page, std::optional<OrderId> order_id,
                                                const std::string& filter_fingerprint) const;
    [[nodiscard]] std::string render_book_json(std::optional<std::uint64_t> event_position, EventVisibility view,
                                               std::size_t depth) const;
    // Null unless the run completed.
    [[nodiscard]] std::optional<RunComparisonInputs> comparison_inputs(const PageQuery* trace_page = nullptr) const;

private:
    ResearchRun() = default;
    void transition_locked(ApiRunState to_state, const std::string& reason);
    void ensure_driver_locked();
    research::RunStop step_driver_locked();
    // Records the terminal state. A non-empty requester_reason (a cancel's
    // reason) becomes the transition reason; the outcome message stays in the
    // terminal diagnostics.
    void finish_locked(research::RunStop stop, const std::string& requester_reason = {});
    void fail_locked(const std::string& outcome_code, const std::string& message);
    void fail_execution_locked(std::string_view error) noexcept;
    void check_limits_locked();
    void persist_record_locked() const;
    std::string render_record_locked() const;
    void persist_artifacts_locked() const;
    void load_artifacts_locked() const;
    [[nodiscard]] std::uint64_t processed_event_count_locked() const noexcept {
        return artifacts_loaded_ ? artifacts_.events.size() : last_worker_event_count_;
    }
    [[nodiscard]] Timestamp simulation_time_locked() const noexcept;
    [[nodiscard]] std::string render_summary_locked() const;
    [[nodiscard]] std::string render_run_locked() const;
    [[nodiscard]] const OwnOrderRecord* find_order_locked(OrderId order_id) const;
    void require_artifacts_ready_locked() const;

    RunCreateParameters parameters_;
    std::string manifest_json_;
    std::string manifest_digest_hex_;
    mutable std::mutex mutex_;
    ApiRunState state_ = ApiRunState::Queued;
    std::vector<RunStateTransitionRecord> history_;
    std::optional<TerminalDiagnosticsRecord> diagnostics_;
    std::optional<std::string> canonical_event_digest_hex_;
    std::optional<std::string> ledger_digest_hex_;
    Timestamp final_simulation_time_ns_ = 0;
    std::uint64_t last_worker_event_count_ = 0;
    std::int64_t last_worker_position_ = 0;
    std::string final_summary_json_;
    std::optional<std::string> pending_limit_failure_code_;  // set when the service stopped the run
    std::string pending_limit_failure_message_;
    double running_wall_seconds_ = 0;

    std::unique_ptr<SimulationDriver> driver_;
    std::unique_ptr<CanonicalEventBuilder> builder_;
    std::size_t consumed_commands_ = 0;
    std::size_t consumed_lifecycle_ = 0;
    std::size_t consumed_fills_ = 0;
    std::size_t consumed_markouts_ = 0;
    std::size_t consumed_samples_ = 0;

    mutable RunArtifacts artifacts_;
    mutable bool artifacts_loaded_ = true;  // false for a restored terminal run until first read
};

// Opaque list cursors: the next index plus a fingerprint of the filters, so a
// cursor reused with changed filters is rejected (HTTP API contract, Pagination).
[[nodiscard]] std::string encode_page_cursor(std::uint64_t next_index, std::string_view filter_fingerprint);
[[nodiscard]] std::optional<std::uint64_t> decode_page_cursor(std::string_view cursor, std::string_view filter_fingerprint);

// Derived seed stream labels exactly as the runner names them.
[[nodiscard]] std::vector<research::DerivedSeed> derive_run_seeds(const research::ExperimentConfig& config, std::uint64_t master_seed);

}  // namespace exsim::api

#endif
