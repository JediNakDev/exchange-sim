# Parent-order execution API integration contract

Date: 2026-11-07. Dependent base: `exchange-simulator` at `6d5c29f`.
Checkout: `/Users/jedi/.t3/worktrees/lob/t3-598ca8e2`.

This implementation exposes the existing C++ `TwapExecutor` and
`ImmediateSweepExecutor` through the research HTTP service. It changes no runner,
engine, persistence, idempotency, reload implementation, service header, frontend
component, client, proxy, package manifest or lockfile. The parent must integrate
this commit with the durability work and implement the frontend before claiming
an integrated browser execution workflow. This note and OpenAPI supersede the
TWAP rejection/deferred-Q4 statements in the older API documentation.

## Discovery and configuration

`GET /v1/service.executableStrategyKinds` now contains the two existing market
makers plus `twap_execution` and `immediate_sweep_execution`. Continue intersecting
known frontend forms with this advertised list. `GET /v1/baselines` preserves the
first two entries and adds these runnable configurations:

| Baseline ID | Strategy kind | Native strategy/build identity |
| --- | --- | --- |
| `twap-execution` | `twap_execution` | `twap_executor/1` |
| `immediate-sweep-execution` | `immediate_sweep_execution` | `immediate_sweep_executor/1` |

Both execution baselines use the existing MM/noise-taker scenario, seeds 1..5,
60-second run, buy parent of 500 shares, start at 1 second, execution duration
30 seconds, five-tick price protection, position cap 500 and child risk cap 200.
The fees, latency components, instrument and background models are identical.
TWAP uses one-second intervals. Change side/quantity/schedule explicitly for a
paired study; do not change the scenario or fees accidentally. Agent RNG stream
seeds are identical across strategies; interaction with the book can change the
realized market path. These are synthetic baselines, not market-impact estimates.

Existing `ExperimentCreateRequest`, preview/create, seed and control shapes stay
the same. The strategy variants are:

```json
{
  "strategyKind": "twap_execution",
  "parentSide": "buy",
  "parentQuantity": "500",
  "sliceIntervalNs": "1000000000",
  "sliceOrderType": "limit",
  "startTimeNs": "1000000000",
  "executionDurationNs": "30000000000",
  "limitOffsetTicks": "5"
}
```

```json
{
  "strategyKind": "immediate_sweep_execution",
  "parentSide": "buy",
  "parentQuantity": "500",
  "startTimeNs": "1000000000",
  "executionDurationNs": "30000000000",
  "limitOffsetTicks": "5"
}
```

Common required fields are `strategyKind`, `parentSide` (`buy` or `sell`) and
`parentQuantity` (positive uint32 shares on the lot grid). TWAP retains its existing
required `sliceIntervalNs` and `sliceOrderType`. New common optional fields map
one-to-one to native runner configuration:

| HTTP field | Native key | Default when absent | Bounds/behavior |
| --- | --- | --- | --- |
| `startTimeNs` | `start` | `1000000000` ns | Less than run duration |
| `executionDurationNs` | `execution_duration` | `30000000000` ns | At most 7 days; zero is allowed for the sweep's initial instant; TWAP requires at least one interval |
| `limitOffsetTicks` | `limit_offset_ticks` | `5` ticks | 0..100000 |
| `sliceIntervalNs` (TWAP only) | `slice_interval` | Required in HTTP | Positive; floor(duration/interval) in 1..1000000 |

Parent quantity may exceed the child cap or position headroom. This is allowed,
and may leave a residual. There is no duplicate child-size parameter:
`risk.maxOrderQuantity` is the native child cap. Both executors round child sizes
down to lots and limit them to remaining parent quantity and position headroom.
The engine still validates instrument order limits. Set the child risk cap at
or below `instrument.maxOrderQuantity` to avoid engine rejections.

**Child semantics:** both executors submit IOC **limit** orders using the
strategy's delivered opposite touch plus `limitOffsetTicks` for buys, minus it
for sells, clamped to the instrument price band. They are not unprotected market
orders. The existing `sliceOrderType` schema still accepts the enum spelling
`market` for compatibility, but semantic validation rejects it at
`/config/strategy/sliceOrderType`. Preview returns 200 with `valid: false` and
`fieldIssues`; create returns 422 `validation_failed`. Unknown fields, malformed
types, and runner range/grid/cross-field validation remain enforced. Runner
cross-field errors can point at `/config`, as with existing strategy validation.

TWAP fires `floor(duration/interval)` slices at `start + k*interval`. Cumulative
integer targets are `parentQuantity*(k+1)/sliceCount`. A slice sends at most one
child for target minus known fills. An in-flight child or missing visible
opposite quote skips that attempt; later slices can catch up. There is no retry
after the final scheduled slice. A duration not divisible by the interval has
no additional final slice. Sweep sends as much remaining quantity as its caps
allow, then retries on the next delivered market update after each child finishes,
through `start + duration` inclusive. It does not re-sweep from a fill callback.
These are the unchanged runner algorithms.

## Results and exact units

`RunSummary` adds optional `execution: ExecutionSummary | null`. Optionality is
intentional: old persisted summaries remain valid and may omit it. Fresh market
maker summaries emit null. Fresh execution summaries include:

- Configured `parentSide`, `parentQuantity`, and `parentDeclared`.
- `observationStatus`: `ongoing` until the configured window has been covered,
  `window_observed` when as-of simulated time reaches start plus duration, or
  `censored_run_end` when a terminal run ends earlier. This is window coverage,
  not a promise of full completion. Even a fully filled parent can have a
  truncated observation window. Partial summaries retain `availability: partial`.
- `benchmark: reference_exchange_mid_at_decision`, nullable exact `benchmarkMid`
  and `visibleMidAtDecision`, and nullable declaration/first-fill/last-fill times.
- Nullable `filledQuantity`, exact `fillNotional` and `netFees`, and exact
  `participationMarketQuantity`. Before declaration these values are unavailable;
  after declaration zero fills/notional/fees and zero participation denominator
  are real observed zeros.
- `unexecutedOpportunityCost: null`: the runner defines no residual opportunity
  cost or full-parent counterfactual implementation shortfall.

The benchmark is the last two-sided **exchange-truth** mid at parent declaration,
carried across one-sided periods. It is independent of the account marking
convention and may differ from the delayed visible mid. Native artifacts call
this same benchmark `arrival_mid`; it is not a separate benchmark at each child's
exchange arrival. There is no second independently observed child-arrival
slippage metric. Do not label it as strategy-visible slippage or full-parent cost.

Seven `MetricKey` additions appear in fresh summaries and paired comparisons:

| Key | Value | Unit/exactness | Better direction |
| --- | --- | --- | --- |
| `completion_ratio` | parent filled / parent quantity | ratio, derived six decimals | higher |
| `residual_qty` | max(parent quantity - parent filled, 0) | exact integer shares | lower |
| `decision_slippage_total` | s * (fill notional - decision mid * filled quantity) | exact currency, five decimals | lower |
| `decision_slippage_per_share` | decision slippage total / filled quantity | currency_per_share, derived six decimals | lower |
| `implementation_shortfall_total` | decision slippage total + signed net fees | exact currency, five decimals | lower |
| `implementation_shortfall_per_share` | shortfall total / filled quantity | currency_per_share, derived six decimals | lower |
| `participation_rate` | parent filled / participation market quantity | ratio, derived six decimals | neither |

Here `s=+1` for buys and `-1` for sells. Positive costs mean worse execution;
negative costs mean price improvement or a rebate benefit. Costs cover only
executed quantity. Completion differs from existing `fill_rate`, whose denominator
is exchange-accepted child shares, including later-cancelled IOC remainders.
Participation denominator is total market volume after parent declaration in event order
through its last fill, including own trades, exactly as defined by the runner.
It does not extend to the run end or use only background volume.

Accounting remains int64 units of 1/10000 currency. Benchmark mids and total costs
retain doubled units (1/20000 currency) without intermediate rounding. HTTP mids
and doubled totals use five fractional digits, not the four-digit `CurrencyAmount`
format. Fill notional and fees use four digits. `exact_fixed_point` costs are
subtracted exactly before comparison dispersion; ratios and per-share means are
`derived_float`, rounded to six digits by the existing adapter formatter. Clients
must retain decimal strings and use the existing exact formatters.

Each metric carries its existing nullable `value`/`unavailableReason` contract.
For these seven keys, reasons are:

| Reason | When |
| --- | --- |
| `not_a_parent_order_strategy` | Market-making run; all seven values null |
| `parent_order_not_declared` | Before the parent's scheduled declaration; all seven values null |
| `no_parent_fills` | Costs/per-share means/participation before any fill; completion 0 and residual full quantity remain observed |
| `no_reference_exchange_mid_at_decision` | Costs unavailable although fills exist; missing benchmark is not replaced with zero or the account mark |
| `no_market_volume_in_execution_window` | Participation denominator zero, if fills exist |

A completed run can leave a parent residual because of liquidity, lot rounding,
child/report delays, risk caps or TWAP's last-slice policy. Cancellation keeps
observed parent metrics and censors a truncated window. Do not turn null/censored
values into zero. Comparisons include these keys even for market makers, with
zero available samples when either paired value is missing.

## Identity, evidence and parent frontend work

Both new driver cases use the runner's existing `StrategyIdentity` and artifact
writer. API run IDs remain independent from canonical native/CLI IDs. Manifest
`strategyBuildId` is `twap_executor/1` or `immediate_sweep_executor/1`, never
`unexecutable`. Comparison still re-executes the expected API configuration and
seed, verifies native configuration/seed/canonical ID, requires replay verification
and byte-for-byte deterministic artifact reproduction, then reads metrics and
samples from the fresh core. No comparison evidence check was weakened.

The parent frontend should:

1. Offer only known forms in `executableStrategyKinds`, with both discovered
   execution baselines. Do not assume exactly two baselines or market makers only.
2. Edit side, quantity, common schedule/price protection, TWAP interval and shared
   risk cap. The executable TWAP child type is limit. Show IOC/visible-touch price
   protection clearly; never silently translate a market choice.
3. Show parent completion/residual, signed executed costs/fees, benchmark and
   participation denominator, with window status and null reasons. A run's
   `completed` state is distinct from complete parent execution.
4. Update exhaustive strategy/metric label maps for the expanded unions, include
   the new optional structured summary, and tolerate older summaries without it.
5. Extend mock/real-binary fixtures for both strategies, both sides, paused-before-
   declaration, zero fills/no benchmark, partial fills/residual, censored windows,
   cancellation, exact half-unit costs and reload/comparison. Existing MM fixtures
   can remain valid with no `execution` property; fresh MM summaries contain seven
   new unavailable metrics. Review tests asserting exact catalogue/metric counts.

`docs/api/openapi.yaml` adds `ImmediateSweepExecutionConfig`,
`ExecutionSummary`, the strategy discriminator mapping and seven metric keys;
TWAP adds three optional parameters without removing existing fields. The schema
family remains `exsim.api/1`, summary version `exsim.run-summary/1`.
`web/src/lib/api/generated-schema.d.ts` is regenerated with openapi-typescript
7.13.0 from that document. No hand-written frontend types or other frontend
files were edited. Older native artifacts, config encodings and existing strategy
identities are unchanged.

## Focused verification and limits

Verification is serial with compile parallelism at most `-j2`. A focused test
binary links library objects, `tests/main.cpp`, the new execution tests and the
three existing API test files. It does not build or run the full test suite.
Tests feed raw HTTP through the parser/router and manually advance the service's
executor in private temporary directories; existing focused HTTP tests exercise
loopback transport. This is not a browser or deployed-service check.

| Gate | Result and wall time | Evidence |
| --- | --- | --- |
| Focused release API binary | 47/47, 2.94 s (11 new execution cases plus 36 existing API cases) | `/tmp/exsim-execution-focused.log` |
| Focused ASan/UBSan API binary | 47/47, 5.18 s; no sanitizer diagnostics | `/tmp/exsim-execution-sanitizers.log` |
| Debug object build, `-j2 -Werror` | Passed, 17.11 s | `/tmp/exsim-execution-debug-build.log` |
| Release compile/relink work | Passed, incremental; final up-to-date gate 0.08 s | `/tmp/exsim-execution-build.log`, `/tmp/exsim-execution-release-build-final.log` |
| OpenAPI response validation | 181/181 captured responses, 13 operation/status branches | `/tmp/exsim-execution-schema.log`, `/tmp/exsim-execution-responses.jsonl` |
| Generated schema types | Regenerated with 7.13.0; second output byte-identical | `/tmp/exsim-execution-types.log` |
| Pre-fix rejection regression | Old translation rejects the new valid config; 0/1 as expected | `/tmp/exsim-execution-before/experiment_config_translation-test.log` |
| Pre-fix metric regression | Old view lacks completion metric; 0/1 as expected | `/tmp/exsim-execution-before/research_run_views-test.log` |
| Scoped whitespace check | `git diff --check` passed | Local git check |

The before variants compile each unmodified `6d5c29f` implementation file into a
private temporary object and substitute it into the focused binary; the working
source is never reverted. Expected before failures are not failures of the final
implementation. Early fixture assertions were corrected for the documented
preview-200/invalid response, queued-to-paused activation, and legitimate TWAP
residual behavior.

The new cases cover discovery; market-child rejection; defaults and parameter
mapping; invalid parent/schedule bounds; buy/sell TWAP/sweep preview/create,
paused step/resume and same-seed event identity; truncated windows and residuals;
zero liquidity; two-seed comparison and service reload with native reproduction;
intact wrong-seed/wrong-config/wrong-strategy native substitution for both kinds;
cancellation; and a `100.00005` benchmark retaining its half-price-unit cost.
Summary costs, completion and participation reconcile with independently written
native summary values, and native reproduction is checked on both strategies.
Corrupt native ledger data is excluded from pairing after reload.

The focused link helper and schema validator are temporary validation tools at
`/tmp/exsim-execution-link.py` and `/tmp/exsim-execution-schema-check.ts`. The
validator uses existing Bun/Ajv dependencies from a prior isolated checkout; no
dependencies were installed or frontend files changed. The reusable C++ tests
support opt-in response capture with `EXSIM_EXECUTION_API_RESPONSES=<jsonl path>`.

Parent owns final combined frontend/backend serial gates, Linux checks, an
independent review, browser validation and integration with durable idempotency.
No full suite, benchmark, Docker action, production service action, delegate,
push, PR or merge was performed.
