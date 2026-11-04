# Deterministic research runner

The CLI executes real synthetic-market runs against the production MatchingEngine.
These artifacts are distinct from `web/mock-server` fixtures. The browser and HTTP
service do not yet read these files. This document defines the disk contract v1.

## Reproduce

```sh
make -j4 test tools EXTRA_CXXFLAGS=-Werror
make -j4 test MODE=debug EXTRA_CXXFLAGS=-Werror
build/release/exsim_research validate config/research/fixed_spread_narrow.conf
build/release/exsim_research run config/research/fixed_spread_narrow.conf --out /tmp/narrow --seeds 1..5
build/release/exsim_research run config/research/fixed_spread_wide.conf --out /tmp/wide --seeds 1..5
build/release/exsim_research compare /tmp/narrow /tmp/wide
```

`run` accepts `--seed N`, `--seeds A..B` (inclusive, at most 100000 seeds),
`--parent-run ID`, `--quiet`, and `--overwrite`. Without a seed option it uses
`experiment.seed`. A sweep writes `sweep.json` and `seed-N/` directories. A single
run writes directly into the output directory. Existing output is refused unless
explicitly overwritten; unknown files in a run directory prevent overwrite.
SIGINT/SIGTERM requests cancellation between chunks. Outputs retain positions and
failure detail; completion does not liquidate or cancel remaining orders.

Exit codes: 0 all runs completed and independent replay matched; 1 failed or
cancelled run; 2 usage; 3 invalid config; 4 artifact/I/O failure; 5 replay mismatch;
6 no completed seed pairs. `compare --json` emits the comparison object instead
of its table. Failed/cancelled runs are excluded, missing metrics are unavailable,
and sample SD uses n-1 (unavailable for fewer than two pairs). Differences are
candidate B minus baseline A. Configuration differences are printed. Different
worlds must not be interpreted as comparable, even if numerical differences exist.

## Simulation and ownership

`Simulation<Strategy>` owns a real engine, bounded scheduler queues, independent
RNGs, ledger, risk host and preallocated record buffers. `run`, `step`, and
`run_until` resume at event boundaries. No simulated loop code writes files,
reads a clock, uses virtual dispatch, or allocates after construction. The driver
writes while yielded and resumes after clearing the recorder. Optional UI work
cannot change event ordering. Queue exhaustion explicitly fails the run; records
are never silently dropped. One event can emit many engine events, so recorder
capacity must accommodate its complete batch, not just the flush margin.

Simulation events sort by (nanosecond time, enqueue sequence). Observations
sort the same way but follow all simulation events at that time. Initial order:
background timers in config order, strategy start, series observation. For an
exchange command, reports enter the queue in engine event order, then market
data. Observations never schedule simulation events. Background agents have
zero latency and observe each engine batch synchronously; this is a declared
synthetic-market assumption. The research strategy sees only delivered top of
book/trades and delivered reports through `StrategyContext`, never the engine.

Decision to departure adds strategy compute time; departure to arrival adds
order latency for new orders and cancel latency for cancel/replace. Exchange
reports add ack latency; top-of-book updates add feed latency. Uniform integer
jitter in [0, configured jitter] uses the latency stream. Feed and report channels
are FIFO. Order-entry FIFO is configurable; disabling it allows cancellation
races. Measured order/cancel durations include compute and FIFO waiting.

Stream seed = splitmix64_finalize((master XOR fnv1a64(label)) +
0x9E3779B97F4A7C15), modulo 2^64. Labels are `strategy`, `latency`, and
`agent.<index>.<kind>.<name>`. The manifest records every derived seed. Changing a
strategy does not change agent streams, although interactions change their fills.
Canonical config entries include defaults in fixed order and exclude master seed.
Canonical commands/events use `sim::fields`, `sim::canonical` and journal schema
1, never raw object storage. Digests are FNV-1a64, not cryptographic identities.

Four compiled baselines: fixed-spread MM, inventory-aware MM (skew and optional
size reduction), TWAP (cumulative equal-time IOC targets, catch-up on later
slices), immediate sweep (IOC plus retries on subsequent delivered data).
TWAP stops after its final scheduled slice. Missing liquidity or delayed reports
can leave a residual. Risk reserves worst-case remaining same-side quantities,
including pending new/replace orders, against delivered inventory. Host rejects
never reach the exchange. Buy and sell limits are independent.

## Accounting and metric conventions

Money is signed int64 price units, 1/10000 currency; quantities are shares,
multiplier one, initially flat. Ledger fills use checked arithmetic and bounded
FIFO lots. Negative per-share fees are rebates. A ledger failure after execution
fails the run. Setup rejects unsafe event/quantity/price products for aggregate
metric arithmetic. Monetary JSON values are exact decimal strings with trailing
zeros trimmed (at least two decimals); integer ids, counts and ns are strings.
Doubled-price amounts preserve half units with up to five decimals. Null means
unavailable. Floating-point summary `metrics` are convenience values only.

cash = initial cash - buy notional + sell notional - fees.
Equity = cash + position * mark.
Net P&L = equity - initial cash = FIFO gross realized + unrealized - fees.
Open cost basis is signed net FIFO lot cost (initial cash - cash - fees +
realized). Unrealized P&L = position * mark - open cost basis. Average cost is
derived after the run.

Reference exchange mid: the last two-sided engine-truth (bid + ask) / 2 at or
before the time in question, held exactly in doubled price units so a half
price unit is never rounded away. A one-sided book does not clear it; it is
unavailable only before the first two-sided book of the run. This matches the
`exchange_mid` convention in docs/API.md. Mark conventions (`[accounting] mark`):
`exchange_mid` (default) uses the reference mid; `last_trade_price` uses the
last engine trade price. `strategy_visible_mid` from the API enum is not
supported and is rejected at config load. Mark, unrealized P&L, equity and net
P&L are written with up to five decimals because they can carry half a price
unit. Without a mark the mark-dependent fields are null and the series sample
counts in `samples_without_mark`; a flat position with no mark reports equity
= cash. Drawdown is evaluated at series observations; it is sampled drawdown,
not every-event drawdown.

Order fill ratio = accepted new orders with any fill / accepted new orders.
Full fill ratio = completely filled orders / accepted new orders. Quantity fill
ratio = filled shares / (accepted new shares + positive replace increases).
Partial count means orders canceled after some execution. Time to first fill
starts at the order decision. Quote exposure counts time with a resting bid,
ask, both, or either; ratios divide by configured run duration. Signed spread
capture = (reference mid before the filling command - fill price) * buy
quantity, reversed for sells; unavailable before the first two-sided book.
Markouts use the reference mid after all simulation events at fill time +
horizon. One row per fill/horizon is measured, unavailable (no two-sided book
had existed yet), or censored. Horizons after a completed run end are censored and excluded, not
zero. Partial failed/cancelled runs retain processed observations and censor pending
probes against their last processed simulated time.

Execution benchmark is the reference (engine-truth) mid at parent decision
time, explicitly separate from the strategy-visible mid. Shortfall cost = signed (fill notional - benchmark *
filled quantity) + fees. It covers executed quantity only; unexecuted opportunity
cost is unavailable. Participation divides parent fills by total market volume
between declaration and last fill, including the strategy's own trades.

## Files

All files except `status.json` are deterministic for the same config source,
seed, build and complete execution. Status contains wall clocks and PID. Manifest
is written before stepping and never updated. Summary is written last; a file
alone is not proof of a successful run: inspect state and replay verification.
I/O failures throw and can leave incomplete artifacts. No HTTP service is included.

| File | Contents |
| --- | --- |
| manifest.json | `exsim.research.manifest/1`; config, builds, seeds, all model assumptions, file encodings |
| experiment.conf | verbatim supplied config source, including comments |
| status.json | `exsim.research.status/1`; atomic replace, state, simulated progress, wall time, diagnostics |
| commands.journal | canonical binary journal, engine setup, commands and footer, consumable by exsim_replay |
| commands.jsonl | engine commands, command index, simulated time, source and optional agent index |
| events.jsonl | independent replay of every engine event in canonical field order; indexes join commands |
| lifecycle.jsonl | strategy submit, exchange arrival/outcome and report delivery, risk rejects, FIFO evidence |
| ledger.jsonl | authoritative strategy fill legs and resulting exact cash, position, realized P&L, fees |
| markouts.jsonl | fill index, horizon, status, exchange mid and signed quantity-weighted markout |
| series.csv | sampled engine and delivered book, inventory, cash, mark and equity; empty cells unavailable |
| summary.json | `exsim.research.summary/1`; terminal accounting, metrics, reconciliation and replay digests |
| sweep.json | `exsim.research.sweep/1`; requested seeds, run directories, outcomes and complete flag |

Canonical command/event JSON payload fields follow the public `sim::fields`
visitor contract; prices are decimal currency strings and all integral fields
are decimal strings. `command_index` and `event_index` are zero-based. Lifecycle
queue evidence is exact engine truth at arrival, before command application:
`orders_ahead`, `qty_ahead`, `level_qty`; it is not a strategy observation or a
prediction that an aggressive order will rest. `reference_time_ns` connects a
stage to the prior request/exchange time. Optional stage-specific fields are null.
Ledger `mid_before` and `spread_capture` reference the book before that command.
Markout `markout` is quantity-weighted, not per-share. `fill_index` joins ledger
and markout rows. Series `position` is truth, `known_position` is delivered state;
`open_bid_qty` and `open_ask_qty` are true remaining strategy quantities.

`series.csv` columns, in order: `time_ns`, `exchange_bid`, `exchange_bid_qty`,
`exchange_ask`, `exchange_ask_qty`, `visible_bid`, `visible_bid_qty`,
`visible_ask`, `visible_ask_qty`, `position`, `known_position`, `cash`,
`realized_pnl`, `fees`, `mark_price`, `unrealized_pnl`, `equity`,
`open_bid_qty`, `open_ask_qty`. `exchange_*` is engine truth at the sample,
`visible_*` is what the strategy had been delivered; an empty price cell means
that side was empty. `mark_price`, `unrealized_pnl` and `equity` are empty when
there is no mark and can carry five decimals.

## HTTP integration differences

docs/API.md and docs/api/openapi.yaml (`exsim.api/1`) define the HTTP schema
family. The runner writes its own disk format, `exsim.research.*/1`, with
snake_case fields. The C++ HTTP service is expected to read these files and
translate; disk JSON does not validate against the OpenAPI schemas as is.
Semantics already agree: FIFO cost basis, cash/equity/net P&L identities, fee
sign, carried exact `exchange_mid` mark, censoring of markouts past the run
end, `fnv1a_64` run digests over canonical encodings, the run state names
(`queued`, `running`, `paused`, `completed`, `cancelled`, `failed`), and event
ordering by (simulation time, enqueue sequence).

What the adapter must translate or supply:

| Area | Disk runner (this document) | API contract | Adapter action |
| --- | --- | --- | --- |
| Schema names | `schema`: `exsim.research.manifest/1`, `summary/1`, `status/1`, `sweep/1`, `compare/1` | `schemaVersion`: `exsim.run-manifest/1`, `exsim.run-summary/1`, `exsim.comparison/1`; log encodings `exsim.canonical-event/1`, `exsim.order-lifecycle/1`, `exsim.ledger/1`, `exsim.series/1` | rename and restructure |
| Field case | snake_case | camelCase | rename |
| Market world | `synthetic_interactive` | `interactive_synthetic` | map the literal |
| Decimals | exact, trailing zeros trimmed to at least two decimals (`"100.04"`, `"1647.816"`) | fixed 4 (price, currency), 5 (mid, mark, markout, spread capture), 6 (derived) digits | re-pad; values are exact, no rounding needed except derived floats |
| Derived metrics | `summary.metrics` JSON numbers (doubles) | `DerivedDecimal` strings with `exactness` | format to 6 digits, round half to even |
| Config digest | `config_hash`: FNV-1a64 of canonical config text (16 hex) | `sha256` | compute SHA-256 over `experiment.conf` or the canonical text in the adapter |
| Strategy kinds | `fixed_spread_market_maker`, `inventory_aware_market_maker`, `twap_executor`, `immediate_sweep_executor` | enum `fixed_spread_market_maker`, `inventory_aware_market_maker`, `twap_execution`; `executableStrategyKinds` lists only fixed spread | map `twap_executor` to `twap_execution`; the API can widen `executableStrategyKinds`; immediate sweep has no API value |
| Seed streams | `strategy`, `latency`, `agent.<index>.<kind>.<name>` with formula in the manifest | `DerivedSeed.streamName` enum `background_market_maker`, `background_noise_taker`, `strategy`, `latency_jitter` | map per agent; several agents of one kind need an API change (enum allows one per kind) |
| Mark conventions | `exchange_mid`, `last_trade_price` | adds `strategy_visible_mid` | reject `strategy_visible_mid` with `validation_failed` |
| Missing mark, flat | equity = cash (mark null) | equity null with `unavailableReason` whenever the mark is null | emit null equity when `mark_price` is null, or agree to the runner value |
| Spread capture reference | reference mid before the command that produced the fill (pre-impact) | "mid at the fill time" | same value if read as the mid in force when the fill occurs; state it in the API text |
| Markout status | `measured`, `unavailable`, `censored` | `available`, `unavailable_no_mark`, `censored_past_run_end` | map one to one |
| Lifecycle stages | `strategy_submitted`, `risk_rejected`, `exchange_arrival`, `exchange_accepted`, `exchange_rejected`, `exchange_rested`, `exchange_fill`, `exchange_canceled`, `exchange_replaced`, `report_delivered` (with `report_kind`) | `LifecycleStageKind` (`arrived_at_exchange`, `acknowledgment_delivered`, `fill_report_delivered`, `cancel_rejected_too_late`, ...) and `CanonicalEventKind` | map; `risk_rejected` matches `risk_limit_blocked_order`; partial vs full fill from `leaves_qty` |
| Side, liquidity | `"BUY"`/`"SELL"`, `"maker"`/`"taker"` | `buy`/`sell`, `maker`/`taker` | lowercase side |
| Termination | `termination_reason`: `end_time_reached`, `event_queue_exhausted`, `cancel_requested`, `event_limit_reached`, `simulation_queue_full`, `observation_queue_full`, `recorder_overflow`, `ledger_rejected_fill`, `engine_output_capacity`, `order_table_corrupt` | `TerminalDiagnostics.outcomeCode` (`completed_at_simulation_bound`, `cancelled_by_request`, `failed_event_bound_exceeded`, `failed_journal_backpressure`, `failed_ledger_overflow`, `failed_internal_error`, ...) | map; queue/recorder/engine capacity map to `failed_internal_error` or `failed_journal_backpressure` |
| Per-order view | lifecycle rows per request and report | `/orders` with `OrderFinalStatus`, `/orders/{id}/lifecycle` | aggregate lifecycle rows by `cl_ord_id` |
| Series | `series.csv`, every sample interval, raw columns | `/series` downsampled, kinds `equity`, `inventory`, `exchange_mid`, `quoted_spread`, `drawdown` | derive `quoted_spread` from visible bid/ask, `drawdown` from running peak of `equity`; raw `exchange_mid` is null when one-sided (use `exchange_bid`/`exchange_ask`, not `mark_price`) |
| Comparison | `exsim_research compare` over two directories, paired by master seed | `/comparisons` by experiment id | reuse `run_compare` logic, or reimplement over summaries |
| Step unit | `Simulation::step` counts scheduler events (simulation and observation) | `step` counts canonical events | adapter steps until N canonical events were appended |
| HTTP/IDs | `run_id` is `<name>-<config hash prefix>-seed<N>`; no idempotency, pagination or wall-clock limit | opaque `run_...`, idempotency keys, cursors, `failed_wall_clock_limit_exceeded` | service concern |

`average_cost_basis` and the convenience `metrics` doubles are derived values;
never feed them back into accounting. No HTTP adapter exists yet; the
artifacts above are the complete interface it should consume.

## Field inventory

Nested objects group the fields described above. `_ns` is simulated nanoseconds,
`qty` is shares, monetary fields are currency, and `*_digest` is unsigned FNV-1a64.
The following paths enumerate the v1 writer fields, including explicit nulls.


### manifest.json

| Path | Encoding |
| --- | --- |
| `schema` | string |
| `artifact_schema_version` | string |
| `run_id` | string |
| `parent_run_id` | null or stage-dependent value |
| `mode` | string |
| `experiment.name` | string |
| `experiment.description` | string |
| `market_world` | string |
| `market_model.description` | string |
| `market_model.background_agent_latency` | string |
| `market_model.background_agent_count` | string |
| `config_hash` | string |
| `config_hash_algorithm` | string |
| `data_hash` | null or stage-dependent value |
| `data_hash_note` | string |
| `config.format` | string |
| `config.entries.experiment.name` | string |
| `config.entries.experiment.description` | string |
| `config.entries.experiment.duration_ns` | string |
| `config.entries.experiment.max_simulation_events` | string |
| `config.entries.instrument.symbol` | string |
| `config.entries.instrument.tick_size` | string |
| `config.entries.instrument.lot_size` | string |
| `config.entries.instrument.max_order_qty` | string |
| `config.entries.instrument.min_price` | string |
| `config.entries.instrument.max_price` | string |
| `config.entries.instrument.reference_price` | string |
| `config.entries.instrument.initial_phase` | string |
| `config.entries.engine.max_orders` | string |
| `config.entries.latency.feed_ns` | string |
| `config.entries.latency.order_ns` | string |
| `config.entries.latency.cancel_ns` | string |
| `config.entries.latency.ack_ns` | string |
| `config.entries.latency.strategy_compute_ns` | string |
| `config.entries.latency.feed_jitter_ns` | string |
| `config.entries.latency.order_jitter_ns` | string |
| `config.entries.latency.cancel_jitter_ns` | string |
| `config.entries.latency.ack_jitter_ns` | string |
| `config.entries.latency.order_channel_fifo` | string |
| `config.entries.fees.maker_fee_per_share` | string |
| `config.entries.fees.taker_fee_per_share` | string |
| `config.entries.fees.auction_fee_per_share` | string |
| `config.entries.risk.max_long_position` | string |
| `config.entries.risk.max_short_position` | string |
| `config.entries.risk.max_order_qty` | string |
| `config.entries.risk.max_open_orders` | string |
| `config.entries.accounting.initial_cash` | string |
| `config.entries.accounting.mark` | string |
| `config.entries.metrics.markout_horizons_ns` | string |
| `config.entries.metrics.sample_interval_ns` | string |
| `config.entries.runner.simulation_queue_capacity` | string |
| `config.entries.runner.observation_queue_capacity` | string |
| `config.entries.runner.recorder_command_capacity` | string |
| `config.entries.runner.recorder_record_capacity` | string |
| `config.entries.runner.recorder_flush_margin` | string |
| `config.entries.strategy.kind` | string |
| `config.entries.strategy.stp` | string |
| `config.entries.strategy.half_spread_ticks` | string |
| `config.entries.strategy.quote_size` | string |
| `config.entries.strategy.requote_interval_ns` | string |
| `config.entries.strategy.requote_threshold_ticks` | string |
| `config.entries.strategy.post_only` | string |
| `config.entries.agent.0.kind` | string |
| `config.entries.agent.0.name` | string |
| `config.entries.agent.0.timer_interval_ns` | string |
| `config.entries.agent.0.owner` | string |
| `config.entries.agent.0.stp` | string |
| `config.entries.agent.0.enabled` | string |
| `config.entries.agent.0.levels` | string |
| `config.entries.agent.0.size` | string |
| `config.entries.agent.0.spread_ticks` | string |
| `config.entries.agent.0.fair_value_ticks` | string |
| `config.entries.agent.0.fair_value_step_ms` | string |
| `config.entries.agent.0.fair_value_step_ticks` | string |
| `config.entries.agent.0.requote_ms` | string |
| `config.entries.agent.0.skew_ticks_per_unit` | string |
| `config.entries.agent.0.cancel_on_halt` | string |
| `config.entries.agent.0.order_capacity` | string |
| `config.entries.agent.1.kind` | string |
| `config.entries.agent.1.name` | string |
| `config.entries.agent.1.timer_interval_ns` | string |
| `config.entries.agent.1.owner` | string |
| `config.entries.agent.1.stp` | string |
| `config.entries.agent.1.enabled` | string |
| `config.entries.agent.1.mean_interval_ms` | string |
| `config.entries.agent.1.min_size` | string |
| `config.entries.agent.1.max_size` | string |
| `config.entries.agent.1.cross_ticks` | string |
| `config.entries.agent.1.limit_ppm` | string |
| `config.entries.agent.1.limit_lifetime_ms` | string |
| `config.entries.agent.1.fair_value_ticks` | string |
| `config.entries.agent.1.order_capacity` | string |
| `master_seed` | string |
| `seed_derivation.formula` | string |
| `seed_derivation.streams[].stream` | string |
| `seed_derivation.streams[].seed` | string |
| `build.engine_build_id` | string |
| `build.runner_build_id` | string |
| `build.canonical_encoding_schema` | string |
| `build.compiler` | string |
| `strategy.kind` | string |
| `strategy.name` | string |
| `strategy.version` | string |
| `strategy.build_id` | string |
| `strategy.session_id` | string |
| `strategy.owner_id` | string |
| `strategy.parameters.stp` | string |
| `strategy.parameters.half_spread_ticks` | string |
| `strategy.parameters.quote_size` | string |
| `strategy.parameters.requote_interval_ns` | string |
| `strategy.parameters.requote_threshold_ticks` | string |
| `strategy.parameters.post_only` | string |
| `background_agents[].index` | string |
| `background_agents[].name` | string |
| `background_agents[].kind` | string |
| `background_agents[].session_id` | string |
| `background_agents[].seed_stream` | string |
| `background_agents[].seed` | string |
| `background_agents[].parameters.timer_interval_ns` | string |
| `background_agents[].parameters.owner` | string |
| `background_agents[].parameters.stp` | string |
| `background_agents[].parameters.enabled` | string |
| `background_agents[].parameters.levels` | string |
| `background_agents[].parameters.size` | string |
| `background_agents[].parameters.spread_ticks` | string |
| `background_agents[].parameters.fair_value_ticks` | string |
| `background_agents[].parameters.fair_value_step_ms` | string |
| `background_agents[].parameters.fair_value_step_ticks` | string |
| `background_agents[].parameters.requote_ms` | string |
| `background_agents[].parameters.skew_ticks_per_unit` | string |
| `background_agents[].parameters.cancel_on_halt` | string |
| `background_agents[].parameters.order_capacity` | string |
| `background_agents[].parameters.mean_interval_ms` | string (noise taker) |
| `background_agents[].parameters.min_size` | string (noise taker) |
| `background_agents[].parameters.max_size` | string (noise taker) |
| `background_agents[].parameters.limit_ppm` | string (noise taker) |
| `background_agents[].parameters.cross_ticks` | string (noise taker) |
| `background_agents[].parameters.limit_lifetime_ms` | string (noise taker) |
| `instrument.instrument_id` | string |
| `instrument.symbol` | string |
| `instrument.tick_size` | string |
| `instrument.lot_size` | string |
| `instrument.max_order_qty` | string |
| `instrument.min_price` | string |
| `instrument.max_price` | string |
| `instrument.reference_price` | string |
| `instrument.initial_phase` | string |
| `instrument.engine_max_orders` | string |
| `latency_model.units` | string |
| `latency_model.feed_ns` | string |
| `latency_model.order_ns` | string |
| `latency_model.cancel_ns` | string |
| `latency_model.ack_ns` | string |
| `latency_model.strategy_compute_ns` | string |
| `latency_model.feed_jitter_ns` | string |
| `latency_model.order_jitter_ns` | string |
| `latency_model.cancel_jitter_ns` | string |
| `latency_model.ack_jitter_ns` | string |
| `latency_model.order_channel_fifo` | boolean |
| `latency_model.jitter_distribution` | string |
| `latency_model.channel_ordering` | string |
| `queue_model.kind` | string |
| `queue_model.description` | string |
| `fee_schedule.units` | string |
| `fee_schedule.maker_fee_per_share` | string |
| `fee_schedule.taker_fee_per_share` | string |
| `fee_schedule.auction_fee_per_share` | string |
| `fee_schedule.fee_rule` | string |
| `risk_limits.max_long_position` | string |
| `risk_limits.max_short_position` | string |
| `risk_limits.max_order_qty` | string |
| `risk_limits.max_open_orders` | string |
| `risk_limits.check` | string |
| `accounting.price_scale` | string |
| `accounting.money_units` | string |
| `accounting.initial_cash` | string |
| `accounting.initial_position` | string |
| `accounting.mark_convention` | string |
| `accounting.mark_definition` | string |
| `accounting.cost_basis` | string |
| `accounting.realized_pnl` | string |
| `accounting.equity` | string |
| `accounting.arithmetic` | string |
| `metrics.markout_horizons_ns[]` | string |
| `metrics.markout_reference` | string |
| `metrics.spread_capture_reference` | string |
| `metrics.execution_benchmark` | string |
| `metrics.reference_mid_definition` | string |
| `metrics.sample_interval_ns` | string |
| `simulation.start_time_ns` | string |
| `simulation.end_time_ns` | string |
| `simulation.max_simulation_events` | string |
| `simulation.simulation_queue_capacity` | string |
| `simulation.observation_queue_capacity` | string |
| `simulation.recorder_command_capacity` | string |
| `simulation.recorder_record_capacity` | string |
| `simulation.recorder_flush_margin` | string |
| `simulation.successful_completion` | string |
| `simulation.time_limit` | string |
| `event_order_policy` | string |
| `sessions.strategy_session_id` | string |
| `sessions.background_agent_session_ids` | string |
| `sessions.scripted_session_ids` | string |
| `files[].name` | string |
| `files[].format` | string |
| `files[].schema` | string |
| `files[].description` | string |

### status.json

| Path | Encoding |
| --- | --- |
| `schema` | string |
| `run_id` | string |
| `state` | string |
| `termination_reason` | string |
| `failure_detail` | string |
| `sim_time_ns` | string |
| `end_time_ns` | string |
| `progress` | derived number |
| `simulation_events` | string |
| `commands_written` | string |
| `summary_written` | boolean |
| `replay_verified` | boolean |
| `wall_clock_started_utc` | string |
| `wall_clock_updated_utc` | string |
| `wall_elapsed_seconds` | derived number |
| `process_id` | string |

### summary.json

| Path | Encoding |
| --- | --- |
| `schema` | string |
| `run_id` | string |
| `master_seed` | string |
| `config_hash` | string |
| `strategy.name` | string |
| `strategy.version` | string |
| `state` | string |
| `termination_reason` | string |
| `failure_detail` | string |
| `simulation.end_time_ns` | string |
| `simulation.final_time_ns` | string |
| `simulation.simulation_events` | string |
| `simulation.observation_events` | string |
| `simulation.simulation_queue_high_water` | string |
| `simulation.observation_queue_high_water` | string |
| `simulation.engine_commands` | string |
| `simulation.strategy_commands` | string |
| `simulation.background_commands` | string |
| `simulation.scripted_commands` | string |
| `simulation.engine_events` | string |
| `simulation.market_data_published` | string |
| `simulation.market_data_delivered` | string |
| `simulation.reports_scheduled` | string |
| `simulation.reports_delivered` | string |
| `simulation.strategy_wakeups` | string |
| `simulation.market_trades` | string |
| `simulation.market_traded_qty` | string |
| `digests.command_digest` | string |
| `digests.event_digest` | string |
| `digests.ledger_digest` | string |
| `digests.command_count` | string |
| `digests.event_count` | string |
| `digests.fill_count` | string |
| `digests.replay_verified` | boolean |
| `accounting.units` | string |
| `accounting.mark_convention` | string |
| `accounting.initial_cash` | string |
| `accounting.initial_position` | string |
| `accounting.cash` | string |
| `accounting.position` | string |
| `accounting.bought_qty` | string |
| `accounting.sold_qty` | string |
| `accounting.buy_notional` | string |
| `accounting.sell_notional` | string |
| `accounting.open_cost_basis` | string |
| `accounting.average_cost_basis` | derived number |
| `accounting.realized_pnl` | string |
| `accounting.fees` | string |
| `accounting.maker_fees` | string |
| `accounting.taker_fees` | string |
| `accounting.auction_fees` | string |
| `accounting.mark_price` | string |
| `accounting.unrealized_pnl` | string |
| `accounting.equity` | string |
| `accounting.net_pnl` | string |
| `accounting.max_drawdown` | string |
| `accounting.max_drawdown_time_ns` | string |
| `accounting.samples_without_mark` | string |
| `accounting.reconciliation.cash_matches_fills` | boolean |
| `accounting.reconciliation.position_matches_fills` | boolean |
| `accounting.reconciliation.fees_match_fills` | boolean |
| `accounting.reconciliation.pnl_identity_holds` | boolean |
| `orders.new_orders_submitted` | string |
| `orders.cancel_requests_submitted` | string |
| `orders.replace_requests_submitted` | string |
| `orders.risk_rejects` | string |
| `orders.risk_rejects_by_reason.invalid_quantity` | string |
| `orders.risk_rejects_by_reason.invalid_price` | string |
| `orders.risk_rejects_by_reason.max_order_quantity` | string |
| `orders.risk_rejects_by_reason.max_open_orders` | string |
| `orders.risk_rejects_by_reason.max_long_position` | string |
| `orders.risk_rejects_by_reason.max_short_position` | string |
| `orders.risk_rejects_by_reason.unknown_order` | string |
| `orders.risk_rejects_by_reason.request_in_flight` | string |
| `orders.risk_rejects_by_reason.not_yet_acknowledged` | string |
| `orders.risk_rejects_by_reason.run_not_active` | string |
| `orders.risk_rejects_by_reason.queue_capacity` | string |
| `orders.cancels_resent_after_ack` | string |
| `orders.orders_accepted` | string |
| `orders.orders_rejected` | string |
| `orders.cancel_requests_rejected` | string |
| `orders.replace_requests_rejected` | string |
| `orders.orders_canceled_by_request` | string |
| `orders.ioc_remainders_canceled` | string |
| `orders.self_trade_cancels` | string |
| `orders.replaces_applied` | string |
| `orders.replaces_priority_kept` | string |
| `orders.orders_with_fill` | string |
| `orders.orders_filled_completely` | string |
| `orders.orders_ended_partially_filled` | string |
| `orders.accepted_order_qty` | string |
| `orders.replace_added_qty` | string |
| `orders.acceptance_ratio` | derived number |
| `orders.order_fill_ratio` | derived number |
| `orders.full_fill_ratio` | derived number |
| `orders.quantity_fill_ratio` | derived number |
| `orders.time_to_first_fill.count` | string |
| `orders.time_to_first_fill.min_ns` | string |
| `orders.time_to_first_fill.max_ns` | string |
| `orders.time_to_first_fill.mean_ns` | derived number |
| `orders.time_to_first_fill.sum_ns` | string |
| `fills.count` | string |
| `fills.qty` | string |
| `fills.maker_count` | string |
| `fills.maker_qty` | string |
| `fills.taker_count` | string |
| `fills.taker_qty` | string |
| `fills.auction_count` | string |
| `fills.auction_qty` | string |
| `fills.spread_capture.reference` | string |
| `fills.spread_capture.fills` | string |
| `fills.spread_capture.qty` | string |
| `fills.spread_capture.unavailable` | string |
| `fills.spread_capture.total` | string |
| `fills.spread_capture.per_share` | derived number |
| `markouts[].horizon_ns` | string |
| `markouts[].reference` | string |
| `markouts[].measured` | string |
| `markouts[].unavailable` | string |
| `markouts[].censored` | string |
| `markouts[].measured_qty` | string |
| `markouts[].total` | string |
| `markouts[].per_share` | derived number |
| `quote_exposure.source` | string |
| `quote_exposure.bid_ns` | string |
| `quote_exposure.ask_ns` | string |
| `quote_exposure.two_sided_ns` | string |
| `quote_exposure.any_side_ns` | string |
| `quote_exposure.two_sided_ratio` | derived number |
| `quote_exposure.any_side_ratio` | derived number |
| `inventory.max_position` | string |
| `inventory.min_position` | string |
| `inventory.max_long_limit` | string |
| `inventory.max_short_limit` | string |
| `inventory.limit_breaches` | string |
| `inventory.final_known_position` | string |
| `inventory.final_open_orders` | string |
| `latency.feed.count` | string |
| `latency.feed.min_ns` | string |
| `latency.feed.max_ns` | string |
| `latency.feed.mean_ns` | derived number |
| `latency.feed.sum_ns` | string |
| `latency.order.count` | string |
| `latency.order.min_ns` | string |
| `latency.order.max_ns` | string |
| `latency.order.mean_ns` | derived number |
| `latency.order.sum_ns` | string |
| `latency.cancel.count` | string |
| `latency.cancel.min_ns` | string |
| `latency.cancel.max_ns` | string |
| `latency.cancel.mean_ns` | derived number |
| `latency.cancel.sum_ns` | string |
| `latency.ack.count` | string |
| `latency.ack.min_ns` | string |
| `latency.ack.max_ns` | string |
| `latency.ack.mean_ns` | derived number |
| `latency.ack.sum_ns` | string |
| `execution` | null or stage-dependent value |
| `background_agents[].name` | string |
| `background_agents[].kind` | string |
| `background_agents[].session_id` | string |
| `background_agents[].seed` | string |
| `background_agents[].commands_submitted` | string |
| `background_agents[].fills_received` | string |
| `background_agents[].filled_qty` | string |
| `background_agents[].rejects_received` | string |
| `background_agents[].inventory` | string |
| `metrics.net_pnl` | derived number |
| `metrics.equity` | derived number |
| `metrics.realized_pnl` | derived number |
| `metrics.unrealized_pnl` | derived number |
| `metrics.fees` | derived number |
| `metrics.final_position` | derived number |
| `metrics.max_position` | derived number |
| `metrics.min_position` | derived number |
| `metrics.limit_breaches` | derived number |
| `metrics.max_drawdown` | derived number |
| `metrics.orders_submitted` | derived number |
| `metrics.orders_accepted` | derived number |
| `metrics.orders_rejected` | derived number |
| `metrics.risk_rejects` | derived number |
| `metrics.cancel_requests` | derived number |
| `metrics.replace_requests` | derived number |
| `metrics.fill_count` | derived number |
| `metrics.filled_qty` | derived number |
| `metrics.maker_qty` | derived number |
| `metrics.taker_qty` | derived number |
| `metrics.order_fill_ratio` | derived number |
| `metrics.quantity_fill_ratio` | derived number |
| `metrics.time_to_first_fill_mean_ns` | derived number |
| `metrics.two_sided_exposure_ratio` | derived number |
| `metrics.spread_capture_per_share` | derived number |
| `metrics.spread_capture_total` | derived number |
| `metrics.markout_100ms_per_share` | derived number |
| `metrics.markout_100ms_censored` | derived number |
| `metrics.markout_1s_per_share` | derived number |
| `metrics.markout_1s_censored` | derived number |
| `metrics.markout_5s_per_share` | derived number |
| `metrics.markout_5s_censored` | derived number |
| `metrics.completion_ratio` | null or stage-dependent value |
| `metrics.residual_qty` | null or stage-dependent value |
| `metrics.implementation_shortfall_per_share` | null or stage-dependent value |
| `metrics.participation_rate` | null or stage-dependent value |
| `metrics.feed_latency_mean_ns` | derived number |
| `metrics.order_latency_mean_ns` | derived number |
| `metrics.cancel_latency_mean_ns` | derived number |
| `metrics.ack_latency_mean_ns` | derived number |
| `metrics.market_traded_qty` | derived number |

### lifecycle.jsonl

| Path | Encoding |
| --- | --- |
| `time_ns` | string |
| `stage` | string |
| `cl_ord_id` | string |
| `command_index` | null or stage-dependent value |
| `request` | string |
| `reference_time_ns` | string |
| `order_id` | string |
| `side` | string |
| `price` | string |
| `qty` | string |
| `leaves_qty` | string |
| `cum_qty` | string |
| `tif` | string |
| `post_only` | boolean |
| `report_kind` | null or stage-dependent value |
| `match_id` | null or stage-dependent value |
| `liquidity` | null or stage-dependent value |
| `fee` | null or stage-dependent value |
| `reject_reason` | null or stage-dependent value |
| `cancel_reason` | null or stage-dependent value |
| `risk_reason` | null or stage-dependent value |
| `priority_kept` | null or stage-dependent value |
| `queue` | null or stage-dependent value |

### ledger.jsonl

| Path | Encoding |
| --- | --- |
| `fill_index` | string |
| `time_ns` | string |
| `command_index` | string |
| `match_id` | string |
| `cl_ord_id` | string |
| `order_id` | string |
| `side` | string |
| `liquidity` | string |
| `price` | string |
| `qty` | string |
| `fee_per_share` | string |
| `fee` | string |
| `cash_after` | string |
| `position_after` | string |
| `realized_pnl_after` | string |
| `fees_after` | string |
| `mid_before` | string |
| `spread_capture` | string |

### markouts.jsonl

| Path | Encoding |
| --- | --- |
| `fill_index` | string |
| `fill_time_ns` | string |
| `horizon_ns` | string |
| `status` | string |
| `side` | string |
| `price` | string |
| `qty` | string |
| `mid` | string |
| `markout` | string |

### Config keys

INI sections accept the keys below. Defaults and resolved values are emitted by
`validate`; the supplied example configs exercise the first vertical slice.
Durations require integer ns/us/ms/s suffixes, money accepts up to four fractional
digits, booleans are true/false. Unknown keys are errors. Agent parameters use
the existing bots config parser and manifest records their resolved values.

`kind`, `stp`, `half_spread_ticks`, `quote_size`, `requote_interval`, `requote_threshold_ticks`, `post_only`, `skew_ticks_per_unit`, `skew_unit`, `scale_size_with_inventory`, `side`, `parent_qty`, `start`, `limit_offset_ticks`, `execution_duration`, `slice_interval`, `max_orders`, `maker_fee_per_share`, `taker_fee_per_share`, `auction_fee_per_share`, `max_long_position`, `max_short_position`, `max_order_qty`, `max_open_orders`, `initial_cash`, `mark`, `name`, `description`, `seed`, `duration`, `max_simulation_events`, `symbol`, `tick_size`, `lot_size`, `min_price`, `max_price`, `reference_price`, `feed`, `order`, `cancel`, `ack`, `strategy_compute`, `feed_jitter`, `order_jitter`, `cancel_jitter`, `ack_jitter`, `order_channel_fifo`, `sample_interval`, `markout_horizons`, `simulation_queue_capacity`, `observation_queue_capacity`, `recorder_command_capacity`, `recorder_record_capacity`, `recorder_flush_margin`, `session_id`, `instrument`, `timer_interval`, `owner`.

## Verification checkpoint, 2026-11-05

Verified on the commit that adds this section, on top of the
`exchange-simulator` merge `36ed695` (parent `e2cdd11`). Every command used
-j4 and `EXTRA_CXXFLAGS=-Werror`; logs:

| Gate | Result | Log |
| --- | --- | --- |
| `make MODE=release test` | 663/663 | `/tmp/exsim-runner-release.log` |
| `make MODE=debug test` (ASan/UBSan) | 661/661 (two production-only allocation tests are not built in debug) | `/tmp/exsim-runner-debug.log` |
| `make all` | clean | `/tmp/exsim-runner-all.log` |
| `make MODE=release test FILTER=research_allocations` | 2/2, zero allocations for the ledger and all four strategies | `/tmp/exsim-runner-allocations.log` |
| Linux (Docker `exsim-dev`), g++ then clang++, release test | g++ 663/663, clang++ 663/663 (clang++ first exposed a `std::pair` over the incomplete `JsonValue`, fixed with `JsonMember`) | `/tmp/exsim-runner-linux.log` |
| 5-seed sweeps narrow and wide, independent replay | 10/10 completed, replay verified | `/tmp/exsim-runner-narrow.log`, `/tmp/exsim-runner-wide.log` |
| Determinism: the narrow sweep run twice, `diff -r -x status.json` | all 51 files except status.json byte-identical | `/tmp/exsim-runner-determinism.log` |
| `exsim_replay` on each narrow journal | hash equals `summary.digests.event_digest` for all 5 | `/tmp/exsim-runner-replay.log` |
| `exsim_research compare` | output below | `/tmp/exsim-runner-compare.log` |

The research suite (50 `research_*` tests plus 9 `position_ledger` tests) covers scheduler
capacity and ordering, pause/step/resume identity, separate delivery
latencies, cancel races, FIFO evidence, every host risk reject reason, worst
case inventory reservation, exact accounting, the carried half-unit reference
mid, fixed-spread and inventory-skew quote placement, TWAP shortfall,
TWAP/sweep completion and residuals, markout censoring including
cancellation, same-seed artifact identity, independent replay, RNG stream
independence, compare pairing/exclusion/null handling over real sweeps,
example config parsing, config identity and error lines, and release-mode
zero allocations.

Remaining integration limits: no HTTP adapter (see the differences table).
Historical replay, external strategy virtual time and arbitrary rewind are
deferred. The CLI exposes batch execution and cancellation; `Simulation`
exposes pause, step and resume for the future controller. Scripted C++
commands can intervene; there is no CLI scenario-file interface. Build ids
default to encoding/version labels; the CLI also hashes its executable.

### Five-seed comparison

Synthetic demonstration, not a calibrated performance prediction. With a 0.01
tick every mid is a whole price unit, so these runs exercise the carried mid
(markouts) but not the half-unit path, which the accounting tests cover.

```text
A: /tmp/exsim-runner-narrow (sweep, 5 runs, experiment fixed_spread_narrow, strategy fixed_spread_market_maker, config 0edd6d9534f6122c)
B: /tmp/exsim-runner-wide (sweep, 5 runs, experiment fixed_spread_wide, strategy fixed_spread_market_maker, config 1413249966561a2b)
paired seeds (5): 1,2,3,4,5
unpaired seeds: A none; B none
excluded runs (not completed): none
config differences (A -> B):
  experiment.name: fixed_spread_narrow -> fixed_spread_wide
  experiment.description: Fixed-spread market maker, half spread 1 tick, MM + noise background -> Fixed-spread market maker, half spread 3 ticks, MM + noise background
  strategy.half_spread_ticks: 1 -> 3
paired differences are B - A; sd is the sample standard deviation over pairs; se = sd / sqrt(n)
metric                                  n   na        mean A        mean B      mean B-A        sd B-A        se B-A       min B-A       max B-A
net_pnl                                 5    0       1746.87       1555.17        -191.7       70.3207       31.4484      -280.512       -98.588
equity                                  5    0   1.00175e+06   1.00156e+06        -191.7       70.3207       31.4484      -280.512       -98.588
realized_pnl                            5    0       1463.88       1457.07        -6.806       70.4781       31.5188        -97.05         89.61
unrealized_pnl                          5    0         3.882        11.764         7.882       4.97623       2.22544           0.3         13.76
fees                                    5    0      -279.112      -86.3364       192.776       3.62335       1.62041       188.498       197.222
final_position                          5    0         157.2          87.8         -69.4       164.613        73.617          -284           159
max_position                            5    0           500           500             0             0             0             0             0
min_position                            5    0          -500          -500             0             0             0             0             0
limit_breaches                          5    0             0             0             0             0             0             0             0
max_drawdown                            5    0        6.5208        9.4678         2.947       2.84679       1.27312        -0.651          7.15
orders_submitted                        5    0         833.4         250.2        -583.2       15.2217       6.80735          -607          -569
orders_accepted                         5    0         833.4         250.2        -583.2       15.2217       6.80735          -607          -569
orders_rejected                         5    0             0             0             0             0             0             0             0
risk_rejects                            5    0             0             0             0             0             0             0             0
cancel_requests                         5    0             0             0             0             0             0             0             0
replace_requests                        5    0          2512        3415.2         903.2       100.408       44.9037           742          1010
fill_count                              5    0        2061.8         720.8         -1341        38.607       17.2656         -1393         -1302
filled_qty                              5    0        139556       43168.2      -96387.8       1811.67       810.205        -98611        -94249
maker_qty                               5    0        139556       43168.2      -96387.8       1811.67       810.205        -98611        -94249
taker_qty                               5    0             0             0             0             0             0             0             0
order_fill_ratio                        5    0      0.999036      0.996817    -0.0022185    0.00343697    0.00153706   -0.00673458    0.00124069
quantity_fill_ratio                     5    0      0.998652      0.995757   -0.00289513   0.000685837   0.000306716   -0.00352732   -0.00172484
time_to_first_fill_mean_ns              5    0   5.80981e+07   1.63434e+08   1.05335e+08   1.29584e+07   5.79517e+06   9.32557e+07   1.26284e+08
two_sided_exposure_ratio                5    0      0.903506      0.887888    -0.0156179     0.0206085     0.0092164    -0.0375181    0.00801068
spread_capture_per_share                5    0     0.0111987     0.0310096     0.0198108    9.2803e-05   4.15028e-05     0.0196544     0.0198877
spread_capture_total                    5    0       1562.81       1338.75      -224.062       56.3946       25.2204       -320.75      -178.605
markout_100ms_per_share                 5    0     0.0102624     0.0341646     0.0239022   0.000571481   0.000255574     0.0230294     0.0244305
markout_100ms_censored                  5    0             4           1.4          -2.6       1.14018      0.509902            -4            -1
markout_1s_per_share                    5    0      0.010251     0.0338601     0.0236091   0.000632283   0.000282766     0.0225282     0.0241681
markout_1s_censored                     5    0            37          15.2         -21.8       4.08656       1.82757           -29           -19
markout_5s_per_share                    5    0     0.0104674     0.0341867     0.0237193   0.000885658   0.000396078      0.022341     0.0245737
markout_5s_censored                     5    0         178.8          68.4        -110.4       7.82943       3.50143          -122          -100
completion_ratio                        0    5           n/a           n/a           n/a           n/a           n/a           n/a           n/a
residual_qty                            0    5           n/a           n/a           n/a           n/a           n/a           n/a           n/a
implementation_shortfall_per_share      0    5           n/a           n/a           n/a           n/a           n/a           n/a           n/a
participation_rate                      0    5           n/a           n/a           n/a           n/a           n/a           n/a           n/a
feed_latency_mean_ns                    5    0       55216.5       55358.8       142.288       52.3082       23.3929       96.6156       228.425
order_latency_mean_ns                   5    0        115256        115168       -88.191       294.705       131.796      -454.129       250.722
cancel_latency_mean_ns                  5    0       95595.1       95878.6       283.504       141.253       63.1704       89.4953       427.907
ack_latency_mean_ns                     5    0        111340        112020       680.019       40.1769       17.9677       643.544        743.42
market_traded_qty                       5    0        270261        259195      -11065.2       581.433       260.025        -11631        -10148
```
