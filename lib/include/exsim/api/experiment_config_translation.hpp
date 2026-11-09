#ifndef EXSIM_API_EXPERIMENT_CONFIG_TRANSLATION_HPP
#define EXSIM_API_EXPERIMENT_CONFIG_TRANSLATION_HPP

// Translation between the API's ExperimentConfig (config/api/openapi.yaml) and
// the deterministic runner's experiment file (research runner).
//
// The API document is validated field by field (shape, types, patterns), then
// rendered to runner INI text while remembering which JSON pointer produced
// each line. The text goes through research::parse_experiment_config, so the
// runner's own validation is authoritative; a ConfigError on line N is
// reported as a field issue at the pointer that produced line N.

#include "exsim/research/experiment_config.hpp"
#include "exsim/research/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exsim::api {

struct FieldIssue {
    std::string field_path;  // JSON pointer into the request body
    std::string issue;
};

inline constexpr std::string_view kExecutableMarketWorld = "interactive_synthetic";

// Everything the service needs from a valid configuration.
struct TranslatedExperimentConfig {
    research::ExperimentConfig runner_config;  // parsed and validated by the runner
    std::string runner_config_text;            // the INI file given to the runner (experiment.conf)
    std::string config_json;                   // the API configuration as submitted (compact)
    std::string config_digest_hex;             // sha256 over the sorted-key compact JSON of the configuration
    std::string market_world;
    std::vector<std::uint64_t> master_seeds;
    std::string mark_convention;               // API name
    std::uint64_t max_canonical_event_count = 0;
    std::uint64_t runner_max_simulation_events = 0;  // derived from the runner's arithmetic bound
    std::uint32_t wall_clock_time_limit_seconds = 0;
    std::string strategy_kind;                 // API name
};

struct ConfigTranslationResult {
    std::vector<FieldIssue> shape_issues;     // the document does not match the schema (422 validation_failed)
    std::vector<FieldIssue> semantic_issues;  // well formed but not executable (422, or preview valid=false)
    bool market_world_supported = true;
    std::optional<TranslatedExperimentConfig> translated;  // set when there are no issues at all
};

// `config` is the value at `pointer_prefix` (usually "/config").
[[nodiscard]] ConfigTranslationResult translate_experiment_config(const research::JsonValue& config, std::string_view pointer_prefix);

// Validated fields of an ExperimentCreateRequest body.
struct ExperimentCreateFields {
    std::string name;
    std::string description;
    std::string derived_from_baseline_id;
    std::optional<std::string> parent_experiment_id;
    const research::JsonValue* config = nullptr;
};

// Shape checks for the request envelope (not the config). Appends issues.
[[nodiscard]] ExperimentCreateFields read_experiment_create_request(const research::JsonValue& body, std::vector<FieldIssue>& issues);

// ---- Baselines ----------------------------------------------------------------

struct BaselineConfiguration {
    std::string baseline_id;
    std::string name;
    std::string description;
    std::string config_json;  // ExperimentConfig
};

[[nodiscard]] const std::vector<BaselineConfiguration>& baseline_configurations();
[[nodiscard]] const BaselineConfiguration* find_baseline(std::string_view baseline_id);

// ---- Assumptions --------------------------------------------------------------

struct AssumptionLine {
    std::string category;
    std::string field_path;  // JSON pointer into ExperimentConfig
    std::string label;
    std::string value;
    std::optional<std::string> unit;
};

// Flattened display lines of a configuration, keyed by JSON pointer (decimal
// strings pass through unchanged). Same rules as the mock service.
[[nodiscard]] std::vector<AssumptionLine> flatten_experiment_config(const research::JsonValue& config);

struct AssumptionDifference {
    AssumptionLine reference;  // candidate line, or baseline line when absent in the candidate
    std::string baseline_value;
    std::string candidate_value;
};

[[nodiscard]] std::vector<AssumptionDifference> list_assumption_differences(const research::JsonValue& baseline_config,
                                                                            const research::JsonValue& candidate_config);

// Rough work estimate for previews: background agent and strategy activity
// per simulated second (an estimate, labelled as such by the contract).
[[nodiscard]] std::uint64_t estimate_canonical_events_per_run(const research::ExperimentConfig& runner_config);
[[nodiscard]] double estimate_wall_clock_seconds_per_run(std::uint64_t canonical_events);

// sha256 digest of a configuration: stable_serialize_json_value, then SHA-256.
[[nodiscard]] std::string configuration_digest_hex(const research::JsonValue& config);

// JSON with object keys sorted (stable digest input).
[[nodiscard]] std::string stable_serialize_json_value(const research::JsonValue& value);

}  // namespace exsim::api

#endif
