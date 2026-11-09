#ifndef EXSIM_RESEARCH_SIMULATION_CORE_HPP
#define EXSIM_RESEARCH_SIMULATION_CORE_HPP

// Deterministic discrete-event research runner: the strategy-independent part.
//
// Simulated time is nanoseconds from 0 to config.limits.duration_ns. Two
// fixed-capacity queues drive the run:
//   * the simulation queue (background agent timers, strategy start/wakeups,
//     strategy requests arriving at the exchange, market-data and execution
//     report deliveries, scripted commands), ordered by (time, enqueue sequence);
//   * the observation queue (series samples, markout probes, the arrival
//     benchmark), ordered the same way, processed only after every simulation
//     event at the same or an earlier time. Observers never schedule
//     simulation events, so they cannot change the simulated outcome.
//
// Event-order policy at equal simulated times: simulation events run in
// enqueue order; observations run after every simulation event at that time.
// Initial events at t = 0 are enqueued as: background agent timers (config
// order), strategy start, then the first series sample. For one engine
// command the strategy's execution reports are enqueued in engine event
// order, then at most one market-data update.
//
// Causal pipeline per strategy request:
//   decision (strategy callback at its delivery time)
//     -> departure after strategy_compute latency
//     -> exchange arrival after order latency (new) or cancel latency (cancel/replace)
//     -> engine processing at arrival time (command ts = arrival time)
//     -> execution reports delivered after ack latency
//     -> market data (top of book / trades) delivered after feed latency.
// Background agents act synchronously at their timers with zero latency and
// see every engine event batch immediately (declared market model).
//
// Hot path: after construction nothing here allocates. Records go to the
// bounded RunRecorder; the run yields (FlushRequired) before it could fill.
// ProcessSimulation owns the production loop and communicates with traderd.
// Simulation<Strategy> (simulation.hpp) retains the in-process test reference.

#include "event.hpp"
#include "matching_engine.hpp"
#include "research/experiment_config.hpp"
#include "research/position_ledger.hpp"
#include "research/run_metrics.hpp"
#include "research/run_records.hpp"
#include "research/strategy_context.hpp"
#include "research/strategy_messages.hpp"
#include "research/timed_event_queue.hpp"
#include "sim/bots/market_maker.hpp"
#include "sim/bots/noise_taker.hpp"
#include "sim/random.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace exsim::research {

inline constexpr Timestamp kNoTime = std::numeric_limits<Timestamp>::max();

enum class RunState : std::uint8_t { Queued = 0, Running = 1, Paused = 2, Completed = 3, Cancelled = 4, Failed = 5 };

[[nodiscard]] constexpr const char* to_string(RunState state) noexcept {
    switch (state) {
        case RunState::Queued: return "queued";
        case RunState::Running: return "running";
        case RunState::Paused: return "paused";
        case RunState::Completed: return "completed";
        case RunState::Cancelled: return "cancelled";
        case RunState::Failed: return "failed";
    }
    return "?";
}

enum class TerminationReason : std::uint8_t {
    None = 0,
    EndTimeReached = 1,          // completed: every event up to duration_ns processed
    EventQueueExhausted = 2,     // completed: nothing left to simulate before duration_ns
    CancelRequested = 3,
    EventLimitReached = 4,       // failed: max_simulation_events before the end time
    SimulationQueueFull = 5,
    ObservationQueueFull = 6,
    RecorderOverflow = 7,
    LedgerRejectedFill = 8,      // failure detail: LedgerStatus
    EngineOutputCapacity = 9,
    OrderTableCorrupt = 10,
};

[[nodiscard]] const char* to_string(TerminationReason reason) noexcept;

enum class RunStop : std::uint8_t {
    Continue = 0,            // internal: a step may be executed
    Completed = 1,
    Failed = 2,
    Cancelled = 3,
    PausedAtTime = 4,        // next event is after RunBounds::pause_at
    StepBudgetExhausted = 5, // RunBounds::max_steps steps executed
    FlushRequired = 6,       // drain the recorder, then call run() again
};

[[nodiscard]] const char* to_string(RunStop stop) noexcept;

struct RunBounds {
    Timestamp pause_at = kNoTime;  // process every event with time <= pause_at, then pause
    std::uint64_t max_steps = std::numeric_limits<std::uint64_t>::max();  // events in this call
};

struct RunDigests {
    std::uint64_t command_digest = 14695981039346656037ull;  // FNV-1a chain over sim::encode(command)
    std::uint64_t event_digest = 14695981039346656037ull;    // sim::hash_event chain, same as exsim_replay
    std::uint64_t ledger_digest = 14695981039346656037ull;   // hash_fill_record chain
    std::uint64_t command_count = 0;
    std::uint64_t event_count = 0;
    std::uint64_t fill_count = 0;
};

struct DerivedSeed {
    std::string stream;
    std::uint64_t seed = 0;
};

enum class SimulationEventKind : std::uint8_t {
    BackgroundAgentTimer = 0,
    StrategyStart = 1,
    StrategyWakeup = 2,
    StrategyRequestArrival = 3,
    MarketDataDelivery = 4,
    ExecutionReportDelivery = 5,
    ScriptedCommand = 6,
    SessionTimer = 7,
};

struct StrategyRequestArrival {
    Command command;           // ts is set to the arrival time when processed
    Timestamp decision_time = 0;
    ClOrdId cl_ord_id = 0;     // target order token
    Side side = Side::BUY;     // side of the target order (kept even if the order is gone on arrival)
};

struct SimulationEvent {
    SimulationEventKind kind = SimulationEventKind::StrategyStart;
    std::uint16_t agent_index = 0;
    std::uint64_t wakeup_generation = 0;
    std::uint64_t protocol_sequence = 0;
    union Data {
        MarketDataUpdate market_data;
        ExecutionReport report;
        StrategyRequestArrival request;
        Command scripted_command;
        Data() noexcept : market_data() {}
    } data;
};

static_assert(std::is_trivially_copyable_v<SimulationEvent>);

enum class ObservationKind : std::uint8_t { SeriesSample = 0, MarkoutProbe = 1 };

struct ObservationEvent {
    ObservationKind kind = ObservationKind::SeriesSample;
    std::uint8_t horizon_index = 0;
    Side side = Side::BUY;
    Quantity qty = 0;
    Price price = 0;
    std::uint64_t fill_index = 0;
    Timestamp fill_time = 0;
};

// Engine-truth tracking of one strategy order. Never visible to the strategy.
struct ExchangeTruthOrder {
    bool live = false;           // submitted and not yet done at the exchange
    bool accepted = false;
    bool resting = false;
    bool filled_once = false;
    Side side = Side::BUY;
    Price price = 0;
    Quantity total_qty = 0;
    Quantity leaves_qty = 0;
    Quantity cum_qty = 0;
    OrderId order_id = 0;
    ClOrdId cl_ord_id = 0;
    Timestamp decision_time = 0;
};

struct BackgroundAgentSummary {
    std::string name;
    BackgroundAgentKind kind = BackgroundAgentKind::MarketMaker;
    SessionId session_id = 0;
    std::uint64_t seed = 0;
    sim::bots::BotStats stats;
    std::int64_t inventory = 0;
};

// Engine configuration used for every research run (also written to the
// command journal so exsim_replay rebuilds the same engine).
[[nodiscard]] EngineConfig research_engine_config(const ExperimentConfig& experiment_config);

// Signed cost of the open FIFO lots in price units (negative when short):
// cash = initial - buys + sells - fees and buys - sells = open cost - realized.
[[nodiscard]] constexpr std::int64_t open_cost_basis(const LedgerState& ledger_state, std::int64_t initial_cash) noexcept {
    return initial_cash - ledger_state.cash - ledger_state.fees + ledger_state.gross_realized_pnl;
}

// Valuation at a doubled mark, in doubled price units (exact for half-unit mids).
struct DoubledValuation {
    std::int64_t unrealized_pnl_times_two = 0;  // position * mark - open cost basis
    std::int64_t equity_times_two = 0;          // cash + position * mark
};

[[nodiscard]] constexpr DoubledValuation value_at_mark(const LedgerState& ledger_state, std::int64_t initial_cash,
                                                       std::int64_t mark_times_two) noexcept {
    const std::int64_t position_value_times_two = ledger_state.position * mark_times_two;
    return {position_value_times_two - 2 * open_cost_basis(ledger_state, initial_cash),
            2 * ledger_state.cash + position_value_times_two};
}

// Optional full-session observation hooks. They do not execute matching or
// advance simulated time. All buffers are owned and bounded at run setup.
struct SessionHooks {
    void* owner = nullptr;
    void (*event)(void*,const Event&,Phase) noexcept = nullptr;
    void (*report)(void*,std::uint64_t) noexcept = nullptr;
    void (*market)(void*,std::uint64_t) noexcept = nullptr;
    void (*market_batch)(void*,std::span<const Event>) noexcept = nullptr;
};

class SimulationCore {
public:
    SimulationCore(const ExperimentConfig& config, std::uint64_t master_seed);
    ~SimulationCore();
    SimulationCore(const SimulationCore&) = delete;
    SimulationCore& operator=(const SimulationCore&) = delete;

    // ---- Inspection (post-run explanation and tests; never given to strategies)
    [[nodiscard]] const ExperimentConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::uint64_t master_seed() const noexcept { return master_seed_; }
    [[nodiscard]] const std::vector<DerivedSeed>& derived_seeds() const noexcept { return derived_seeds_; }
    [[nodiscard]] RunState state() const noexcept { return state_; }
    [[nodiscard]] TerminationReason termination() const noexcept { return termination_; }
    [[nodiscard]] std::uint64_t failure_detail() const noexcept { return failure_detail_; }
    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    [[nodiscard]] Timestamp end_time() const noexcept { return config_.limits.duration_ns; }
    [[nodiscard]] std::uint64_t simulation_events_processed() const noexcept { return simulation_events_processed_; }
    [[nodiscard]] std::uint64_t observation_events_processed() const noexcept { return observation_events_processed_; }
    [[nodiscard]] std::size_t simulation_queue_high_water_mark() const noexcept { return simulation_queue_.high_water_mark(); }
    [[nodiscard]] std::size_t observation_queue_high_water_mark() const noexcept { return observation_queue_.high_water_mark(); }
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }
    [[nodiscard]] const PositionLedger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const RunMetrics& metrics() const noexcept { return metrics_; }
    [[nodiscard]] const RunDigests& digests() const noexcept { return digests_; }
    [[nodiscard]] const StrategyVisibleState& strategy_view() const noexcept { return visible_; }
    [[nodiscard]] RunRecorder& recorder() noexcept { return recorder_; }
    [[nodiscard]] const RunRecorder& recorder() const noexcept { return recorder_; }
    [[nodiscard]] TopOfBook exchange_top_of_book() const noexcept;
    // Twice the reference exchange mid: the last two-sided exchange mid at or
    // before now (a one-sided book does not clear it). Empty until the first
    // two-sided book. Exact, so a mid on half a price unit stays exact.
    [[nodiscard]] std::optional<std::int64_t> reference_mid_times_two() const noexcept {
        if (!has_reference_mid_) return std::nullopt;
        return reference_mid_times_two_;
    }
    // Twice the mark under the configured convention (exchange_mid: the
    // reference mid; last_trade: the engine's last trade price).
    [[nodiscard]] std::optional<std::int64_t> mark_times_two() const noexcept;
    [[nodiscard]] std::vector<BackgroundAgentSummary> background_agent_summaries() const;
    [[nodiscard]] const ExchangeTruthOrder* truth_order(ClOrdId cl_ord_id) const noexcept;

    void attach_session_hooks(SessionHooks hooks) noexcept { session_hooks_ = hooks; }

    // ---- Control ---------------------------------------------------------
    // Cancels the run before its next step (state Cancelled).
    void request_stop() noexcept { stop_requested_ = true; }
    // Setup/test hook: a recorded exchange command from a scripted session
    // (not the strategy's or an agent's). Returns false if rejected or full.
    [[nodiscard]] bool schedule_scripted_command(Timestamp time, const Command& command) noexcept;

    // ---- Strategy host actions (called through StrategyContext) -----------
    [[nodiscard]] ClOrdId strategy_submit_limit_order(Side side, Price price, Quantity qty, TimeInForce tif, bool post_only) noexcept;
    bool strategy_cancel_order(ClOrdId cl_ord_id) noexcept;
    bool strategy_replace_order(ClOrdId cl_ord_id, Price price, Quantity new_total_qty, bool force_priority_loss = false) noexcept;
    void strategy_wake_at(Timestamp time) noexcept;
    void strategy_declare_parent_order(Side side, Quantity qty) noexcept;
    void record_execution_decision(const ExecutionDecision& row) noexcept;
    [[nodiscard]] sim::Rng& strategy_rng() noexcept { return strategy_rng_; }
    [[nodiscard]] std::int64_t strategy_position_headroom(Side side, ClOrdId excluded_order) const noexcept;

protected:
    // Steps of the run loop used by Simulation<Strategy>.
    void begin_run() noexcept;
    [[nodiscard]] RunStop prepare_step(const RunBounds& bounds, std::uint64_t steps_taken) noexcept;
    [[nodiscard]] bool next_step_is_observation() const noexcept { return next_is_observation_; }
    void run_observation_step() noexcept;
    // Pops the next simulation event and advances time. Strategy-facing kinds
    // are returned to the template for dispatch; others are processed here.
    [[nodiscard]] bool pop_simulation_event(SimulationEvent& strategy_event) noexcept;
    // Applies a delivered message to the strategy view before the callback.
    void deliver_market_data(const MarketDataUpdate& update) noexcept;
    void deliver_execution_report(const ExecutionReport& report) noexcept;
    [[nodiscard]] bool accept_wakeup(const SimulationEvent& event) noexcept;
    void after_strategy_callback() noexcept;

    StrategyContext& strategy_context() noexcept { return context_; }

private:
    SessionHooks session_hooks_{};
    struct BackgroundExchangeContext;
    struct BackgroundAgentSlot {
        BackgroundAgentKind kind = BackgroundAgentKind::MarketMaker;
        std::size_t position = 0;  // index into the kind's vector
        Timestamp timer_interval_ns = 1;
    };

    void fail(TerminationReason reason, std::uint64_t detail = 0) noexcept;
    void process_background_timer(std::uint16_t agent_index) noexcept;
    void process_strategy_arrival(const StrategyRequestArrival& request) noexcept;
    void process_exchange_command(const Command& command, CommandSource source, std::uint16_t agent_index,
                                  const StrategyRequestArrival* request) noexcept;
    // FIFO queue at one price level: orders and quantity ahead of `order_id`
    // (all of them when it is 0 or absent) and the level's total leaves.
    struct LevelQueue {
        std::uint64_t qty_ahead = 0;
        std::uint64_t level_qty = 0;
        std::uint32_t orders_ahead = 0;
        bool found = false;
    };
    [[nodiscard]] LifecycleRecord capture_strategy_arrival(const StrategyRequestArrival& request,
                                                           std::uint64_t command_index) noexcept;
    [[nodiscard]] LevelQueue measure_level_queue(Side side, Price price, OrderId order_id) const noexcept;
    bool handle_strategy_event(const Event& event, std::uint64_t command_index) noexcept;
    void handle_strategy_fill(const Event& event, const FillLeg& leg, Side side, std::uint64_t command_index) noexcept;
    void schedule_report(const ExecutionReport& report) noexcept;
    void publish_market_data(const TopOfBook& top, std::uint32_t trade_count, std::uint64_t traded_qty, Price last_price,
                             Aggressor last_aggressor) noexcept;
    void update_quote_exposure() noexcept;
    void close_quote_exposure(Timestamp time) noexcept;
    void take_series_sample() noexcept;
    void process_markout_probe(const ObservationEvent& probe) noexcept;
    [[nodiscard]] bool continue_finishing() noexcept;
    void push_lifecycle(const LifecycleRecord& record) noexcept;
    [[nodiscard]] Timestamp delay_with_jitter(Timestamp base, Timestamp jitter) noexcept;
    [[nodiscard]] Timestamp schedule_order_channel(Timestamp latency, Timestamp jitter) noexcept;
    [[nodiscard]] bool push_simulation(Timestamp time, const SimulationEvent& event) noexcept;
    [[nodiscard]] bool push_observation(Timestamp time, const ObservationEvent& event) noexcept;
    [[nodiscard]] StrategyOrderView* view_slot(ClOrdId cl_ord_id) noexcept;
    [[nodiscard]] ExchangeTruthOrder* truth_slot(ClOrdId cl_ord_id) noexcept;
    void release_view(StrategyOrderView& view) noexcept;
    [[nodiscard]] bool risk_reject(RiskRejectReason reason, CommandType request, Side side, Price price, Quantity qty,
                                   ClOrdId cl_ord_id) noexcept;
    [[nodiscard]] std::uint64_t worst_case_open_qty(Side side, ClOrdId excluded_order) const noexcept;
    [[nodiscard]] bool valid_price(Price price) const noexcept;
    [[nodiscard]] bool send_strategy_cancel(StrategyOrderView& view) noexcept;

    ExperimentConfig config_;
    std::uint64_t master_seed_;
    std::vector<DerivedSeed> derived_seeds_;

    MatchingEngine engine_;
    InstrumentId instrument_id_ = 0;
    EventBuffer event_buffer_;

    std::vector<std::unique_ptr<sim::bots::MarketMaker>> market_makers_;
    std::vector<std::unique_ptr<sim::bots::NoiseTaker>> noise_takers_;
    std::vector<BackgroundAgentSlot> agent_slots_;

    TimedEventQueue<SimulationEvent> simulation_queue_;
    TimedEventQueue<ObservationEvent> observation_queue_;

    PositionLedger ledger_;
    RunRecorder recorder_;
    RunMetrics metrics_;
    RunDigests digests_;

    StrategyVisibleState visible_;
    std::unique_ptr<ExchangeTruthOrder[]> truth_orders_;
    StrategyContext context_;
    sim::Rng strategy_rng_;
    sim::Rng latency_rng_;

    RunState state_ = RunState::Queued;
    TerminationReason termination_ = TerminationReason::None;
    std::uint64_t failure_detail_ = 0;
    bool stop_requested_ = false;
    bool started_ = false;
    bool finishing_ = false;
    bool terminal_sample_taken_ = false;
    bool next_is_observation_ = false;
    bool in_strategy_callback_ = false;
    Timestamp now_ = 0;
    Timestamp last_sample_time_ = kNoTime;
    std::uint64_t simulation_events_processed_ = 0;
    std::uint64_t observation_events_processed_ = 0;

    ClOrdId next_cl_ord_id_ = 1;
    std::uint64_t wakeup_generation_ = 0;
    Timestamp pending_wakeup_time_ = kNoTime;
    std::uint64_t market_data_sequence_ = 0;
    TopOfBook last_published_top_;
    bool has_reference_mid_ = false;
    std::int64_t reference_mid_times_two_ = 0;
    Timestamp order_channel_last_arrival_ = 0;
    Timestamp feed_channel_last_delivery_ = 0;
    Timestamp ack_channel_last_delivery_ = 0;
};

}  // namespace exsim::research

#endif
