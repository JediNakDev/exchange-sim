#ifndef EXSIM_MD_MDP_PUBLISHER_HPP
#define EXSIM_MD_MDP_PUBLISHER_HPP

#include "matching_engine.hpp"
#include "md/publisher.hpp"
#include "protocol/mdp3.hpp"
#include "server/config.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace exsim::md {
enum class MdpStream { Incremental, Snapshot, Instrument };

// Original incremental packets, including their original technical headers.
// Eviction never changes a sequence number or manufactures missing history.
class MdpReplayRing {
public:
    MdpReplayRing(std::size_t capacity, std::size_t payload)
        : capacity_(capacity), payload_(payload) {
        if (!capacity || !payload || capacity > SIZE_MAX / payload)
            throw std::invalid_argument("MDP replay capacity");
        bytes_ = std::make_unique<char[]>(capacity * payload);
        lengths_ = std::make_unique<std::size_t[]>(capacity);
    }
    bool append(std::uint32_t sequence, std::span<const char> packet) noexcept {
        if (sequence != next_ || packet.size() > payload_) return false;
        const auto slot = sequence % capacity_;
        std::memcpy(bytes_.get() + slot * payload_, packet.data(), packet.size());
        lengths_[slot] = packet.size();
        ++next_;
        if (next_ - first_ > capacity_) ++first_;
        return true;
    }
    [[nodiscard]] std::span<const char> get(std::uint32_t sequence) const noexcept {
        if (sequence < first_ || sequence >= next_) return {};
        const auto slot = sequence % capacity_;
        return {bytes_.get() + slot * payload_, lengths_[slot]};
    }
    [[nodiscard]] std::uint64_t first_sequence() const noexcept { return first_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }
private:
    std::size_t capacity_, payload_;
    std::uint64_t first_ = 1, next_ = 1;
    std::unique_ptr<char[]> bytes_;
    std::unique_ptr<std::size_t[]> lengths_;
};

// One channel, outright instruments, MBP up to ten levels. Each command is
// one event; packets may split an event but only its last message sets EOE.
// Snapshot loops run synchronously between commands on the same reactor.
template<class Sink>
class MdpPublisher {
public:
    MdpPublisher(const server::ServerConfig& config, Sink sink)
        : config_(config.market_data), instruments_(config.instruments), states_(config.instruments.size() + 1),
          ring_(config_.replay_buffer_packets, config_.max_payload), sink_(std::move(sink)) {
        if (config_.book_depth == 0 || config_.book_depth > 10 || config_.max_payload < 1400 ||
            config_.max_payload > 65507 || config_.channel_id == 0 || config_.channel_id > INT16_MAX)
            throw std::invalid_argument("MDP publisher configuration");
        packet_.resize(config_.max_payload);
        touched_.reserve(instruments_.size());
    }

    PublishStatus start(Timestamp now, std::uint64_t epoch_now, const MatchingEngine& engine) {
        if (state_ != PublisherState::Setup) return PublishStatus::NotAllowedInState;
        if (epoch_now < now || engine.instrument_count() != instruments_.size() || engine.live_order_count() != 0)
            return PublishStatus::InvalidConfig;
        for (std::size_t i = 0; i < instruments_.size(); ++i) {
            const auto& instrument = instruments_[i];
            std::int64_t price = 0;
            if (instrument.cme_security_id <= 0 || instrument.cme_security_id == INT32_MAX ||
                !protocol::cme::price_to_price9(instrument.max_price, price) ||
                instrument.lot_size > INT32_MAX / 10000u || instrument.max_order_qty >= INT32_MAX)
                return PublishStatus::InvalidInstrument;
            for (std::size_t j = 0; j < i; ++j)
                if (instruments_[j].cme_security_id == instrument.cme_security_id) return PublishStatus::DuplicateInstrument;
            states_[i + 1].phase = engine.phase(static_cast<InstrumentId>(i + 1));
            states_[i + 1].last_event = epoch_now;
        }
        epoch_origin_ = epoch_now - now;
        definition_time_ = epoch_now;
        engine_ = &engine;
        state_ = PublisherState::Live;
        last_incremental_ = now;
        const auto size = protocol::mdp3::encode_channel_reset(epoch_now, static_cast<std::int16_t>(config_.channel_id), message_);
        emit(size, now, MdpStream::Incremental);
        definitions(now);
        snapshots(now);
        return reason_;
    }

    PublishStatus on_events(std::span<const Event> events) noexcept {
        if (state_ != PublisherState::Live) return PublishStatus::NotAllowedInState;
        for (const auto id : touched_) {
            auto& state = states_[id];
            state.touched = false;
            state.trade_count = 0;
            state.update_count = 0;
        }
        touched_.clear();
        Timestamp now = last_incremental_;
        std::size_t messages = 0;
        std::size_t trade_messages = 0;
        std::size_t book_messages = 0;
        for (const auto& event : events) {
            now = std::max(now, event.ts);
            if (event.ts > UINT64_MAX - epoch_origin_) return fail(PublishStatus::TimestampOutsideDay);
            if (event.instrument == 0) {
                if (event.type != EventType::Rejected && event.type != EventType::MassCancelDone)
                    return fail(PublishStatus::UnknownInstrument);
                continue;
            }
            if (event.instrument >= states_.size()) return fail(PublishStatus::UnknownInstrument);
            auto& state = states_[event.instrument];
            if (!state.touched) {
                state.touched = true;
                touched_.push_back(event.instrument);
            }
            if (event.type == EventType::Fill) {
                std::int64_t price = 0;
                if (!protocol::cme::price_to_price9(event.fill.price, price) || event.fill.qty >= INT32_MAX)
                    return fail(PublishStatus::UnrepresentableField);
                ++state.trade_count;
                ++trade_messages;
                ++messages;
            }
        }
        std::sort(touched_.begin(), touched_.end());
        for (const auto i : touched_) {
            auto& state = states_[i];
            const auto id = static_cast<InstrumentId>(i);
            state.next_bid_count = engine_->depth(id, Side::BUY, state.next_bids.data(), config_.book_depth);
            state.next_ask_count = engine_->depth(id, Side::SELL, state.next_asks.data(), config_.book_depth);
            if (!derive_updates(state, state.bids, state.bid_count, state.next_bids, state.next_bid_count, protocol::mdp3::BookSide::Bid) ||
                !derive_updates(state, state.asks, state.ask_count, state.next_asks, state.next_ask_count, protocol::mdp3::BookSide::Ask) ||
                state.report_sequence + std::uint64_t{state.trade_count} + state.update_count >= UINT32_MAX)
                return fail(PublishStatus::UnrepresentableField);
            if (state.update_count) { ++messages; ++book_messages; }
            if (state.phase != engine_->phase(id)) ++messages;
        }
        if (ring_.next_sequence() + messages >= UINT32_MAX) return fail(PublishStatus::UnrepresentableField);
        // Validation above covers the whole batch before any sequence or book
        // projection changes. Preallocated buffers fit every supported message.
        for (const auto& event : events) {
            if (event.type != EventType::Fill) continue;
            auto& state = states_[event.instrument];
            protocol::mdp3::TradeEntry trade;
            (void)protocol::cme::price_to_price9(event.fill.price, trade.price);
            trade.quantity = static_cast<std::int32_t>(event.fill.qty);
            trade.security_id = instruments_[event.instrument - 1].cme_security_id;
            trade.report_sequence = ++state.report_sequence;
            trade.order_count = 2;
            trade.aggressor_side = static_cast<std::uint8_t>(event.fill.aggressor);
            if (event.fill.match_id < UINT32_MAX) trade.trade_entry_id = static_cast<std::uint32_t>(event.fill.match_id);
            const std::array trades{trade};
            const std::array orders{protocol::mdp3::TradeOrder{event.fill.buy.order_id, trade.quantity},
                                    protocol::mdp3::TradeOrder{event.fill.sell.order_id, trade.quantity}};
            const auto flag = static_cast<std::uint8_t>((--messages == 0 ? protocol::mdp3::kEndOfEvent : 0) |
                                                       (--trade_messages == 0 ? protocol::mdp3::kLastTrade : 0));
            emit(protocol::mdp3::encode_trades(epoch_origin_ + event.ts, flag, trades, orders, message_), now, MdpStream::Incremental);
        }
        for (const auto i : touched_) {
            auto& state = states_[i];
            const auto security = instruments_[i - 1].cme_security_id;
            if (state.update_count) {
                for (std::size_t j = 0; j < state.update_count; ++j) {
                    state.updates[j].security_id = security;
                    state.updates[j].report_sequence = ++state.report_sequence;
                }
                const auto flag = static_cast<std::uint8_t>((--messages == 0 ? protocol::mdp3::kEndOfEvent : 0) |
                                                           (--book_messages == 0 ? 0x04 : 0));
                emit(protocol::mdp3::encode_book(epoch_origin_ + now, flag,
                     std::span(state.updates).first(state.update_count), message_), now, MdpStream::Incremental);
            }
            const auto phase = engine_->phase(static_cast<InstrumentId>(i));
            const bool published_event = state.update_count || state.trade_count || state.phase != phase;
            if (state.phase != phase) {
                const auto flag = --messages == 0 ? protocol::mdp3::kEndOfEvent : std::uint8_t{0};
                emit(protocol::mdp3::encode_security_status(epoch_origin_ + now, security, trade_date(now), phase, flag, message_),
                     now, MdpStream::Incremental);
            }
            state.bids = state.next_bids; state.bid_count = state.next_bid_count;
            state.asks = state.next_asks; state.ask_count = state.next_ask_count;
            state.phase = phase;
            if (published_event) state.last_event = epoch_origin_ + now;
        }
        return reason_;
    }

    void poll(Timestamp now, std::uint64_t heartbeat_interval_ns) noexcept {
        if (state_ != PublisherState::Live) return;
        if (now >= last_snapshot_ && now - last_snapshot_ >= std::uint64_t{config_.snapshot_interval_ms} * 1000000) snapshots(now);
        if (now >= last_definition_ && now - last_definition_ >= std::uint64_t{config_.instrument_interval_ms} * 1000000) definitions(now);
        if (now >= last_incremental_ && now - last_incremental_ >= heartbeat_interval_ns)
            emit(protocol::mdp3::encode_heartbeat(message_), now, MdpStream::Incremental);
    }
    void shutdown(Timestamp now) noexcept {
        if (state_ != PublisherState::Live) return;
        snapshots(now);
        state_ = PublisherState::Ended;
    }
    [[nodiscard]] PublisherState state() const noexcept { return state_; }
    [[nodiscard]] PublishStatus stop_reason() const noexcept { return reason_; }
    [[nodiscard]] const MdpReplayRing& ring() const noexcept { return ring_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return ring_.next_sequence(); }

private:
    struct State {
        std::array<LevelInfo, 10> bids{}, asks{}, next_bids{}, next_asks{};
        std::size_t bid_count = 0, ask_count = 0, next_bid_count = 0, next_ask_count = 0;
        std::array<protocol::mdp3::BookEntry, 40> updates{};
        std::size_t update_count = 0;
        std::uint32_t report_sequence = 0, trade_count = 0;
        std::uint64_t last_event = 0;
        Phase phase = Phase::Continuous;
        bool touched = false;
    };

    bool derive_updates(State& state, std::array<LevelInfo, 10> current, std::size_t count,
                        const std::array<LevelInfo, 10>& desired, std::size_t desired_count,
                        protocol::mdp3::BookSide side) noexcept {
        using Action = protocol::mdp3::UpdateAction;
        auto append = [&](const LevelInfo& level, std::size_t rank, Action action) {
            if (level.qty >= INT32_MAX || level.order_count >= INT32_MAX || state.update_count == state.updates.size()) return false;
            protocol::mdp3::BookEntry entry;
            if (!protocol::cme::price_to_price9(level.price, entry.price)) return false;
            entry.quantity = static_cast<std::int32_t>(level.qty);
            entry.order_count = static_cast<std::int32_t>(level.order_count);
            entry.level = static_cast<std::uint8_t>(rank + 1);
            entry.action = action; entry.side = side;
            state.updates[state.update_count++] = entry;
            return true;
        };
        for (std::size_t i = 0; i < desired_count; ++i) {
            if (desired[i].qty >= INT32_MAX || desired[i].order_count >= INT32_MAX) return false;
            if (i < count && current[i].price == desired[i].price) {
                if ((current[i].qty != desired[i].qty || current[i].order_count != desired[i].order_count) &&
                    !append(desired[i], i, Action::Change)) return false;
                current[i] = desired[i];
                continue;
            }
            auto found = i;
            while (found < count && current[found].price != desired[i].price) ++found;
            if (found < count) {
                while (current[i].price != desired[i].price) {
                    if (!append(current[i], i, Action::Delete)) return false;
                    for (std::size_t j = i + 1; j < count; ++j) current[j - 1] = current[j];
                    --count;
                }
                if ((current[i].qty != desired[i].qty || current[i].order_count != desired[i].order_count) &&
                    !append(desired[i], i, Action::Change)) return false;
            } else {
                // Remove old, better levels before inserting a worse price.
                // Keeping them temporarily below the new level would create
                // an unsorted book for clients applying entries in order.
                while (i < count && (side == protocol::mdp3::BookSide::Bid ?
                       current[i].price > desired[i].price : current[i].price < desired[i].price)) {
                    if (!append(current[i], i, Action::Delete)) return false;
                    for (std::size_t j = i + 1; j < count; ++j) current[j - 1] = current[j];
                    --count;
                }
                if (!append(desired[i], i, Action::New)) return false;
                count = std::min(count + 1, std::size_t{config_.book_depth});
                for (std::size_t j = count - 1; j > i; --j) current[j] = current[j - 1];
            }
            current[i] = desired[i];
        }
        while (count > desired_count) {
            --count;
            if (!append(current[count], count, Action::Delete)) return false;
        }
        return true;
    }

    PublishStatus fail(PublishStatus reason) noexcept { state_ = PublisherState::Stopped; return reason_ = reason; }
    std::uint16_t trade_date(Timestamp now) const noexcept {
        const auto days = (epoch_origin_ + now) / 86400000000000ull;
        return days < UINT16_MAX ? static_cast<std::uint16_t>(days) : UINT16_MAX;
    }
    void emit(std::size_t size, Timestamp now, MdpStream stream) noexcept {
        if (state_ != PublisherState::Live) return;
        if (!size || now > UINT64_MAX - epoch_origin_) { (void)fail(PublishStatus::UnrepresentableField); return; }
        const auto next = stream == MdpStream::Incremental ? ring_.next_sequence() :
                          stream == MdpStream::Snapshot ? snapshot_sequence_ : definition_sequence_;
        if (next >= UINT32_MAX) { (void)fail(PublishStatus::UnrepresentableField); return; }
        const auto bytes = protocol::cme::encode_mdp_packet(static_cast<std::uint32_t>(next), epoch_origin_ + now,
                            std::span(message_).first(size), packet_);
        if (!bytes) { (void)fail(PublishStatus::UnrepresentableField); return; }
        const auto packet = std::span<const char>(packet_.data(), bytes);
        if (stream == MdpStream::Incremental) {
            if (!ring_.append(static_cast<std::uint32_t>(next), packet)) { (void)fail(PublishStatus::UnrepresentableField); return; }
            last_incremental_ = now;
        } else if (stream == MdpStream::Snapshot) ++snapshot_sequence_;
        else ++definition_sequence_;
        sink_(packet, stream);
    }
    void definitions(Timestamp now) noexcept {
        last_definition_ = now;
        for (std::size_t i = 0; i < instruments_.size(); ++i) {
            const auto& instrument = instruments_[i];
            protocol::mdp3::FutureDefinition definition;
            const auto end = std::find(instrument.cme_symbol.begin(), instrument.cme_symbol.end(), char{0});
            definition.symbol = end == instrument.cme_symbol.begin() ? instrument.symbol.view() :
                std::string_view(instrument.cme_symbol.data(), static_cast<std::size_t>(end - instrument.cme_symbol.begin()));
            definition.security_id = instrument.cme_security_id;
            definition.channel_id = static_cast<std::int16_t>(config_.channel_id);
            definition.market_segment = config_.market_segment_id;
            definition.depth = config_.book_depth;
            definition.phase = states_[i + 1].phase;
            definition.total_reports = static_cast<std::uint32_t>(instruments_.size());
            definition.timestamp = definition_time_;
            definition.trade_date = trade_date(now);
            definition.lot_size = instrument.lot_size;
            definition.max_quantity = instrument.max_order_qty;
            definition.maturity_date = instrument.cme_maturity_date;
            definition.contract_size = instrument.cme_contract_size;
            (void)protocol::cme::price_to_price9(instrument.tick_size, definition.tick_price9);
            (void)protocol::cme::price_to_price9(instrument.reference_price, definition.reference_price9);
            (void)protocol::cme::price_to_price9(instrument.min_price, definition.low_price9);
            (void)protocol::cme::price_to_price9(instrument.max_price, definition.high_price9);
            emit(protocol::mdp3::encode_future_definition(definition, message_), now, MdpStream::Instrument);
        }
    }
    void snapshots(Timestamp now) noexcept {
        last_snapshot_ = now;
        const auto boundary = static_cast<std::uint32_t>(ring_.next_sequence() - 1);
        for (std::size_t i = 1; i < states_.size(); ++i) {
            const auto& state = states_[i];
            protocol::mdp3::Snapshot snapshot;
            snapshot.last_packet_sequence = boundary;
            snapshot.total_reports = static_cast<std::uint32_t>(instruments_.size());
            snapshot.security_id = instruments_[i - 1].cme_security_id;
            snapshot.report_sequence = state.report_sequence;
            snapshot.transact_time = state.last_event;
            snapshot.last_update_time = definition_time_;
            snapshot.trade_date = trade_date(now);
            snapshot.trading_status = protocol::mdp3::trading_status(state.phase);
            (void)protocol::cme::price_to_price9(instruments_[i - 1].min_price, snapshot.low_limit);
            (void)protocol::cme::price_to_price9(instruments_[i - 1].max_price, snapshot.high_limit);
            std::array<protocol::mdp3::SnapshotEntry, 20> entries{};
            std::size_t count = 0;
            auto side = [&](const auto& levels, std::size_t size, char type) {
                for (std::size_t j = 0; j < size; ++j) {
                    auto& entry = entries[count++];
                    (void)protocol::cme::price_to_price9(levels[j].price, entry.price);
                    entry.quantity = static_cast<std::int32_t>(levels[j].qty);
                    entry.order_count = static_cast<std::int32_t>(levels[j].order_count);
                    entry.level = static_cast<std::int8_t>(j + 1);
                    entry.entry_type = type;
                }
            };
            side(state.bids, state.bid_count, '0'); side(state.asks, state.ask_count, '1');
            emit(protocol::mdp3::encode_snapshot(snapshot, std::span(entries).first(count), message_), now, MdpStream::Snapshot);
        }
    }

    server::MarketDataSection config_;
    std::vector<InstrumentConfig> instruments_;
    std::vector<State> states_;
    std::vector<InstrumentId> touched_;
    MdpReplayRing ring_;
    Sink sink_;
    const MatchingEngine* engine_ = nullptr;
    PublisherState state_ = PublisherState::Setup;
    PublishStatus reason_ = PublishStatus::Ok;
    std::uint64_t epoch_origin_ = 0, definition_time_ = 0, snapshot_sequence_ = 1, definition_sequence_ = 1;
    Timestamp last_incremental_ = 0, last_snapshot_ = 0, last_definition_ = 0;
    std::array<char, 1360> message_{};
    std::vector<char> packet_;
};
}  // namespace exsim::md
#endif
