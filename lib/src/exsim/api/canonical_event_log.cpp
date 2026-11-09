#include "api/canonical_event_log.hpp"

#include "api/api_json.hpp"

#include <algorithm>
#include <stdexcept>

namespace exsim::api {

namespace {

using research::CommandRecord;
using research::CommandSource;
using research::LifecycleRecord;
using research::LifecycleStage;

constexpr const char* kCanonicalEventKindNames[kCanonicalEventKindCount] = {
    "market_data_delivered",    "strategy_order_submitted", "strategy_cancel_submitted", "strategy_replace_submitted",
    "order_arrived_at_exchange", "cancel_arrived_at_exchange", "replace_arrived_at_exchange", "order_accepted",
    "order_rejected",           "order_rested",             "order_filled",              "order_canceled",
    "order_replaced",           "execution_report_delivered", "risk_limit_blocked_order", "phase_changed",
    "simulation_bounds_reached",
};
constexpr const char* kActorNames[] = {"research_strategy", "background_market_maker", "background_noise_taker", "exchange", "runner"};
constexpr const char* kVisibilityNames[] = {"exchange_truth", "strategy_visible"};

DeliveredReportKind delivered_report_kind(const LifecycleRecord& row) noexcept {
    switch (row.report_kind) {
        case research::ReportKind::Accepted: return DeliveredReportKind::Accepted;
        case research::ReportKind::Rejected:
            return row.request == CommandType::Cancel ? DeliveredReportKind::CancelRejected : DeliveredReportKind::Rejected;
        case research::ReportKind::Rested: return DeliveredReportKind::Rested;
        case research::ReportKind::Fill: return DeliveredReportKind::Filled;
        case research::ReportKind::Canceled: return DeliveredReportKind::Canceled;
        case research::ReportKind::Replaced: return DeliveredReportKind::Replaced;
    }
    return DeliveredReportKind::Accepted;
}

}  // namespace

std::string describe_canonical_event(const CanonicalEventRecord& record) {
    const std::string side = record.has(kHasSide) ? std::string(to_api_side(record.side)) + " " : "";
    const std::string quantity = record.has(kHasQuantity) ? std::to_string(record.quantity) : "";
    const std::string at_price = record.has(kHasPrice) ? " @ " + format_price_units(record.price) : "";
    const std::string token = record.has(kHasClientOrderId) ? " (client order " + std::to_string(record.client_order_id) + ")" : "";
    const std::string order = record.has(kHasOrderId) ? " order " + std::to_string(record.order_id) : "";
    switch (record.kind) {
        case CanonicalEventKind::MarketDataDelivered:
            return "top of book delivered: " + (record.has(kHasBestBid) ? format_price_units(record.best_bid_price) : std::string("-")) + " / " +
                   (record.has(kHasBestAsk) ? format_price_units(record.best_ask_price) : std::string("-"));
        case CanonicalEventKind::StrategyOrderSubmitted: return "strategy submitted limit " + side + quantity + at_price + token;
        case CanonicalEventKind::StrategyCancelSubmitted: return "strategy requested cancel" + token;
        case CanonicalEventKind::StrategyReplaceSubmitted: return "strategy requested replace to " + quantity + at_price + token;
        case CanonicalEventKind::OrderArrivedAtExchange: return std::string(to_api_string(record.actor)) + " order " + side + quantity + at_price + " reached the exchange";
        case CanonicalEventKind::CancelArrivedAtExchange: return std::string(to_api_string(record.actor)) + " cancel reached the exchange" + token;
        case CanonicalEventKind::ReplaceArrivedAtExchange: return std::string(to_api_string(record.actor)) + " replace reached the exchange" + token;
        case CanonicalEventKind::OrderAccepted: return "accepted" + order + " " + side + quantity + at_price;
        case CanonicalEventKind::OrderRejected:
            return std::string(to_api_string(record.rejected_command)) + " rejected: " + to_api_string(record.reject_reason) + token;
        case CanonicalEventKind::OrderRested:
            return "order " + std::to_string(record.order_id) + " rests " + std::to_string(record.leaves_quantity) + at_price;
        case CanonicalEventKind::OrderFilled:
            return "fill " + quantity + at_price + " match " + std::to_string(record.match_id) + (record.has(kIsOwnOrder) ? " (own order)" : "");
        case CanonicalEventKind::OrderCanceled:
            return "order " + std::to_string(record.order_id) + " canceled " + quantity + " (" + to_api_string(record.cancel_reason) + ")";
        case CanonicalEventKind::OrderReplaced:
            return "order " + std::to_string(record.replaced_old_order_id) + " replaced by " + std::to_string(record.order_id) + " " + quantity + at_price +
                   (record.has(kPriorityKept) ? " (priority kept)" : " (priority lost)");
        case CanonicalEventKind::ExecutionReportDelivered: return std::string("report ") + to_api_string(record.report_kind) + " delivered" + token;
        case CanonicalEventKind::RiskLimitBlockedOrder:
            return std::string("risk check blocked ") + to_api_string(record.rejected_command) + ": " + research::to_string(record.risk_reason) + token;
        case CanonicalEventKind::PhaseChanged: return std::string("trading phase is now ") + to_api_string(record.new_phase);
        case CanonicalEventKind::SimulationBoundsReached: return "simulation end time reached; open orders and positions are kept as residuals";
    }
    return "";
}


const char* to_api_string(CanonicalEventKind kind) noexcept { return kCanonicalEventKindNames[static_cast<std::size_t>(kind)]; }

const char* to_api_string(PipelineStage stage) noexcept {
    switch (stage) {
        case PipelineStage::FeedDelivery: return "feed_delivery";
        case PipelineStage::StrategyProcessing: return "strategy_processing";
        case PipelineStage::OutboundOrder: return "outbound_order";
        case PipelineStage::ExchangeProcessing: return "exchange_processing";
        case PipelineStage::ReportDelivery: return "report_delivery";
        case PipelineStage::RunnerControl: return "runner_control";
    }
    return "runner_control";
}

const char* to_api_string(EventActor actor) noexcept { return kActorNames[static_cast<std::size_t>(actor)]; }
const char* to_api_string(EventVisibility visibility) noexcept { return kVisibilityNames[static_cast<std::size_t>(visibility)]; }

const char* to_api_string(DeliveredReportKind kind) noexcept {
    switch (kind) {
        case DeliveredReportKind::Accepted: return "accepted";
        case DeliveredReportKind::Rejected: return "rejected";
        case DeliveredReportKind::Rested: return "rested";
        case DeliveredReportKind::Filled: return "filled";
        case DeliveredReportKind::Canceled: return "canceled";
        case DeliveredReportKind::Replaced: return "replaced";
        case DeliveredReportKind::CancelRejected: return "cancel_rejected";
    }
    return "accepted";
}

const char* to_api_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None: return "invalid_field";
        case RejectReason::UnknownInstrument: return "unknown_instrument";
        case RejectReason::InvalidClOrdId: return "invalid_client_order_id";
        case RejectReason::DuplicateClOrdId: return "duplicate_client_order_id";
        case RejectReason::InvalidQuantity: return "invalid_quantity";
        case RejectReason::InvalidPrice: return "invalid_price";
        case RejectReason::InvalidField: return "invalid_field";
        case RejectReason::MarketClosed: return "market_closed";
        case RejectReason::InstrumentHalted: return "instrument_halted";
        case RejectReason::NotAllowedInPhase: return "not_allowed_in_phase";
        case RejectReason::PostOnlyWouldCross: return "post_only_would_cross";
        case RejectReason::CapacityExceeded: return "capacity_exceeded";
        case RejectReason::UnknownOrder: return "unknown_order";
    }
    return "invalid_field";
}

const char* to_api_string(CancelReason reason) noexcept {
    switch (reason) {
        case CancelReason::UserRequested: return "user_requested";
        case CancelReason::ImmediateOrCancel: return "immediate_or_cancel";
        case CancelReason::FillOrKill: return "fill_or_kill";
        case CancelReason::SelfTradePrevention: return "self_trade_prevention";
        case CancelReason::MassCancel: return "mass_cancel";
        case CancelReason::CancelOnDisconnect: return "cancel_on_disconnect";
        case CancelReason::EndOfDay: return "end_of_day";
    }
    return "user_requested";
}

const char* to_api_string(CommandType command) noexcept {
    switch (command) {
        case CommandType::NewOrder: return "new_order";
        case CommandType::Cancel: return "cancel";
        case CommandType::Replace: return "replace";
        case CommandType::MassCancel: return "mass_cancel";
        case CommandType::SetPhase: return "set_phase";
    }
    return "new_order";
}

const char* to_api_string(Phase phase) noexcept {
    switch (phase) {
        case Phase::Closed: return "closed";
        case Phase::PreOpen: return "pre_open";
        case Phase::Continuous: return "continuous";
        case Phase::Halted: return "halted";
        case Phase::PreClose: return "pre_close";
    }
    return "closed";
}

const char* to_api_side(Side side) noexcept { return side == Side::BUY ? "buy" : "sell"; }

const char* to_api_liquidity(research::Liquidity liquidity) noexcept {
    switch (liquidity) {
        case research::Liquidity::Maker: return "maker";
        case research::Liquidity::Taker: return "taker";
        case research::Liquidity::Auction: return "auction";
    }
    return "maker";
}

std::optional<CanonicalEventKind> parse_canonical_event_kind(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kCanonicalEventKindCount; ++index)
        if (text == kCanonicalEventKindNames[index]) return static_cast<CanonicalEventKind>(index);
    return std::nullopt;
}

std::optional<EventActor> parse_event_actor(std::string_view text) noexcept {
    for (std::size_t index = 0; index < std::size(kActorNames); ++index)
        if (text == kActorNames[index]) return static_cast<EventActor>(index);
    return std::nullopt;
}

std::optional<EventVisibility> parse_event_visibility(std::string_view text) noexcept {
    for (std::size_t index = 0; index < std::size(kVisibilityNames); ++index)
        if (text == kVisibilityNames[index]) return static_cast<EventVisibility>(index);
    return std::nullopt;
}

void write_canonical_event_json(ApiJsonWriter& json, const CanonicalEventRecord& record, std::uint64_t event_position) {
    const auto unsigned_or_null = [&](bool present, std::uint64_t value) {
        if (present) json.unsigned_text(value);
        else json.null();
    };
    const auto price_or_null = [&](bool present, Price value) {
        if (present) json.string(format_price_units(value));
        else json.null();
    };
    const auto text_or_null = [&](bool present, const char* value) {
        if (present) json.string(value);
        else json.null();
    };
    json.begin_object();
    json.key("eventPosition").unsigned_text(event_position);
    json.key("simulationTimeNs").unsigned_text(record.simulation_time_ns);
    json.key("enqueueSequence").unsigned_text(record.enqueue_sequence);
    json.key("stage").string(to_api_string(record.stage));
    json.key("eventKind").string(to_api_string(record.kind));
    json.key("visibility").string(to_api_string(record.visibility));
    json.key("actor").string(to_api_string(record.actor));
    json.key("isOwnOrder").boolean(record.has(kIsOwnOrder));
    json.key("orderId");
    unsigned_or_null(record.has(kHasOrderId), record.order_id);
    json.key("clientOrderId");
    unsigned_or_null(record.has(kHasClientOrderId), record.client_order_id);
    json.key("side");
    text_or_null(record.has(kHasSide), to_api_side(record.side));
    json.key("price");
    price_or_null(record.has(kHasPrice), record.price);
    json.key("quantity");
    unsigned_or_null(record.has(kHasQuantity), record.quantity);
    json.key("leavesQuantity");
    unsigned_or_null(record.has(kHasLeavesQuantity), record.leaves_quantity);
    json.key("cumulativeQuantity");
    unsigned_or_null(record.has(kHasCumulativeQuantity), record.cumulative_quantity);
    json.key("matchId");
    unsigned_or_null(record.has(kHasMatchId), record.match_id);
    json.key("liquidityRole");
    text_or_null(record.has(kHasLiquidityRole), to_api_liquidity(record.liquidity));
    json.key("rejectReason");
    text_or_null(record.has(kHasRejectReason), to_api_string(record.reject_reason));
    json.key("rejectedCommand");
    text_or_null(record.has(kHasRejectedCommand), to_api_string(record.rejected_command));
    json.key("cancelReason");
    text_or_null(record.has(kHasCancelReason), to_api_string(record.cancel_reason));
    json.key("reportKind");
    text_or_null(record.has(kHasReportKind), to_api_string(record.report_kind));
    json.key("bestBidPrice");
    price_or_null(record.has(kHasBestBid), record.best_bid_price);
    json.key("bestAskPrice");
    price_or_null(record.has(kHasBestAsk), record.best_ask_price);
    json.key("sourceEventPosition");
    unsigned_or_null(record.source_event_position != kNoEventPosition, record.source_event_position);
    json.key("newPhase");
    text_or_null(record.has(kHasNewPhase), to_api_string(record.new_phase));
    json.key("description").string(describe_canonical_event(record));
    json.end_object();
}

// ---- Builder ---------------------------------------------------------------------

CanonicalEventBuilder::CanonicalEventBuilder(const research::ExperimentConfig& config)
    : config_(config),
      engine_(std::make_unique<MatchingEngine>(research::research_engine_config(config))),
      event_buffer_(required_event_capacity(research::research_engine_config(config))) {
    instrument_ = engine_->add_instrument(config_.instrument);
    if (instrument_ == 0) throw std::runtime_error("shadow engine rejected the instrument");
}

CanonicalEventBuilder::~CanonicalEventBuilder() = default;

EventActor CanonicalEventBuilder::actor_for_session(SessionId session) const noexcept {
    if (session == research::kStrategySessionId) return EventActor::ResearchStrategy;
    if (session >= 2 && static_cast<std::size_t>(session - 2) < config_.agents.size())
        return config_.agents[session - 2].kind == research::BackgroundAgentKind::MarketMaker ? EventActor::BackgroundMarketMaker
                                                                                              : EventActor::BackgroundNoiseTaker;
    return EventActor::Runner;
}

research::TopOfBook CanonicalEventBuilder::shadow_top_of_book() const noexcept {
    research::TopOfBook top;
    LevelInfo level{};
    if (engine_->depth(instrument_, Side::BUY, &level, 1) == 1) {
        top.has_bid = true;
        top.bid_price = level.price;
        top.bid_qty = level.qty;
    }
    if (engine_->depth(instrument_, Side::SELL, &level, 1) == 1) {
        top.has_ask = true;
        top.ask_price = level.price;
        top.ask_qty = level.qty;
    }
    return top;
}

void CanonicalEventBuilder::append_step(const research::SimulationCore& core, std::span<const CommandRecord> new_commands,
                                        std::span<const LifecycleRecord> new_lifecycle_rows, std::uint64_t step_sequence,
                                        std::vector<CanonicalEventRecord>& log) {
    // 1. A market-data delivery to the strategy (one per step at most).
    const research::MarketDataUpdate& delivered = core.strategy_view().last_market_data;
    if (delivered.sequence != last_delivered_market_data_sequence_) {
        last_delivered_market_data_sequence_ = delivered.sequence;
        CanonicalEventRecord record;
        record.simulation_time_ns = core.now();
        record.enqueue_sequence = step_sequence;
        record.kind = CanonicalEventKind::MarketDataDelivered;
        record.stage = PipelineStage::FeedDelivery;
        record.actor = EventActor::ResearchStrategy;
        record.visibility = EventVisibility::StrategyVisible;
        if (delivered.top.has_bid) {
            record.present |= kHasBestBid;
            record.best_bid_price = delivered.top.bid_price;
            record.best_bid_quantity = delivered.top.bid_qty;
        }
        if (delivered.top.has_ask) {
            record.present |= kHasBestAsk;
            record.best_ask_price = delivered.top.ask_price;
            record.best_ask_quantity = delivered.top.ask_qty;
        }
        while (!market_data_source_positions_.empty() && market_data_source_positions_.front().first < delivered.sequence)
            market_data_source_positions_.pop_front();
        if (!market_data_source_positions_.empty() && market_data_source_positions_.front().first == delivered.sequence) {
            record.source_event_position = market_data_source_positions_.front().second;
            market_data_source_positions_.pop_front();
        } else if (consistency_error_.empty()) {
            consistency_error_ = "market data sequence " + std::to_string(delivered.sequence) + " was not published by the shadow engine";
        }
        log.push_back(record);
    }

    // 2. Strategy-side rows, in the order the runner recorded them.
    for (const LifecycleRecord& row : new_lifecycle_rows) {
        CanonicalEventRecord record;
        record.simulation_time_ns = row.time;
        record.enqueue_sequence = step_sequence;
        record.actor = EventActor::ResearchStrategy;
        record.visibility = EventVisibility::StrategyVisible;
        record.present |= kIsOwnOrder | kHasClientOrderId | kHasSide;
        record.client_order_id = row.cl_ord_id;
        record.side = row.side;
        switch (row.stage) {
            case LifecycleStage::StrategySubmitted:
                record.stage = PipelineStage::StrategyProcessing;
                if (row.request == CommandType::NewOrder) {
                    record.kind = CanonicalEventKind::StrategyOrderSubmitted;
                    record.present |= kHasPrice | kHasQuantity;
                    record.price = row.price;
                    record.quantity = row.qty;
                } else if (row.request == CommandType::Cancel) {
                    record.kind = CanonicalEventKind::StrategyCancelSubmitted;
                    if (row.order_id != 0) {
                        record.present |= kHasOrderId;
                        record.order_id = row.order_id;
                    }
                } else {
                    record.kind = CanonicalEventKind::StrategyReplaceSubmitted;
                    record.present |= kHasPrice | kHasQuantity;
                    record.price = row.price;
                    record.quantity = row.qty;
                    if (row.order_id != 0) {
                        record.present |= kHasOrderId;
                        record.order_id = row.order_id;
                    }
                }
                break;
            case LifecycleStage::RiskRejected:
                record.stage = PipelineStage::StrategyProcessing;
                record.kind = CanonicalEventKind::RiskLimitBlockedOrder;
                record.present |= kHasRejectedCommand | kRiskReasonPresent;
                record.rejected_command = row.request;
                record.risk_reason = row.risk_reason;
                if (row.request != CommandType::Cancel) {
                    record.present |= kHasPrice | kHasQuantity;
                    record.price = row.price;
                    record.quantity = row.qty;
                }
                if (row.cl_ord_id == 0) record.present &= ~static_cast<std::uint32_t>(kHasClientOrderId);
                break;
            case LifecycleStage::ReportDelivered: {
                record.stage = PipelineStage::ReportDelivery;
                record.kind = CanonicalEventKind::ExecutionReportDelivered;
                record.report_kind = delivered_report_kind(row);
                record.present |= kHasReportKind;
                if (row.order_id != 0) {
                    record.present |= kHasOrderId;
                    record.order_id = row.order_id;
                }
                switch (row.report_kind) {
                    case research::ReportKind::Fill:
                        record.present |= kHasPrice | kHasQuantity | kHasLeavesQuantity | kHasCumulativeQuantity | kHasMatchId | kHasLiquidityRole;
                        record.match_id = row.match_id;
                        record.liquidity = row.liquidity;
                        break;
                    case research::ReportKind::Rejected:
                        record.present |= kHasRejectReason | kHasRejectedCommand;
                        record.reject_reason = row.reject_reason;
                        record.rejected_command = row.request;
                        break;
                    case research::ReportKind::Canceled:
                        record.present |= kHasPrice | kHasQuantity | kHasLeavesQuantity | kHasCumulativeQuantity | kHasCancelReason;
                        record.cancel_reason = row.cancel_reason;
                        break;
                    default:
                        record.present |= kHasPrice | kHasQuantity | kHasLeavesQuantity | kHasCumulativeQuantity;
                        break;
                }
                record.price = row.price;
                record.quantity = row.qty;
                record.leaves_quantity = row.leaves_qty;
                record.cumulative_quantity = row.cum_qty;
                if (!pending_report_source_positions_.empty()) {
                    record.source_event_position = pending_report_source_positions_.front();
                    pending_report_source_positions_.pop_front();
                } else if (consistency_error_.empty()) {
                    consistency_error_ = "an execution report was delivered without a matching exchange event";
                }
                break;
            }
            default: continue;  // exchange-side rows: reproduced from the shadow engine below
        }
        log.push_back(record);
    }

    // 3. Exchange commands with their engine events.
    for (const CommandRecord& command : new_commands) {
        const LifecycleRecord* arrival_row = nullptr;
        for (const LifecycleRecord& row : new_lifecycle_rows)
            if (row.stage == LifecycleStage::ExchangeArrival && row.command_index == command.command_index) arrival_row = &row;
        append_command(command, arrival_row, step_sequence, log);
    }
    if (!new_commands.empty() && consistency_error_.empty() && !(shadow_top_of_book() == core.exchange_top_of_book()))
        consistency_error_ = "the shadow engine's top of book differs from the runner's after command " +
                             std::to_string(new_commands.back().command_index);
}

void CanonicalEventBuilder::append_command(const CommandRecord& command_record, const LifecycleRecord* arrival_row,
                                           std::uint64_t step_sequence, std::vector<CanonicalEventRecord>& log) {
    const Command& command = command_record.command;
    EventActor submitting_actor = EventActor::Runner;
    if (command_record.source == CommandSource::Strategy) submitting_actor = EventActor::ResearchStrategy;
    else if (command_record.source == CommandSource::BackgroundAgent)
        submitting_actor = actor_for_session(research::background_agent_session(command_record.agent_index));
    const bool own_command = command_record.source == CommandSource::Strategy;

    std::uint64_t arrival_position = kNoEventPosition;
    if (command.type != CommandType::SetPhase) {
        CanonicalEventRecord arrival;
        arrival.simulation_time_ns = command_record.time;
        arrival.enqueue_sequence = step_sequence;
        arrival.command_index = command_record.command_index;
        arrival.stage = PipelineStage::OutboundOrder;
        arrival.actor = submitting_actor;
        arrival.visibility = EventVisibility::ExchangeTruth;
        if (own_command) arrival.present |= kIsOwnOrder;
        arrival.present |= kHasClientOrderId;
        switch (command.type) {
            case CommandType::NewOrder:
                arrival.kind = CanonicalEventKind::OrderArrivedAtExchange;
                arrival.client_order_id = command.new_order.cl_ord_id;
                arrival.session = command.new_order.session;
                arrival.present |= kHasSide | kHasQuantity;
                arrival.side = command.new_order.side;
                arrival.quantity = command.new_order.qty;
                if (command.new_order.type == OrdType::Limit) {
                    arrival.present |= kHasPrice;
                    arrival.price = command.new_order.price;
                }
                break;
            case CommandType::Cancel:
                arrival.kind = CanonicalEventKind::CancelArrivedAtExchange;
                arrival.client_order_id = command.cancel.orig_cl_ord_id;
                arrival.session = command.cancel.session;
                break;
            case CommandType::Replace:
                arrival.kind = CanonicalEventKind::ReplaceArrivedAtExchange;
                arrival.client_order_id = command.replace.orig_cl_ord_id;
                arrival.session = command.replace.session;
                arrival.present |= kHasPrice | kHasQuantity;
                arrival.price = command.replace.price;
                arrival.quantity = command.replace.qty;
                break;
            case CommandType::MassCancel:
                arrival.kind = CanonicalEventKind::CancelArrivedAtExchange;
                arrival.client_order_id = command.mass_cancel.cl_ord_id;
                arrival.session = command.mass_cancel.session;
                break;
            case CommandType::SetPhase: break;
        }
        if (arrival_row != nullptr) {
            arrival.side = arrival_row->side;
            arrival.present |= kHasSide;
            if (arrival_row->has_queue_position) {
                arrival.present |= kHasQueuePosition;
                arrival.queue_shares_ahead = arrival_row->queue_ahead_qty;
                arrival.queue_orders_ahead = arrival_row->queue_orders_ahead;
                arrival.queue_level_shares = arrival_row->level_qty;
                arrival.order_id = arrival_row->order_id;  // the order whose queue position this is (not rendered)
            }
        }
        arrival_position = log.size();
        log.push_back(arrival);
    }

    event_buffer_.clear();
    if (engine_->handle(command, event_buffer_) != HandleResult::Applied) {
        if (consistency_error_.empty()) consistency_error_ = "the shadow engine ran out of event capacity";
        return;
    }

    std::uint32_t trade_count = 0;
    for (const Event& event : std::span<const Event>(event_buffer_.begin(), event_buffer_.size())) {
        CanonicalEventRecord record;
        record.simulation_time_ns = command_record.time;
        record.enqueue_sequence = step_sequence;
        record.command_index = command_record.command_index;
        record.command_arrival_position = arrival_position;
        record.stage = PipelineStage::ExchangeProcessing;
        record.actor = EventActor::Exchange;
        record.visibility = EventVisibility::ExchangeTruth;
        bool reports_to_strategy = false;
        switch (event.type) {
            case EventType::Accepted: {
                const auto& accepted = event.accepted;
                record.kind = CanonicalEventKind::OrderAccepted;
                record.present |= kHasOrderId | kHasClientOrderId | kHasSide | kHasQuantity | kHasLeavesQuantity | kHasCumulativeQuantity;
                record.order_id = accepted.order_id;
                record.client_order_id = accepted.cl_ord_id;
                record.session = accepted.session;
                record.side = accepted.side;
                record.quantity = accepted.qty;
                record.leaves_quantity = accepted.qty;
                if (accepted.ord_type == OrdType::Limit) {
                    record.present |= kHasPrice;
                    record.price = accepted.price;
                }
                reports_to_strategy = accepted.session == research::kStrategySessionId;
                break;
            }
            case EventType::Rejected: {
                const auto& rejected = event.rejected;
                record.kind = CanonicalEventKind::OrderRejected;
                record.present |= kHasClientOrderId | kHasRejectReason | kHasRejectedCommand;
                record.client_order_id = rejected.cmd == CommandType::NewOrder ? rejected.cl_ord_id : rejected.orig_cl_ord_id;
                record.session = rejected.session;
                record.reject_reason = rejected.reason;
                record.rejected_command = rejected.cmd;
                if (rejected.order_id != 0) {
                    record.present |= kHasOrderId;
                    record.order_id = rejected.order_id;
                }
                const bool silent_ouch = config_.order_entry_protocol == server::Protocol::Ouch42 &&
                    (rejected.cmd == CommandType::Cancel ||
                    (rejected.cmd == CommandType::Replace && rejected.reason == RejectReason::UnknownOrder));
                reports_to_strategy = rejected.session == research::kStrategySessionId && !silent_ouch;
                break;
            }
            case EventType::Rested: {
                const auto& rested = event.rested;
                record.kind = CanonicalEventKind::OrderRested;
                record.present |= kHasOrderId | kHasClientOrderId | kHasSide | kHasPrice | kHasLeavesQuantity;
                record.order_id = rested.order_id;
                record.client_order_id = rested.cl_ord_id;
                record.session = rested.session;
                record.side = rested.side;
                record.price = rested.price;
                record.leaves_quantity = rested.leaves_qty;
                reports_to_strategy = rested.session == research::kStrategySessionId;
                break;
            }
            case EventType::Fill: {
                const auto& fill = event.fill;
                ++trade_count;
                const bool buy_is_own = fill.buy.session == research::kStrategySessionId;
                const bool sell_is_own = fill.sell.session == research::kStrategySessionId;
                // Displayed leg: the strategy's when it is involved, else the resting (maker) leg.
                Side shown_side = Side::BUY;
                if (buy_is_own) shown_side = Side::BUY;
                else if (sell_is_own) shown_side = Side::SELL;
                else shown_side = fill.aggressor == Aggressor::Buy ? Side::SELL : Side::BUY;
                const FillLeg& shown = shown_side == Side::BUY ? fill.buy : fill.sell;
                const FillLeg& other = shown_side == Side::BUY ? fill.sell : fill.buy;
                record.kind = CanonicalEventKind::OrderFilled;
                record.present |= kHasOrderId | kHasClientOrderId | kHasSide | kHasPrice | kHasQuantity | kHasLeavesQuantity |
                                  kHasCumulativeQuantity | kHasMatchId;
                record.order_id = shown.order_id;
                record.client_order_id = shown.cl_ord_id;
                record.session = shown.session;
                record.side = shown_side;
                record.price = fill.price;
                record.quantity = fill.qty;
                record.leaves_quantity = shown.leaves_qty;
                record.cumulative_quantity = shown.cum_qty;
                record.match_id = fill.match_id;
                record.buy_leg_order_id = fill.buy.order_id;
                record.sell_leg_order_id = fill.sell.order_id;
                record.buy_leg_leaves = fill.buy.leaves_qty;
                record.sell_leg_leaves = fill.sell.leaves_qty;
                record.buy_leg_session = fill.buy.session;
                record.sell_leg_session = fill.sell.session;
                if (buy_is_own || sell_is_own) {
                    record.present |= kIsOwnOrder | kHasLiquidityRole;
                    const bool shown_is_aggressor = (shown_side == Side::BUY && fill.aggressor == Aggressor::Buy) ||
                                                    (shown_side == Side::SELL && fill.aggressor == Aggressor::Sell);
                    record.liquidity = fill.aggressor == Aggressor::None ? research::Liquidity::Auction
                                       : shown_is_aggressor               ? research::Liquidity::Taker
                                                                          : research::Liquidity::Maker;
                    record.counterparty_actor = actor_for_session(other.session);
                }
                const std::uint64_t position = log.size();
                if (buy_is_own) {
                    pending_report_source_positions_.push_back(position);
                    own_fill_event_positions_.push_back(position);
                }
                if (sell_is_own) {
                    pending_report_source_positions_.push_back(position);
                    own_fill_event_positions_.push_back(position);
                }
                break;
            }
            case EventType::Canceled: {
                const auto& canceled = event.canceled;
                record.kind = CanonicalEventKind::OrderCanceled;
                record.present |= kHasOrderId | kHasClientOrderId | kHasSide | kHasPrice | kHasQuantity | kHasLeavesQuantity |
                                  kHasCumulativeQuantity | kHasCancelReason;
                record.order_id = canceled.order_id;
                record.client_order_id = canceled.cl_ord_id;
                record.session = canceled.session;
                record.side = canceled.side;
                record.price = canceled.price;
                record.quantity = canceled.canceled_qty;
                record.leaves_quantity = canceled.leaves_qty;
                record.cumulative_quantity = canceled.cum_qty;
                record.cancel_reason = canceled.reason;
                if (canceled.was_resting) record.present |= kWasResting;
                reports_to_strategy = canceled.session == research::kStrategySessionId;
                break;
            }
            case EventType::Replaced: {
                const auto& replaced = event.replaced;
                record.kind = CanonicalEventKind::OrderReplaced;
                record.present |= kHasOrderId | kHasClientOrderId | kHasSide | kHasPrice | kHasQuantity | kHasLeavesQuantity |
                                  kHasCumulativeQuantity;
                record.order_id = replaced.new_order_id;
                record.replaced_old_order_id = replaced.old_order_id;
                record.client_order_id = replaced.cl_ord_id;
                record.session = replaced.session;
                record.side = replaced.side;
                record.price = replaced.price;
                record.quantity = replaced.qty;
                record.leaves_quantity = replaced.leaves_qty;
                record.cumulative_quantity = replaced.cum_qty;
                if (replaced.priority_kept) record.present |= kPriorityKept;
                reports_to_strategy = replaced.session == research::kStrategySessionId;
                break;
            }
            case EventType::PhaseChanged:
                record.kind = CanonicalEventKind::PhaseChanged;
                record.present |= kHasNewPhase;
                record.new_phase = event.phase_changed.new_phase;
                break;
            case EventType::MassCancelDone:
            case EventType::AuctionSummary: continue;  // no canonical kind; the constituent events are logged
        }
        if (record.session == research::kStrategySessionId && record.kind != CanonicalEventKind::PhaseChanged) record.present |= kIsOwnOrder;
        if (reports_to_strategy) pending_report_source_positions_.push_back(log.size());
        log.push_back(record);
    }

    // Full ITCH delivery includes deeper-book changes and every nonempty event batch.
    const research::TopOfBook top_after = shadow_top_of_book();
    if (event_buffer_.size() != 0 || trade_count > 0 || !(top_after == last_published_top_)) {
        last_published_top_ = top_after;
        ++published_market_data_count_;
        const std::uint64_t source = log.empty() ? kNoEventPosition : log.size() - 1;
        market_data_source_positions_.emplace_back(published_market_data_count_, source);
    }
}

void CanonicalEventBuilder::append_simulation_bounds_reached(Timestamp time, std::uint64_t step_sequence,
                                                             std::vector<CanonicalEventRecord>& log) {
    CanonicalEventRecord record;
    record.simulation_time_ns = time;
    record.enqueue_sequence = step_sequence;
    record.kind = CanonicalEventKind::SimulationBoundsReached;
    record.stage = PipelineStage::RunnerControl;
    record.actor = EventActor::Runner;
    record.visibility = EventVisibility::ExchangeTruth;
    log.push_back(record);
}

// ---- Exchange-truth book ---------------------------------------------------------

ExchangeTruthBook::RestingOrder* ExchangeTruthBook::find(OrderId order_id) {
    for (auto& order : orders_)
        if (order.order_id == order_id) return &order;
    return nullptr;
}

const ExchangeTruthBook::RestingOrder* ExchangeTruthBook::find(OrderId order_id) const {
    for (const auto& order : orders_)
        if (order.order_id == order_id) return &order;
    return nullptr;
}

void ExchangeTruthBook::erase(OrderId order_id) {
    const auto found = std::find_if(orders_.begin(), orders_.end(), [&](const RestingOrder& order) { return order.order_id == order_id; });
    if (found != orders_.end()) orders_.erase(found);
}

void ExchangeTruthBook::apply(const CanonicalEventRecord& record) {
    switch (record.kind) {
        case CanonicalEventKind::OrderRested:
            orders_.push_back({record.order_id, record.side, record.price, record.leaves_quantity, record.session});
            break;
        case CanonicalEventKind::OrderFilled:
            for (const auto& [order_id, leaves] : {std::pair{record.buy_leg_order_id, record.buy_leg_leaves},
                                                   std::pair{record.sell_leg_order_id, record.sell_leg_leaves}}) {
                RestingOrder* order = find(order_id);
                if (order == nullptr) continue;
                if (leaves == 0) erase(order_id);
                else order->leaves = leaves;
            }
            break;
        case CanonicalEventKind::OrderCanceled:
            if (!record.has(kWasResting)) break;
            if (record.leaves_quantity == 0) erase(record.order_id);
            else if (RestingOrder* order = find(record.order_id)) order->leaves = record.leaves_quantity;
            break;
        case CanonicalEventKind::OrderReplaced:
            if (record.has(kPriorityKept)) {
                if (RestingOrder* order = find(record.order_id)) order->leaves = record.leaves_quantity;
            } else {
                erase(record.replaced_old_order_id);  // re-enters through matching; a Rested event puts it back
            }
            break;
        default: break;
    }
}

std::vector<ExchangeTruthBook::Level> ExchangeTruthBook::levels(Side side, std::size_t depth) const {
    std::vector<Level> result;
    for (const RestingOrder& order : orders_) {
        if (order.side != side) continue;
        auto level = std::find_if(result.begin(), result.end(), [&](const Level& candidate) { return candidate.price == order.price; });
        if (level == result.end()) {
            result.push_back({order.price, 0, 0, 0});
            level = result.end() - 1;
        }
        level->total_shares += order.leaves;
        ++level->order_count;
        if (order.session == research::kStrategySessionId) level->own_shares += order.leaves;
    }
    std::sort(result.begin(), result.end(), [&](const Level& left, const Level& right) {
        return side == Side::BUY ? left.price > right.price : left.price < right.price;
    });
    if (result.size() > depth) result.resize(depth);
    return result;
}

ExchangeTruthBook::QueuePosition ExchangeTruthBook::queue_position(OrderId order_id) const {
    QueuePosition position;
    const RestingOrder* target = find(order_id);
    if (target == nullptr) return position;
    position.found = true;
    position.price = target->price;
    bool reached = false;
    for (const RestingOrder& order : orders_) {
        if (order.side != target->side || order.price != target->price) continue;
        position.level_shares += order.leaves;
        if (order.order_id == order_id) reached = true;
        else if (!reached) {
            position.shares_ahead += order.leaves;
            ++position.orders_ahead;
        }
    }
    return position;
}

}  // namespace exsim::api
