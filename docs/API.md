# exsim research API

Date: 2026-11-04. Contract: [`docs/api/openapi.yaml`](api/openapi.yaml) (OpenAPI 3.1, paths under `/v1`, schema family `exsim.api/1`).

This is the HTTP contract between the C++ research service and its clients: the Next.js research UI in `web/`, the CLI and research scripts. PRODUCT.md explains why it is HTTP and OpenAPI rather than tRPC. The C++ service is the source of truth for matching, accounting and simulated time. Clients display what the API returns and never recompute matching or ledger results.

The C++ service in `tools/exsim_api/` implements this contract using the deterministic research runner. `bun run dev` in `web/` builds and starts it with Next.js. `bun run dev:mock` explicitly selects the TypeScript mock for UI-only work. Real-service response schemas are checked by `web/e2e/exsim-api-binary.e2e.ts`.

## Conventions

### Units and number encoding

JSON numbers are used only for small bounded values: counts in pages, `sampleCount`, heartbeat intervals and similar. Everything that is exact or can exceed 2^53 travels as a decimal string.

| Schema | Meaning | Format |
| --- | --- | --- |
| `UnsignedIntegerString` | ids, quantities in shares, counts, sequence numbers, seeds, event positions | `^(0\|[1-9][0-9]*)$` |
| `SignedIntegerString` | signed positions in shares | optional leading `-` |
| `NanosecondTimestamp` | simulated time since the start of the run's simulated session | unsigned integer string |
| `NanosecondDuration` | latencies, markout horizons | unsigned integer string |
| `PriceDecimal`, `CurrencyAmount` | prices, cash, fees, P&L | exactly 4 fractional digits (engine fixed point 1/10000) |
| `MidPriceDecimal`, `MarkoutAmount` | mids, marks, markouts, spread capture | exactly 5 fractional digits, because a mid can fall on half a price unit |
| `DerivedDecimal` | means, standard deviations, ratios computed after the ledger is recorded | exactly 6 fractional digits, float64 then rounded half to even |
| `ExactDecimal` | a field shared by metrics of several units (`MetricValue.value`, comparison values) | scale implied by the accompanying `unit` and `exactness` |

Wall-clock times (`createdAt`, `observedAt`, connection times) are RFC 3339 strings. They never feed a simulation result.

**Client rule:** never pass an exact decimal string through a JavaScript `number` for arithmetic or display. In `web/`, exact values are formatted and compared with `src/lib/exact-decimal-format.ts`, which uses BigInt fixed point. Floats appear only in `src/lib/chart-derived-floats.ts`, which turns strings into plot positions. Chart tooltips show the original strings. An ESLint rule (`no-restricted-syntax` in `web/eslint.config.mjs`) rejects `Number()` and `parseFloat` everywhere else in `web/src`.

Metric values carry an `exactness` of `exact_fixed_point` or `derived_float`. The UI labels derived values, and they never re-enter accounting.

### Accounting and marks

- `cash = initialCash - sum(buy price x qty) + sum(sell price x qty) - sum(fees)`. Fees are `feePerShare x qty`. A negative fee is a maker rebate.
- `equity = cash + position x mark`, and `netPnl = equity - initialCash = grossRealizedPnl + unrealizedPnl - netFees`. The realized/unrealized split uses FIFO cost basis, matching the C++ position ledger and mock artifacts.
- **Mark convention `exchange_mid`:** the mark is the last two-sided exchange mid at or before the mark time. A one-sided book does not clear the mark. The mark, and anything derived from it (equity, unrealized P&L, markouts), is `null` with an `unavailableReason` only when no two-sided book has existed yet. A missing mark never becomes zero. The raw `exchange_mid` series is not carried forward: it reports `null` while the book is one-sided.
- **Markouts:** a buy is `mid(t + h) - price` and a sell is `price - mid(t + h)`, both per share and times quantity for totals. A markout whose horizon passes the observable end (the run end, or the simulation time of the last processed event for a partial summary) has status `censored_past_run_end`. It is excluded from totals and counted in `censoredFillCount`. It is never reported as zero.
- `spreadCaptureTotal` is the same formula with the mid at the fill time.

### Identifiers

Experiment and run identifiers are opaque strings (`exp_...`, `run_...`); order ids and client order ids are unsigned integer strings, as the exchange assigns them. Clients must not parse any of them. A run is one execution of one experiment for one master seed. Seeds are decimal strings; derived per-agent seeds are listed in the manifest.

### Pagination

List endpoints (`/v1/experiments`, `/v1/runs`, `/events`, `/orders`, `/fills`) take `limit` (1 to 1000, default 100) and `cursor`. The response has `{ items, page: { nextCursor, limit } }`. `nextCursor` is opaque and is `null` at the end. Every other filter must be repeated unchanged with a cursor; a changed filter returns `invalid_request`. Event pages are ordered by event position, so a cursor stays valid while a run is still appending events.

### Idempotency

Every mutating POST (`/v1/experiments`, `/v1/runs`, and the `cancel`, `pause`, `resume` and `step` run controls) requires an `Idempotency-Key` header: 8 to 128 characters of `[A-Za-z0-9_-]`. The service keeps keys for at least 24 hours.

- Same key, identical body: the original response is replayed, with `Idempotent-Replayed: true`.
- Same key, different body: `409 idempotency_key_reused_with_different_request`.
- Missing key: `400 idempotency_key_required`.

`POST /v1/experiment-previews` creates nothing and needs no key. The UI derives the experiment key from the reviewed preview and the run keys as `<experiment key>-seed-<seed>`, so a retried "create and start" never duplicates runs.

### Errors

Every error body is `{ "error": ApiError }` with `code`, `message` (not stable), `requestId`, `retryable`, `fieldIssues` (JSON-pointer `fieldPath` plus `issue`) and, for run-state errors, `currentRunState`. Clients should match on `code`.

| `code` | HTTP | When |
| --- | --- | --- |
| `invalid_request` | 400 | malformed JSON, unknown query value, bad cursor, `step` count out of range |
| `validation_failed` | 422 | the request is well formed but the configuration is invalid; see `fieldIssues` |
| `not_found` | 404 | unknown experiment, run, order or baseline |
| `idempotency_key_required` | 400 | a mutating POST arrived without `Idempotency-Key` |
| `idempotency_key_reused_with_different_request` | 409 | same key with a different body |
| `invalid_state_transition` | 409 | the control is not in the run's `allowedActions` (for example, step while running or cancel after completion) |
| `action_not_supported_for_mode` | 409 | a deterministic-only control was sent in real-time protocol mode |
| `artifact_not_ready` | 409 | asking for a book or artifact past the processed event position |
| `unsupported_market_world` | 422 | a market world that v1 cannot execute (only `interactive_synthetic` runs) |
| `unsupported_schema_version` | 400 | the request's `Exsim-Schema-Version` header names another schema family |
| `comparison_not_paired` | 409 | baseline and candidate are the same experiment |
| `internal_error` | 500 | service fault; diagnostics are in the service log under `requestId` |
| `service_unavailable` | 503 | the service is starting or stopping, or the UI proxy cannot reach it; `retryable: true` |

A comparison of two experiments with no shared completed seed, or with different market worlds, is not an error. It returns 200 with `comparable: false` and `incomparabilityReasons`, and the UI must not present it as a paired result.

### Run states and controls

| State | Allowed actions |
| --- | --- |
| `queued` | `cancel` |
| `running` | `pause`, `cancel` |
| `paused` | `resume`, `step`, `cancel` |
| `completed`, `cancelled`, `failed` | none |

`GET /v1/runs/{runId}` returns `allowedActions`, so clients do not need to hard-code this table. `step` advances a paused run by at least `canonicalEventCount` canonical events and leaves it paused. It processes whole scheduler events, so the count can overshoot when a scheduler event emits several canonical events. Pause takes effect at the next scheduled event boundary. A run created with `startPaused: true` stays paused at event 0. A UI disconnect never changes a run. Summaries are `availability: partial` until the run is terminal. Cancelled and failed runs keep their residual positions and `terminalDiagnostics`.

### Polling cadence

v1 uses polling only; there is no stream. These are the UI defaults, and other clients should stay at or below these rates:

| Resource | Interval |
| --- | --- |
| an active run (`queued`, `running`, `paused`) on the run and inspect pages | 1 s; stop once terminal |
| runs on an experiment page | 1 s while any run is active, 10 s when idle |
| the experiment list | 3 s |
| `/v1/connections` | 2 s |
| a terminal run's artifacts | once; they are immutable |

Series are downsampled by the service (`maxPoints`, using the last value in each bucket). The audit artifacts (events, orders, fills) are complete and paginated.

### Versioning

- **URL major version:** `/v1`. Breaking changes get `/v2`. Additive changes (new optional fields, endpoints or enum values) stay in v1, and clients must tolerate unknown enum values by displaying them verbatim.
- **Response header:** every response carries `Exsim-Schema-Version: exsim.api/1`. A client may send the same header to pin the family; a mismatch fails with `unsupported_schema_version` before any work is done. The UI client always sends it.
- **Artifact versions:** each artifact names its own encoding, so persisted artifacts stay readable when the HTTP family moves:

| Artifact | `schemaVersion` | Where |
| --- | --- | --- |
| run manifest | `exsim.run-manifest/1` | `GET /runs/{runId}/manifest` |
| run summary | `exsim.run-summary/1` | `GET /runs/{runId}/summary` |
| comparison | `exsim.comparison/1` | `GET /comparisons` |
| canonical event log | `exsim.canonical-event/1` | `manifest.artifactEncodings.canonicalEventEncoding` |
| order lifecycle log | `exsim.order-lifecycle/1` | `manifest.artifactEncodings.lifecycleLogEncoding` |
| ledger | `exsim.ledger/1` | `manifest.artifactEncodings.ledgerEncoding` |
| downsampled series | `exsim.series/1` | `manifest.artifactEncodings.seriesEncoding` |

Run digests (`run.digests`) are `fnv1a_64` over the ordered canonical encoding, as in docs/SCENARIOS.md, and are null until the run is terminal. Configuration and input-data digests are `sha256`.

## Endpoints and the questions they answer

The question numbers refer to the "Questions that drive the first version" table in [PRODUCT.md](PRODUCT.md):

- Q1: Does my market maker earn the spread after costs?
- Q2: Why did that quote fill, or miss a fill?
- Q3: How sensitive is the strategy to delay?
- Q4: How should I execute a parent order?
- Q5: What happens when the market becomes hostile?
- Q6: Can I trust a change in performance?

| Endpoint | Purpose | Questions |
| --- | --- | --- |
| `GET /v1/service` | service identity, build ids, supported modes, market worlds and strategies | Q6 (build provenance) |
| `GET /v1/baselines` | built-in baselines (fixed spread at 2 and 4 ticks) to derive from | Q1, Q6 |
| `POST /v1/experiment-previews` | validate a configuration and show its assumptions and estimated work before anything starts | Q1, Q3, Q6 |
| `GET /v1/experiments` | saved experiments with run-state counts | Q6 |
| `POST /v1/experiments` | save a reusable configuration linked to its baseline and parent | Q1, Q3, Q6 |
| `GET /v1/experiments/{experimentId}` | one experiment, its configuration and digest | Q6 |
| `GET /v1/runs` | runs filtered by experiment, state or seed | Q6 |
| `POST /v1/runs` | queue one run for one master seed | Q1, Q3, Q6 |
| `GET /v1/runs/{runId}` | state, progress, state history, allowed actions, digests, terminal diagnostics | Q5, Q6 |
| `POST /v1/runs/{runId}/cancel` | cancel; residual position and diagnostics are kept | Q5 |
| `POST /v1/runs/{runId}/pause` | pause at the next event boundary | Q2 |
| `POST /v1/runs/{runId}/resume` | resume a paused run | Q2 |
| `POST /v1/runs/{runId}/step` | step a paused run by N canonical events | Q2 |
| `GET /v1/runs/{runId}/manifest` | immutable manifest: seeds, latency, queue, fee, risk and mark models, encodings | Q3, Q6 |
| `GET /v1/runs/{runId}/summary` | net P&L, fees and rebates, inventory, drawdown, spread capture, markouts, fill rates, latency, limit breaches | Q1, Q3, Q5 |
| `GET /v1/runs/{runId}/series` | downsampled equity, inventory, drawdown, exchange mid and quoted spread | Q1, Q5 |
| `GET /v1/runs/{runId}/events` | canonical event log with stage, actor, and whether the strategy saw it | Q2, Q5, Q6 |
| `GET /v1/runs/{runId}/orders` | own orders with final status and fill totals | Q2 |
| `GET /v1/runs/{runId}/orders/{orderId}/lifecycle` | submit, arrival, ack, replace and cancel timestamps, latency breakdown, FIFO-ahead evidence, cancel-race outcome | Q2, Q3 |
| `GET /v1/runs/{runId}/fills` | fills with maker/taker, ledger effect, spread capture and markouts per horizon | Q1, Q2 |
| `GET /v1/runs/{runId}/book` | book at an event position, as exchange truth or strategy-visible | Q2, Q3 |
| `GET /v1/comparisons` | paired seeds, assumption differences, per-seed metric differences with dispersion, aligned series | Q1, Q3, Q6 |
| `GET /v1/connections` | read-only OUCH/FIX sessions, ITCH feed gaps and recovery, fault scenarios | Q5 (real-time mode) |

The UI pages map onto these endpoints:

- **Experiments** (`/experiments`, `/experiments/new`, `/experiments/{id}`): baselines, previews, experiments and runs.
- **Run** (`/runs/{id}`): run, controls, summary, series and manifest.
- **Inspect** (`/runs/{id}/inspect`): events, book, orders, lifecycle and fills.
- **Compare** (`/compare`): comparisons.
- **Connections** (`/connections`): connections.

### Coverage gaps (deferred)

- **Q4 (parent-order execution):** `twap_execution` is in the strategy enum, but `/v1/service` lists `fixed_spread_market_maker` and `inventory_aware_market_maker` in `executableStrategyKinds`, and other strategies are rejected with `validation_failed`. The first vertical slice in PRODUCT.md is market making only. Parent-order metrics (implementation shortfall, signed slippage, completion, residual quantity) and their `MetricKey` values will be added once the runner executes TWAP. The UI does not offer TWAP yet.
- **Q5 (hostile markets):** price jumps, halts and stale or gapped data inside deterministic runs need scenario events that the runner has not defined yet. v1 exposes limit breaches, terminal state and real-time feed gaps only.
- **Historical worlds:** `historical_reconstruction` and `historical_counterfactual_replay` appear in `MarketWorld` so that manifests and comparisons can label them. They return `unsupported_market_world` until a fill and queue model exists.
- **Streaming:** this version uses polling. A browser-compatible stream from the long-lived control service is deferred until interactive monitoring needs it.
- **Comparison drill-down:** the compare page links each per-seed difference to the paired runs and their inspect views, but no endpoint yet finds the event range where two runs diverge.

## Alignment with the deterministic research runner

The service links the runner library directly. One executor thread advances independent
`Simulation<Strategy>` instances in round-robin slices, with a mutex per run and at
most eight live simulations by default (including paused runs). This keeps event-boundary
controls synchronous without subprocess command/control IPC. HTTP work uses separate
bounded connection threads. Simulation and accounting remain entirely in C++.

`docs/RESEARCH_RUNNER.md` describes the CLI disk formats, `exsim.research.*/1`.
Those files are not HTTP response bodies. The API adapter performs these mappings:

| Area | HTTP mapping |
| --- | --- |
| Config | Validate JSON shape, translate to the runner's configuration text and apply runner validation. Support interactive synthetic fixed-spread and inventory-aware market makers. TWAP and historical worlds remain rejected. |
| Strategy | Expose `halfSpreadTicks`, `quoteQuantity`, `requoteIntervalNs`, `requoteThresholdTicks`, `postOnly`, and the inventory skew/unit/size controls directly. The earlier mock-only quote width fields are replaced. |
| Latency | Jitter is an added delay in `[0, jitterNs]` on every channel, rather than symmetric jitter. Optional `strategyComputeNs` and `orderChannelFifo` map to the runner's compute and order-channel settings. |
| Seeds | Preserve runner stream labels (`strategy`, `latency`, `agent.<index>.<kind>.<name>`) and values. `DerivedSeed.streamName` is a string, allowing multiple agents of each kind. |
| Digests | SHA-256 of stable serialized JSON configuration and HTTP manifest. Run event/ledger digests are the runner's FNV-1a64 canonical engine-event and ledger encodings, not a hash of the richer HTTP event rows. |
| Artifacts | Runner writer stores its own files under `runs/<runId>/runner/`, including its journal and replay verification. The adapter stores HTTP manifest/summary, run state/history, and versioned local binary indexes in the enclosing directory. Binary indexes require the same record layout and are not a portable export format. |
| Events | Replay runner commands through a shadow engine to recover exchange events, then attach lifecycle, request, feed and execution-report deliveries. Index canonical positions for pagination and inspection. Record replace and rested/rejected report stages explicitly. |
| Orders | Group strategy requests and fills by client order id across replacements. Lifecycle and fill queries accept every exchange id assigned to that order. |
| Books | Exchange truth is replayed from event zero with all levels and counts. Strategy-visible book is only the last delivered top of book, with unknown order/own-share counts null. It does not expose future exchange state. |
| Accounting | Use runner ledger, FIFO, carried mark and metric records. Missing mark yields null HTTP equity even when flat. Spread capture uses the mid immediately before the fill-producing command. Exact values have fixed 4/5 digit scales; derived statistics have 6 digits. |
| Markouts | `measured`, `unavailable`, `censored` become `available`, `unavailable_no_mark`, `censored_past_run_end`. `strategy_visible_mid` marking is rejected. |
| Series | Downsample with the last sample per bucket; inventory and equity come from samples, raw exchange mid from bid/ask, quoted spread from visible bid/ask, and drawdown from the running equity peak. |
| Comparison | Pair completed runs by master seed, compute exact differences before derived dispersion, and align sample series. Different worlds or missing shared completed seeds return noncomparable results. |
| Terminal states | Time/empty queue complete; cancellation cancels. Event/wall limits, recorder backpressure, queue/engine capacity, ledger failures, replay mismatch and other failures have explicit outcome codes in OpenAPI. |
| Connections | Return `not_running` with no sessions. This research process does not monitor the separate real-time server. |

Pause occurs between scheduler steps. Step processes whole scheduler events until the
requested canonical count is met, and may complete the run. The service applies its
canonical-event and active-wall-time bounds in addition to runner bounds. Active runs
are failed on service shutdown; completed artifacts remain queryable after restart.
An abrupt process loss cannot reconstruct the in-memory partial inspection indexes.

HTTP limits: 32 connections, 1000 requests per connection, 8 KiB request line,
16 KiB/64 headers, 1 MiB body, 64 JSON nesting levels, and 10 second socket
read/write timeouts. Excess idempotency keys (100,000 retained by default) reject
new mutations with `503 service_unavailable` before execution; existing keys replay.
The data directory is for one service process. There is no multi-process writer
coordination or remote authentication.

The runner QA branch at `ea8642b` adds stronger artifact verification, work budgets,
and pre-command FIFO evidence without changing the public record layout consumed
here. Its current differences table was read in the QA owner's checkout. Recheck
API tests when those fixes are integrated; do not copy disk JSON into HTTP bodies.
