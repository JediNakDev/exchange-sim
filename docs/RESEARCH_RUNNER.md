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
Open cost basis is signed net FIFO lot cost. Average cost is derived after the
run. The mark is floor(current two-sided exchange mid) in price units, or last
trade under `last_trade`. No current mid means no mark, even if an earlier mid
existed. Flat equity is cash without requiring a mark. Drawdown is evaluated at
series observations; it is sampled drawdown, not every-event drawdown.

Order fill ratio = accepted new orders with any fill / accepted new orders.
Full fill ratio = completely filled orders / accepted new orders. Quantity fill
ratio = filled shares / (accepted new shares + positive replace increases).
Partial count means orders canceled after some execution. Time to first fill
starts at the order decision. Quote exposure counts time with a resting bid,
ask, both, or either; ratios divide by configured run duration. Signed spread
capture = (mid before filling command - fill price) * buy quantity, reversed for
sells. Markouts use the current two-sided exchange mid after all simulation
events at fill time + horizon. One row per fill/horizon is measured, unavailable,
or censored. Horizons after a completed run end are censored and excluded, not
zero. Partial failed/cancelled runs retain processed observations and censor pending
probes against their last processed simulated time.

Execution benchmark is engine-truth mid at parent decision time, explicitly
separate from visible mid. Shortfall cost = signed (fill notional - benchmark *
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

## HTTP integration differences

The completed API/UI branch defines another schema family. This disk format
intentionally retains distinct `exsim.research.*` names and snake_case fields:
an adapter must translate, not claim disk JSON passes OpenAPI validation.

| Disk runner | Current API/UI contract |
| --- | --- |
| `schema`, exsim.research.manifest/1 and summary/1 | `schemaVersion`, exsim.run-manifest/1 and exsim.run-summary/1 |
| `synthetic_interactive` | `interactive_synthetic` |
| FIFO realized/unrealized | average cost split |
| current floor mid, unavailable when one-sided | carried-forward exact half-unit mid |
| FNV-1a64 config digest | SHA-256 config digest |
| trimmed exact decimal strings | fixed 4/5 fractional digits |
| derived JSON numbers in convenience metrics | derived decimal strings with exactness metadata |
| step counts scheduler and observation events | API describes canonical-event counts |
| four compiled strategies, parent execution metrics | first UI slice only fixed-spread MM |

FIFO and mark differences require an explicit product decision or API revision;
format conversion alone cannot reconcile those semantics. Do not recompute
accounting in TypeScript. A future C++ HTTP adapter must preserve these assumptions
in its manifest or reject incompatible requests. Real runs under `/tmp/research-real-*`
are generated by the CLI, never copied from `web/mock-server` fixtures.

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

## Verification checkpoint, 2026-11-04

Implementation commit `b06a276`, parent integration merge `e44b5c6` (parent
`4f442396723b29cf721269cf58d2d6d6142ed462`). Follow-up commit records allocation
migration, bounded metric arithmetic, cancellation censoring and this contract.

Commands above use -j4 and -Werror. Exact logs:

- `/tmp/research-final-release.log`: full release suite, known server-core failure.
- `/tmp/research-final-debug.log`: full ASan/UBSan suite, same server-core failure.
- `/tmp/research-final-release-focused.log` and `/tmp/research-final-debug-focused.log`: research suites only.
- `/tmp/research-final-tools.log`: all native tools.
- `/tmp/research-linux.log`: full GCC -Werror attempt, blocked by unrelated fuzz header.
- `/tmp/research-final-linux.log`: GCC -Werror research tests, ledger tests, framework self-tests and all tools.
- `/tmp/research-narrow.log`, `/tmp/research-wide.log`: real five-seed CLI sweeps, each run independently replay verified.
- `/tmp/research-compare.log`: complete paired compare output below.

The full-suite failure is
`tests/exsim/server/core/dispatch_tests.cpp:174`: SubmitResult actual 3, expected
Applied (0), in `output_capacity_failure_applies_nothing_and_core_recovers`.
It is outside runner ownership. Full GCC compilation is blocked by
`fuzz/targets/price_text.hpp` using `unsigned __int128` under -Wpedantic -Werror.
No server, shared engine, fuzz, or Makefile changes were made to bypass either.

Focused Linux reproduction (Docker image `exsim-dev`):

```sh
docker run --rm -v "$PWD:/src:ro" -w /tmp exsim-dev sh -c '
mkdir research-check
cp -R /src/include /src/src /src/tests /src/tools /src/examples /src/benchmark /src/Makefile research-check/
cd research-check
make -j4 test tools CXX=g++ EXTRA_CXXFLAGS=-Werror TEST_SRCS="tests/main.cpp tests/framework/alloc_counter.cpp tests/framework/alloc_counter_tests.cpp $(find tests/exsim/research -name "*.cpp")"
'
```

Research coverage includes scheduler capacity/order, pause/resume identity,
separate delivery latencies, cancel race, FIFO evidence, exact accounting,
missing marks, same-seed artifact identity, independent replay, RNG independence,
TWAP and sweep completion/residuals, pending-order risk reservations, markout
censoring including cancellation, paired sample dispersion, invalid config,
config identity and release zero allocations for every strategy and ledger.

Remaining integration limits: the HTTP adapter is not implemented. API accounting
and mark semantics require agreement as described above. Historical replay,
external strategy virtual time and arbitrary rewind are deferred product work.
The CLI exposes batch execution and cancellation; C++ Simulation exposes pause,
step and resume for the future controller. Scripted C++ commands can intervene;
there is no CLI scenario-file interface. Build IDs default to encoding/version
labels; the CLI additionally hashes its complete executable for provenance.

### Actual five-seed comparison

These are synthetic demonstrations, not calibrated performance predictions.

```text
A: /tmp/research-real-narrow (sweep, 5 runs, experiment fixed_spread_narrow, strategy fixed_spread_market_maker, config 0edd6d9534f6122c)
B: /tmp/research-real-wide (sweep, 5 runs, experiment fixed_spread_wide, strategy fixed_spread_market_maker, config 1413249966561a2b)
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
spread_capture_per_share                5    0     0.0111691     0.0310048     0.0198356   8.23966e-05   3.68489e-05     0.0196903     0.0198888
spread_capture_total                    5    0       1545.63       1313.83      -231.807        60.289       26.9621      -334.935      -178.925
markout_100ms_per_share                 5    0     0.0102814     0.0342289     0.0239475   0.000510631   0.000228361     0.0231202     0.0244701
markout_100ms_censored                  5    0             4           1.4          -2.6       1.14018      0.509902            -4            -1
markout_1s_per_share                    5    0     0.0102892     0.0339451     0.0236559   0.000570769   0.000255256     0.0227884     0.0242441
markout_1s_censored                     5    0            37          15.2         -21.8       4.08656       1.82757           -29           -19
markout_5s_per_share                    5    0     0.0104998      0.034069     0.0235692    0.00073027   0.000326587     0.0225495     0.0245656
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
