#include "exsim/research/simulation_core.hpp"

#include "exsim/research/seed_derivation.hpp"
#include "exsim/sim/canonical.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>

namespace exsim::research {

namespace {

ExperimentConfig validated_copy(const ExperimentConfig& experiment_config) {
    ExperimentConfig copy = experiment_config;
    validate_experiment_config(copy);
    return copy;
}

// Power of two, at least twice the open-order limit, so the strategy's order
// tokens (sequential) always find a free slot within a few probes.
std::size_t strategy_order_table_capacity(const RiskLimits& risk) {
    std::size_t capacity = 16;
    while (capacity < 2 * static_cast<std::size_t>(risk.max_open_orders)) capacity *= 2;
    return capacity;
}

// Open FIFO lots never exceed |position| / lot_size, and the pre-trade check
// bounds the true position by the larger position limit.
std::size_t ledger_lot_capacity(const ExperimentConfig& experiment_config) {
    const auto largest_limit = std::max(experiment_config.risk.max_long_position, experiment_config.risk.max_short_position);
    const auto lots = static_cast<std::uint64_t>(largest_limit) / experiment_config.instrument.lot_size + 1;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(lots, 16, std::uint64_t{1} << 20));
}

void stamp_command_time(Command& command, Timestamp time) noexcept {
    switch (command.type) {
        case CommandType::NewOrder: command.new_order.ts = time; break;
        case CommandType::Cancel: command.cancel.ts = time; break;
        case CommandType::Replace: command.replace.ts = time; break;
        case CommandType::MassCancel: command.mass_cancel.ts = time; break;
        case CommandType::SetPhase: command.set_phase.ts = time; break;
    }
}

[[nodiscard]] std::int64_t signed_quantity(Side side, Quantity qty) noexcept {
    return side == Side::BUY ? static_cast<std::int64_t>(qty) : -static_cast<std::int64_t>(qty);
}

// Value of a fill relative to a mid, doubled to stay exact:
// buy  -> (2 * mid - 2 * price) * qty, sell -> (2 * price - 2 * mid) * qty.
[[nodiscard]] std::int64_t edge_versus_mid_times_two(Side side, std::int64_t mid_times_two, Price price, Quantity qty) noexcept {
    const std::int64_t per_share = mid_times_two - 2 * price;
    return (side == Side::BUY ? per_share : -per_share) * static_cast<std::int64_t>(qty);
}

[[nodiscard]] Liquidity fill_liquidity(Aggressor aggressor, Side side) noexcept {
    if (aggressor == Aggressor::None) return Liquidity::Auction;
    const bool aggressor_is_buy = aggressor == Aggressor::Buy;
    return aggressor_is_buy == (side == Side::BUY) ? Liquidity::Taker : Liquidity::Maker;
}

}  // namespace

EngineConfig research_engine_config(const ExperimentConfig& experiment_config) {
    EngineConfig engine_config;
    engine_config.max_instruments = 1;
    engine_config.max_orders = experiment_config.engine.max_orders;
    return engine_config;
}

const char* to_string(TerminationReason reason) noexcept {
    switch (reason) {
        case TerminationReason::None: return "none";
        case TerminationReason::EndTimeReached: return "end_time_reached";
        case TerminationReason::EventQueueExhausted: return "event_queue_exhausted";
        case TerminationReason::CancelRequested: return "cancel_requested";
        case TerminationReason::EventLimitReached: return "event_limit_reached";
        case TerminationReason::SimulationQueueFull: return "simulation_queue_full";
        case TerminationReason::ObservationQueueFull: return "observation_queue_full";
        case TerminationReason::RecorderOverflow: return "recorder_overflow";
        case TerminationReason::LedgerRejectedFill: return "ledger_rejected_fill";
        case TerminationReason::EngineOutputCapacity: return "engine_output_capacity";
        case TerminationReason::OrderTableCorrupt: return "order_table_corrupt";
    }
    return "?";
}

const char* to_string(RunStop stop) noexcept {
    switch (stop) {
        case RunStop::Continue: return "continue";
        case RunStop::Completed: return "completed";
        case RunStop::Failed: return "failed";
        case RunStop::Cancelled: return "cancelled";
        case RunStop::PausedAtTime: return "paused_at_time";
        case RunStop::StepBudgetExhausted: return "step_budget_exhausted";
        case RunStop::FlushRequired: return "flush_required";
    }
    return "?";
}

// ---- StrategyContext actions --------------------------------------------

std::int64_t StrategyContext::position_headroom(Side side, ClOrdId excluded_order) const noexcept {
    return core_.strategy_position_headroom(side, excluded_order);
}
ClOrdId StrategyContext::submit_limit_order(Side side, Price price, Quantity qty, TimeInForce tif, bool post_only) noexcept {
    return core_.strategy_submit_limit_order(side, price, qty, tif, post_only);
}
bool StrategyContext::cancel_order(ClOrdId cl_ord_id) noexcept { return core_.strategy_cancel_order(cl_ord_id); }
bool StrategyContext::replace_order(ClOrdId cl_ord_id, Price price, Quantity new_total_qty) noexcept {
    return core_.strategy_replace_order(cl_ord_id, price, new_total_qty);
}
void StrategyContext::wake_at(Timestamp time) noexcept { core_.strategy_wake_at(time); }
void StrategyContext::declare_parent_order(Side side, Quantity qty) noexcept { core_.strategy_declare_parent_order(side, qty); }
sim::Rng& StrategyContext::rng() noexcept { return core_.strategy_rng(); }

// ---- Background agents ----------------------------------------------------

// BotContext for the seeded background agents: synchronous submission into
// the shared engine at the current simulated time.
struct SimulationCore::BackgroundExchangeContext {
    SimulationCore& core;
    std::uint16_t agent_index;

    void submit(const Command& command) noexcept {
        core.process_exchange_command(command, CommandSource::BackgroundAgent, agent_index, nullptr);
    }
    [[nodiscard]] Timestamp now() const noexcept { return core.now_; }
    [[nodiscard]] const InstrumentConfig* instrument(InstrumentId id) const noexcept { return core.engine_.instrument(id); }
};

// ---- Construction -----------------------------------------------------------

SimulationCore::SimulationCore(const ExperimentConfig& experiment_config, std::uint64_t run_master_seed)
    : config_(validated_copy(experiment_config)),
      master_seed_(run_master_seed),
      engine_(research_engine_config(config_)),
      event_buffer_(required_event_capacity(research_engine_config(config_))),
      simulation_queue_(config_.limits.simulation_queue_capacity),
      observation_queue_(config_.limits.observation_queue_capacity),
      ledger_(config_.accounting.initial_cash, ledger_lot_capacity(config_)),
      recorder_(config_.limits.recorder_command_capacity, config_.limits.recorder_record_capacity,
                effective_recorder_flush_margin(config_)),
      context_(*this, visible_),
      strategy_rng_(derive_stream_seed(run_master_seed, "strategy")),
      latency_rng_(derive_stream_seed(run_master_seed, "latency")) {
    instrument_id_ = engine_.add_instrument(config_.instrument);
    if (instrument_id_ != kResearchInstrumentId) throw std::invalid_argument("research instrument rejected by the engine");

    level_order_capacity_ = config_.engine.max_orders;
    level_order_ids_ = std::make_unique<OrderId[]>(level_order_capacity_);

    derived_seeds_.push_back({"strategy", derive_stream_seed(run_master_seed, "strategy")});
    derived_seeds_.push_back({"latency", derive_stream_seed(run_master_seed, "latency")});
    for (std::size_t index = 0; index < config_.agents.size(); ++index) {
        const auto& agent = config_.agents[index];
        const std::string label = "agent." + std::to_string(index) + "." + std::string(to_string(agent.kind)) + "." + agent.name;
        const std::uint64_t seed = derive_stream_seed(run_master_seed, label);
        derived_seeds_.push_back({label, seed});
        BackgroundAgentSlot slot;
        slot.kind = agent.kind;
        slot.timer_interval_ns = std::max<Timestamp>(1, background_agent_timer_interval(agent));
        if (agent.kind == BackgroundAgentKind::MarketMaker) {
            auto bot_config = agent.market_maker;
            bot_config.seed = seed;
            slot.position = market_makers_.size();
            market_makers_.push_back(std::make_unique<sim::bots::MarketMaker>(bot_config));
        } else {
            auto bot_config = agent.noise_taker;
            bot_config.seed = seed;
            slot.position = noise_takers_.size();
            noise_takers_.push_back(std::make_unique<sim::bots::NoiseTaker>(bot_config));
        }
        agent_slots_.push_back(slot);
    }

    visible_.instrument = config_.instrument;
    visible_.risk = config_.risk;
    visible_.order_capacity = strategy_order_table_capacity(config_.risk);
    visible_.orders = std::make_unique<StrategyOrderView[]>(visible_.order_capacity);
    truth_orders_ = std::make_unique<ExchangeTruthOrder[]>(visible_.order_capacity);
}

SimulationCore::~SimulationCore() = default;

// ---- Inspection -------------------------------------------------------------

TopOfBook SimulationCore::exchange_top_of_book() const noexcept {
    TopOfBook top;
    LevelInfo level{};
    if (engine_.depth(instrument_id_, Side::BUY, &level, 1) == 1) {
        top.has_bid = true;
        top.bid_price = level.price;
        top.bid_qty = level.qty;
    }
    if (engine_.depth(instrument_id_, Side::SELL, &level, 1) == 1) {
        top.has_ask = true;
        top.ask_price = level.price;
        top.ask_qty = level.qty;
    }
    return top;
}

std::optional<std::int64_t> SimulationCore::mark_times_two() const noexcept {
    if (config_.accounting.mark == MarkConvention::LastTrade) {
        const auto last_trade = engine_.last_trade_price(instrument_id_);
        if (!last_trade) return std::nullopt;
        return 2 * *last_trade;
    }
    return reference_mid_times_two();
}

std::vector<BackgroundAgentSummary> SimulationCore::background_agent_summaries() const {
    std::vector<BackgroundAgentSummary> summaries;
    for (std::size_t index = 0; index < agent_slots_.size(); ++index) {
        const auto& slot = agent_slots_[index];
        const sim::bots::BotState& bot = slot.kind == BackgroundAgentKind::MarketMaker
                                             ? static_cast<const sim::bots::BotState&>(*market_makers_[slot.position])
                                             : static_cast<const sim::bots::BotState&>(*noise_takers_[slot.position]);
        BackgroundAgentSummary summary;
        summary.name = config_.agents[index].name;
        summary.kind = slot.kind;
        summary.session_id = bot.config().session_id;
        summary.seed = bot.config().seed;
        summary.stats = bot.stats();
        summary.inventory = bot.inventory();
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

const ExchangeTruthOrder* SimulationCore::truth_order(ClOrdId cl_ord_id) const noexcept {
    if (cl_ord_id == 0) return nullptr;
    const auto& truth = truth_orders_[cl_ord_id & (visible_.order_capacity - 1)];
    return truth.live && truth.cl_ord_id == cl_ord_id ? &truth : nullptr;
}

// ---- Control ----------------------------------------------------------------

bool SimulationCore::schedule_scripted_command(Timestamp time, const Command& command) noexcept {
    if (state_ == RunState::Completed || state_ == RunState::Failed || state_ == RunState::Cancelled) return false;
    if (time < now_ || time > end_time()) return false;
    SessionId session = 0;
    switch (command.type) {
        case CommandType::NewOrder: session = command.new_order.session; break;
        case CommandType::Cancel: session = command.cancel.session; break;
        case CommandType::Replace: session = command.replace.session; break;
        case CommandType::MassCancel: session = command.mass_cancel.session; break;
        case CommandType::SetPhase: break;
    }
    // Scripted sessions must not impersonate the strategy or an agent.
    if (session == kStrategySessionId) return false;
    for (std::size_t index = 0; index < agent_slots_.size(); ++index)
        if (session == background_agent_session(index)) return false;
    SimulationEvent event;
    event.kind = SimulationEventKind::ScriptedCommand;
    event.data.scripted_command = command;
    return push_simulation(time, event);
}

// ---- Run loop steps -----------------------------------------------------------

void SimulationCore::begin_run() noexcept {
    if (started_) return;
    started_ = true;
    state_ = RunState::Running;
    for (std::size_t index = 0; index < agent_slots_.size(); ++index) {
        SimulationEvent timer;
        timer.kind = SimulationEventKind::BackgroundAgentTimer;
        timer.agent_index = static_cast<std::uint16_t>(index);
        if (!push_simulation(0, timer)) return;
    }
    SimulationEvent start;
    start.kind = SimulationEventKind::StrategyStart;
    if (!push_simulation(0, start)) return;
    ObservationEvent sample;
    sample.kind = ObservationKind::SeriesSample;
    (void)push_observation(0, sample);
}

RunStop SimulationCore::prepare_step(const RunBounds& bounds, std::uint64_t steps_taken) noexcept {
    switch (state_) {
        case RunState::Completed: return RunStop::Completed;
        case RunState::Failed:
        case RunState::Cancelled: {
            if (recorder_.needs_flush()) return RunStop::FlushRequired;
            const RunStop terminal_stop = state_ == RunState::Failed ? RunStop::Failed : RunStop::Cancelled;
            close_quote_exposure(now_);
            if (!continue_finishing() && recorder_.needs_flush()) return RunStop::FlushRequired;
            return terminal_stop;
        }
        default: break;
    }
    if (stop_requested_) {
        state_ = RunState::Cancelled;
        termination_ = TerminationReason::CancelRequested;
        return prepare_step(bounds, steps_taken);
    }
    state_ = RunState::Running;
    if (recorder_.needs_flush()) return RunStop::FlushRequired;
    if (finishing_) {
        if (!continue_finishing()) return state_ == RunState::Failed ? RunStop::Failed : RunStop::FlushRequired;
        return RunStop::Completed;
    }
    if (steps_taken >= bounds.max_steps) {
        state_ = RunState::Paused;
        return RunStop::StepBudgetExhausted;
    }
    // A wake_at to a new time leaves the superseded wakeup in the queue (a
    // heap cannot remove it). Discard it here without counting it as a
    // simulation event, so max_simulation_events and simulation_events do not
    // depend on how often a strategy reschedules its timer.
    while (!simulation_queue_.empty() && simulation_queue_.top_payload().kind == SimulationEventKind::StrategyWakeup &&
           simulation_queue_.top_payload().wakeup_generation != wakeup_generation_)
        (void)simulation_queue_.pop();
    const Timestamp end = end_time();
    const bool has_simulation = !simulation_queue_.empty() && simulation_queue_.top_time() <= end;
    const bool has_observation = !observation_queue_.empty() && observation_queue_.top_time() <= end;
    Timestamp next_time = kNoTime;
    if (has_simulation) next_time = simulation_queue_.top_time();
    // Equal times: simulation first, so observations see the settled state.
    next_is_observation_ = has_observation && (!has_simulation || observation_queue_.top_time() < next_time);
    if (next_is_observation_) next_time = observation_queue_.top_time();
    const bool pause_before_end = bounds.pause_at < end;
    if (pause_before_end && next_time > bounds.pause_at) {
        if (bounds.pause_at > now_) now_ = visible_.now = bounds.pause_at;
        state_ = RunState::Paused;
        return RunStop::PausedAtTime;
    }
    if (next_time == kNoTime) {
        finishing_ = true;
        termination_ = simulation_queue_.empty() ? TerminationReason::EventQueueExhausted : TerminationReason::EndTimeReached;
        close_quote_exposure(end);
        now_ = visible_.now = end;
        if (!continue_finishing()) return state_ == RunState::Failed ? RunStop::Failed : RunStop::FlushRequired;
        return RunStop::Completed;
    }
    if (!next_is_observation_ && simulation_events_processed_ >= config_.limits.max_simulation_events) {
        fail(TerminationReason::EventLimitReached, simulation_events_processed_);
        return RunStop::Failed;
    }
    return RunStop::Continue;
}

void SimulationCore::run_observation_step() noexcept {
    const auto entry = observation_queue_.pop();
    now_ = visible_.now = entry.time;
    ++observation_events_processed_;
    switch (entry.payload.kind) {
        case ObservationKind::SeriesSample: {
            take_series_sample();
            const Timestamp next = now_ + config_.metrics.sample_interval_ns;
            if (next <= end_time()) {
                ObservationEvent sample;
                sample.kind = ObservationKind::SeriesSample;
                (void)push_observation(next, sample);
            }
            break;
        }
        case ObservationKind::MarkoutProbe: process_markout_probe(entry.payload); break;
    }
}

bool SimulationCore::pop_simulation_event(SimulationEvent& strategy_event) noexcept {
    const auto entry = simulation_queue_.pop();
    now_ = visible_.now = entry.time;
    ++simulation_events_processed_;
    switch (entry.payload.kind) {
        case SimulationEventKind::BackgroundAgentTimer: process_background_timer(entry.payload.agent_index); return false;
        case SimulationEventKind::StrategyRequestArrival: process_strategy_arrival(entry.payload.data.request); return false;
        case SimulationEventKind::ScriptedCommand:
            process_exchange_command(entry.payload.data.scripted_command, CommandSource::Scripted, 0, nullptr);
            return false;
        case SimulationEventKind::StrategyStart:
        case SimulationEventKind::StrategyWakeup:
        case SimulationEventKind::MarketDataDelivery:
        case SimulationEventKind::ExecutionReportDelivery:
            strategy_event = entry.payload;
            in_strategy_callback_ = true;
            return true;
    }
    return false;
}

void SimulationCore::deliver_market_data(const MarketDataUpdate& update) noexcept {
    ++metrics_.market_data_delivered;
    metrics_.feed_latency.add(now_ - update.exchange_time);
    visible_.top_of_book = update.top;
    visible_.last_market_data = update;
    if (update.top.two_sided()) {
        visible_.has_seen_two_sided_book = true;
        visible_.last_two_sided_mid_times_two = update.top.mid_times_two();
    }
}

void SimulationCore::deliver_execution_report(const ExecutionReport& report) noexcept {
    ++metrics_.reports_delivered;
    metrics_.ack_latency.add(now_ - report.exchange_time);

    LifecycleRecord row;
    row.time = now_;
    row.reference_time = report.exchange_time;
    row.stage = LifecycleStage::ReportDelivered;
    row.report_kind = report.kind;
    row.cl_ord_id = report.cl_ord_id;
    row.order_id = report.order_id;
    row.match_id = report.match_id;
    row.side = report.side;
    row.price = report.price;
    row.qty = report.qty;
    row.leaves_qty = report.leaves_qty;
    row.cum_qty = report.cum_qty;
    row.liquidity = report.liquidity;
    row.request = report.rejected_command;
    row.reject_reason = report.reject_reason;
    row.cancel_reason = report.cancel_reason;
    row.priority_kept = report.priority_kept;
    push_lifecycle(row);

    StrategyOrderView* view = view_slot(report.cl_ord_id);
    if (view != nullptr && !(view->live && view->cl_ord_id == report.cl_ord_id)) view = nullptr;

    switch (report.kind) {
        case ReportKind::Accepted:
            if (view != nullptr) {
                view->state = OrderViewState::Live;
                view->order_id = report.order_id;
            }
            break;
        case ReportKind::Rejected:
            if (view == nullptr) break;
            if (report.rejected_command == CommandType::NewOrder) {
                release_view(*view);
            } else {
                // A cancel that outran its order (non-FIFO channel) is re-sent once the order rests.
                if (report.rejected_command == CommandType::Cancel && report.reject_reason == RejectReason::UnknownOrder &&
                    view->state == OrderViewState::PendingNew)
                    view->cancel_after_ack = true;
                view->pending = PendingRequest::None;
            }
            break;
        case ReportKind::Rested:
            if (view == nullptr) break;
            view->state = OrderViewState::Live;
            view->order_id = report.order_id;
            view->price = report.price;
            view->leaves_qty = report.leaves_qty;
            if (view->cancel_after_ack && view->pending == PendingRequest::None) {
                view->cancel_after_ack = false;
                if (send_strategy_cancel(*view)) ++metrics_.cancels_resent_after_ack;
            }
            break;
        case ReportKind::Fill:
            visible_.known_position += signed_quantity(report.side, report.qty);
            (report.side == Side::BUY ? visible_.known_bought_qty : visible_.known_sold_qty) += report.qty;
            if (view == nullptr) break;
            view->leaves_qty = report.leaves_qty;
            view->cum_qty = report.cum_qty;
            if (report.leaves_qty == 0) release_view(*view);
            break;
        case ReportKind::Canceled:
            if (view == nullptr) break;
            if (report.leaves_qty == 0) {
                release_view(*view);
            } else {
                view->leaves_qty = report.leaves_qty;
                view->cum_qty = report.cum_qty;
                view->pending = PendingRequest::None;
            }
            break;
        case ReportKind::Replaced:
            if (view == nullptr) break;
            view->order_id = report.order_id;
            view->price = report.price;
            view->total_qty = report.qty;
            view->leaves_qty = report.leaves_qty;
            view->cum_qty = report.cum_qty;
            view->pending = PendingRequest::None;
            view->requested_price = 0;
            view->requested_total_qty = 0;
            break;
    }
}

bool SimulationCore::accept_wakeup(const SimulationEvent& event) noexcept {
    if (event.wakeup_generation != wakeup_generation_) return false;  // superseded by a later wake_at
    pending_wakeup_time_ = kNoTime;
    ++metrics_.strategy_wakeups;
    return true;
}

void SimulationCore::after_strategy_callback() noexcept { in_strategy_callback_ = false; }

// ---- Exchange processing ------------------------------------------------------

void SimulationCore::process_background_timer(std::uint16_t agent_index) noexcept {
    const BackgroundAgentSlot& slot = agent_slots_[agent_index];
    BackgroundExchangeContext context{*this, agent_index};
    Timestamp next = now_ + slot.timer_interval_ns;
    if (slot.kind == BackgroundAgentKind::MarketMaker) {
        market_makers_[slot.position]->on_timer(now_, context);
    } else {
        auto& taker = *noise_takers_[slot.position];
        taker.on_timer(now_, context);
        const Timestamp arrival = taker.next_arrival();
        if (arrival > now_ && arrival != sim::bots::kNever) next = arrival;
    }
    if (state_ == RunState::Failed || next > end_time()) return;
    SimulationEvent timer;
    timer.kind = SimulationEventKind::BackgroundAgentTimer;
    timer.agent_index = agent_index;
    (void)push_simulation(next, timer);
}

void SimulationCore::process_strategy_arrival(const StrategyRequestArrival& request) noexcept {
    const Timestamp travel = now_ - request.decision_time;
    if (request.command.type == CommandType::NewOrder) metrics_.order_latency.add(travel);
    else metrics_.cancel_latency.add(travel);
    process_exchange_command(request.command, CommandSource::Strategy, 0, &request);
}

void SimulationCore::process_exchange_command(const Command& command, CommandSource source, std::uint16_t agent_index,
                                              const StrategyRequestArrival* request) noexcept {
    if (state_ == RunState::Failed) return;
    Command stamped = command;
    stamp_command_time(stamped, now_);
    // Queue evidence describes the book the request meets on arrival, so it
    // is taken before the engine applies the command.
    LifecycleRecord arrival_row;
    if (request != nullptr) arrival_row = capture_strategy_arrival(*request, digests_.command_count);
    event_buffer_.clear();
    if (engine_.handle(stamped, event_buffer_) != HandleResult::Applied) {
        fail(TerminationReason::EngineOutputCapacity, digests_.command_count);
        return;
    }
    const std::uint64_t command_index = digests_.command_count;
    CommandRecord record;
    record.command_index = command_index;
    record.time = now_;
    record.source = source;
    record.agent_index = agent_index;
    record.command = stamped;
    if (!recorder_.commands.push(record)) {
        fail(TerminationReason::RecorderOverflow, command_index);
        return;
    }
    digests_.command_digest = sim::hash_bytes(sim::encode(stamped), digests_.command_digest);
    ++digests_.command_count;
    ++metrics_.engine_commands;
    switch (source) {
        case CommandSource::Strategy: ++metrics_.strategy_commands; break;
        case CommandSource::BackgroundAgent: ++metrics_.background_commands; break;
        case CommandSource::Scripted: ++metrics_.scripted_commands; break;
    }

    const std::span<const Event> events(event_buffer_.begin(), event_buffer_.size());
    for (const Event& event : events) digests_.event_digest = sim::hash_event(event, digests_.event_digest);
    digests_.event_count += events.size();
    metrics_.engine_events += events.size();

    // Background agents see every batch immediately (zero-latency market model).
    for (auto& maker : market_makers_) maker->on_events(events);
    for (auto& taker : noise_takers_) taker->on_events(events);

    if (request != nullptr) push_lifecycle(arrival_row);

    std::uint32_t trade_count = 0;
    std::uint64_t traded_qty = 0;
    Price last_trade_price = 0;
    Aggressor last_trade_aggressor = Aggressor::None;
    bool strategy_touched = false;
    for (const Event& event : events) {
        if (event.type == EventType::Fill) {
            ++trade_count;
            traded_qty += event.fill.qty;
            last_trade_price = event.fill.price;
            last_trade_aggressor = event.fill.aggressor;
            ++metrics_.market_trades;
            metrics_.market_traded_qty += event.fill.qty;
        }
        strategy_touched |= handle_strategy_event(event, command_index);
        if (state_ == RunState::Failed) return;
    }
    if (strategy_touched) update_quote_exposure();

    const TopOfBook top_after = exchange_top_of_book();
    // Fills above used the reference mid from before this command.
    if (top_after.two_sided()) {
        has_reference_mid_ = true;
        reference_mid_times_two_ = top_after.mid_times_two();
    }
    if (trade_count > 0 || !(top_after == last_published_top_))
        publish_market_data(top_after, trade_count, traded_qty, last_trade_price, last_trade_aggressor);
}

LifecycleRecord SimulationCore::capture_strategy_arrival(const StrategyRequestArrival& request,
                                                         std::uint64_t command_index) noexcept {
    LifecycleRecord row;
    row.time = now_;
    row.reference_time = request.decision_time;
    row.command_index = command_index;
    row.cl_ord_id = request.cl_ord_id;
    row.stage = LifecycleStage::ExchangeArrival;
    row.request = request.command.type;
    row.side = request.side;
    if (request.command.type == CommandType::NewOrder) {
        const NewOrder& order = request.command.new_order;
        row.side = order.side;
        row.tif = order.tif;
        row.price = order.price;
        row.qty = order.qty;
        row.post_only = order.post_only;
        // Not in the book yet: the evidence is the queue it would join at its
        // price (every order there is ahead of it). Not a prediction that an
        // aggressive order will rest.
        if (order.type == OrdType::Limit) {
            const LevelQueue level = measure_level_queue(order.side, order.price, 0);
            row.has_queue_position = true;
            row.queue_orders_ahead = level.orders_ahead;
            row.queue_ahead_qty = level.qty_ahead;
            row.level_qty = level.level_qty;
        }
        return row;
    }
    if (request.command.type == CommandType::Replace) {
        row.price = request.command.replace.price;
        row.qty = request.command.replace.qty;
    }
    // Cancel or replace: the target order as it stands when the request
    // arrives, with its place in the FIFO queue at its current price. An order
    // already filled or canceled has no position.
    if (const auto info = engine_.find_order(kStrategySessionId, request.cl_ord_id)) {
        row.order_id = info->order_id;
        row.leaves_qty = info->leaves_qty;
        row.cum_qty = info->cum_qty;
        const LevelQueue level = measure_level_queue(info->side, info->price, info->order_id);
        row.has_queue_position = level.found;
        row.queue_orders_ahead = level.orders_ahead;
        row.queue_ahead_qty = level.qty_ahead;
        row.level_qty = level.level_qty;
    }
    return row;
}

SimulationCore::LevelQueue SimulationCore::measure_level_queue(Side side, Price price, OrderId order_id) noexcept {
    LevelQueue level;
    const std::size_t count = engine_.level_orders(instrument_id_, side, price, level_order_ids_.get(), level_order_capacity_);
    for (std::size_t position = 0; position < count; ++position) {
        const auto resting = engine_.find_order(level_order_ids_[position]);
        if (!resting) continue;
        level.level_qty += resting->leaves_qty;
        if (order_id != 0 && level_order_ids_[position] == order_id) {
            level.found = true;
        } else if (!level.found) {
            level.qty_ahead += resting->leaves_qty;
            ++level.orders_ahead;
        }
    }
    return level;
}

bool SimulationCore::handle_strategy_event(const Event& event, std::uint64_t command_index) noexcept {
    LifecycleRecord row;
    row.time = now_;
    row.command_index = command_index;
    ExecutionReport report;
    report.exchange_time = now_;
    switch (event.type) {
        case EventType::Accepted: {
            const auto& accepted = event.accepted;
            if (accepted.session != kStrategySessionId) return false;
            ExchangeTruthOrder* truth = truth_slot(accepted.cl_ord_id);
            if (truth == nullptr) {
                fail(TerminationReason::OrderTableCorrupt, accepted.cl_ord_id);
                return false;
            }
            truth->accepted = true;
            truth->order_id = accepted.order_id;
            ++metrics_.orders_accepted;
            metrics_.accepted_order_qty += accepted.qty;
            row.stage = LifecycleStage::ExchangeAccepted;
            row.cl_ord_id = accepted.cl_ord_id;
            row.order_id = accepted.order_id;
            row.side = accepted.side;
            row.tif = accepted.tif;
            row.post_only = accepted.post_only;
            row.price = accepted.price;
            row.qty = accepted.qty;
            row.leaves_qty = accepted.qty;
            report.kind = ReportKind::Accepted;
            report.cl_ord_id = accepted.cl_ord_id;
            report.order_id = accepted.order_id;
            report.side = accepted.side;
            report.price = accepted.price;
            report.qty = accepted.qty;
            report.leaves_qty = accepted.qty;
            break;
        }
        case EventType::Rejected: {
            const auto& rejected = event.rejected;
            if (rejected.session != kStrategySessionId) return false;
            const ClOrdId target = rejected.cmd == CommandType::NewOrder ? rejected.cl_ord_id : rejected.orig_cl_ord_id;
            if (const ExchangeTruthOrder* truth = truth_order(target)) row.side = truth->side;
            if (rejected.cmd == CommandType::NewOrder) {
                ++metrics_.orders_rejected;
                if (ExchangeTruthOrder* truth = truth_slot(target)) *truth = ExchangeTruthOrder{};
            } else if (rejected.cmd == CommandType::Cancel) {
                ++metrics_.cancel_requests_rejected;
            } else if (rejected.cmd == CommandType::Replace) {
                ++metrics_.replace_requests_rejected;
            }
            row.stage = LifecycleStage::ExchangeRejected;
            row.cl_ord_id = target;
            row.order_id = rejected.order_id;
            row.request = rejected.cmd;
            row.reject_reason = rejected.reason;
            report.kind = ReportKind::Rejected;
            report.cl_ord_id = target;
            report.order_id = rejected.order_id;
            report.side = row.side;
            report.rejected_command = rejected.cmd;
            report.reject_reason = rejected.reason;
            break;
        }
        case EventType::Rested: {
            const auto& rested = event.rested;
            if (rested.session != kStrategySessionId) return false;
            ExchangeTruthOrder* truth = truth_slot(rested.cl_ord_id);
            if (truth == nullptr) {
                fail(TerminationReason::OrderTableCorrupt, rested.cl_ord_id);
                return false;
            }
            truth->resting = true;
            truth->price = rested.price;
            truth->leaves_qty = rested.leaves_qty;
            truth->order_id = rested.order_id;
            row.stage = LifecycleStage::ExchangeRested;
            row.cl_ord_id = rested.cl_ord_id;
            row.order_id = rested.order_id;
            row.side = rested.side;
            row.price = rested.price;
            row.leaves_qty = rested.leaves_qty;
            row.cum_qty = truth->cum_qty;
            report.kind = ReportKind::Rested;
            report.cl_ord_id = rested.cl_ord_id;
            report.order_id = rested.order_id;
            report.side = rested.side;
            report.price = rested.price;
            report.qty = truth->total_qty;
            report.leaves_qty = rested.leaves_qty;
            report.cum_qty = truth->cum_qty;
            break;
        }
        case EventType::Fill: {
            bool touched = false;
            if (event.fill.buy.session == kStrategySessionId) {
                handle_strategy_fill(event, event.fill.buy, Side::BUY, command_index);
                touched = true;
            }
            if (state_ != RunState::Failed && event.fill.sell.session == kStrategySessionId) {
                handle_strategy_fill(event, event.fill.sell, Side::SELL, command_index);
                touched = true;
            }
            return touched;
        }
        case EventType::Canceled: {
            const auto& canceled = event.canceled;
            if (canceled.session != kStrategySessionId) return false;
            ExchangeTruthOrder* truth = truth_slot(canceled.cl_ord_id);
            if (truth == nullptr) {
                fail(TerminationReason::OrderTableCorrupt, canceled.cl_ord_id);
                return false;
            }
            truth->leaves_qty = canceled.leaves_qty;
            truth->cum_qty = canceled.cum_qty;
            if (canceled.leaves_qty == 0) {
                switch (canceled.reason) {
                    case CancelReason::UserRequested: ++metrics_.orders_canceled_by_request; break;
                    case CancelReason::ImmediateOrCancel:
                    case CancelReason::FillOrKill: ++metrics_.ioc_remainders_canceled; break;
                    case CancelReason::SelfTradePrevention: ++metrics_.self_trade_cancels; break;
                    default: break;
                }
                if (canceled.cum_qty > 0) ++metrics_.orders_ended_partially_filled;
                *truth = ExchangeTruthOrder{};
            }
            row.stage = LifecycleStage::ExchangeCanceled;
            row.cl_ord_id = canceled.cl_ord_id;
            row.order_id = canceled.order_id;
            row.side = canceled.side;
            row.price = canceled.price;
            row.qty = canceled.canceled_qty;
            row.leaves_qty = canceled.leaves_qty;
            row.cum_qty = canceled.cum_qty;
            row.cancel_reason = canceled.reason;
            report.kind = ReportKind::Canceled;
            report.cl_ord_id = canceled.cl_ord_id;
            report.order_id = canceled.order_id;
            report.side = canceled.side;
            report.price = canceled.price;
            report.qty = canceled.canceled_qty;
            report.leaves_qty = canceled.leaves_qty;
            report.cum_qty = canceled.cum_qty;
            report.cancel_reason = canceled.reason;
            break;
        }
        case EventType::Replaced: {
            const auto& replaced = event.replaced;
            if (replaced.session != kStrategySessionId) return false;
            ExchangeTruthOrder* truth = truth_slot(replaced.orig_cl_ord_id);
            if (truth == nullptr || replaced.cl_ord_id != replaced.orig_cl_ord_id) {
                fail(TerminationReason::OrderTableCorrupt, replaced.orig_cl_ord_id);
                return false;
            }
            ++metrics_.replaces_applied;
            if (replaced.priority_kept) ++metrics_.replaces_priority_kept;
            if (replaced.qty > truth->total_qty) metrics_.replace_added_qty += replaced.qty - truth->total_qty;
            truth->order_id = replaced.new_order_id;
            truth->price = replaced.price;
            truth->total_qty = replaced.qty;
            truth->leaves_qty = replaced.leaves_qty;
            truth->cum_qty = replaced.cum_qty;
            // Losing priority takes the order off the book; Rested puts it back.
            if (!replaced.priority_kept) truth->resting = false;
            row.stage = LifecycleStage::ExchangeReplaced;
            row.cl_ord_id = replaced.cl_ord_id;
            row.order_id = replaced.new_order_id;
            row.side = replaced.side;
            row.price = replaced.price;
            row.qty = replaced.qty;
            row.leaves_qty = replaced.leaves_qty;
            row.cum_qty = replaced.cum_qty;
            row.priority_kept = replaced.priority_kept;
            report.kind = ReportKind::Replaced;
            report.cl_ord_id = replaced.cl_ord_id;
            report.order_id = replaced.new_order_id;
            report.side = replaced.side;
            report.price = replaced.price;
            report.qty = replaced.qty;
            report.leaves_qty = replaced.leaves_qty;
            report.cum_qty = replaced.cum_qty;
            report.priority_kept = replaced.priority_kept;
            break;
        }
        default: return false;
    }
    push_lifecycle(row);
    schedule_report(report);
    return true;
}

void SimulationCore::handle_strategy_fill(const Event& event, const FillLeg& leg, Side side, std::uint64_t command_index) noexcept {
    ExchangeTruthOrder* truth = truth_slot(leg.cl_ord_id);
    if (truth == nullptr) {
        fail(TerminationReason::OrderTableCorrupt, leg.cl_ord_id);
        return;
    }
    const Price price = event.fill.price;
    const Quantity qty = event.fill.qty;
    const Liquidity liquidity = fill_liquidity(event.fill.aggressor, side);
    const std::int64_t fee_per_share = liquidity == Liquidity::Maker   ? config_.fees.maker_fee_per_share
                                       : liquidity == Liquidity::Taker ? config_.fees.taker_fee_per_share
                                                                       : config_.fees.auction_fee_per_share;
    const std::int64_t fee = fee_per_share * static_cast<std::int64_t>(qty);
    const LedgerStatus status = ledger_.record_fill(side, price, qty, fee_per_share);
    if (status != LedgerStatus::Ok) {
        fail(TerminationReason::LedgerRejectedFill, static_cast<std::uint64_t>(status));
        return;
    }
    const LedgerState& ledger_state = ledger_.state();

    if (!truth->filled_once) {
        truth->filled_once = true;
        ++metrics_.orders_with_fill;
        metrics_.time_to_first_fill.add(now_ - truth->decision_time);
    }
    truth->leaves_qty = leg.leaves_qty;
    truth->cum_qty = leg.cum_qty;
    const Timestamp decision_time = truth->decision_time;
    if (leg.leaves_qty == 0) {
        ++metrics_.orders_filled_completely;
        *truth = ExchangeTruthOrder{};
    }

    FillRecord fill;
    fill.fill_index = digests_.fill_count;
    fill.exchange_time = now_;
    fill.command_index = command_index;
    fill.match_id = event.fill.match_id;
    fill.cl_ord_id = leg.cl_ord_id;
    fill.order_id = leg.order_id;
    fill.side = side;
    fill.liquidity = liquidity;
    fill.price = price;
    fill.qty = qty;
    fill.fee_per_share = fee_per_share;
    fill.fee = fee;
    fill.cash_after = ledger_state.cash;
    fill.position_after = ledger_state.position;
    fill.realized_pnl_after = ledger_state.gross_realized_pnl;
    fill.fees_after = ledger_state.fees;
    fill.has_mid_before = has_reference_mid_;
    if (fill.has_mid_before) {
        fill.mid_before_times_two = reference_mid_times_two_;
        fill.spread_capture_times_two = edge_versus_mid_times_two(side, fill.mid_before_times_two, price, qty);
    }
    if (!recorder_.fills.push(fill)) {
        fail(TerminationReason::RecorderOverflow, fill.fill_index);
        return;
    }
    digests_.ledger_digest = hash_fill_record(fill, digests_.ledger_digest);
    ++digests_.fill_count;

    ++metrics_.fill_count;
    metrics_.filled_qty += qty;
    switch (liquidity) {
        case Liquidity::Maker:
            ++metrics_.maker_fill_count;
            metrics_.maker_qty += qty;
            metrics_.maker_fees += fee;
            break;
        case Liquidity::Taker:
            ++metrics_.taker_fill_count;
            metrics_.taker_qty += qty;
            metrics_.taker_fees += fee;
            break;
        case Liquidity::Auction:
            ++metrics_.auction_fill_count;
            metrics_.auction_qty += qty;
            metrics_.auction_fees += fee;
            break;
    }
    const std::int64_t notional = price * static_cast<std::int64_t>(qty);
    (side == Side::BUY ? metrics_.buy_notional : metrics_.sell_notional) += notional;
    if (fill.has_mid_before) {
        ++metrics_.spread_capture_fills;
        metrics_.spread_capture_qty += qty;
        metrics_.spread_capture_total_times_two += fill.spread_capture_times_two;
    } else {
        ++metrics_.spread_capture_unavailable;
    }
    metrics_.max_position = std::max(metrics_.max_position, ledger_state.position);
    metrics_.min_position = std::min(metrics_.min_position, ledger_state.position);
    if (ledger_state.position > config_.risk.max_long_position || -ledger_state.position > config_.risk.max_short_position)
        ++metrics_.limit_breaches;

    auto& parent = metrics_.parent;
    if (parent.declared && decision_time >= parent.decision_time) {
        if (side == parent.side) {
            if (parent.filled_qty == 0) parent.first_fill_time = now_;
            parent.last_fill_time = now_;
            parent.filled_qty += qty;
            parent.fill_notional += notional;
            parent.fees += fee;
            parent.market_volume_at_last_fill = metrics_.market_traded_qty;
        } else {
            parent.opposite_side_qty += qty;
        }
    }

    for (std::size_t horizon = 0; horizon < config_.metrics.markout_horizon_count; ++horizon) {
        ObservationEvent probe;
        probe.kind = ObservationKind::MarkoutProbe;
        probe.horizon_index = static_cast<std::uint8_t>(horizon);
        probe.side = side;
        probe.qty = qty;
        probe.price = price;
        probe.fill_index = fill.fill_index;
        probe.fill_time = now_;
        if (!push_observation(now_ + config_.metrics.markout_horizons_ns[horizon], probe)) return;
    }

    LifecycleRecord row;
    row.time = now_;
    row.command_index = command_index;
    row.stage = LifecycleStage::ExchangeFill;
    row.cl_ord_id = leg.cl_ord_id;
    row.order_id = leg.order_id;
    row.match_id = event.fill.match_id;
    row.side = side;
    row.price = price;
    row.qty = qty;
    row.leaves_qty = leg.leaves_qty;
    row.cum_qty = leg.cum_qty;
    row.fee = fee;
    row.liquidity = liquidity;
    push_lifecycle(row);

    ExecutionReport report;
    report.kind = ReportKind::Fill;
    report.exchange_time = now_;
    report.cl_ord_id = leg.cl_ord_id;
    report.order_id = leg.order_id;
    report.side = side;
    report.price = price;
    report.qty = qty;
    report.leaves_qty = leg.leaves_qty;
    report.cum_qty = leg.cum_qty;
    report.liquidity = liquidity;
    report.match_id = event.fill.match_id;
    schedule_report(report);
}

void SimulationCore::schedule_report(const ExecutionReport& report) noexcept {
    if (state_ == RunState::Failed) return;
    Timestamp delivery = now_ + delay_with_jitter(config_.latency.ack_ns, config_.latency.ack_jitter_ns);
    delivery = std::max(delivery, ack_channel_last_delivery_);
    ack_channel_last_delivery_ = delivery;
    SimulationEvent event;
    event.kind = SimulationEventKind::ExecutionReportDelivery;
    event.data.report = report;
    if (push_simulation(delivery, event)) ++metrics_.reports_scheduled;
}

void SimulationCore::publish_market_data(const TopOfBook& top, std::uint32_t trade_count, std::uint64_t traded_qty,
                                         Price last_price, Aggressor last_aggressor) noexcept {
    MarketDataUpdate update;
    update.sequence = ++market_data_sequence_;
    update.exchange_time = now_;
    update.top = top;
    update.trade_count = trade_count;
    update.traded_qty = traded_qty;
    update.last_trade_price = last_price;
    update.last_trade_aggressor = last_aggressor;
    last_published_top_ = top;
    Timestamp delivery = now_ + delay_with_jitter(config_.latency.feed_ns, config_.latency.feed_jitter_ns);
    delivery = std::max(delivery, feed_channel_last_delivery_);
    feed_channel_last_delivery_ = delivery;
    SimulationEvent event;
    event.kind = SimulationEventKind::MarketDataDelivery;
    event.data.market_data = update;
    if (push_simulation(delivery, event)) ++metrics_.market_data_published;
}

// ---- Quote exposure, samples, markouts ------------------------------------------

void SimulationCore::close_quote_exposure(Timestamp time) noexcept {
    if (time <= metrics_.exposure_since) return;
    const Timestamp elapsed = time - metrics_.exposure_since;
    if (metrics_.bid_resting) metrics_.bid_exposure_ns += elapsed;
    if (metrics_.ask_resting) metrics_.ask_exposure_ns += elapsed;
    if (metrics_.bid_resting && metrics_.ask_resting) metrics_.two_sided_exposure_ns += elapsed;
    if (metrics_.bid_resting || metrics_.ask_resting) metrics_.any_side_exposure_ns += elapsed;
    metrics_.exposure_since = time;
}

void SimulationCore::update_quote_exposure() noexcept {
    close_quote_exposure(now_);
    bool bid = false;
    bool ask = false;
    for (std::size_t slot = 0; slot < visible_.order_capacity; ++slot) {
        const auto& truth = truth_orders_[slot];
        if (!truth.live || !truth.resting || truth.leaves_qty == 0) continue;
        (truth.side == Side::BUY ? bid : ask) = true;
    }
    metrics_.bid_resting = bid;
    metrics_.ask_resting = ask;
}

void SimulationCore::take_series_sample() noexcept {
    SeriesSample sample;
    sample.time = now_;
    sample.exchange = exchange_top_of_book();
    sample.visible = visible_.top_of_book;
    const LedgerState& ledger_state = ledger_.state();
    sample.position = ledger_state.position;
    sample.known_position = visible_.known_position;
    sample.cash = ledger_state.cash;
    sample.realized_pnl = ledger_state.gross_realized_pnl;
    sample.fees = ledger_state.fees;
    if (const auto mark = mark_times_two()) {
        const DoubledValuation valuation = value_at_mark(ledger_state, config_.accounting.initial_cash, *mark);
        sample.has_mark = true;
        sample.mark_times_two = *mark;
        sample.has_equity = true;
        sample.unrealized_pnl_times_two = valuation.unrealized_pnl_times_two;
        sample.equity_times_two = valuation.equity_times_two;
    } else {
        ++metrics_.samples_without_mark;
        if (ledger_state.position == 0) {  // nothing to value: equity is cash
            sample.has_equity = true;
            sample.equity_times_two = 2 * ledger_state.cash;
        }
    }
    for (std::size_t slot = 0; slot < visible_.order_capacity; ++slot) {
        const auto& truth = truth_orders_[slot];
        if (!truth.live || !truth.resting) continue;
        (truth.side == Side::BUY ? sample.open_bid_qty : sample.open_ask_qty) += truth.leaves_qty;
    }
    if (sample.has_equity) {
        if (!metrics_.has_equity || sample.equity_times_two > metrics_.peak_equity_times_two)
            metrics_.peak_equity_times_two = sample.equity_times_two;
        metrics_.has_equity = true;
        const std::int64_t drawdown_times_two = metrics_.peak_equity_times_two - sample.equity_times_two;
        if (drawdown_times_two > metrics_.max_drawdown_times_two) {
            metrics_.max_drawdown_times_two = drawdown_times_two;
            metrics_.max_drawdown_time = now_;
        }
    }
    last_sample_time_ = now_;
    if (!recorder_.samples.push(sample)) fail(TerminationReason::RecorderOverflow, now_);
}

void SimulationCore::process_markout_probe(const ObservationEvent& probe) noexcept {
    MarkoutRecord record;
    record.fill_index = probe.fill_index;
    record.fill_time = probe.fill_time;
    record.horizon_index = probe.horizon_index;
    record.horizon_ns = config_.metrics.markout_horizons_ns[probe.horizon_index];
    record.side = probe.side;
    record.price = probe.price;
    record.qty = probe.qty;
    auto& accumulator = metrics_.markouts[probe.horizon_index];
    if (has_reference_mid_) {
        record.status = MarkoutStatus::Measured;
        record.mid_times_two = reference_mid_times_two_;
        record.markout_times_two = edge_versus_mid_times_two(probe.side, record.mid_times_two, probe.price, probe.qty);
        ++accumulator.measured;
        accumulator.measured_qty += probe.qty;
        accumulator.total_times_two += record.markout_times_two;
    } else {
        record.status = MarkoutStatus::Unavailable;
        ++accumulator.unavailable;
    }
    if (!recorder_.markouts.push(record)) fail(TerminationReason::RecorderOverflow, probe.fill_index);
}

// Resumable end-of-run work: terminal sample, then every markout probe still
// queued is censored (its horizon is past the end). Returns false when the
// recorder must be flushed first (or the run failed).
bool SimulationCore::continue_finishing() noexcept {
    const RunState terminal_state = state_;
    if (!terminal_sample_taken_) {
        terminal_sample_taken_ = true;
        if (last_sample_time_ != end_time()) take_series_sample();
        if (state_ == RunState::Failed && terminal_state != RunState::Failed) return false;
    }
    while (!observation_queue_.empty()) {
        if (recorder_.needs_flush()) return false;
        const auto entry = observation_queue_.pop();
        if (entry.payload.kind != ObservationKind::MarkoutProbe) continue;
        MarkoutRecord record;
        record.fill_index = entry.payload.fill_index;
        record.fill_time = entry.payload.fill_time;
        record.horizon_index = entry.payload.horizon_index;
        record.horizon_ns = config_.metrics.markout_horizons_ns[entry.payload.horizon_index];
        record.status = MarkoutStatus::Censored;
        record.side = entry.payload.side;
        record.price = entry.payload.price;
        record.qty = entry.payload.qty;
        ++metrics_.markouts[entry.payload.horizon_index].censored;
        if (!recorder_.markouts.push(record)) {
            fail(TerminationReason::RecorderOverflow, record.fill_index);
            return false;
        }
    }
    state_ = terminal_state == RunState::Failed || terminal_state == RunState::Cancelled ? terminal_state : RunState::Completed;
    return true;
}

// ---- Strategy host actions --------------------------------------------------------

std::uint64_t SimulationCore::worst_case_open_qty(Side side, ClOrdId excluded_order) const noexcept {
    std::uint64_t total = 0;
    for (std::size_t slot = 0; slot < visible_.order_capacity; ++slot) {
        const auto& view = visible_.orders[slot];
        if (!view.live || view.side != side || view.cl_ord_id == excluded_order) continue;
        total += view.worst_case_remaining_qty();
    }
    return total;
}

std::int64_t SimulationCore::strategy_position_headroom(Side side, ClOrdId excluded_order) const noexcept {
    const std::int64_t limit = side == Side::BUY ? config_.risk.max_long_position : config_.risk.max_short_position;
    const std::int64_t exposure = (side == Side::BUY ? visible_.known_position : -visible_.known_position) +
                                  static_cast<std::int64_t>(worst_case_open_qty(side, excluded_order));
    return exposure >= limit ? 0 : limit - exposure;
}

bool SimulationCore::valid_price(Price price) const noexcept {
    const auto& instrument = config_.instrument;
    return price >= instrument.min_price && price <= instrument.max_price && price % instrument.tick_size == 0;
}

bool SimulationCore::risk_reject(RiskRejectReason reason, CommandType request, Side side, Price price, Quantity qty,
                                 ClOrdId cl_ord_id) noexcept {
    ++metrics_.risk_rejects;
    ++metrics_.risk_rejects_by_reason[static_cast<std::size_t>(reason)];
    visible_.last_risk_reject = reason;
    LifecycleRecord row;
    row.time = now_;
    row.stage = LifecycleStage::RiskRejected;
    row.request = request;
    row.side = side;
    row.price = price;
    row.qty = qty;
    row.cl_ord_id = cl_ord_id;
    row.risk_reason = reason;
    push_lifecycle(row);
    return false;
}

Timestamp SimulationCore::delay_with_jitter(Timestamp base, Timestamp jitter) noexcept {
    if (jitter == 0) return base;
    return base + static_cast<Timestamp>(latency_rng_.between(0, static_cast<std::int64_t>(jitter)));
}

Timestamp SimulationCore::schedule_order_channel(Timestamp latency, Timestamp jitter) noexcept {
    Timestamp arrival = now_ + config_.latency.strategy_compute_ns + delay_with_jitter(latency, jitter);
    if (config_.latency.order_channel_fifo) {
        arrival = std::max(arrival, order_channel_last_arrival_);
        order_channel_last_arrival_ = arrival;
    }
    return arrival;
}

bool SimulationCore::push_simulation(Timestamp time, const SimulationEvent& event) noexcept {
    if (!simulation_queue_.push(time, event)) {
        fail(TerminationReason::SimulationQueueFull, time);
        return false;
    }
    return true;
}

bool SimulationCore::push_observation(Timestamp time, const ObservationEvent& event) noexcept {
    if (!observation_queue_.push(time, event)) {
        fail(TerminationReason::ObservationQueueFull, time);
        return false;
    }
    return true;
}

void SimulationCore::push_lifecycle(const LifecycleRecord& record) noexcept {
    if (!recorder_.lifecycle.push(record)) fail(TerminationReason::RecorderOverflow, record.time);
}

StrategyOrderView* SimulationCore::view_slot(ClOrdId cl_ord_id) noexcept {
    if (cl_ord_id == 0) return nullptr;
    return &visible_.orders[cl_ord_id & (visible_.order_capacity - 1)];
}

ExchangeTruthOrder* SimulationCore::truth_slot(ClOrdId cl_ord_id) noexcept {
    if (cl_ord_id == 0) return nullptr;
    auto& truth = truth_orders_[cl_ord_id & (visible_.order_capacity - 1)];
    return truth.live && truth.cl_ord_id == cl_ord_id ? &truth : nullptr;
}

void SimulationCore::release_view(StrategyOrderView& view) noexcept {
    if (view.live && visible_.open_order_count > 0) --visible_.open_order_count;
    view = StrategyOrderView{};
}

void SimulationCore::fail(TerminationReason reason, std::uint64_t detail) noexcept {
    if (state_ == RunState::Failed) return;
    state_ = RunState::Failed;
    termination_ = reason;
    failure_detail_ = detail;
}

ClOrdId SimulationCore::strategy_submit_limit_order(Side side, Price price, Quantity qty, TimeInForce tif,
                                                    bool post_only) noexcept {
    const auto reject = [&](RiskRejectReason reason) {
        (void)risk_reject(reason, CommandType::NewOrder, side, price, qty, 0);
        return ClOrdId{0};
    };
    if (!in_strategy_callback_ || state_ != RunState::Running) return reject(RiskRejectReason::RunNotActive);
    if (qty == 0 || qty % config_.instrument.lot_size != 0) return reject(RiskRejectReason::InvalidQuantity);
    if (qty > config_.risk.max_order_qty) return reject(RiskRejectReason::MaxOrderQuantity);
    if (!valid_price(price)) return reject(RiskRejectReason::InvalidPrice);
    if (visible_.open_order_count >= config_.risk.max_open_orders) return reject(RiskRejectReason::MaxOpenOrders);
    if (strategy_position_headroom(side, 0) < static_cast<std::int64_t>(qty))
        return reject(side == Side::BUY ? RiskRejectReason::MaxLongPosition : RiskRejectReason::MaxShortPosition);

    // Next token whose slot is free in both the view and the truth table.
    ClOrdId cl_ord_id = 0;
    for (std::size_t probe = 0; probe < visible_.order_capacity; ++probe) {
        const ClOrdId candidate = next_cl_ord_id_++;
        const std::size_t slot = candidate & (visible_.order_capacity - 1);
        if (!visible_.orders[slot].live && !truth_orders_[slot].live) {
            cl_ord_id = candidate;
            break;
        }
    }
    if (cl_ord_id == 0) return reject(RiskRejectReason::MaxOpenOrders);

    const Timestamp arrival = schedule_order_channel(config_.latency.order_ns, config_.latency.order_jitter_ns);
    NewOrder order;
    order.ts = arrival;
    order.instrument = instrument_id_;
    order.session = kStrategySessionId;
    order.owner = kStrategyOwnerId;
    order.cl_ord_id = cl_ord_id;
    order.side = side;
    order.type = OrdType::Limit;
    order.tif = tif;
    order.stp = config_.strategy.stp;
    order.post_only = post_only;
    order.price = price;
    order.qty = qty;
    SimulationEvent event;
    event.kind = SimulationEventKind::StrategyRequestArrival;
    event.data.request.command = Command{order};
    event.data.request.decision_time = now_;
    event.data.request.cl_ord_id = cl_ord_id;
    event.data.request.side = side;
    if (!push_simulation(arrival, event)) return reject(RiskRejectReason::QueueCapacity);

    const std::size_t slot = cl_ord_id & (visible_.order_capacity - 1);
    StrategyOrderView& view = visible_.orders[slot];
    view = StrategyOrderView{};
    view.live = true;
    view.state = OrderViewState::PendingNew;
    view.side = side;
    view.tif = tif;
    view.cl_ord_id = cl_ord_id;
    view.price = price;
    view.total_qty = qty;
    view.leaves_qty = qty;
    view.submitted_at = now_;
    ++visible_.open_order_count;

    ExchangeTruthOrder& truth = truth_orders_[slot];
    truth = ExchangeTruthOrder{};
    truth.live = true;
    truth.side = side;
    truth.price = price;
    truth.total_qty = qty;
    truth.leaves_qty = qty;
    truth.cl_ord_id = cl_ord_id;
    truth.decision_time = now_;

    ++metrics_.new_orders_submitted;
    visible_.last_risk_reject = RiskRejectReason::None;
    LifecycleRecord row;
    row.time = now_;
    row.reference_time = arrival;
    row.stage = LifecycleStage::StrategySubmitted;
    row.request = CommandType::NewOrder;
    row.cl_ord_id = cl_ord_id;
    row.side = side;
    row.tif = tif;
    row.price = price;
    row.qty = qty;
    row.leaves_qty = qty;
    row.post_only = post_only;
    push_lifecycle(row);
    return cl_ord_id;
}

bool SimulationCore::send_strategy_cancel(StrategyOrderView& view) noexcept {
    const Timestamp arrival = schedule_order_channel(config_.latency.cancel_ns, config_.latency.cancel_jitter_ns);
    CancelOrder cancel;
    cancel.ts = arrival;
    cancel.session = kStrategySessionId;
    cancel.orig_cl_ord_id = view.cl_ord_id;
    SimulationEvent event;
    event.kind = SimulationEventKind::StrategyRequestArrival;
    event.data.request.command = Command{cancel};
    event.data.request.decision_time = now_;
    event.data.request.cl_ord_id = view.cl_ord_id;
    event.data.request.side = view.side;
    if (!push_simulation(arrival, event)) return false;
    view.pending = PendingRequest::Cancel;
    ++metrics_.cancel_requests_submitted;
    LifecycleRecord row;
    row.time = now_;
    row.reference_time = arrival;
    row.stage = LifecycleStage::StrategySubmitted;
    row.request = CommandType::Cancel;
    row.cl_ord_id = view.cl_ord_id;
    row.order_id = view.order_id;
    row.side = view.side;
    row.price = view.price;
    row.leaves_qty = view.leaves_qty;
    row.cum_qty = view.cum_qty;
    push_lifecycle(row);
    return true;
}

bool SimulationCore::strategy_cancel_order(ClOrdId cl_ord_id) noexcept {
    StrategyOrderView* view = view_slot(cl_ord_id);
    if (view != nullptr && !(view->live && view->cl_ord_id == cl_ord_id)) view = nullptr;
    const Side side = view != nullptr ? view->side : Side::BUY;
    const Price price = view != nullptr ? view->price : 0;
    if (!in_strategy_callback_ || state_ != RunState::Running)
        return risk_reject(RiskRejectReason::RunNotActive, CommandType::Cancel, side, price, 0, cl_ord_id);
    if (view == nullptr) return risk_reject(RiskRejectReason::UnknownOrder, CommandType::Cancel, side, price, 0, cl_ord_id);
    if (view->pending != PendingRequest::None || view->cancel_after_ack)
        return risk_reject(RiskRejectReason::RequestInFlight, CommandType::Cancel, side, price, 0, cl_ord_id);
    if (!send_strategy_cancel(*view))
        return risk_reject(RiskRejectReason::QueueCapacity, CommandType::Cancel, side, price, 0, cl_ord_id);
    visible_.last_risk_reject = RiskRejectReason::None;
    return true;
}

bool SimulationCore::strategy_replace_order(ClOrdId cl_ord_id, Price price, Quantity new_total_qty) noexcept {
    StrategyOrderView* view = view_slot(cl_ord_id);
    if (view != nullptr && !(view->live && view->cl_ord_id == cl_ord_id)) view = nullptr;
    const Side side = view != nullptr ? view->side : Side::BUY;
    const auto reject = [&](RiskRejectReason reason) {
        return risk_reject(reason, CommandType::Replace, side, price, new_total_qty, cl_ord_id);
    };
    if (!in_strategy_callback_ || state_ != RunState::Running) return reject(RiskRejectReason::RunNotActive);
    if (view == nullptr) return reject(RiskRejectReason::UnknownOrder);
    if (view->state == OrderViewState::PendingNew) return reject(RiskRejectReason::NotYetAcknowledged);
    if (view->pending != PendingRequest::None || view->cancel_after_ack) return reject(RiskRejectReason::RequestInFlight);
    if (new_total_qty <= view->cum_qty || new_total_qty % config_.instrument.lot_size != 0)
        return reject(RiskRejectReason::InvalidQuantity);
    const Quantity new_remaining = new_total_qty - view->cum_qty;
    if (new_remaining > config_.risk.max_order_qty) return reject(RiskRejectReason::MaxOrderQuantity);
    if (!valid_price(price)) return reject(RiskRejectReason::InvalidPrice);
    // While in flight the order may still execute its old leaves or its new remainder.
    const Quantity worst_remaining = std::max(view->leaves_qty, new_remaining);
    if (strategy_position_headroom(side, cl_ord_id) < static_cast<std::int64_t>(worst_remaining))
        return reject(side == Side::BUY ? RiskRejectReason::MaxLongPosition : RiskRejectReason::MaxShortPosition);

    const Timestamp arrival = schedule_order_channel(config_.latency.cancel_ns, config_.latency.cancel_jitter_ns);
    ReplaceOrder replace;
    replace.ts = arrival;
    replace.session = kStrategySessionId;
    replace.orig_cl_ord_id = cl_ord_id;
    replace.cl_ord_id = 0;  // keep the token
    replace.price = price;
    replace.qty = new_total_qty;
    SimulationEvent event;
    event.kind = SimulationEventKind::StrategyRequestArrival;
    event.data.request.command = Command{replace};
    event.data.request.decision_time = now_;
    event.data.request.cl_ord_id = cl_ord_id;
    event.data.request.side = view->side;
    if (!push_simulation(arrival, event)) return reject(RiskRejectReason::QueueCapacity);

    view->pending = PendingRequest::Replace;
    view->requested_price = price;
    view->requested_total_qty = new_total_qty;
    ++metrics_.replace_requests_submitted;
    visible_.last_risk_reject = RiskRejectReason::None;
    LifecycleRecord row;
    row.time = now_;
    row.reference_time = arrival;
    row.stage = LifecycleStage::StrategySubmitted;
    row.request = CommandType::Replace;
    row.cl_ord_id = cl_ord_id;
    row.order_id = view->order_id;
    row.side = side;
    row.price = price;
    row.qty = new_total_qty;
    row.leaves_qty = view->leaves_qty;
    row.cum_qty = view->cum_qty;
    push_lifecycle(row);
    return true;
}

void SimulationCore::strategy_wake_at(Timestamp time) noexcept {
    if (!in_strategy_callback_ || state_ != RunState::Running) return;
    if (time < now_) time = now_;
    if (time == pending_wakeup_time_) return;
    ++wakeup_generation_;
    if (time > end_time()) {
        pending_wakeup_time_ = kNoTime;
        return;
    }
    pending_wakeup_time_ = time;
    SimulationEvent event;
    event.kind = SimulationEventKind::StrategyWakeup;
    event.wakeup_generation = wakeup_generation_;
    (void)push_simulation(time, event);
}

void SimulationCore::strategy_declare_parent_order(Side side, Quantity qty) noexcept {
    auto& parent = metrics_.parent;
    if (!in_strategy_callback_ || parent.declared || qty == 0) return;
    parent.declared = true;
    parent.side = side;
    parent.qty = qty;
    parent.decision_time = now_;
    parent.visible_at_decision = visible_.top_of_book;
    parent.market_volume_at_decision = metrics_.market_traded_qty;
    parent.market_volume_at_last_fill = metrics_.market_traded_qty;
    // Arrival benchmark: exchange truth at the decision instant.
    parent.has_decision_mid = has_reference_mid_;
    if (parent.has_decision_mid) parent.decision_mid_times_two = reference_mid_times_two_;
}

}  // namespace exsim::research
