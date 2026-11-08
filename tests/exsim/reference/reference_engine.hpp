#ifndef EXSIM_TESTS_REFERENCE_REFERENCE_ENGINE_HPP
#define EXSIM_TESTS_REFERENCE_REFERENCE_ENGINE_HPP

// ReferenceEngine: the test oracle for exsim::MatchingEngine.
//
// A literal, deliberately naive transcription of docs/SPEC.md. Every
// handler is written in the same order as the spec section it implements
// (section numbers are quoted in comments) so a reviewer can check it line
// by line against the document. It optimises for being obviously correct:
// std::map / std::list everywhere, linear scans, no pools, no caching. It
// allocates freely and must never be used on a hot path.
//
// Spec ambiguities are resolved in one place each (search for "AMBIGUITY")
// and listed in docs/SCENARIOS.md ("Remaining contract interpretations").
//
// Mutations: the harness self-tests need a way to prove the differential
// fuzzer actually detects behavioural differences. `Mutations` switches on
// small, realistic bugs (wrong FIFO order, off-by-one tie-break, ...). With
// the default (all false) the engine implements SPEC.md exactly.

#include <exsim/command.hpp>
#include <exsim/event.hpp>
#include <exsim/instrument.hpp>
#include <exsim/matching_engine.hpp>
#include <exsim/types.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace exsim::ref {

// Deliberate deviations from SPEC.md, used only by harness self-tests.
struct Mutations {
    bool lifo_within_level = false;            // 4.5: match the newest order of a level first
    bool uncross_tie_prefers_higher = false;   // 9: final tie-break picks the higher price
    bool fok_counts_self_matches = false;      // 4.3: FOK pre-check ignores STP
    bool replace_qty_up_keeps_priority = false;  // 6: qty increase at same price keeps priority
    bool mass_cancel_descending = false;       // 7: mass cancel in descending OrderId order
    bool capacity_off_by_one = false;          // 4.1.9: reject one order early
    bool end_of_day_expires_gtc = false;       // 8.3: GTC orders also expire
    bool stp_uses_resting_mode = false;        // 4.5: STP mode taken from the resting order

    [[nodiscard]] bool any() const noexcept {
        return lifo_within_level || uncross_tie_prefers_higher || fok_counts_self_matches ||
               replace_qty_up_keeps_priority || mass_cancel_descending || capacity_off_by_one ||
               end_of_day_expires_gtc || stp_uses_resting_mode;
    }
};

class ReferenceEngine {
public:
    explicit ReferenceEngine(const EngineConfig& config = {}, const Mutations& mutations = {})
        : config_(config), mutations_(mutations) { (void)required_event_capacity(config_); }

    ReferenceEngine(const ReferenceEngine&) = delete;
    ReferenceEngine& operator=(const ReferenceEngine&) = delete;

    // ---- SPEC 2: instruments ---------------------------------------------
    [[nodiscard]] InstrumentId add_instrument(const InstrumentConfig& config) {
        if (!valid_instrument_config(config) || find_instrument(config.symbol) != 0 ||
            instruments_.size() >= config_.max_instruments) return 0;
        Instrument instrument;
        instrument.config = config;
        instrument.phase = config.initial_phase;
        instruments_.push_back(std::move(instrument));
        return static_cast<InstrumentId>(instruments_.size());
    }

    [[nodiscard]] HandleResult handle(const Command& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
    [[nodiscard]] HandleResult handle(const NewOrder& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
    [[nodiscard]] HandleResult handle(const CancelOrder& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
    [[nodiscard]] HandleResult handle(const ReplaceOrder& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
    [[nodiscard]] HandleResult handle(const MassCancel& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
    [[nodiscard]] HandleResult handle(const SetPhase& cmd, EventBuffer& out) {
        if (!out.can_append(required_event_capacity(config_))) return HandleResult::InsufficientOutput;
        apply(cmd, out);
        return HandleResult::Applied;
    }
private:
    // ---- Commands ---------------------------------------------------------
    void apply(const Command& cmd, EventBuffer& out) {
        switch (cmd.type) {
            case CommandType::NewOrder: apply(cmd.new_order, out); return;
            case CommandType::Cancel: apply(cmd.cancel, out); return;
            case CommandType::Replace: apply(cmd.replace, out); return;
            case CommandType::MassCancel: apply(cmd.mass_cancel, out); return;
            case CommandType::SetPhase: apply(cmd.set_phase, out); return;
        }
        // AMBIGUITY: an out-of-range CommandType is not specified; it is ignored.
    }

    // SPEC 4: NewOrder.
    void apply(const NewOrder& cmd, EventBuffer& out) {
        const RejectReason reason = validate_new_order(cmd);
        if (reason != RejectReason::None) {
            // SPEC 12: echo the request session.
            emit_rejected(out, cmd.ts, cmd.instrument, CommandType::NewOrder, reason, cmd.session, cmd.cl_ord_id,
                          /*orig_cl_ord_id=*/0, /*order_id=*/0);
            return;
        }
        Instrument& instrument = instrument_state(cmd.instrument);

        // 4.2 Acceptance.
        Order order;
        order.order_id = next_order_id_++;
        order.cl_ord_id = cmd.cl_ord_id;
        order.instrument = cmd.instrument;
        order.session = cmd.session;
        order.owner = cmd.owner;
        order.side = cmd.side;
        order.type = cmd.type;
        order.tif = cmd.tif;
        order.stp = cmd.stp;
        order.post_only = cmd.post_only;
        order.price = cmd.price;  // verbatim, also for market orders
        order.qty = cmd.qty;
        order.leaves_qty = cmd.qty;
        order.cum_qty = 0;
        emit_accepted(out, cmd.ts, order);

        if (is_call_phase(instrument.phase)) {
            rest(order, cmd.ts, out);  // 4.4, no matching, no cross check
            return;
        }

        if (order.tif == TimeInForce::FOK && !fok_can_fill(instrument, order)) {
            // 4.3: AMBIGUITY: the pre-check also runs for market FOK orders
            // ("FOK pre-check if tif == FOK"); a market order's TIF is only
            // ignored for the disposition in 4.6.
            emit_canceled(out, cmd.ts, order, CancelReason::FillOrKill, /*canceled_qty=*/order.qty,
                          /*request_cl_ord_id=*/0, /*was_resting=*/false, /*cum_qty_override=*/0);
            return;
        }
        const bool aggressor_canceled_by_stp = match(instrument, order, cmd.ts, out);
        dispose_after_matching(order, aggressor_canceled_by_stp, cmd.ts, out);
    }

    // SPEC 5: CancelOrder.
    void apply(const CancelOrder& cmd, EventBuffer& out) {
        Order* order = find_live(cmd.session, cmd.orig_cl_ord_id);
        if (order == nullptr) {
            emit_rejected(out, cmd.ts, /*instrument=*/0, CommandType::Cancel, RejectReason::UnknownOrder, cmd.session,
                          cmd.cl_ord_id, cmd.orig_cl_ord_id, /*order_id=*/0);
            return;
        }
        const Order removed = *order;
        Quantity target = 0;
        if (cmd.reduce_to_total_qty) {
            target = cmd.reduce_to_total_qty > order->cum_qty ? cmd.reduce_to_total_qty - order->cum_qty : 0;
            if (target >= order->leaves_qty) return;
        }
        emit_canceled(out, cmd.ts, removed, CancelReason::UserRequested,
                      removed.leaves_qty - target, cmd.cl_ord_id, true, std::nullopt, target);
        if (!target) remove_from_book(removed.order_id);
        else { order->leaves_qty = target; order->qty = order->cum_qty + target; }
    }

    // SPEC 6: ReplaceOrder.
    void apply(const ReplaceOrder& cmd, EventBuffer& out) {
        Order* found = find_live(cmd.session, cmd.orig_cl_ord_id);
        const ClOrdId new_token = cmd.cl_ord_id != 0 ? cmd.cl_ord_id : cmd.orig_cl_ord_id;
        auto reject = [&](RejectReason reason) {
            emit_rejected(out, cmd.ts, found ? found->instrument : InstrumentId{0}, CommandType::Replace, reason,
                          cmd.session, cmd.cl_ord_id, cmd.orig_cl_ord_id, found ? found->order_id : OrderId{0});
        };
        if (found == nullptr) return reject(RejectReason::UnknownOrder);
        if (cmd.cl_ord_id > kMaxClOrdId) return reject(RejectReason::InvalidClOrdId);
        if (new_token != cmd.orig_cl_ord_id && find_live(cmd.session, new_token) != nullptr) {
            return reject(RejectReason::DuplicateClOrdId);
        }
        Instrument& instrument = instrument_state(found->instrument);
        if (!quantity_valid(instrument.config, cmd.qty)) return reject(RejectReason::InvalidQuantity);
        if (!price_valid(instrument.config, cmd.price)) return reject(RejectReason::InvalidPrice);
        if (instrument.phase == Phase::Closed) return reject(RejectReason::MarketClosed);
        if (instrument.phase == Phase::Halted) return reject(RejectReason::InstrumentHalted);
        if (found->post_only && instrument.phase == Phase::Continuous &&
            would_cross_opposite_best(instrument, found->side, cmd.price)) {
            return reject(RejectReason::PostOnlyWouldCross);
        }

        const Order old = *found;

        if (cmd.qty <= old.cum_qty) {
            // Reduced to (or below) the filled quantity: the order is done.
            emit_canceled(out, cmd.ts, old, CancelReason::UserRequested, old.leaves_qty, cmd.cl_ord_id,
                          /*was_resting=*/true);
            remove_from_book(old.order_id);
            return;
        }

        const Quantity new_leaves = cmd.qty - old.cum_qty;
        bool priority_kept = !cmd.force_priority_loss && cmd.price == old.price && new_leaves <= old.leaves_qty;
        if (mutations_.replace_qty_up_keeps_priority && cmd.price == old.price) priority_kept = true;

        if (priority_kept) {
            Order& order = *found;
            reindex_token(order, new_token);
            order.qty = cmd.qty;
            order.leaves_qty = new_leaves;
            emit_replaced(out, cmd.ts, old, order, /*priority_kept=*/true);
            return;
        }

        // Priority lost: leave the book, get a new OrderId, re-enter as aggressor.
        remove_from_book(old.order_id);
        Order order = old;
        order.order_id = next_order_id_++;
        order.cl_ord_id = new_token;
        order.price = cmd.price;
        order.qty = cmd.qty;
        order.leaves_qty = new_leaves;
        emit_replaced(out, cmd.ts, old, order, /*priority_kept=*/false);
        if (is_call_phase(instrument.phase)) {
            rest(order, cmd.ts, out);
            return;
        }
        const bool aggressor_canceled_by_stp = match(instrument, order, cmd.ts, out);
        dispose_after_matching(order, aggressor_canceled_by_stp, cmd.ts, out);
    }

    // SPEC 7: MassCancel.
    void apply(const MassCancel& cmd, EventBuffer& out) {
        const CancelReason reason = cmd.cancel_on_disconnect ? CancelReason::CancelOnDisconnect : CancelReason::MassCancel;
        std::vector<OrderId> targets;  // ascending OrderId (std::map order)
        for (const auto& [order_id, order] : live_) {
            if (order.session == cmd.session && (cmd.instrument == 0 || order.instrument == cmd.instrument)) {
                targets.push_back(order_id);
            }
        }
        if (mutations_.mass_cancel_descending) std::reverse(targets.begin(), targets.end());
        for (const OrderId order_id : targets) {
            const Order order = live_.at(order_id);
            emit_canceled(out, cmd.ts, order, reason, order.leaves_qty, cmd.cl_ord_id, /*was_resting=*/true);
            remove_from_book(order_id);
        }
        Event e = make_event(EventType::MassCancelDone, cmd.instrument, cmd.ts);
        e.mass_cancel_done = MassCancelDoneEvent{cmd.session, cmd.cl_ord_id,
                                                 static_cast<std::uint32_t>(targets.size()), reason};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    // SPEC 8: SetPhase.
    void apply(const SetPhase& cmd, EventBuffer& out) {
        auto reject = [&](RejectReason reason) {
            // SPEC 12: echo requested instrument.
            emit_rejected(out, cmd.ts, cmd.instrument, CommandType::SetPhase, reason, /*session=*/0,
                          /*cl_ord_id=*/0, /*orig_cl_ord_id=*/0, /*order_id=*/0);
        };
        if (cmd.instrument != 0 && !instrument_known(cmd.instrument)) return reject(RejectReason::UnknownInstrument);
        if (!phase_in_range(cmd.phase)) return reject(RejectReason::InvalidField);

        if (cmd.instrument != 0) {
            apply_phase(cmd.instrument, cmd.phase, cmd.ts, out);
            return;
        }
        for (std::size_t index = 0; index < instruments_.size(); ++index) {
            apply_phase(static_cast<InstrumentId>(index + 1), cmd.phase, cmd.ts, out);
        }
    }

public:
    // ---- Queries ----------------------------------------------------------
    [[nodiscard]] std::size_t instrument_count() const noexcept { return instruments_.size(); }

    [[nodiscard]] const InstrumentConfig* instrument(InstrumentId id) const noexcept {
        return instrument_known(id) ? &instruments_[id - 1].config : nullptr;
    }

    [[nodiscard]] InstrumentId find_instrument(const Symbol& symbol) const noexcept {
        for (std::size_t index = 0; index < instruments_.size(); ++index) {
            if (instruments_[index].config.symbol == symbol) return static_cast<InstrumentId>(index + 1);
        }
        return 0;
    }

    // AMBIGUITY: phase() of an unknown instrument is unspecified; Closed is returned.
    [[nodiscard]] Phase phase(InstrumentId id) const noexcept {
        return instrument_known(id) ? instruments_[id - 1].phase : Phase::Closed;
    }

    [[nodiscard]] std::optional<Price> best_bid(InstrumentId id) const noexcept {
        return instrument_known(id) ? best_price(instruments_[id - 1], Side::BUY) : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> best_ask(InstrumentId id) const noexcept {
        return instrument_known(id) ? best_price(instruments_[id - 1], Side::SELL) : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> last_trade_price(InstrumentId id) const noexcept {
        return instrument_known(id) ? instruments_[id - 1].last_trade_price : std::nullopt;
    }

    std::size_t depth(InstrumentId id, Side side, LevelInfo* out, std::size_t max_levels) const noexcept {
        if (!instrument_known(id)) return 0;
        std::size_t written = 0;
        for (const Price price : prices_in_priority_order(instruments_[id - 1], side)) {
            if (written == max_levels) break;
            const Level& level = levels(instruments_[id - 1], side).at(price);
            out[written++] = LevelInfo{price, level_quantity(level), static_cast<std::uint32_t>(level.size())};
        }
        return written;
    }

    std::size_t level_orders(InstrumentId id, Side side, Price price, OrderId* out,
                             std::size_t max_orders) const noexcept {
        if (!instrument_known(id)) return 0;
        const auto& side_levels = levels(instruments_[id - 1], side);
        const auto it = side_levels.find(price);
        if (it == side_levels.end()) return 0;
        std::size_t written = 0;
        for (const OrderId order_id : it->second) {
            if (written == max_orders) break;
            out[written++] = order_id;
        }
        return written;
    }

    [[nodiscard]] std::optional<OrderInfo> find_order(SessionId session, ClOrdId cl_ord_id) const noexcept {
        const auto it = by_token_.find({session, cl_ord_id});
        if (it == by_token_.end()) return std::nullopt;
        return to_info(live_.at(it->second));
    }

    [[nodiscard]] std::optional<OrderInfo> find_order(OrderId order_id) const noexcept {
        const auto it = live_.find(order_id);
        if (it == live_.end()) return std::nullopt;
        return to_info(it->second);
    }

    [[nodiscard]] std::size_t live_order_count() const noexcept { return live_.size(); }

    // SPEC 9: the uncross computation without executing it.
    [[nodiscard]] AuctionIndicative auction_indicative(InstrumentId id) const noexcept {
        AuctionIndicative result;
        if (!instrument_known(id)) return result;
        const std::optional<AuctionPrice> auction = compute_auction_price(instruments_[id - 1]);
        if (!auction) {
            const auto& i = instruments_[id - 1];
            std::uint64_t buy = 0, sell = 0;
            for (const auto& [p, l] : i.bids) buy += level_quantity(l);
            for (const auto& [p, l] : i.asks) sell += level_quantity(l);
            result.imbalance_qty = buy > sell ? buy - sell : sell - buy;
            result.imbalance_side = buy > sell ? Aggressor::Buy : buy < sell ? Aggressor::Sell : Aggressor::None;
            return result;
        }
        result.crossed = true;
        result.price = auction->price;
        result.paired_qty = auction->paired_qty;
        result.imbalance_qty = auction->buy_qty > auction->sell_qty ? auction->buy_qty - auction->sell_qty
                                                                    : auction->sell_qty - auction->buy_qty;
        result.imbalance_side = auction->buy_qty > auction->sell_qty   ? Aggressor::Buy
                                : auction->buy_qty < auction->sell_qty ? Aggressor::Sell
                                                                       : Aggressor::None;
        return result;
    }

    // Every live order in ascending OrderId order.
    template <typename F>
    void for_each_order(F&& f) const {
        for (const auto& [order_id, order] : live_) f(to_info(order));
    }

private:
    struct Order {
        OrderId order_id = 0;
        ClOrdId cl_ord_id = 0;
        InstrumentId instrument = 0;
        SessionId session = 0;
        OwnerId owner = 0;
        Side side = Side::BUY;
        OrdType type = OrdType::Limit;
        TimeInForce tif = TimeInForce::Day;
        StpMode stp = StpMode::None;
        bool post_only = false;
        Price price = 0;
        Quantity qty = 0;
        Quantity leaves_qty = 0;
        Quantity cum_qty = 0;
    };

    using Level = std::list<OrderId>;  // FIFO: front = oldest = highest priority

    struct Instrument {
        InstrumentConfig config;
        Phase phase = Phase::Closed;
        std::optional<Price> last_trade_price;
        std::map<Price, Level> bids;  // best bid = highest key
        std::map<Price, Level> asks;  // best ask = lowest key
    };

    struct AuctionPrice {
        Price price = 0;
        std::uint64_t paired_qty = 0;  // V(p*)
        std::uint64_t buy_qty = 0;     // B(p*)
        std::uint64_t sell_qty = 0;    // S(p*)
    };

    // ---- Small spec predicates -------------------------------------------
    static bool is_call_phase(Phase phase) noexcept { return phase == Phase::PreOpen || phase == Phase::PreClose; }

    static bool phase_in_range(Phase phase) noexcept { return static_cast<std::uint8_t>(phase) <= 4; }

    // SPEC 8.2: the transitions that run the uncross on a crossed book.
    static bool transition_runs_uncross(Phase new_phase) noexcept {
        return new_phase == Phase::Continuous || new_phase == Phase::Closed;
    }

    static bool price_valid(const InstrumentConfig& config, Price price) noexcept {
        return config.min_price <= price && price <= config.max_price && price % config.tick_size == 0;
    }

    static bool quantity_valid(const InstrumentConfig& config, Quantity qty) noexcept {
        return qty > 0 && qty % config.lot_size == 0 && qty <= config.max_order_qty;
    }

    static bool enums_in_range(const NewOrder& cmd) noexcept {
        return static_cast<std::uint8_t>(cmd.side) <= 1 && static_cast<std::uint8_t>(cmd.type) <= 1 &&
               static_cast<std::uint8_t>(cmd.tif) <= 3 && static_cast<std::uint8_t>(cmd.stp) <= 3;
    }

    static bool is_ioc_or_fok(TimeInForce tif) noexcept { return tif == TimeInForce::IOC || tif == TimeInForce::FOK; }

    bool instrument_known(InstrumentId id) const noexcept { return id >= 1 && id <= instruments_.size(); }

    Instrument& instrument_state(InstrumentId id) { return instruments_[id - 1]; }

    static std::map<Price, Level>& levels(Instrument& instrument, Side side) {
        return side == Side::BUY ? instrument.bids : instrument.asks;
    }
    static const std::map<Price, Level>& levels(const Instrument& instrument, Side side) {
        return side == Side::BUY ? instrument.bids : instrument.asks;
    }

    static Side opposite(Side side) noexcept { return side == Side::BUY ? Side::SELL : Side::BUY; }

    // Prices of one side, best first.
    static std::vector<Price> prices_in_priority_order(const Instrument& instrument, Side side) {
        std::vector<Price> prices;
        for (const auto& [price, level] : levels(instrument, side)) prices.push_back(price);
        if (side == Side::BUY) std::reverse(prices.begin(), prices.end());
        return prices;
    }

    static std::optional<Price> best_price(const Instrument& instrument, Side side) {
        const auto& side_levels = levels(instrument, side);
        if (side_levels.empty()) return std::nullopt;
        return side == Side::BUY ? side_levels.rbegin()->first : side_levels.begin()->first;
    }

    // SPEC 4.1.8 / 6.6: buy crosses if price >= best ask; sell if price <= best bid.
    static bool would_cross_opposite_best(const Instrument& instrument, Side side, Price price) {
        const std::optional<Price> best = best_price(instrument, opposite(side));
        if (!best) return false;
        return side == Side::BUY ? price >= *best : price <= *best;
    }

    // SPEC 4.5: does the resting price cross the incoming order?
    static bool crosses(const Order& aggressor, Price resting_price) noexcept {
        if (aggressor.type == OrdType::Market) return true;
        return aggressor.side == Side::BUY ? aggressor.price >= resting_price : aggressor.price <= resting_price;
    }

    // SPEC 4.5: self-match is decided by the aggressor's STP mode and owner.
    bool is_self_match(const Order& aggressor, const Order& resting) const noexcept {
        const StpMode mode = mutations_.stp_uses_resting_mode ? resting.stp : aggressor.stp;
        return mode != StpMode::None && aggressor.owner != 0 && aggressor.owner == resting.owner;
    }

    StpMode effective_stp(const Order& aggressor, const Order& resting) const noexcept {
        return mutations_.stp_uses_resting_mode ? resting.stp : aggressor.stp;
    }

    Order* find_live(SessionId session, ClOrdId cl_ord_id) {
        const auto it = by_token_.find({session, cl_ord_id});
        return it == by_token_.end() ? nullptr : &live_.at(it->second);
    }

    // ---- SPEC 4.1 validation ----------------------------------------------
    RejectReason validate_new_order(const NewOrder& cmd) const {
        if (!instrument_known(cmd.instrument)) return RejectReason::UnknownInstrument;
        if (cmd.session == 0 || cmd.cl_ord_id == 0 || cmd.cl_ord_id > kMaxClOrdId) return RejectReason::InvalidClOrdId;
        if (by_token_.count({cmd.session, cmd.cl_ord_id}) != 0) return RejectReason::DuplicateClOrdId;
        if (!enums_in_range(cmd)) return RejectReason::InvalidField;
        if (cmd.post_only && (cmd.type == OrdType::Market || is_ioc_or_fok(cmd.tif))) return RejectReason::InvalidField;
        const Instrument& instrument = instruments_[cmd.instrument - 1];
        if (!quantity_valid(instrument.config, cmd.qty)) return RejectReason::InvalidQuantity;
        if (cmd.type == OrdType::Limit && !price_valid(instrument.config, cmd.price)) return RejectReason::InvalidPrice;
        if (instrument.phase == Phase::Closed) return RejectReason::MarketClosed;
        if (instrument.phase == Phase::Halted) return RejectReason::InstrumentHalted;
        if (is_call_phase(instrument.phase) && (cmd.type == OrdType::Market || is_ioc_or_fok(cmd.tif))) {
            return RejectReason::NotAllowedInPhase;
        }
        if (cmd.post_only && instrument.phase == Phase::Continuous &&
            would_cross_opposite_best(instrument, cmd.side, cmd.price)) {
            return RejectReason::PostOnlyWouldCross;
        }
        const std::size_t capacity =
            mutations_.capacity_off_by_one && config_.max_orders > 0 ? config_.max_orders - 1 : config_.max_orders;
        if (live_.size() >= capacity) return RejectReason::CapacityExceeded;
        return RejectReason::None;
    }

    // ---- SPEC 4.3 FOK pre-check -------------------------------------------
    bool fok_can_fill(const Instrument& instrument, const Order& aggressor) const {
        std::uint64_t available = 0;
        const Side resting_side = opposite(aggressor.side);
        for (const Price price : prices_in_priority_order(instrument, resting_side)) {
            if (!crosses(aggressor, price)) break;
            for (const OrderId order_id : levels(instrument, resting_side).at(price)) {
                const Order& resting = live_.at(order_id);
                if (!mutations_.fok_counts_self_matches && is_self_match(aggressor, resting)) {
                    if (effective_stp(aggressor, resting) == StpMode::CancelResting) continue;  // skip R
                    return available >= aggressor.qty;                                        // stop walking
                }
                available += resting.leaves_qty;
                if (available >= aggressor.qty) return true;
            }
        }
        return available >= aggressor.qty;
    }

    // ---- SPEC 4.5 continuous matching -------------------------------------
    // Returns true if the aggressor was cancelled by self-trade prevention.
    bool match(Instrument& instrument, Order& aggressor, Timestamp ts, EventBuffer& out) {
        const Side resting_side = opposite(aggressor.side);
        auto& opposite_levels = levels(instrument, resting_side);
        while (aggressor.leaves_qty > 0 && !opposite_levels.empty()) {
            const Price best = *best_price(instrument, resting_side);
            if (!crosses(aggressor, best)) break;
            const Level& level = opposite_levels.at(best);
            const OrderId resting_id = mutations_.lifo_within_level ? level.back() : level.front();
            Order& resting = live_.at(resting_id);

            if (is_self_match(aggressor, resting)) {
                const Order resting_copy = resting;
                switch (effective_stp(aggressor, resting)) {
                    case StpMode::CancelResting:
                        emit_canceled(out, ts, resting_copy, CancelReason::SelfTradePrevention, resting_copy.leaves_qty,
                                      0, /*was_resting=*/true);
                        remove_from_book(resting_id);
                        continue;
                    case StpMode::CancelAggressor:
                        emit_canceled(out, ts, aggressor, CancelReason::SelfTradePrevention, aggressor.leaves_qty, 0,
                                      /*was_resting=*/false);
                        return true;
                    case StpMode::CancelBoth:
                        emit_canceled(out, ts, resting_copy, CancelReason::SelfTradePrevention, resting_copy.leaves_qty,
                                      0, /*was_resting=*/true);
                        remove_from_book(resting_id);
                        emit_canceled(out, ts, aggressor, CancelReason::SelfTradePrevention, aggressor.leaves_qty, 0,
                                      /*was_resting=*/false);
                        return true;
                    case StpMode::None: break;  // unreachable: self-match requires a mode
                }
            }

            const Quantity qty = std::min(aggressor.leaves_qty, resting.leaves_qty);
            aggressor.leaves_qty -= qty;
            aggressor.cum_qty += qty;
            resting.leaves_qty -= qty;
            resting.cum_qty += qty;
            const Aggressor aggressor_side = aggressor.side == Side::BUY ? Aggressor::Buy : Aggressor::Sell;
            if (aggressor.side == Side::BUY) {
                emit_fill(out, ts, aggressor.instrument, resting.price, qty, aggressor_side, aggressor, resting);
            } else {
                emit_fill(out, ts, aggressor.instrument, resting.price, qty, aggressor_side, resting, aggressor);
            }
            instrument.last_trade_price = resting.price;
            if (resting.leaves_qty == 0) remove_from_book(resting_id);
        }
        return false;
    }

    // ---- SPEC 4.6 disposition ---------------------------------------------
    void dispose_after_matching(const Order& aggressor, bool canceled_by_stp, Timestamp ts, EventBuffer& out) {
        if (canceled_by_stp || aggressor.leaves_qty == 0) return;
        if (aggressor.type == OrdType::Market || is_ioc_or_fok(aggressor.tif)) {
            emit_canceled(out, ts, aggressor, CancelReason::ImmediateOrCancel, aggressor.leaves_qty, 0,
                          /*was_resting=*/false);
            return;
        }
        rest(aggressor, ts, out);
    }

    // ---- SPEC 4.4 resting ---------------------------------------------------
    void rest(const Order& order, Timestamp ts, EventBuffer& out) {
        Instrument& instrument = instrument_state(order.instrument);
        levels(instrument, order.side)[order.price].push_back(order.order_id);
        live_[order.order_id] = order;
        by_token_[{order.session, order.cl_ord_id}] = order.order_id;
        Event e = make_event(EventType::Rested, order.instrument, ts);
        e.rested = RestedEvent{order.order_id, order.cl_ord_id, order.session, order.side, order.price, order.leaves_qty};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    void remove_from_book(OrderId order_id) {
        const Order order = live_.at(order_id);
        auto& side_levels = levels(instrument_state(order.instrument), order.side);
        Level& level = side_levels.at(order.price);
        level.erase(std::find(level.begin(), level.end(), order_id));
        if (level.empty()) side_levels.erase(order.price);
        by_token_.erase({order.session, order.cl_ord_id});
        live_.erase(order_id);
    }

    void reindex_token(Order& order, ClOrdId new_token) {
        by_token_.erase({order.session, order.cl_ord_id});
        order.cl_ord_id = new_token;
        by_token_[{order.session, order.cl_ord_id}] = order.order_id;
    }

    // ---- SPEC 8 phase transition for one instrument ------------------------
    void apply_phase(InstrumentId id, Phase new_phase, Timestamp ts, EventBuffer& out) {
        Instrument& instrument = instrument_state(id);
        const Phase old_phase = instrument.phase;
        if (new_phase == old_phase) return;  // 8.1
        if (transition_runs_uncross(new_phase) && book_crossed(instrument)) uncross(id, ts, out);  // 8.2
        if (new_phase == Phase::Closed) {  // 8.3 end-of-day expiry
            std::vector<OrderId> expiring;
            for (const auto& [order_id, order] : live_) {
                const bool expires = order.tif == TimeInForce::Day ||
                                     (mutations_.end_of_day_expires_gtc && order.tif == TimeInForce::GTC);
                if (order.instrument == id && expires) expiring.push_back(order_id);
            }
            for (const OrderId order_id : expiring) {
                const Order order = live_.at(order_id);
                emit_canceled(out, ts, order, CancelReason::EndOfDay, order.leaves_qty, 0, /*was_resting=*/true);
                remove_from_book(order_id);
            }
        }
        instrument.phase = new_phase;  // 8.4
        Event e = make_event(EventType::PhaseChanged, id, ts);
        e.phase_changed = PhaseChangedEvent{old_phase, new_phase};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    static bool book_crossed(const Instrument& instrument) {
        const std::optional<Price> bid = best_price(instrument, Side::BUY);
        const std::optional<Price> ask = best_price(instrument, Side::SELL);
        return bid && ask && *bid >= *ask;
    }

    // ---- SPEC 9 auction uncross ---------------------------------------------
    std::optional<AuctionPrice> compute_auction_price(const Instrument& instrument) const {
        if (!book_crossed(instrument)) return std::nullopt;
        const Price best_bid_price = *best_price(instrument, Side::BUY);
        const Price best_ask_price = *best_price(instrument, Side::SELL);
        const Price reference = instrument.last_trade_price ? *instrument.last_trade_price
                                                            : instrument.config.reference_price;

        std::set<Price> candidates;  // ascending
        for (const auto& [price, level] : instrument.bids) {
            if (price >= best_ask_price && price <= best_bid_price) candidates.insert(price);
        }
        for (const auto& [price, level] : instrument.asks) {
            if (price >= best_ask_price && price <= best_bid_price) candidates.insert(price);
        }

        std::optional<AuctionPrice> best;
        std::uint64_t best_abs_imbalance = 0;
        std::uint64_t best_distance = 0;
        for (const Price p : candidates) {
            std::uint64_t buy_qty = 0;   // B(p): buys priced >= p
            std::uint64_t sell_qty = 0;  // S(p): sells priced <= p
            for (const auto& [price, level] : instrument.bids) {
                if (price >= p) buy_qty += level_quantity(level);
            }
            for (const auto& [price, level] : instrument.asks) {
                if (price <= p) sell_qty += level_quantity(level);
            }
            const std::uint64_t paired = std::min(buy_qty, sell_qty);
            const std::uint64_t abs_imbalance = buy_qty > sell_qty ? buy_qty - sell_qty : sell_qty - buy_qty;
            const std::uint64_t distance = p >= reference ? static_cast<std::uint64_t>(p) - static_cast<std::uint64_t>(reference)
                                                          : static_cast<std::uint64_t>(reference) - static_cast<std::uint64_t>(p);
            bool better = false;
            if (!best) {
                better = true;
            } else if (paired != best->paired_qty) {
                better = paired > best->paired_qty;            // maximise V
            } else if (abs_imbalance != best_abs_imbalance) {
                better = abs_imbalance < best_abs_imbalance;   // then minimise |I|
            } else if (distance != best_distance) {
                better = distance < best_distance;             // then closest to reference
            } else {
                // Full tie: candidates are visited in ascending order, so keeping the
                // first one picks the lower price.
                better = mutations_.uncross_tie_prefers_higher;
            }
            if (better) {
                best = AuctionPrice{p, paired, buy_qty, sell_qty};
                best_abs_imbalance = abs_imbalance;
                best_distance = distance;
            }
        }
        return best;
    }

    std::uint64_t level_quantity(const Level& level) const {
        std::uint64_t qty = 0;
        for (const OrderId order_id : level) qty += live_.at(order_id).leaves_qty;
        return qty;
    }

    void uncross(InstrumentId id, Timestamp ts, EventBuffer& out) {
        Instrument& instrument = instrument_state(id);
        const AuctionPrice auction = *compute_auction_price(instrument);
        const Price p = auction.price;
        while (!instrument.bids.empty() && !instrument.asks.empty()) {
            const Price bid_price = *best_price(instrument, Side::BUY);
            const Price ask_price = *best_price(instrument, Side::SELL);
            if (bid_price < p || ask_price > p) break;
            const OrderId buy_id = instrument.bids.at(bid_price).front();
            const OrderId sell_id = instrument.asks.at(ask_price).front();
            Order& buy = live_.at(buy_id);
            Order& sell = live_.at(sell_id);
            const Quantity qty = std::min(buy.leaves_qty, sell.leaves_qty);
            buy.leaves_qty -= qty;
            buy.cum_qty += qty;
            sell.leaves_qty -= qty;
            sell.cum_qty += qty;
            emit_fill(out, ts, id, p, qty, Aggressor::None, buy, sell);  // STP not applied
            if (buy.leaves_qty == 0) remove_from_book(buy_id);
            if (sell.leaves_qty == 0) remove_from_book(sell_id);
        }
        Event e = make_event(EventType::AuctionSummary, id, ts);
        e.auction_summary = AuctionSummaryEvent{p, auction.paired_qty, next_match_id_++};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
        instrument.last_trade_price = p;
    }

    // ---- Event construction -------------------------------------------------
    static Event make_event(EventType type, InstrumentId instrument, Timestamp ts) {
        Event e{};
        e.type = type;
        e.instrument = instrument;
        e.ts = ts;
        return e;
    }

    static void emit_rejected(EventBuffer& out, Timestamp ts, InstrumentId instrument, CommandType cmd,
                              RejectReason reason, SessionId session, ClOrdId cl_ord_id, ClOrdId orig_cl_ord_id,
                              OrderId order_id) {
        Event e = make_event(EventType::Rejected, instrument, ts);
        e.rejected = RejectedEvent{cmd, reason, session, cl_ord_id, orig_cl_ord_id, order_id};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    static void emit_accepted(EventBuffer& out, Timestamp ts, const Order& o) {
        Event e = make_event(EventType::Accepted, o.instrument, ts);
        e.accepted = AcceptedEvent{o.order_id, o.cl_ord_id, o.session, o.owner, o.side, o.type,
                                   o.tif,      o.stp,       o.post_only, o.price, o.qty};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    // Canceled{order fields, canceled_qty, cum_qty = order.cum_qty unless overridden}.
    static void emit_canceled(EventBuffer& out, Timestamp ts, const Order& o, CancelReason reason,
                              Quantity canceled_qty, ClOrdId request_cl_ord_id, bool was_resting,
                              std::optional<Quantity> cum_qty_override = std::nullopt, Quantity leaves = 0) {
        Event e = make_event(EventType::Canceled, o.instrument, ts);
        e.canceled = CanceledEvent{o.order_id, o.cl_ord_id, request_cl_ord_id, o.session, o.side, o.price,
                                   canceled_qty, cum_qty_override ? *cum_qty_override : o.cum_qty, reason, was_resting, leaves};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    static void emit_replaced(EventBuffer& out, Timestamp ts, const Order& before, const Order& after,
                              bool priority_kept) {
        Event e = make_event(EventType::Replaced, before.instrument, ts);
        e.replaced = ReplacedEvent{before.order_id,  after.order_id, before.cl_ord_id, after.cl_ord_id,
                                   before.session,   before.side,    before.price,     after.price,
                                   before.leaves_qty, after.qty,     after.leaves_qty, after.cum_qty,
                                   priority_kept};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    void emit_fill(EventBuffer& out, Timestamp ts, InstrumentId instrument, Price price, Quantity qty,
                   Aggressor aggressor, const Order& buy, const Order& sell) {
        Event e = make_event(EventType::Fill, instrument, ts);
        e.fill = FillEvent{next_match_id_++, price, qty, aggressor,
                           FillLeg{buy.order_id, buy.cl_ord_id, buy.session, buy.owner, buy.leaves_qty, buy.cum_qty},
                           FillLeg{sell.order_id, sell.cl_ord_id, sell.session, sell.owner, sell.leaves_qty,
                                   sell.cum_qty}};
        if (!out.push(e)) throw std::logic_error("reference preflight bound");
    }

    static OrderInfo to_info(const Order& o) {
        return OrderInfo{o.order_id, o.cl_ord_id, o.instrument, o.session, o.owner,  o.side,      o.tif,
                         o.stp,      o.post_only, o.price,      o.qty,     o.leaves_qty, o.cum_qty};
    }

    EngineConfig config_;
    Mutations mutations_;
    std::vector<Instrument> instruments_;                        // index = InstrumentId - 1
    std::map<OrderId, Order> live_;                              // resting orders, ascending OrderId
    std::map<std::pair<SessionId, ClOrdId>, OrderId> by_token_;  // (session, token) -> live order
    OrderId next_order_id_ = 1;
    MatchId next_match_id_ = 1;
};

}  // namespace exsim::ref

#endif
