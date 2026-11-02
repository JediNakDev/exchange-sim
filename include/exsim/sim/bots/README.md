# Background liquidity agents

These are synthetic background market participants, separate from the research
strategy, accounting ledger, runner, latency models, and event scheduler.

## Public API

- `BotConfig` holds inline name, session, owner, instrument, STP, seed, and enabled state.
- Each bot exclusively owns its session's engine ClOrdId namespace.
- `BotName` stores up to 32 bytes without dynamic memory.
- `BotContext` checks `submit(Command)`, `now()`, and `instrument(id)` at compile time.
- `MarketMakerConfig` and `NoiseTakerConfig` extend the shared identity.
- `MarketMaker(config)` allocates its order table during construction.
- `NoiseTaker(config)` allocates its order table during construction.
- `on_timer(timestamp, context)` drives work with caller-owned nanoseconds.
- `on_events(span<const Event>)` consumes every instrument/session batch.
- `set_enabled(bool)` marks a start/stop request; the next timer applies it.
- Disabled agents cancel their orders, including while halted or closed.
- `enabled()`, `config()`, `phase()`, and `inventory()` expose state.
- `orders()` exposes a const `OwnOrderTable` view, including pending requests.
- `stats()` exposes submission, rejection, fill, cancellation, and capacity counters.
- `MarketMaker::fair_value_ticks()` exposes the current random-walk value.
- `NoiseTaker::next_arrival()` exposes the next sampled deadline after scheduling.
- `OwnOrderTable` offers bounded allocate/find/erase/slot iteration.
- Table capacity rounds up to a power of two within [8, 65536].
- Exhaustion skips a submission and increments `capacity_skips`; IDs never wrap.
- `BotSet<N>` owns inline optional storage and at most N total agents.
- Setup-only `add(config)` returns a pointer or nullptr for capacity/duplicate sessions.
- Registry `on_timer`, `on_events`, and `set_enabled` fan out without virtual calls.
- `apply_bot_option(config, key, value)` parses one startup option transactionally.
- Unknown/invalid options return false and preserve the config.
- `deadline` saturates timestamp addition, using `kNever` as a sentinel.

## Lifecycle and model

Construct and register agents before entering the simulation loop. Timer calls and
all event batches must be ordered on a single caller thread with nondecreasing
timestamps and a consistent epoch. The agents read no clock, sleep, or create
threads. Instrument configuration is immutable after setup. Callbacks may arrive
inside `submit`, after the timer, or before the next timer. Deliver every command's
complete batch, once, in command order. `on_events` never submits, avoiding recursive
order generation. Do not re-enter `on_timer` while it is already executing.

Pending orders reserve capacity before submission. Rejected new requests release
it; rejected cancel/replace requests preserve the confirmed state unless the order
is unknown. Replaces retain the token (`cl_ord_id = 0`). Fill legs update cumulative
quantity and signed inventory, including partial fills. Cancellation events
release orders only when post-cancel leaves is zero; reductions retain order
identity and update confirmed leaves/cumulative quantity. Both old and requested prices constrain quoting until a replace
is acknowledged, because a processed replace may have been rejected.

The maker retains desired prices, replenishes partial leaves via total-quantity
replaces, and moves the unblocked side before the other side. It cancels excess
orders or orders whose cumulative quantity would exceed max_order_qty on refill.
Normal movement uses one replace per changed level. Pending acknowledgments or
capacity limits may require subsequent timer calls to converge. A fill marks the
ladder dirty; replenishment happens on the next timer, never in the callback.
Quotes use unique ticks, omit levels outside the band, and remain uncrossed even
in call phases. At a band edge a full two-sided ladder may be impossible.
`spread_ticks` is the half spread. The fair value starts at `fair_value_ticks`
or the instrument reference if zero. A zero fair-value step interval freezes it.
Skipped timer periods consume every due random-walk step deterministically.
Inventory skew is integer ticks per size unit; fractional ticks truncate toward
zero, and checked arithmetic clamps the center to the band.

The taker deliberately does not reconstruct BBO from potentially incomplete events.
IOC prices are the buy-side upper band or sell-side lower band, crossing all
available opposite liquidity. `cross_ticks` sets the reference-relative distance
for occasional DAY limits. These are post-only, so a reference-relative price
that would cross another session is rejected. Their lifetime starts at submission;
expired orders are canceled on the next timer even outside Continuous. Arrivals
are sampled from a discretized exponential distribution with an integer Q32 inverse
CDF and a one-nanosecond minimum. No floating-point/libm behavior affects the
command stream. Sparse timers consume all due arrivals but stamp commands with
the supplied current time. The runner should call at event deadlines when exact
arrival timestamps matter. Non-Continuous phases and stop/start reset the taker's
arrival schedule. Disabled/halted makers freeze quoting and catch up fair-value
steps when active again. Large timestamp jumps can therefore require substantial
bounded-state catch-up work; these objects do not implement a scheduler.

## Startup options

Numbers are decimal, must consume the complete value, and do not accept signs for
unsigned fields. Booleans accept `true`, `false`, `1`, `0`. Names truncate to 32 bytes.

| Shared key | Default / meaning |
| --- | --- |
| name | empty, inline display name |
| session_id | 0, must be assigned a unique nonzero session |
| owner | 0, STP owner group |
| instrument | 0, must be assigned a nonzero instrument |
| seed | 1 |
| enabled | true |
| stp | cancel_resting; also none, cancel_aggressor, cancel_both |
| order_capacity | 128; accepted parser range 1..65536 |
| fair_value_ticks | 0, instrument reference fallback |

| Maker key | Default |
| --- | --- |
| levels | 3; parser range 1..64, direct config clamps at 64 |
| size | 100; rounded down to lots and capped at instrument maximum |
| spread_ticks | 1; direct zero becomes one tick |
| fair_value_step_ms | 1000; zero disables the walk |
| fair_value_step_ticks | 1 |
| requote_ms | 100; positive through parser |
| skew_ticks_per_unit | 0; nonnegative integer |
| cancel_on_halt | true; Closed always cancels |

| Taker key | Default |
| --- | --- |
| mean_interval_ms | 100; positive through parser |
| min_size | 1; rounded up to lots |
| max_size | 100; rounded down to lots and capped |
| cross_ticks | 2; passive distance, zero becomes one tick |
| limit_ppm | 10000; parser range 0..1000000 |
| limit_lifetime_ms | 1000; zero cancels on next timer |

Cross-field configuration validation remains with the startup integrator: resolve
user/symbol to IDs, reject shared sessions, check size ranges against lots, and
size capacity for desired ladders/lifetimes. An impossible size range submits
nothing; it does not allocate or emit invalid quantities. Context buffers must
also be bounded and allocated before the hot path.

## Integration boundaries

No MatchingEngine implementation is linked by these agents or their tests. The
fake exchange covers the consumed lifecycle contracts but is not an independent
engine oracle and does not implement auctions. Validate actual engine integration
after the separately owned canonical contract repairs land. In particular retain
fill-leg post-fill leaves/cumulative fields, replace TOTAL quantity semantics,
zero-token replace semantics, cancel/replace UnknownOrder rejection correlation,
instrument-level PhaseChanged delivery, and EOD cancellation before Closed.

The original brief says spread_ticks is a half spread; SERVER.md says only
"spread_ticks wide". This implementation follows the explicit half-spread brief.
Every batch must reach every agent, including events for other sessions and
instrument phase changes. The future server must wire these calls and parser
hooks; this branch does not modify server configuration or claim integrated
research-runner, latency, or accounting behavior.

The integrated canonical contract supports partial cancellation reductions.
`sim_bots_allocations.active_hot_paths_allocate_zero` in
`tests/exsim/sim/bots/bots_allocation_tests.cpp` runs inside the shared test
binary and counts allocations with `testfw::AllocationScope`; it is skipped
under ASan/TSan, where the counter is unavailable.
