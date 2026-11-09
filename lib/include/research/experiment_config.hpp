#ifndef EXSIM_RESEARCH_EXPERIMENT_CONFIG_HPP
#define EXSIM_RESEARCH_EXPERIMENT_CONFIG_HPP

// Experiment configuration for the deterministic research runner.
//
// Parsed once at setup time (ordinary allocating code, exceptions allowed).
// Format: INI sections, `key = value`, `#` or `;` comments. Durations take a
// unit suffix (ns, us, ms, s); prices and money are decimal currency with at
// most four decimals. See research runner for every key and
// config/research/*.conf for complete examples.
//
// The canonical rendering (canonical_config_entries) lists every resolved
// value, defaults included, in a fixed order. Its FNV-1a 64 hash is the
// manifest's config_hash. The master seed is not part of it, so all runs of
// a seed sweep share one config_hash.

#include "instrument.hpp"
#include "server/config.hpp"
#include "sim/bots/market_maker.hpp"
#include "sim/bots/noise_taker.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exsim::research {

inline constexpr Timestamp kNanosPerMicrosecond = 1'000;
inline constexpr Timestamp kNanosPerMillisecond = 1'000'000;
inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;

enum class StrategyKind : std::uint8_t {
    FixedSpreadMarketMaker = 0,
    InventoryAwareMarketMaker = 1,
    TwapExecutor = 2,
    ImmediateSweepExecutor = 3,
};

[[nodiscard]] std::string_view to_string(StrategyKind kind) noexcept;

// How unrealized P&L and equity are marked. Missing marks stay unavailable.
enum class MarkConvention : std::uint8_t {
    ExchangeMid = 0,  // "exchange_mid": last two-sided engine-truth mid at or before the time, exact
                      // to half a price unit, carried across one-sided books; none before the first
    LastTrade = 1,    // "last_trade_price": engine last trade price; none before the first trade
};

[[nodiscard]] std::string_view to_string(MarkConvention mark) noexcept;

// Separate latency components, all nanoseconds of simulated time. Each
// message draws an extra uniform delay in [0, jitter] from the "latency" RNG
// stream; every channel stays FIFO (delivery time never decreases).
struct LatencyModel {
    Timestamp feed_ns = 50 * kNanosPerMicrosecond;    // exchange event -> market data at the strategy
    Timestamp order_ns = 100 * kNanosPerMicrosecond;  // strategy new order -> exchange arrival
    Timestamp cancel_ns = 100 * kNanosPerMicrosecond; // strategy cancel/replace -> exchange arrival
    Timestamp ack_ns = 100 * kNanosPerMicrosecond;    // exchange ack/fill/cancel -> report at the strategy
    Timestamp strategy_compute_ns = 0;                // decision -> departure from the strategy
    Timestamp feed_jitter_ns = 0;
    Timestamp order_jitter_ns = 0;
    Timestamp cancel_jitter_ns = 0;
    Timestamp ack_jitter_ns = 0;
    // true: new/cancel/replace share one FIFO order-entry channel, so a cancel
    // never overtakes an earlier order. false: each request arrives after its
    // own latency, so a faster cancel can race ahead of its order.
    bool order_channel_fifo = true;
};

// Fixed point: price units (1/10000 currency) per share. Negative = rebate.
struct FeeSchedule {
    std::int64_t maker_fee_per_share = 0;
    std::int64_t taker_fee_per_share = 0;
    std::int64_t auction_fee_per_share = 0;  // fills with Aggressor::None
};

// Pre-trade checks in the strategy host, before an order leaves the strategy.
struct RiskLimits {
    std::int64_t max_long_position = 1'000;
    std::int64_t max_short_position = 1'000;
    Quantity max_order_qty = 1'000;
    std::uint32_t max_open_orders = 16;
};

struct AccountingConfig {
    std::int64_t initial_cash = 1'000'000 * kPriceScale;  // price units
    MarkConvention mark = MarkConvention::ExchangeMid;
};

inline constexpr std::size_t kMaxMarkoutHorizons = 8;

struct MetricsConfig {
    std::array<Timestamp, kMaxMarkoutHorizons> markout_horizons_ns{};
    std::size_t markout_horizon_count = 0;
    Timestamp sample_interval_ns = 100 * kNanosPerMillisecond;
};

struct RunnerLimits {
    Timestamp duration_ns = 60 * kNanosPerSecond;
    std::uint64_t max_simulation_events = 50'000'000;
    std::size_t simulation_queue_capacity = 65'536;
    std::size_t observation_queue_capacity = 65'536;
    std::size_t recorder_command_capacity = 65'536;
    std::size_t recorder_record_capacity = 65'536;
    std::size_t recorder_flush_margin = 4'096;
};

struct EngineSettings {
    std::size_t max_orders = 4'096;
};

struct QuotingParameters {
    std::uint32_t half_spread_ticks = 1;
    Quantity quote_size = 100;
    Timestamp requote_interval_ns = 50 * kNanosPerMillisecond;
    std::uint32_t requote_threshold_ticks = 1;
    bool post_only = true;
};

struct InventorySkewParameters {
    std::uint32_t skew_ticks_per_unit = 1;  // center shift per skew_unit of known inventory
    Quantity skew_unit = 100;
    bool scale_size_with_inventory = true;   // shrink the side that adds inventory toward its limit
};

struct ParentOrderParameters {
    Side side = Side::BUY;
    Quantity parent_qty = 1'000;
    Timestamp start_ns = 1 * kNanosPerSecond;
    Timestamp execution_duration_ns = 30 * kNanosPerSecond;  // TWAP only
    Timestamp slice_interval_ns = 1 * kNanosPerSecond;       // TWAP only
    std::uint32_t limit_offset_ticks = 5;  // IOC limit = visible opposite best +/- offset
};

struct StrategyConfig {
    StrategyKind kind = StrategyKind::FixedSpreadMarketMaker;
    StpMode stp = StpMode::CancelAggressor;
    QuotingParameters quoting;
    InventorySkewParameters inventory;
    ParentOrderParameters parent;
};

enum class BackgroundAgentKind : std::uint8_t { MarketMaker = 0, NoiseTaker = 1 };

[[nodiscard]] std::string_view to_string(BackgroundAgentKind kind) noexcept;

struct BackgroundAgentConfig {
    BackgroundAgentKind kind = BackgroundAgentKind::MarketMaker;
    std::string name;
    // Market maker: fixed timer period (0 = its requote_ms). Noise taker:
    // fallback period when it has no future arrival (0 = its mean_interval_ms).
    Timestamp timer_interval_ns = 0;
    sim::bots::MarketMakerConfig market_maker;  // used when kind == MarketMaker
    sim::bots::NoiseTakerConfig noise_taker;    // used when kind == NoiseTaker
};

inline constexpr std::size_t kMaxBackgroundAgents = 16;
inline constexpr SessionId kStrategySessionId = 1;
inline constexpr OwnerId kStrategyOwnerId = 1;
inline constexpr InstrumentId kResearchInstrumentId = 1;

// Session of background agent `index` (configuration order).
[[nodiscard]] constexpr SessionId background_agent_session(std::size_t index) noexcept {
    return static_cast<SessionId>(2 + index);
}

struct ExperimentConfig {
    std::string name = "experiment";
    std::string description;
    std::uint64_t default_seed = 1;
    InstrumentConfig instrument;
    EngineSettings engine;
    server::Protocol order_entry_protocol = server::Protocol::Ouch42;
    LatencyModel latency;
    FeeSchedule fees;
    RiskLimits risk;
    AccountingConfig accounting;
    MetricsConfig metrics;
    RunnerLimits limits;
    StrategyConfig strategy;
    std::vector<BackgroundAgentConfig> agents;
};

class ConfigError : public std::runtime_error {
public:
    ConfigError(std::size_t line, const std::string& message)
        : std::runtime_error(line == 0 ? message : "line " + std::to_string(line) + ": " + message), line_(line) {}
    [[nodiscard]] std::size_t line() const noexcept { return line_; }

private:
    std::size_t line_;
};

// A valid configuration with an instrument and no background agents.
[[nodiscard]] ExperimentConfig default_experiment_config();

// Parses and validates; throws ConfigError.
[[nodiscard]] ExperimentConfig parse_experiment_config(std::string_view text);

// Cross-field checks; throws ConfigError (line 0). Also fills derived agent
// identities (session, owner, instrument) so callers may build configs in code.
void validate_experiment_config(ExperimentConfig& config);

using ConfigEntries = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] ConfigEntries strategy_parameter_entries(const StrategyConfig& strategy);
[[nodiscard]] ConfigEntries background_agent_parameter_entries(const BackgroundAgentConfig& agent);
[[nodiscard]] ConfigEntries canonical_config_entries(const ExperimentConfig& config);
[[nodiscard]] std::string canonical_config_text(const ExperimentConfig& config);
[[nodiscard]] std::uint64_t experiment_config_hash(const ExperimentConfig& config);

// The most records one scheduler step can push into the recorder, derived
// from the configured limits. The run checks free space only between steps,
// so the flush margin must cover a whole step (research runner,
// "Recorder flush margin"). `commands`: engine commands of one step (a
// background agent timer can submit several). `records`: lifecycle rows or
// ledger fills of one step.
struct RecorderStepBound {
    std::size_t commands = 0;
    std::size_t records = 0;
};
[[nodiscard]] RecorderStepBound recorder_step_bound(const ExperimentConfig& config) noexcept;

// max(recorder_flush_margin, both step bounds): the margin the run uses.
[[nodiscard]] std::size_t effective_recorder_flush_margin(const ExperimentConfig& config) noexcept;

// Effective background timer period of an agent (see timer_interval_ns).
[[nodiscard]] Timestamp background_agent_timer_interval(const BackgroundAgentConfig& agent) noexcept;

// Parsing helpers shared with the CLI and tests.
[[nodiscard]] bool parse_duration(std::string_view text, Timestamp& out) noexcept;
[[nodiscard]] std::string format_duration(Timestamp nanoseconds);

}  // namespace exsim::research

#endif
