#ifndef EXSIM_MD_PUBLISHER_HPP
#define EXSIM_MD_PUBLISHER_HPP

// MarketDataPublisher: engine event batches -> ITCH 5.0 messages ->
// MoldUDP64 packets, following the normative mapping in docs/GATEWAYS.md
// section 4 and the runtime contract in docs/SERVER.md section 3.
//
//   MarketDataPublisher<Sink> publisher(config, sink);
//   publisher.add_instruments_from(engine);       // setup: locate = InstrumentId
//   publisher.start_session(now);                  // S 'O', R + H per instrument, S 'S'
//   per command: publisher.on_events(batch);       // packs, flushes at batch end
//   timers:      publisher.poll(now, engine);      // NOII during call phases, heartbeats
//   shutdown:    publisher.end_of_day(now);        // S 'E', S 'C', End of Session
//
// `PacketSink` is any callable `sink(std::span<const char> packet)`; the
// server passes one that forwards to sim::PacketFaultInjector and then the
// UDP sender. The span is valid only during the call. Every sequenced
// message is also kept in a RetransmissionRing for the RetransmissionServer.
//
// Timestamps: every time value (event ts, `now`) is on the engine command
// clock. ITCH timestamp = time - day_origin, which must lie inside the
// trading day. A batch with any out-of-day timestamp, unregistered
// instrument or unrepresentable price is not published at all and stops the
// session (state() == Stopped); the server must then end the session.
//
// Hot path (on_events, flush, poll): no heap allocation, no exceptions
// thrown by the publisher itself, no virtual dispatch.

#include <exsim/event.hpp>
#include <exsim/instrument.hpp>
#include <exsim/matching_engine.hpp>  // AuctionIndicative
#include <exsim/md/feed_common.hpp>
#include <exsim/md/retransmission_ring.hpp>
#include <exsim/protocol/itch50.hpp>
#include <exsim/protocol/moldudp64.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace exsim::md {

struct PublisherConfig {
    std::string_view session = "EXSIM";                 // MoldUDP64 session, <= 10 chars
    std::size_t max_payload = 1400;                      // bytes per packet including the 20-byte header
    std::size_t retransmit_buffer_messages = 1u << 16;   // ring capacity
    std::size_t max_instruments = 64;                    // highest stock locate
    Timestamp day_origin = 0;                            // ITCH timestamp = time - day_origin
    std::uint64_t heartbeat_interval_ns = 1'000'000'000; // heartbeat after this much silence
    std::uint64_t noii_interval_ns = 1'000'000'000;      // NOII cadence in call phases; 0 disables
};

enum class PublishStatus : std::uint8_t {
    Ok = 0,
    InvalidConfig,         // validate_publisher_config failed
    InvalidInstrument,     // band not representable in ITCH Price(4), or locate out of range
    DuplicateInstrument,   // locate already registered
    NotAllowedInState,     // e.g. add_instrument after start, on_events before start or after stop
    TimestampOutsideDay,   // stops a live session (start_session: rejected, stays in Setup)
    UnknownInstrument,     // event for an unregistered locate; stops the session
    UnrepresentableField,  // price outside ITCH range or encode failure; stops the session
};

enum class PublisherState : std::uint8_t {
    Setup = 0,  // instruments may be added
    Live,       // start_session done; publishing
    Stopped,    // a batch failed validation; only end_session() is allowed
    Ended,      // End of Session sent; poll repeats it every heartbeat interval
};

struct PublisherStats {
    std::uint64_t packets_sent = 0;             // data, heartbeat and end-of-session packets
    std::uint64_t messages_sequenced = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t end_of_session_packets_sent = 0;
    std::uint64_t imbalance_messages_sent = 0;
};

[[nodiscard]] inline PublishStatus validate_publisher_config(const PublisherConfig& config) noexcept {
    MoldSessionName session{};
    if (!make_mold_session_name(config.session, session)) return PublishStatus::InvalidConfig;
    if (config.max_payload < kMinimumMaxPayload || config.max_payload > kMaximumMaxPayload)
        return PublishStatus::InvalidConfig;
    if (config.retransmit_buffer_messages == 0 ||
        config.retransmit_buffer_messages > std::numeric_limits<std::size_t>::max() / 64)
        return PublishStatus::InvalidConfig;
    if (config.max_instruments == 0 || config.max_instruments > std::numeric_limits<InstrumentId>::max())
        return PublishStatus::InvalidConfig;
    if (config.heartbeat_interval_ns == 0) return PublishStatus::InvalidConfig;
    // The whole trading day must be representable on the command clock.
    if (config.day_origin > std::numeric_limits<Timestamp>::max() - kNanosecondsPerTradingDay)
        return PublishStatus::InvalidConfig;
    return PublishStatus::Ok;
}

// ITCH has no price band field, but every price it carries is Price(4):
// the instrument band must fit [1, 2^32 - 1].
[[nodiscard]] inline bool itch_representable_instrument(const InstrumentConfig& config) noexcept {
    return valid_instrument_config(config) && config.min_price > 0 && config.max_price <= kMaxItchPrice;
}

// GATEWAYS.md section 4 phase -> Stock Trading Action state.
[[nodiscard]] constexpr char itch_trading_state(Phase phase) noexcept {
    switch (phase) {
        case Phase::Continuous: return 'T';
        case Phase::PreOpen:
        case Phase::PreClose: return 'Q';
        case Phase::Halted:
        case Phase::Closed: return 'H';
    }
    return 'H';
}

// Cross type of a Cross Trade 'Q' by the phase the instrument is leaving.
[[nodiscard]] constexpr char itch_cross_type_leaving(Phase phase) noexcept {
    return phase == Phase::PreOpen ? 'O' : phase == Phase::PreClose ? 'C' : 'H';
}

[[nodiscard]] constexpr bool is_call_phase(Phase phase) noexcept {
    return phase == Phase::PreOpen || phase == Phase::PreClose;
}

template <class PacketSink>
class MarketDataPublisher {
public:
    // Setup only: allocates the packet buffer, ring and instrument table.
    // Throws std::invalid_argument for a config validate_publisher_config rejects.
    MarketDataPublisher(const PublisherConfig& config, PacketSink sink)
        : config_(checked(config)),
          packet_storage_(std::make_unique<char[]>(config.max_payload)),
          packet_builder_(std::span<char>(packet_storage_.get(), config.max_payload), config.max_payload),
          ring_(config.retransmit_buffer_messages),
          instruments_(std::make_unique<PublishedInstrument[]>(config.max_instruments + 1)),
          sink_(std::move(sink)) {
        (void)make_mold_session_name(config.session, session_name_);
        config_.session = {};  // the caller's string need not outlive construction; session_name_ holds it
    }

    MarketDataPublisher(const MarketDataPublisher&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;

    // ---- setup ----------------------------------------------------------
    [[nodiscard]] PublishStatus add_instrument(InstrumentId locate, const InstrumentConfig& instrument) noexcept {
        if (state_ != PublisherState::Setup) return PublishStatus::NotAllowedInState;
        if (locate == 0 || locate > config_.max_instruments || !itch_representable_instrument(instrument))
            return PublishStatus::InvalidInstrument;
        PublishedInstrument& slot = instruments_[locate];
        if (slot.registered) return PublishStatus::DuplicateInstrument;
        slot.registered = true;
        slot.symbol = instrument.symbol;
        slot.reference_price = instrument.reference_price;
        slot.round_lot_size = instrument.lot_size;
        slot.phase = instrument.initial_phase;
        return PublishStatus::Ok;
    }

    // Registers every engine instrument under its InstrumentId.
    template <class Engine>
    [[nodiscard]] PublishStatus add_instruments_from(const Engine& engine) noexcept {
        for (std::size_t index = 1; index <= engine.instrument_count(); ++index) {
            const auto locate = static_cast<InstrumentId>(index);
            const InstrumentConfig* instrument = engine.instrument(locate);
            if (instrument == nullptr) return PublishStatus::InvalidInstrument;
            const PublishStatus status = add_instrument(locate, *instrument);
            if (status != PublishStatus::Ok) return status;
        }
        return PublishStatus::Ok;
    }

    // System Event 'O', Stock Directory 'R' and Stock Trading Action 'H' per
    // registered instrument (ascending locate), System Event 'S'; flushed.
    [[nodiscard]] PublishStatus start_session(Timestamp now) {
        if (state_ != PublisherState::Setup) return PublishStatus::NotAllowedInState;
        std::uint64_t itch_time = 0;
        if (!itch_timestamp_from_event_time(now, config_.day_origin, itch_time))
            return PublishStatus::TimestampOutsideDay;
        state_ = PublisherState::Live;
        current_time_ = now;
        last_packet_time_ = now;
        last_imbalance_time_ = now;
        bool encoded = publish_system_event(itch_time, 'O');
        for (std::size_t locate = 1; encoded && locate <= config_.max_instruments; ++locate) {
            const PublishedInstrument& instrument = instruments_[locate];
            if (!instrument.registered) continue;
            protocol::itch50::StockDirectory directory;
            directory.stock_locate = static_cast<std::uint16_t>(locate);
            directory.timestamp = itch_time;
            directory.stock = instrument.symbol;
            directory.market_category = 'Q';
            directory.financial_status = 'N';
            directory.round_lot_size = instrument.round_lot_size;
            directory.round_lots_only = 'N';
            directory.issue_classification = 'C';
            directory.issue_subtype = {'Z', ' '};
            directory.authenticity = 'P';
            directory.short_sale_threshold = 'N';
            directory.ipo_flag = 'N';
            directory.luld_tier = ' ';
            directory.etp_flag = 'N';
            directory.etp_leverage = 0;
            directory.inverse_indicator = 'N';
            encoded = publish(directory) &&
                      publish_trading_action(static_cast<InstrumentId>(locate), itch_time, instrument.phase);
        }
        encoded = encoded && publish_system_event(itch_time, 'S');
        flush();
        if (!encoded) return stop(PublishStatus::UnrepresentableField);
        return PublishStatus::Ok;
    }

    // ---- hot path ---------------------------------------------------------
    // Publishes one command batch (or several, concatenated) and flushes the
    // last packet: a packet is never held across batches.
    [[nodiscard]] PublishStatus on_events(std::span<const Event> batch) {
        if (state_ != PublisherState::Live) return PublishStatus::NotAllowedInState;
        const PublishStatus validation = validate_batch(batch);
        if (validation != PublishStatus::Ok) return stop(validation);
        consumed_rested_order_id_ = 0;
        for (std::size_t index = 0; index < batch.size(); ++index) {
            if (!publish_event(batch, index)) {
                flush();  // everything sequenced must reach the wire
                return stop(PublishStatus::UnrepresentableField);
            }
        }
        flush();
        return PublishStatus::Ok;
    }
    [[nodiscard]] PublishStatus on_events(const EventBuffer& batch) {
        return on_events(std::span<const Event>(batch.begin(), batch.size()));
    }

    // Emits the open packet, if any. on_events already flushes; this is for
    // callers that interleave other output.
    void flush() {
        if (!packet_open_) return;
        sink_(packet_builder_.packet());
        packet_open_ = false;
        last_packet_time_ = current_time_;
        ++stats_.packets_sent;
    }

    // Timers: NOII for every call-phase instrument each noii_interval_ns,
    // then a heartbeat (or, after end of session, a repeated End of
    // Session) once heartbeat_interval_ns has passed without a packet.
    // `IndicativeSource` provides auction_indicative(id) and
    // last_trade_price(id) (exsim::MatchingEngine does).
    template <class IndicativeSource>
    [[nodiscard]] PublishStatus poll(Timestamp now, const IndicativeSource& source) {
        if (state_ == PublisherState::Live) {
            const PublishStatus status = publish_imbalances(now, source);
            if (status != PublishStatus::Ok) return status;
        }
        return poll_heartbeat(now);
    }

    template <class IndicativeSource>
    [[nodiscard]] PublishStatus publish_imbalances(Timestamp now, const IndicativeSource& source) {
        if (state_ != PublisherState::Live) return PublishStatus::NotAllowedInState;
        if (config_.noii_interval_ns == 0 || !interval_elapsed(last_imbalance_time_, now, config_.noii_interval_ns))
            return PublishStatus::Ok;
        last_imbalance_time_ = now;
        bool any_call_phase = false;
        for (std::size_t locate = 1; locate <= config_.max_instruments; ++locate)
            any_call_phase = any_call_phase || (instruments_[locate].registered && is_call_phase(instruments_[locate].phase));
        if (!any_call_phase) return PublishStatus::Ok;
        std::uint64_t itch_time = 0;
        if (!itch_timestamp_from_event_time(now, config_.day_origin, itch_time))
            return stop(PublishStatus::TimestampOutsideDay);
        current_time_ = now;
        for (std::size_t locate = 1; locate <= config_.max_instruments; ++locate) {
            const PublishedInstrument& instrument = instruments_[locate];
            if (!instrument.registered || !is_call_phase(instrument.phase)) continue;
            const auto id = static_cast<InstrumentId>(locate);
            const AuctionIndicative indicative = source.auction_indicative(id);
            const std::optional<Price> last_trade = source.last_trade_price(id);
            protocol::itch50::NetOrderImbalance imbalance;
            imbalance.stock_locate = id;
            imbalance.timestamp = itch_time;
            // Simplified NOII (GATEWAYS.md): paired shares only when crossed;
            // near = far = indicative price when crossed, else zero.
            imbalance.paired_shares = indicative.crossed ? indicative.paired_qty : 0;
            imbalance.imbalance_shares = indicative.imbalance_qty;
            imbalance.imbalance_direction = indicative.imbalance_side == Aggressor::Buy    ? 'B'
                                            : indicative.imbalance_side == Aggressor::Sell ? 'S'
                                                                                           : 'N';
            imbalance.stock = instrument.symbol;
            imbalance.far_price = indicative.crossed ? indicative.price : 0;
            imbalance.near_price = imbalance.far_price;
            imbalance.current_reference_price = last_trade ? *last_trade : instrument.reference_price;
            imbalance.cross_type = instrument.phase == Phase::PreOpen ? 'O' : 'C';
            imbalance.price_variation = ' ';
            if (!publish(imbalance)) {
                flush();
                return stop(PublishStatus::UnrepresentableField);
            }
            ++stats_.imbalance_messages_sent;
        }
        flush();
        return PublishStatus::Ok;
    }

    [[nodiscard]] PublishStatus poll_heartbeat(Timestamp now) {
        if (state_ != PublisherState::Live && state_ != PublisherState::Ended)
            return PublishStatus::NotAllowedInState;
        flush();
        if (!interval_elapsed(last_packet_time_, now, config_.heartbeat_interval_ns)) return PublishStatus::Ok;
        current_time_ = now;
        send_header_only_packet(state_ == PublisherState::Ended);
        return PublishStatus::Ok;
    }

    // ---- shutdown -----------------------------------------------------------
    // System Event 'E' (end of system hours), 'C' (end of messages), flush,
    // then the MoldUDP64 End of Session packet.
    [[nodiscard]] PublishStatus end_of_day(Timestamp now) {
        if (state_ != PublisherState::Live) return PublishStatus::NotAllowedInState;
        std::uint64_t itch_time = 0;
        if (!itch_timestamp_from_event_time(now, config_.day_origin, itch_time))
            return stop(PublishStatus::TimestampOutsideDay);
        current_time_ = now;
        const bool encoded = publish_system_event(itch_time, 'E') && publish_system_event(itch_time, 'C');
        flush();
        if (!encoded) return stop(PublishStatus::UnrepresentableField);
        return end_session(now);
    }

    // MoldUDP64 End of Session (count 0xFFFF, sequence = next sequence)
    // without further ITCH messages; also valid from Stopped.
    [[nodiscard]] PublishStatus end_session(Timestamp now) {
        if (state_ != PublisherState::Live && state_ != PublisherState::Stopped)
            return PublishStatus::NotAllowedInState;
        flush();
        current_time_ = now;
        state_ = PublisherState::Ended;
        send_header_only_packet(true);
        return PublishStatus::Ok;
    }

    // ---- queries --------------------------------------------------------------
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] const RetransmissionRing& ring() const noexcept { return ring_; }
    [[nodiscard]] const MoldSessionName& session_name() const noexcept { return session_name_; }
    [[nodiscard]] std::size_t max_payload() const noexcept { return config_.max_payload; }
    [[nodiscard]] PublisherState state() const noexcept { return state_; }
    [[nodiscard]] PublishStatus stop_reason() const noexcept { return stop_reason_; }
    [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }
    [[nodiscard]] Phase phase(InstrumentId locate) const noexcept {
        return locate != 0 && locate <= config_.max_instruments ? instruments_[locate].phase : Phase::Closed;
    }
    [[nodiscard]] PacketSink& sink() noexcept { return sink_; }
    [[nodiscard]] const PacketSink& sink() const noexcept { return sink_; }

private:
    struct PublishedInstrument {
        Symbol symbol{};
        Price reference_price = 0;
        Quantity round_lot_size = 0;
        Phase phase = Phase::Closed;  // tracked from PhaseChanged; the phase being left at AuctionSummary
        bool registered = false;
    };

    static const PublisherConfig& checked(const PublisherConfig& config) {
        if (validate_publisher_config(config) != PublishStatus::Ok)
            throw std::invalid_argument("invalid market-data publisher config");
        return config;
    }

    static bool interval_elapsed(Timestamp since, Timestamp now, std::uint64_t interval) noexcept {
        return now >= since && now - since >= interval;
    }

    static bool itch_price(Price price) noexcept { return price >= 0 && price <= kMaxItchPrice; }

    static bool produces_itch_output(const Event& event) noexcept {
        switch (event.type) {
            case EventType::Rested:
            case EventType::Fill:
            case EventType::Replaced:
            case EventType::PhaseChanged:
            case EventType::AuctionSummary: return true;
            case EventType::Canceled: return event.canceled.was_resting;
            default: return false;
        }
    }

    bool registered(InstrumentId locate) const noexcept {
        return locate != 0 && locate <= config_.max_instruments && instruments_[locate].registered;
    }

    // Read-only pre-pass: nothing of a failing batch reaches the wire.
    PublishStatus validate_batch(std::span<const Event> batch) const noexcept {
        for (const Event& event : batch) {
            if (!produces_itch_output(event)) continue;
            if (!registered(event.instrument)) return PublishStatus::UnknownInstrument;
            std::uint64_t itch_time = 0;
            if (!itch_timestamp_from_event_time(event.ts, config_.day_origin, itch_time))
                return PublishStatus::TimestampOutsideDay;
            bool prices_fit = true;
            switch (event.type) {
                case EventType::Rested: prices_fit = itch_price(event.rested.price); break;
                case EventType::Fill: prices_fit = itch_price(event.fill.price); break;
                case EventType::Replaced: prices_fit = itch_price(event.replaced.price); break;
                case EventType::AuctionSummary: prices_fit = itch_price(event.auction_summary.price); break;
                default: break;
            }
            if (!prices_fit) return PublishStatus::UnrepresentableField;
        }
        return PublishStatus::Ok;
    }

    PublishStatus stop(PublishStatus reason) noexcept {
        if (state_ == PublisherState::Live) {
            state_ = PublisherState::Stopped;
            stop_reason_ = reason;
        }
        return reason;
    }

    // True when the event at `index` is a priority-losing Replaced whose new
    // order's next event in the batch is its Rested (GATEWAYS.md: emit U and
    // consume that Rested). Returns the Rested event or nullptr.
    static const RestedEvent* rested_replacement(std::span<const Event> batch, std::size_t index) noexcept {
        const OrderId new_order_id = batch[index].replaced.new_order_id;
        for (std::size_t next = index + 1; next < batch.size(); ++next) {
            const Event& event = batch[next];
            switch (event.type) {
                case EventType::Rested:
                    if (event.rested.order_id == new_order_id) return &event.rested;
                    break;
                case EventType::Fill:
                    if (event.fill.buy.order_id == new_order_id || event.fill.sell.order_id == new_order_id)
                        return nullptr;
                    break;
                case EventType::Canceled:
                    if (event.canceled.order_id == new_order_id) return nullptr;
                    break;
                case EventType::Replaced:
                    if (event.replaced.old_order_id == new_order_id || event.replaced.new_order_id == new_order_id)
                        return nullptr;
                    break;
                case EventType::Accepted:
                    if (event.accepted.order_id == new_order_id) return nullptr;
                    break;
                default: break;
            }
        }
        return nullptr;
    }

    bool publish_event(std::span<const Event> batch, std::size_t index) {
        namespace itch = protocol::itch50;
        const Event& event = batch[index];
        if (!produces_itch_output(event)) return true;
        current_time_ = event.ts;
        const std::uint64_t itch_time = event.ts - config_.day_origin;  // validated by validate_batch
        const InstrumentId locate = event.instrument;
        PublishedInstrument& instrument = instruments_[locate];
        switch (event.type) {
            case EventType::Rested: {
                const RestedEvent& rested = event.rested;
                if (consumed_rested_order_id_ != 0 && rested.order_id == consumed_rested_order_id_) {
                    consumed_rested_order_id_ = 0;  // already shown by the Order Replace 'U'
                    return true;
                }
                itch::AddOrder add;
                add.stock_locate = locate;
                add.timestamp = itch_time;
                add.order_reference = rested.order_id;
                add.side = rested.side;
                add.shares = rested.leaves_qty;
                add.stock = instrument.symbol;
                add.price = rested.price;
                return publish(add);
            }
            case EventType::Fill: {
                const FillEvent& fill = event.fill;
                if (fill.aggressor == Aggressor::None) {
                    // Auction: both legs were displayed; buy leg first.
                    itch::OrderExecutedWithPrice executed;
                    executed.stock_locate = locate;
                    executed.timestamp = itch_time;
                    executed.shares = fill.qty;
                    executed.match_number = fill.match_id;
                    executed.printable = 'N';
                    executed.price = fill.price;
                    executed.order_reference = fill.buy.order_id;
                    if (!publish(executed)) return false;
                    executed.order_reference = fill.sell.order_id;
                    return publish(executed);
                }
                // Continuous: only the resting leg was ever displayed.
                itch::OrderExecuted executed;
                executed.stock_locate = locate;
                executed.timestamp = itch_time;
                executed.order_reference = fill.aggressor == Aggressor::Buy ? fill.sell.order_id : fill.buy.order_id;
                executed.shares = fill.qty;
                executed.match_number = fill.match_id;
                return publish(executed);
            }
            case EventType::Canceled: {
                const CanceledEvent& canceled = event.canceled;
                if (canceled.leaves_qty == 0) return publish_delete(locate, itch_time, canceled.order_id);
                return publish_cancel(locate, itch_time, canceled.order_id, canceled.canceled_qty);
            }
            case EventType::Replaced: {
                const ReplacedEvent& replaced = event.replaced;
                if (replaced.priority_kept) {
                    if (replaced.leaves_qty >= replaced.old_leaves_qty) return true;  // token change only
                    return publish_cancel(locate, itch_time, replaced.old_order_id,
                                          replaced.old_leaves_qty - replaced.leaves_qty);
                }
                const RestedEvent* rested = rested_replacement(batch, index);
                if (rested == nullptr) return publish_delete(locate, itch_time, replaced.old_order_id);
                itch::OrderReplace replace;
                replace.stock_locate = locate;
                replace.timestamp = itch_time;
                replace.original_reference = replaced.old_order_id;
                replace.new_reference = rested->order_id;
                replace.shares = rested->leaves_qty;
                replace.price = rested->price;
                consumed_rested_order_id_ = rested->order_id;
                return publish(replace);
            }
            case EventType::PhaseChanged:
                instrument.phase = event.phase_changed.new_phase;
                return publish_trading_action(locate, itch_time, event.phase_changed.new_phase);
            case EventType::AuctionSummary: {
                itch::CrossTrade cross;
                cross.stock_locate = locate;
                cross.timestamp = itch_time;
                cross.shares = event.auction_summary.matched_qty;
                cross.stock = instrument.symbol;
                cross.price = event.auction_summary.price;
                cross.match_number = event.auction_summary.match_id;
                cross.cross_type = itch_cross_type_leaving(instrument.phase);
                return publish(cross);
            }
            default: return true;
        }
    }

    bool publish_delete(InstrumentId locate, std::uint64_t itch_time, OrderId order_id) {
        protocol::itch50::OrderDelete deleted;
        deleted.stock_locate = locate;
        deleted.timestamp = itch_time;
        deleted.order_reference = order_id;
        return publish(deleted);
    }

    bool publish_cancel(InstrumentId locate, std::uint64_t itch_time, OrderId order_id, Quantity canceled_shares) {
        protocol::itch50::OrderCancel cancel;
        cancel.stock_locate = locate;
        cancel.timestamp = itch_time;
        cancel.order_reference = order_id;
        cancel.shares = canceled_shares;
        return publish(cancel);
    }

    bool publish_trading_action(InstrumentId locate, std::uint64_t itch_time, Phase phase) {
        protocol::itch50::StockTradingAction action;
        action.stock_locate = locate;
        action.timestamp = itch_time;
        action.stock = instruments_[locate].symbol;
        action.trading_state = itch_trading_state(phase);
        action.reserved = ' ';
        return publish(action);
    }

    bool publish_system_event(std::uint64_t itch_time, char event_code) {
        protocol::itch50::SystemEvent system;
        system.timestamp = itch_time;
        system.event = event_code;
        return publish(system);
    }

    template <class Message>
    bool publish(const Message& message) {
        char bytes[kMaxItchMessageSize];
        const std::size_t length = protocol::itch50::encode(message, bytes, sizeof bytes);
        if (length == 0) return false;
        return sequence_message(std::span<const char>(bytes, length));
    }

    // Assigns the next sequence number, keeps the message for retransmission
    // and appends it to the open packet, emitting the packet first if full.
    bool sequence_message(std::span<const char> message) {
        if (!packet_open_) open_packet();
        protocol::Status appended = packet_builder_.append(message);
        if (appended == protocol::Status::full) {
            flush();
            open_packet();
            appended = packet_builder_.append(message);
        }
        if (appended != protocol::Status::ok) return false;
        if (!ring_.append(next_sequence_, message)) return false;
        ++next_sequence_;
        ++stats_.messages_sequenced;
        return true;
    }

    void open_packet() noexcept {
        (void)packet_builder_.reset(std::string_view(session_name_.data(), session_name_.size()), next_sequence_);
        packet_open_ = true;
    }

    void send_header_only_packet(bool end_of_session) {
        (void)packet_builder_.reset(std::string_view(session_name_.data(), session_name_.size()), next_sequence_,
                                    end_of_session);
        sink_(packet_builder_.packet());
        packet_open_ = false;
        last_packet_time_ = current_time_;
        ++stats_.packets_sent;
        if (end_of_session) {
            ++stats_.end_of_session_packets_sent;
        } else {
            ++stats_.heartbeats_sent;
        }
    }

    PublisherConfig config_;
    MoldSessionName session_name_{};
    std::unique_ptr<char[]> packet_storage_;
    protocol::moldudp64::PacketBuilder packet_builder_;
    RetransmissionRing ring_;
    std::unique_ptr<PublishedInstrument[]> instruments_;
    PacketSink sink_;
    PublisherState state_ = PublisherState::Setup;
    PublishStatus stop_reason_ = PublishStatus::Ok;
    PublisherStats stats_{};
    std::uint64_t next_sequence_ = 1;
    OrderId consumed_rested_order_id_ = 0;
    Timestamp current_time_ = 0;
    Timestamp last_packet_time_ = 0;
    Timestamp last_imbalance_time_ = 0;
    bool packet_open_ = false;
};

}  // namespace exsim::md

#endif
