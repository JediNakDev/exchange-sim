# Trader research platform decisions

Date: 2026-11-02. First workflow selected by the user: execution and market-making research. This document defines the product direction; it does not claim these features are implemented.

## Product goal

A trader should be able to configure a market experiment, run a strategy, compare the result against a baseline, and explain each fill, missed fill, cancellation race, and inventory change. Preserve the protocol-fidelity and performance goals of the exchange simulator while adding the experiment workflow around it.

Initial scope is a local, single-venue, cash-equity-style research environment matching the current FIFO book and OUCH/FIX/ITCH work. Other asset classes require explicit contract multipliers, sessions, fees, and matching rules before support is claimed.

## Questions that drive the first version

| Trader question | Required experiment | Required evidence |
| --- | --- | --- |
| Does my market maker earn the spread after costs? | Fixed-spread baseline, then inventory-aware quoting across several seeds | Net P&L, fees/rebates, inventory, drawdown, spread capture, signed markouts |
| Why did that quote fill, or miss a fill? | Replay the relevant event sequence | Submit, exchange-arrival, acknowledgment, replace/cancel, and fill timestamps; FIFO quantity ahead; matching rule and book state |
| How sensitive is the strategy to delay? | Paired runs changing feed, outbound order, acknowledgment, and cancel latency separately | Fill rate, adverse selection, cancel-race outcomes, and net P&L under the declared latency model |
| How should I execute a parent order? | Compare an immediate sweep with a TWAP baseline using the same market scenario | Completion, residual inventory, signed slippage, implementation shortfall, participation, fees |
| What happens when the market becomes hostile? | Thin liquidity, a price jump, a halt, or stale/gapped data | Position limits, stale-quote response, order recovery, and terminal order/account state |
| Can I trust a change in performance? | Repeat a baseline and candidate across a fixed seed set | Immutable manifests, identical-run hashes, paired differences, dispersion, and an inspectable event log |

TWAP and fixed/inventory-aware market making are transparent research baselines, not claims of profitable production strategies. Initially use built-in, compiled C++ strategies with explicit parameters. Arbitrary uploaded code, a visual strategy builder, and Python integration are later work.

## Architecture decision

Use the browser as the primary research interface, a CLI as the automation/debugging interface, and C++ as the source of truth. A rich full-screen terminal UI is deferred because it would duplicate charts, experiment comparison, and lifecycle inspection work needed in the browser.

```text
Next.js / TypeScript research UI        CLI / future research scripts
                  |                         |
       HTTP JSON control, manifests, results, event queries
                  |                         |
        C++ research API / run controller / artifact reader
                  |
        C++ simulation runner + accounting + strategy host
                  |
        pure MatchingEngine + commands/events + journal

External strategy <--> OUCH/FIX C++ server <--> same MatchingEngine
External feed consumer <--- ITCH/MoldUDP64 publisher
```

The pure engine reads no wall clock, performs no network I/O, and uses no randomness. The runner owns simulation time, scheduling, seeded agents, latency, risk policy, and accounting. Gateway state remains outside the engine.

The HTTP/control adapter must not execute request parsing, JSON encoding, or disk queries on the matching hot path. Use bounded command/observation handoffs; snapshots are read from immutable published state or completed artifacts. A full telemetry queue may discard/coalesce optional UI updates and increment an observable counter. A full authoritative journal queue must fail/pause the run explicitly rather than silently lose evidence. The offline deterministic runner may slow down to preserve complete output. No-allocation guarantees apply to the stated simulation/gateway hot paths, not to HTTP handlers or frontend rendering.

Each run owns independent engine, RNG, strategy, and accounting state. Process-per-run isolation is a straightforward initial boundary; do not introduce shared multi-run engine state.

### Frontend stack and API fit

Follow the user's global conventions: **Bun, Next.js, TypeScript, Tailwind CSS, shadcn/ui**. Reuse components before inventing new ones. Additional state/chart/testing libraries need a concrete requirement, not a default scaffold dependency.

**Recommendation: start without tRPC.** The user reopened this decision for a fit assessment. tRPC provides strong inference between TypeScript servers and clients, but cannot infer a native C++ server's schema. A Next.js tRPC wrapper would still need the C++ API contract. Use an OpenAPI-described HTTP API and generated TypeScript types/client at that boundary. Next.js route handlers may proxy a same-origin request or manage browser credentials when needed. Do not duplicate matching, accounting, or run-control rules in Next.js. Reconsider tRPC only if substantial TypeScript-owned application logic emerges.

Browser run control must use idempotency keys where retries could create duplicate runs or interventions. Never send raw shell commands, filesystem paths, or exchange admin strings directly from the browser for execution. Local-only service access is the initial deployment; remote multi-user hosting needs a separate access-control design.

Begin with polling run status and querying persisted/downsampled results. Add a browser-compatible stream from a long-lived control service when interactive monitoring needs it. Persistent connections should not depend on serverless request lifetimes. Exact prices, quantities, IDs, sequence numbers, and nanosecond timestamps travel as decimal strings where JavaScript numbers would lose precision. Define schema versions, units, pagination, request errors, and run-state transitions explicitly.

## Two execution modes, one engine

1. **Deterministic research mode:** an offline discrete-event runner advances simulated time to the next scheduled event. It supports pause/step, bounded runs, repeated seeds, and faster-than-real-time execution without sleeping or depending on a browser connection. Declare and test a stable ordering for equal-time events using an enqueue sequence. Feed delivery, strategy processing, outbound orders, exchange processing, and report delivery are distinct stages. A strategy sees only events delivered to it, never future exchange state. Use independently seeded streams for market agents and strategy randomness, recording the seed derivation.
2. **Real-time protocol mode:** the C++ reactor serves actual OUCH/FIX clients and publishes ITCH. Wall-clock scheduling, OS timing, and external strategy behavior affect arrival order. A captured command stream can reproduce engine outcomes, but a repeated live network session is not promised to be identical. This mode tests connectivity, recovery, protocol behavior, and real client integration.

Do not try to fast-forward an unconstrained external process by changing the server clock. Accelerated external strategies require an explicit synchronized virtual-time protocol, which is deferred.

The existing SERVER.md wall-clock loop describes real-time mode. PRODUCT.md adds a separate runner around the same engine rather than replacing exchange protocols with HTTP.

## Market worlds and realism

- **Interactive synthetic market, first:** seeded background liquidity and takers trade in the same book as the research strategy. Orders consume liquidity and affect the simulated book. Other agents respond only according to their specified models. This is a controlled synthetic experiment, not calibrated real-world market-impact prediction.
- **Historical reconstruction, existing foundation:** the ITCH tool reconstructs an observed historical book. This is useful validation/data infrastructure, not yet strategy backtesting.
- **Historical counterfactual replay, later:** a separate fill/queue model is required to place hypothetical orders beside an immutable recorded stream. Historical executions/cancellations refer to recorded order IDs and cannot simply be applied to a freely mutated interactive book. Declare data granularity, queue assumptions, latency, missing-data policy, and the small-order/no-feedback assumption. A price touch alone is insufficient evidence of a passive fill.

UI labels and result manifests must identify the market world. Do not blend synthetic and historical results into a comparison without exposing that difference.

Engine-truth queue state may be inspected after the run to explain an outcome. A live strategy or delayed trader view gets only its permitted, delivered information. Mark whether displayed queue position is exact simulated truth or an estimate from limited market data.

## Run artifacts and metrics

Every run gets an immutable manifest containing the run/schema version, engine and strategy build identifiers, configuration and data hashes, strategy parameters, market model, master/derived seeds, latency/queue models, fee schedule, risk limits, instrument rules, initial cash/positions, mark convention, simulation bounds, and event-order policy. Store a versioned canonical command/event encoding, ordered-field digest, lifecycle log, ledger, summary, and downsampled series. Never hash raw C++ object storage or serialize union padding.

Record run states: queued, running, paused, completed, cancelled, failed. Define successful completion and time limits. Preserve failure/cancellation diagnostics and residual positions. UI disconnects do not stop a run. Manual strategy orders or scenario interventions become recorded commands; changing assumptions creates a new run linked to its parent. Seeking initially means replay from the beginning to an event boundary; state snapshots and arbitrary rewind are later optimizations.

Minimum metrics:

- Cash, signed position, average cost basis, realized/unrealized P&L, net fees/rebates, and equity. Publish the exact accounting and marking convention; missing terminal marks remain unavailable rather than silently becoming zero. Attribute fills to the correct strategy/session and distinguish maker/taker where defined.
- Accepted/rejected/submitted orders, filled shares, partial fills, cancels/replaces, quote exposure time, time-to-fill, and precisely defined fill-rate denominators.
- Signed spread capture and markouts at configured horizons, using the declared exchange-mid or strategy-visible-mid series. Do not include horizons past run end as zero; mark them censored/unavailable.
- Execution shortfall/slippage against a declared decision/arrival benchmark, completion, and residual parent-order quantity.
- Inventory extremes and limit breaches, maximum drawdown, and separate feed/order/ack/cancel latency measurements.

Aggregate costs and cash in documented fixed-point units with checked arithmetic. Research summaries may derive floating-point statistics after the ledger is recorded. Test accounting identities and round-trip fee cases. Paired seed sweeps report distributions and sample counts; a single synthetic run is a demonstration, not evidence of predictive profitability.

## UI direction

The landing workflow is **Experiments -> Compare runs -> Inspect an outcome**. It should help answer why a strategy behaves as it does, with live monitoring as one supported view.

1. **Experiments:** create an experiment from a baseline; select market scenario, strategy, bounds, seed set, fees/risk, and the separate latency components. Show assumptions and estimated work before starting. Save a reusable configuration, with every executed run retaining its own immutable manifest.
2. **Compare:** aligned equity, inventory, drawdown, markout/fill distributions, and cost attribution for baseline versus candidate. Keep scenario/seed/fee/model differences visible. Link a divergence to its event range.
3. **Inspect:** simulation time and event position, strategy-visible and exchange-truth book views, own orders, fills, inventory, and a lifecycle timeline. Clicking a fill or missed-fill candidate focuses the related submit/arrival/cancel/market events. Step precisely by scheduled event/command boundary, with recorded timing and FIFO evidence.
4. **Connections/faults:** real-time sessions, feed gaps, recovery status, and reproducible fault scenarios. Keep engineer-only admin detail here rather than making it the trader's starting screen.

Use a dense desktop workspace with clear units, keyboard-friendly tables, and explicit run/mode/simulation-time state. Support compact screens by stacking views. Display and chart updates may be throttled/downsampled, but exported audit data remains complete. Avoid misleading animated live ticks during a paused or completed run.

## First useful vertical slice

One instrument, a fixed-spread research strategy, separately seeded market-maker/noise-taker background agents, configurable feed/order/cancel delays, explicit fees and inventory limits, and a bounded offline run. The trader launches it from the Next.js UI, compares two quote widths on the same seed set, then clicks a fill to inspect the order lifecycle, FIFO position, and inventory/P&L effect. CLI runs export the same artifacts headlessly.

Acceptance: identical manifests and deterministic inputs give identical canonical event/ledger hashes; UI connected/disconnected makes no difference; costs and inventory reconcile to fills; delayed strategies cannot observe future state; declared hot paths allocate zero after initialization; browser and CLI read the same results. Complete the foundational contract/review fixes and integrated server before treating partial branches as ready for this slice.

## References consulted for the design

- [HftBacktest order-fill assumptions](https://hftbacktest.readthedocs.io/en/py-v2.1.0/order_fill.html): historical replay cannot respond to hypothetical orders; queue/fill and impact assumptions need explicit treatment.
- [NautilusTrader backtest sequencing](https://nautilustrader.io/docs/latest/concepts/backtesting/execution-flow/): useful prior art for the causal ordering of market updates, callbacks, and delayed commands. Our ordering still needs its own explicit contract.
- [Next.js backend-for-frontend guide](https://nextjs.org/docs/app/guides/backend-for-frontend): HTTP route handlers and deployment constraints; Next.js remains an application/control interface.
- [tRPC introduction](https://trpc.io/docs) and [OpenAPI TypeScript goals](https://openapi-ts.dev/about): TypeScript inference is useful across TS-owned boundaries; schema-generated types fit our C++ boundary and other clients.

These sources support design constraints, not claims that the proposed platform already implements them.
