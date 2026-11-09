// Randomized invariant test (SPEC 11): random commands over 2 instruments,
// 3 sessions and every phase, with the book checked after every step.
//
// Checked after every command, through the public API only:
//   * book invariants (never crossed in Continuous/Closed, level sums, FIFO
//     and lookup consistency, leaves + cum == qty),
//   * an event-derived shadow of live leaves equals the engine's live orders,
//   * global quantity conservation:
//       sum(Accepted.qty) + sum(Replaced.leaves_qty - Replaced.old_leaves_qty)
//         == 2 * sum(Fill.qty) + sum(Canceled.canceled_qty) + sum(live leaves),
//   * order ids and match ids strictly increase, every event carries the
//     command timestamp,
//   * a second engine fed the same stream produces the same events and state,
//   * a tight output buffer either applies the command or returns
//     InsufficientOutput with output and engine untouched; the retry with a
//     roomy buffer must then produce exactly the events of the twin engine.

#include <test/exsim/engine/arena_test_support.hpp>

#include <algorithm>
#include <map>
#include <random>

using namespace arena_test;

namespace {

constexpr std::size_t kInstrumentCount = 2;
constexpr std::size_t kMaxLiveOrders = 24;  // small so the arena fills and reuses slots
const SessionId kRandomSessions[] = {kAliceSession, kBobSession, kCarolSession};
const OwnerId kRandomOwners[] = {0, kAliceFirm, kBobFirm};

struct RandomCommandSource {
    std::mt19937_64 random;
    Timestamp next_ts = 1;

    explicit RandomCommandSource(std::uint64_t seed) : random(seed) {}

    std::uint64_t below(std::uint64_t bound) { return random() % bound; }

    // Mostly valid values with a sprinkle of invalid ones, so rejections and
    // edge cases are exercised alongside matching.
    Price random_price() {
        const std::uint64_t roll = below(40);
        if (roll == 0) return 5;      // off grid
        if (roll == 1) return 210;    // above the band
        if (roll == 2) return 0;
        return static_cast<Price>(10 * (6 + below(9)));  // 60..140 around the reference
    }

    Quantity random_qty() {
        const std::uint64_t roll = below(40);
        if (roll == 0) return 0;
        if (roll == 1) return 1001;  // above max_order_qty
        return 1 + below(12);
    }

    ClOrdId random_token() { return 1 + below(24); }  // small space: duplicates and hits on live orders

    Command next(const MatchingEngine& engine) {
        const Timestamp ts = next_ts++;
        const SessionId session = kRandomSessions[below(3)];
        const InstrumentId instrument = static_cast<InstrumentId>(1 + below(kInstrumentCount + (below(30) == 0)));
        const std::uint64_t kind = below(100);
        if (kind < 62) {
            NewOrder order = limit_order(session, random_token(), below(2) == 0 ? Side::BUY : Side::SELL,
                                         random_price(), random_qty(), instrument);
            order.ts = ts;
            order.owner = kRandomOwners[below(3)];
            order.stp = static_cast<StpMode>(below(4));
            // Mostly resting orders so the book gets deep: Day 45%, GTC 35%,
            // IOC 12%, FOK 8%.
            const std::uint64_t tif_roll = below(100);
            order.tif = tif_roll < 45 ? TimeInForce::Day
                        : tif_roll < 80 ? TimeInForce::GTC
                        : tif_roll < 92 ? TimeInForce::IOC
                                        : TimeInForce::FOK;
            if (below(12) == 0) order.type = OrdType::Market;
            order.post_only = below(10) == 0;
            return order;
        }
        // Cancel and replace usually target a live order so they do real work.
        ClOrdId target_token = random_token();
        SessionId target_session = session;
        const std::vector<OrderInfo> live = all_live_orders(engine);
        if (!live.empty() && below(5) != 0) {
            const OrderInfo& target = live[below(live.size())];
            target_token = target.cl_ord_id;
            target_session = target.session;
        }
        if (kind < 76) {
            const Quantity reduce_to = below(3) == 0 ? random_qty() : 0;
            return CancelOrder{ts, target_session, target_token, below(4) == 0 ? 0 : random_token(), reduce_to};
        }
        if (kind < 94) {
            const auto target = engine.find_order(target_session, target_token);
            Price price = random_price();
            if (target && below(2) == 0) price = target->price;  // exercise priority kept
            Quantity qty = random_qty();
            if (target && below(3) == 0) qty = target->cum_qty + (target->leaves_qty > 1 ? below(target->leaves_qty) : 0);
            const ClOrdId new_token = below(3) == 0 ? 0 : random_token();
            return ReplaceOrder{ts, target_session, target_token, new_token, price, qty, below(6) == 0};
        }
        if (kind < 96) {
            const InstrumentId filter = below(3) == 0 ? 0 : instrument;
            return MassCancel{ts, session, filter, random_token(), below(2) == 0};
        }
        // Phase changes: Continuous most often so matching dominates.
        static const Phase kPhases[] = {Phase::Continuous, Phase::Continuous, Phase::PreOpen, Phase::PreClose,
                                        Phase::Halted, Phase::Closed};
        const InstrumentId phase_target = below(4) == 0 ? 0 : instrument;
        return SetPhase{ts, phase_target, kPhases[below(6)]};
    }
};

struct ConservationLedger {
    std::int64_t accepted_qty = 0;
    std::int64_t replacement_delta = 0;
    std::int64_t filled_qty = 0;
    std::int64_t canceled_qty = 0;
    std::size_t replaced_count = 0;
    std::size_t auction_count = 0;
    std::size_t capacity_rejection_count = 0;
    OrderId highest_order_id = 0;
    MatchId highest_match_id = 0;
    std::map<OrderId, Quantity> shadow_leaves;  // event-derived live leaves

    void set_shadow(OrderId order_id, Quantity leaves) {
        if (leaves == 0) shadow_leaves.erase(order_id);
        else shadow_leaves[order_id] = leaves;
    }

    void observe_new_order_id(OrderId order_id) {
        CHECK_GT(order_id, highest_order_id);
        highest_order_id = order_id;
    }

    void observe_match_id(MatchId match_id) {
        CHECK_GT(match_id, highest_match_id);
        highest_match_id = match_id;
    }

    void record(const EventBuffer& events, std::size_t first, Timestamp command_ts) {
        for (std::size_t i = first; i < events.size(); ++i) {
            const Event& event = events[i];
            CHECK_EQ(event.ts, command_ts);
            switch (event.type) {
                case EventType::Accepted:
                    accepted_qty += static_cast<std::int64_t>(event.accepted.qty);
                    observe_new_order_id(event.accepted.order_id);
                    set_shadow(event.accepted.order_id, event.accepted.qty);
                    break;
                case EventType::Rested:
                    CHECK_EQ(shadow_leaves[event.rested.order_id], event.rested.leaves_qty);
                    break;
                case EventType::Fill:
                    filled_qty += static_cast<std::int64_t>(event.fill.qty);
                    observe_match_id(event.fill.match_id);
                    set_shadow(event.fill.buy.order_id, event.fill.buy.leaves_qty);
                    set_shadow(event.fill.sell.order_id, event.fill.sell.leaves_qty);
                    break;
                case EventType::Canceled:
                    canceled_qty += static_cast<std::int64_t>(event.canceled.canceled_qty);
                    set_shadow(event.canceled.order_id, event.canceled.leaves_qty);
                    break;
                case EventType::Replaced: {
                    const ReplacedEvent& replaced = event.replaced;
                    ++replaced_count;
                    replacement_delta += static_cast<std::int64_t>(replaced.leaves_qty) -
                                         static_cast<std::int64_t>(replaced.old_leaves_qty);
                    CHECK_EQ(replaced.leaves_qty, replaced.qty - replaced.cum_qty);
                    if (replaced.priority_kept) {
                        CHECK_EQ(replaced.new_order_id, replaced.old_order_id);
                    } else {
                        observe_new_order_id(replaced.new_order_id);
                    }
                    shadow_leaves.erase(replaced.old_order_id);
                    set_shadow(replaced.new_order_id, replaced.leaves_qty);
                    break;
                }
                case EventType::AuctionSummary:
                    ++auction_count;
                    observe_match_id(event.auction_summary.match_id);
                    break;
                case EventType::Rejected:
                    if (event.rejected.reason == RejectReason::CapacityExceeded) ++capacity_rejection_count;
                    break;
                case EventType::MassCancelDone:
                case EventType::PhaseChanged:
                    break;
            }
        }
    }

    void check_against(const MatchingEngine& engine) const {
        std::int64_t live_leaves = 0;
        std::map<OrderId, Quantity> engine_leaves;
        for (const OrderInfo& order : all_live_orders(engine)) {
            live_leaves += static_cast<std::int64_t>(order.leaves_qty);
            engine_leaves[order.order_id] = order.leaves_qty;
        }
        CHECK_EQ(accepted_qty + replacement_delta, 2 * filled_qty + canceled_qty + live_leaves);
        CHECK(engine_leaves == shadow_leaves);
    }
};

void run_randomized_invariant_sweep(std::uint64_t seed, int step_count) {
    const EngineConfig config{4, kMaxLiveOrders};
    ArenaEngineFixture fixture(Phase::Continuous, kInstrumentCount, config);
    ArenaEngineFixture twin(Phase::Continuous, kInstrumentCount, config);
    RandomCommandSource source(seed);
    ConservationLedger ledger;
    const Event marker{EventType::PhaseChanged, 0, 777, {.phase_changed = {Phase::Closed, Phase::Closed}}};
    std::size_t refused_attempts = 0;
    std::size_t deepest_live_order_count = 0;

    for (int step = 0; step < step_count; ++step) {
        const Command command = source.next(fixture.engine);

        // A tight buffer with one marker already inside: the engine either
        // fits the whole command or refuses it without touching anything.
        const std::size_t tight_capacity = 1 + source.below(5);
        EventBuffer tight_output(tight_capacity);
        REQUIRE(tight_output.push(marker));
        const std::uint64_t state_before = book_state_digest(fixture.engine);
        const HandleResult tight_result = fixture.engine.handle(command, tight_output);

        EventBuffer twin_events(required_event_capacity(config));
        REQUIRE_EQ(twin.engine.handle(command, twin_events), HandleResult::Applied);

        REQUIRE(tight_output.size() >= 1);
        CHECK_EQ(tight_output[0].ts, Timestamp{777});
        if (tight_result == HandleResult::InsufficientOutput) {
            ++refused_attempts;
            CHECK_EQ(tight_output.size(), std::size_t{1});
            CHECK_EQ(book_state_digest(fixture.engine), state_before);
            fixture.events.clear();
            REQUIRE_EQ(fixture.engine.handle(command, fixture.events), HandleResult::Applied);
            CHECK_EQ(event_stream_digest(fixture.events), event_stream_digest(twin_events));
            ledger.record(fixture.events, 0, command.ts());
        } else {
            REQUIRE_EQ(tight_result, HandleResult::Applied);
            // Compare the appended events (after the marker) with the twin.
            EventBuffer appended(tight_output.size());
            for (std::size_t i = 1; i < tight_output.size(); ++i) REQUIRE(appended.push(tight_output[i]));
            CHECK_EQ(event_stream_digest(appended), event_stream_digest(twin_events));
            ledger.record(tight_output, 1, command.ts());
        }

        CHECK_LE(fixture.engine.live_order_count(), kMaxLiveOrders);
        deepest_live_order_count = std::max(deepest_live_order_count, fixture.engine.live_order_count());
        check_book_invariants(fixture.engine);
        ledger.check_against(fixture.engine);
        CHECK_EQ(book_state_digest(fixture.engine), book_state_digest(twin.engine));
    }
    // The sweep must actually exercise both outcomes of the preflight and
    // produce real trading, otherwise it proves nothing.
    CHECK_GT(refused_attempts, std::size_t{0});
    CHECK_GT(ledger.filled_qty, 0);
    CHECK_GT(ledger.canceled_qty, 0);
    CHECK_GT(ledger.replaced_count, std::size_t{0});
    CHECK_GT(ledger.auction_count, std::size_t{0});
    CHECK_EQ(deepest_live_order_count, kMaxLiveOrders);
    CHECK_GT(ledger.capacity_rejection_count, std::size_t{0});
}

}  // namespace

TEST_CASE(arena_randomized, invariants_hold_after_every_command_seed_1) {
    run_randomized_invariant_sweep(1, 6000);
}

TEST_CASE(arena_randomized, invariants_hold_after_every_command_seed_20261103) {
    run_randomized_invariant_sweep(20261103, 6000);
}

TEST_CASE(arena_randomized, invariants_hold_after_every_command_seed_deadbeef) {
    run_randomized_invariant_sweep(0xdeadbeefULL, 6000);
}
