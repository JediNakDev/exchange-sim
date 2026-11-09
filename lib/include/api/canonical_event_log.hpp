#ifndef EXSIM_API_CANONICAL_EVENT_LOG_HPP
#define EXSIM_API_CANONICAL_EVENT_LOG_HPP

// The API's canonical event log (HTTP API contract, `GET /v1/runs/{runId}/events`),
// built from what the deterministic runner records.
//
// The runner records every engine command it executes (CommandRecord) and
// the strategy's side of the pipeline (LifecycleRecord), but not the engine's
// events. The builder feeds the recorded, time-stamped commands to a shadow
// MatchingEngine in the same order, which reproduces the engine's events
// exactly (the engine is deterministic, and the runner's own replay check in
// RunArtifactWriter::finish relies on the same property). Market-data and
// report deliveries are tied back to the exchange event they report.
//
// Records are fixed size and trivially copyable so a run's log can be stored
// as a flat binary file and read back by position.

#include "event.hpp"
#include "matching_engine.hpp"
#include "research/experiment_config.hpp"
#include "research/run_records.hpp"
#include "research/simulation_core.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace exsim::api {

class ApiJsonWriter;

enum class CanonicalEventKind : std::uint8_t {
    MarketDataDelivered = 0,
    StrategyOrderSubmitted,
    StrategyCancelSubmitted,
    StrategyReplaceSubmitted,
    OrderArrivedAtExchange,
    CancelArrivedAtExchange,
    ReplaceArrivedAtExchange,
    OrderAccepted,
    OrderRejected,
    OrderRested,
    OrderFilled,
    OrderCanceled,
    OrderReplaced,
    ExecutionReportDelivered,
    RiskLimitBlockedOrder,
    PhaseChanged,
    SimulationBoundsReached,
};
inline constexpr std::size_t kCanonicalEventKindCount = 17;

enum class PipelineStage : std::uint8_t {
    FeedDelivery = 0,
    StrategyProcessing,
    OutboundOrder,
    ExchangeProcessing,
    ReportDelivery,
    RunnerControl,
};

enum class EventActor : std::uint8_t {
    ResearchStrategy = 0,
    BackgroundMarketMaker,
    BackgroundNoiseTaker,
    Exchange,
    Runner,
};

enum class EventVisibility : std::uint8_t { ExchangeTruth = 0, StrategyVisible = 1 };

// API ExecutionReportKind.
enum class DeliveredReportKind : std::uint8_t { Accepted = 0, Rejected, Rested, Filled, Canceled, Replaced, CancelRejected };

inline constexpr std::uint64_t kNoEventPosition = ~std::uint64_t{0};

// Presence bits of the nullable fields.
enum CanonicalEventField : std::uint32_t {
    kHasOrderId = 1u << 0,
    kHasClientOrderId = 1u << 1,
    kHasSide = 1u << 2,
    kHasPrice = 1u << 3,
    kHasQuantity = 1u << 4,
    kHasLeavesQuantity = 1u << 5,
    kHasCumulativeQuantity = 1u << 6,
    kHasMatchId = 1u << 7,
    kHasLiquidityRole = 1u << 8,
    kHasRejectReason = 1u << 9,
    kHasRejectedCommand = 1u << 10,
    kHasCancelReason = 1u << 11,
    kHasReportKind = 1u << 12,
    kHasBestBid = 1u << 13,
    kHasBestAsk = 1u << 14,
    kHasNewPhase = 1u << 15,
    kHasQueuePosition = 1u << 16,     // arrival of an own order that rested (runner FIFO evidence)
    kIsOwnOrder = 1u << 17,
    kWasResting = 1u << 18,           // canceled: the order was on the book
    kPriorityKept = 1u << 19,         // replaced
    kRiskReasonPresent = 1u << 20,
};

struct CanonicalEventRecord {
    Timestamp simulation_time_ns = 0;
    std::uint64_t enqueue_sequence = 0;     // scheduler step that produced the event
    std::uint64_t source_event_position = kNoEventPosition;   // delivered events: the reported exchange event
    std::uint64_t command_arrival_position = kNoEventPosition;  // exchange events: arrival of the command
    std::uint64_t command_index = research::kNoCommandIndex;          // runner command index (exchange side)
    OrderId order_id = 0;
    ClOrdId client_order_id = 0;
    MatchId match_id = 0;
    Price price = 0;
    Price best_bid_price = 0;
    Price best_ask_price = 0;
    std::uint64_t best_bid_quantity = 0;  // market_data_delivered: delivered top-of-book sizes
    std::uint64_t best_ask_quantity = 0;
    // Book maintenance (exchange_truth replay): both fill legs, and the order
    // a replace removed.
    OrderId buy_leg_order_id = 0;
    OrderId sell_leg_order_id = 0;
    OrderId replaced_old_order_id = 0;
    std::uint64_t queue_shares_ahead = 0;
    std::uint64_t queue_level_shares = 0;
    std::uint32_t queue_orders_ahead = 0;
    Quantity quantity = 0;
    Quantity leaves_quantity = 0;
    Quantity cumulative_quantity = 0;
    Quantity buy_leg_leaves = 0;
    Quantity sell_leg_leaves = 0;
    std::uint32_t present = 0;  // CanonicalEventField bits
    SessionId session = 0;      // session of order_id (book replay, ownership)
    SessionId buy_leg_session = 0;
    SessionId sell_leg_session = 0;
    CanonicalEventKind kind = CanonicalEventKind::MarketDataDelivered;
    PipelineStage stage = PipelineStage::ExchangeProcessing;
    EventActor actor = EventActor::Exchange;
    EventVisibility visibility = EventVisibility::ExchangeTruth;
    Side side = Side::BUY;
    research::Liquidity liquidity = research::Liquidity::Maker;
    RejectReason reject_reason = RejectReason::None;
    CommandType rejected_command = CommandType::NewOrder;
    CancelReason cancel_reason = CancelReason::UserRequested;
    DeliveredReportKind report_kind = DeliveredReportKind::Accepted;
    Phase new_phase = Phase::Closed;
    research::RiskRejectReason risk_reason = research::RiskRejectReason::None;
    EventActor counterparty_actor = EventActor::Exchange;  // own fills
    std::uint8_t reserved[3] = {};

    [[nodiscard]] bool has(CanonicalEventField field) const noexcept { return (present & field) != 0; }
};
static_assert(std::is_trivially_copyable_v<CanonicalEventRecord>);

[[nodiscard]] const char* to_api_string(CanonicalEventKind kind) noexcept;
[[nodiscard]] const char* to_api_string(PipelineStage stage) noexcept;
[[nodiscard]] const char* to_api_string(EventActor actor) noexcept;
[[nodiscard]] const char* to_api_string(EventVisibility visibility) noexcept;
[[nodiscard]] const char* to_api_string(DeliveredReportKind kind) noexcept;
[[nodiscard]] const char* to_api_string(RejectReason reason) noexcept;
[[nodiscard]] const char* to_api_string(CancelReason reason) noexcept;
[[nodiscard]] const char* to_api_string(CommandType command) noexcept;
[[nodiscard]] const char* to_api_string(Phase phase) noexcept;
[[nodiscard]] const char* to_api_side(Side side) noexcept;
[[nodiscard]] const char* to_api_liquidity(research::Liquidity liquidity) noexcept;
[[nodiscard]] std::optional<CanonicalEventKind> parse_canonical_event_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<EventActor> parse_event_actor(std::string_view text) noexcept;
[[nodiscard]] std::optional<EventVisibility> parse_event_visibility(std::string_view text) noexcept;

// The event's human-readable `description`.
[[nodiscard]] std::string describe_canonical_event(const CanonicalEventRecord& record);

// Writes one CanonicalEvent object (config/api/openapi.yaml).
void write_canonical_event_json(ApiJsonWriter& json, const CanonicalEventRecord& record, std::uint64_t event_position);

// Builds the canonical log step by step while the run executes.
class CanonicalEventBuilder {
public:
    explicit CanonicalEventBuilder(const research::ExperimentConfig& config);
    ~CanonicalEventBuilder();
    CanonicalEventBuilder(const CanonicalEventBuilder&) = delete;
    CanonicalEventBuilder& operator=(const CanonicalEventBuilder&) = delete;

    // Appends the events of one scheduler step. `new_commands` and
    // `new_lifecycle_rows` are the recorder rows the step added; `core` is the
    // simulation right after the step. `step_sequence` is the scheduler step count.
    void append_step(const research::SimulationCore& core, std::span<const research::CommandRecord> new_commands,
                     std::span<const research::LifecycleRecord> new_lifecycle_rows, std::uint64_t step_sequence,
                     std::vector<CanonicalEventRecord>& log);

    void append_simulation_bounds_reached(Timestamp time, std::uint64_t step_sequence, std::vector<CanonicalEventRecord>& log);

    // Positions of the strategy's fill events in FillRecord order (fill_index).
    [[nodiscard]] const std::vector<std::uint64_t>& own_fill_event_positions() const noexcept { return own_fill_event_positions_; }

    // The shadow engine (exchange truth right after the last appended command).
    [[nodiscard]] const MatchingEngine& shadow_engine() const noexcept { return *engine_; }
    [[nodiscard]] InstrumentId shadow_instrument() const noexcept { return instrument_; }

    // Set when the shadow engine disagreed with the runner (never expected);
    // the run then fails with failed_internal_error.
    [[nodiscard]] const std::string& consistency_error() const noexcept { return consistency_error_; }

private:
    void append_command(const research::CommandRecord& command_record, const research::LifecycleRecord* arrival_row,
                        std::uint64_t step_sequence, std::vector<CanonicalEventRecord>& log);
    [[nodiscard]] EventActor actor_for_session(SessionId session) const noexcept;
    [[nodiscard]] research::TopOfBook shadow_top_of_book() const noexcept;

    research::ExperimentConfig config_;
    std::unique_ptr<MatchingEngine> engine_;
    InstrumentId instrument_ = 0;
    EventBuffer event_buffer_;
    research::TopOfBook last_published_top_;
    std::uint64_t published_market_data_count_ = 0;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> market_data_source_positions_;  // (sequence, position)
    std::uint64_t last_delivered_market_data_sequence_ = 0;
    std::deque<std::uint64_t> pending_report_source_positions_;  // own exchange events awaiting delivery, FIFO
    std::vector<std::uint64_t> own_fill_event_positions_;
    std::string consistency_error_;
};

// Event-sourced exchange-truth book (L3) rebuilt from the canonical log.
class ExchangeTruthBook {
public:
    struct RestingOrder {
        OrderId order_id = 0;
        Side side = Side::BUY;
        Price price = 0;
        Quantity leaves = 0;
        SessionId session = 0;
    };
    struct Level {
        Price price = 0;
        std::uint64_t total_shares = 0;
        std::uint64_t order_count = 0;
        std::uint64_t own_shares = 0;
    };
    struct QueuePosition {
        bool found = false;
        Price price = 0;
        std::uint64_t shares_ahead = 0;
        std::uint64_t orders_ahead = 0;
        std::uint64_t level_shares = 0;
    };

    void apply(const CanonicalEventRecord& record);
    [[nodiscard]] std::vector<Level> levels(Side side, std::size_t depth) const;
    [[nodiscard]] QueuePosition queue_position(OrderId order_id) const;

private:
    // Per side: price -> FIFO list of orders.
    std::vector<RestingOrder> orders_;  // in arrival (priority) order; erased lazily
    void erase(OrderId order_id);
    RestingOrder* find(OrderId order_id);
    [[nodiscard]] const RestingOrder* find(OrderId order_id) const;
};

}  // namespace exsim::api

#endif
