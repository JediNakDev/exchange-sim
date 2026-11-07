# Independent market-making quant persona review, 2026-11-08

## Verdict and boundaries

**Request fixes and a new independent recheck before human first-use validation.** No P0 accounting corruption was found. MM-01 and MM-02 are P1 gaps in explaining the current market-making task. Three further P2 findings concern interpretation. This is an expert review, not an unaided human usability pass or clearance of the whole simulator.

Reviewer checkout `/Users/jedi/.t3/worktrees/lob/t3-73e61748`, base `d61c329b2a8cb77aeb163fb4e8aaf0395a725aaa`. No delegates, builds, compilation, dependency installation, broad tests, server startup, production operations or production edits. Only review artifacts were written. Parent must record integration and commission a NEW independent recheck after fixes. Reviewer does not implement or self-clear findings.

Reviewed native service: frontend `http://127.0.0.1:45772`, API `http://127.0.0.1:45773/v1`; supplied BUILD_ID `ZbCMVWqStD4QzLA8AwzWD`, unchanged parent `build/release/exsim_api`. Build ID is supplied provenance, not independently cryptographically verified here. API `/service` reports four executable strategies and interactive synthetic market only. All new experiments use `MM-REVIEW`; no other persona's experiment/run was modified.

Prior knowledge before browser observation: task brief, user constraints, current-state WORK_PLAN, general market-making/FIFO/accounting knowledge. No UI code or README had been read. First browser action was `preview_status` (available false); the mandated `preview_open(reuseExistingTab:false)` succeeded and owned tab `tab_5_e40125e3-02a4-4ae8-98a3-dbbb53b4744f`. No Playwright fallback was necessary. Initial empty-library view and task entry were observed before reading PRODUCT/ARCHITECTURE, targeted SPEC/API/OpenAPI and source. Snapshot refs were preferred; snapshot trees truncate below the inspector event table, so observed DOM selectors were used for fill/order selection. One CSS selector call was rejected; direct DOM selection then succeeded, without changing simulation data. Own tab was closed at handoff.

## Real task and receipts

Browser: Experiments -> Research market making -> name `MM-REVIEW baseline` -> seeds `101,102,103` -> Review assumptions -> Create and start 3 runs -> Analyze a completed run -> Explain fills and queue events -> first fill. Default 60-second fixed spread, half spread 1 tick, 100 shares, 50 ms requote, 500-share risk cap, maker rebate 0.002 USD/share. Background: one 3-level maker and one noise taker, random-walk fair value, 20 ms mean taker interval. No calibration assumption.

Then created four native API variants, each copied from the baseline and changing one hypothesis. API use was intentional for exact controls after observing baseline UI; this does not claim browser variant-form coverage. Each has three completed seeds. Skew is a strategy treatment requiring three additional schema fields, with size scaling explicitly false so size scaling is not a second treatment.

| Treatment | Experiment | Only treatment versus baseline | Net P&L USD, seeds 101 / 102 / 103 |
| --- | --- | --- | --- |
| Baseline | `exp_cb36794a01e24837` | fixed spread | 1677.064 / 1794.820 / 1749.721 |
| Width | `exp_f339383a2e9cf78b` | half spread 1 -> 2 ticks | 1423.244 / 1457.598 / 1468.824 |
| Inventory skew | `exp_61ccdd391b89b2e5` | inventory-aware, 1 tick per 100 known shares, scaling false | -7711.579 / -2654.846 / -11970.416 |
| Feed delay | `exp_4c9c7a45e2c9af9c` | 50 microseconds -> 100 ms | 1214.557 / 1309.898 / 1335.664 |
| Cancel path | `exp_b55b38fbbbbacef1` | 80 microseconds -> 100 ms | 837.521 / 924.602 / 931.879 |

These are synthetic scenario outcomes, not recommendations for a trading strategy. Source-confirmed replace requests use the cancel path, and FIFO can delay subsequent new orders; the cancel treatment is not an independent measurement of cancel-only transport. Raw configs, run IDs, summaries and all four service comparisons are saved in [evidence](PERSONA_MM_EVIDENCE_2026-11-08/). `study.py` creates fresh variants; do not rerun it against an unapproved service. `collect.py` reads the recorded owned studies. `reconcile.py` is a focused read-only arithmetic probe, not a broad test suite.

The skew comparison was also inspected in browser at `/compare?baseline=exp_cb36794a01e24837&candidate=exp_61ccdd391b89b2e5`. It correctly shows three pairs, strategy assumptions, candidate-minus-baseline differences and worse net outcomes on every seed. [Comparison screenshot](PERSONA_MM_EVIDENCE_2026-11-08/skew-comparison.png).

## Findings

Severity convention: P0 research harm; P1 core task; P2 friction/interpretation; P3 polish. All findings below were observed on native data and traced to source. Consequences are reviewer inference, separately identified. No finding claims the engine lacks evidence merely because a UI omits it.

### MM-01, P1: replace failures disappear from the latency-study outcome summary

**Observed:** `/runs/run_e53e95c790dc7414` (cancel treatment, seed 103) reports submitted/accepted/rejected `607/604/3`, canceled `0`, cancel races lost `0`. Yet `/v1/runs/run_e53e95c790dc7414/events?eventKinds=order_rejected&limit=1000` contains **504 own replace/unknown_order rejections plus one own replace/post_only_would_cross rejection**. The other three own rejections are new orders; 65 additional new-order rejections belong to background agents and were excluded from this count.

**Repro/evidence:** Inspect -> Own orders -> first order 7. At #48 (0.063100 s), replace submitted; #69 (0.105074 s), old order fills; #104/#105 (0.163120 s), replacement arrives and is rejected unknown_order. The panel says `Cancel n/a`; no replacement-latency statistic or race outcome is provided. [Browser transcript](PERSONA_MM_EVIDENCE_2026-11-08/cancel-own-order-browser.txt), [screenshot](PERSONA_MM_EVIDENCE_2026-11-08/cancel-own-order.png), `cancel-103-rejections.json`, `cancel-103-summary.json`.

**Source-confirmed:** `simulation_core.cpp:1398` strategy replacement uses `schedule_order_channel(cancel_ns, cancel_jitter_ns)`; lines 697-712 count new, cancel and replace rejects separately. `research_run_views.cpp:417` exports only cancel rejects as cancelRaceLostCount; replacement counters are not included in summary metrics. Lifecycle latency at lines 719-720 and 820 only pairs cancel submission/arrival. Setup `experiment-create-view.tsx:501` describes Cancel as strategy cancel to exchange, omitting replace traffic.

**Trader consequence (inferred):** The treatment looks like slower cancels with no races, while it actually loses hundreds of quote-update races. The headline rejected count is valid for new orders but appears exhaustive without a new-order qualifier. A trader cannot explain the measured P&L/fill degradation without manually searching canonical events.

**Fix:** Label the configured path `Cancel / replace`; distinguish new-order rejects, cancel requests/outcomes and replace requests/outcomes, with request denominators and reject reasons. Add replace submit-to-arrival latency and link failed quote updates to their fills. Do not reclassify every rejection as a lost race: unknown-order after fill needs explicit causal evidence.

**Acceptance:** This exact study visibly shows 505 own rejected replacements, of which 504 are unknown_order, separate from 3 rejected new orders and 0 rejected cancels. Order 7 explains its fill-before-replace race and approximately 100.020 ms replace transit. Summary and comparison expose the same definitions.

### MM-02, P1: FIFO evidence omits the queue reset at replacement

**Observed:** Baseline seed103 `/runs/run_7edbf747de8f7c91/inspect`, first fill match4/event45/order7. Timeline includes #50 priority-losing replacement 7 -> 12 and #51 order12 resting. FIFO table contains #24 initial rest, #36/#44 advances, #45 and #81 immediately-before-fill, but **no #50/#51 reset/rest evidence**. The displayed chain can traverse a priority loss without showing how much priority was lost.

**Evidence:** `baseline-103-orders-7-lifecycle.json`, `baseline-fill-browser.txt`, [screenshot](PERSONA_MM_EVIDENCE_2026-11-08/baseline-fill.png). This example has zero shares ahead immediately before both fills; the missing reset still prevents auditing what happened between them. Do not infer a nonzero queue loss from this example.

**Source-confirmed:** `research_run_views.cpp:758-806` finds only the first OrderRested and writes `rested` evidence only when `position == rested_position`. It does not emit evidence for later re-rests or increases in shares ahead. SPEC lines 211-223 require price changes/size increases to lose priority under the documented conditions. Canonical events preserve the replacement; this is a lifecycle projection gap, not evidence of FIFO engine failure.

**Trader consequence (inferred):** Replenishing a partially filled quote can lose priority. The missing reset obstructs the central question of why a quote filled later or missed an intervening trade.

**Fix:** Emit queue evidence at every replacement/rest transition, including old/new exchange IDs, price, priority kept/lost, shares/orders ahead and level total. Show quantity ahead at the replacement's new price, not just the chain's original resting price. Explicitly mark evidence truncation if capped.

**Acceptance:** First chain includes #51 post-replace queue position. A focused fixture with nonzero shares ahead after a priority-losing replace must show the reset and subsequent queue advances. A size reduction retaining priority must say so. This is current-workflow evidence, not a request for a new counterfactual fill model.

### MM-03, P2: selected fill identity is conflated with the final replacement chain

**Observed:** Same baseline fill selects event45/order7/buy61, but explanation heading reads `Order 12: buy 161 @ 99.9900`, `filled, 161 filled in 2 fills`. The fill subpanel correctly shows match4/buy61. Own orders likewise puts latest ID12 and final total161 alongside original submit #20, when order7 was submitted for100. These are different times and identities without a chain label.

**Source-confirmed:** Lifecycle calls resolve an alias to OwnOrderRecord and `write_order_summary` emits the latest chain state; `run-inspect-view.tsx:215-218` labels it simply `Order`. This is not the historical selected-fill cache bug: the selected ledger and markouts matched event45 correctly.

**Trader consequence (inferred):** The reader may conclude 161 shares were resting at fill time or investigate order12 instead of order7, especially when prices also change.

**Fix:** Keep `Selected fill: order7, event45` explicit; label the separate section `Client order chain1: 7 -> 12`, with original/current quantities and as-of time. Include IDs on lifecycle transitions and FIFO rows.

**Acceptance:** Selecting match4 cannot imply it executed on order12. Final chain totals remain available and original submit100, fill61 and replacement total161 are unambiguous. A sibling fill selection retains correct ledger identity.

### MM-04, P2: comparison uses raw markout totals without eligible exposure

**Observed:** Skew comparison promotes `Markout total @100ms` (USD) with a better/worse judgement. It does not show per-share markouts or eligible fill/share counts there. The run view does show positive-is-favorable USD/share and eligible fill counts, but the denominator is eligible **shares**, not the displayed fill count. Run API does not return the eligible share count.

**Evidence:** Four `*-comparison.json` files; `baseline-103-summary.json`; `reconciliation.txt`. Baseline103 has 2014 eligible fills but **135665 eligible shares** at100ms, 1977/133167 at1s, 1845/124664 at5s. Total/eligible shares reproduces each reported mean. Arithmetic is correct.

**Source-confirmed:** `research_run_views.cpp:445-462` exports counts and divides by internal `measured_qty`, without exporting that quantity. Summary metrics used by comparisons export markout_total only.

**Trader consequence (inferred):** A width change can reduce filled volume and total markout while improving conditional fill quality. The comparison cannot distinguish these explanations without leaving it and collecting fills. Positive future-mid-minus-fill-price also includes initial spread edge; it is not pure post-fill mid-price drift.

**Fix:** Compare quantity-weighted signed USD/share markouts with eligible shares, eligible fill counts, no-mark and censored counts per horizon for both experiments. Retain USD totals for economic scale. State `buy: future mid - fill; sell: fill - future mid`, exchange-mid reference and weighting. If claiming post-fill movement/adverse drift separately, show signed future-mid minus fill-time-mid as a distinct metric.

**Acceptance:** The three baseline denominators above are visible/auditable; comparison never presents a total-only adverse-selection decision. Horizon eligibility remains separate from completed seed eligibility. Missing marks/censored horizons remain unavailable, not zero.

### MM-05, P2: non-directional changes are falsely counted as ties

**Observed:** Skew comparison ending position says `Directional change; no preferred direction`, mean +19.666667 shares, but `better/worse/tied 0/0/3`. Exact per-seed differences are +82, -270, +247, all nonzero. Max positions and fill counts have the same problem. Exact screenshot/text can be reproduced at the comparison route above.

**Source-confirmed:** Service comparison data supplies neutral metrics without an optimization preference; UI renders the resulting counts as better/worse/tied rather than suppressing that categorization. The exact differences remain correct. This is a semantic display defect, not evidence that the numeric paired differences are wrong.

**Trader consequence (inferred):** `3 tied` implies unchanged exposure when every seed changed. Signed ending inventory has no universal better direction.

**Fix:** For neutral metrics show increased/decreased/unchanged, or omit outcome counts and say no preferred direction. Reserve tied for exact equality; optionally compare absolute ending exposure as a separately defined risk metric.

**Acceptance:** Ending-position differences above yield two increased, one decreased, zero unchanged, with no preferred direction. True zero differences still show unchanged.

MM-05 exact source: `research_service.cpp:1022` computes actual zero-difference ties, then line1043 replaces that value with all pairs when `better == "neither"`; `experiment-compare-view.tsx:75,105` labels/displays those values as tied. Fix the contract semantics as well as the presentation.

## What works and what the numbers permit

- The task entry and setup make the research question obvious. Review-before-launch reports exact service assumptions and three seeds. Human duration units, tick/share units, fee signs, jitter and separate latency paths were readable. The synthetic single-venue warning is appropriately prominent.
- Baseline all three seeds completed with no new-order rejection; seed103 reaches both +/-500 share caps, ending -199. Risk blocked count is zero: this is not evidence of unlimited risk or of a tested breach; strategies can constrain quote size before a request is blocked. Inventory, drawdown and residual orders remain visible. The 100 ms feed variant generates 46/0/75 new-order rejections across seeds, demonstrating that delayed visible-state post-only intentions can be rejected at exchange truth.
- Accounting reconciled independently from all 2015 native baseline103 fills using Decimal: signed inventory -199; fees -271.4460; cash 1021660.6660; terminal mark100.05500; equity1001749.72100; net1749.72100. `reconciliation.txt` is the exact log. Fees are rebates here. Gross realized1477.2800 + unrealized0.99500 - fees(-271.4460) = net1749.72100. Spread capture1523.56500 is a fill-edge diagnostic and does not equal realized profit. No accounting identity failure is asserted.
- Fill match4 genuinely identifies the background noise taker. Buy61 at99.9900 against fill-time mid100.00000 gives +0.01000/share edge and -0.1220 fee. One-second future mid99.97500 gives -0.01500/share, total -0.91500; the sign is correct. All eligible counts/totals and share-weighted means reconciled, and end-of-run horizons are censored rather than zero-filled.
- At fill event45 exchange truth has39 own bid shares remaining, while delivered top-of-book still shows119 bid shares sourced from event40. The fill report is #47, after exchange fill #45. This usefully distinguishes delivered information from omniscient book truth. It does not prove all causality paths are correct.
- Initial FIFO evidence is excellent: 100 shares ahead at #24, 19 after #36, zero after #44, then own fill #45. At #36 the background maker receives the trade while our quote waits; this is concrete non-fill evidence at that moment, not an unexplained price-touch fill assumption. Own-order lifecycle selection also worked for the slow-path filled order with a pending replacement. No complete missed-fill opportunity taxonomy or arbitrary counterfactual model was validated.
- Comparison clearly shows paired seeds, candidate-minus-baseline sign, assumption differences, sample n, dispersion and exact per-seed results with approximate charts. Three seeds are a workflow check, not statistical evidence of a robust live improvement. Mean inventory charts can hide offsetting per-seed exposure; per-seed inventory paths/risk extremes should remain a required research view.

## Model limits and decisive objections

The default background market has one mechanically replenishing maker and one noise taker. This can produce high fill rates and rebates; baseline103 filled135723 of135923 accepted shares (0.998529), counting replace increases. It is not a hit probability per quote or an empirical live liquidity estimate. The denominator explanation is present and valuable. Background model parameters and fair-value walk are exposed; the review did not independently audit their RNG law, independence, response calibration or cross-treatment exogenous path equality. Shared seed labels do not alone prove identical endogenous market paths once a strategy affects the book.

The skew losses are a real native result, not a proof that skew is generally harmful or an identified implementation bug. Positive spread capture in all skew seeds alongside negative net P&L is precisely why net outcomes, markouts and inventory are needed. A useful follow-up attribution view would reconcile fill-edge, subsequent inventory repricing and fees under an explicit sampling convention. Do not manufacture an additive identity by subtracting average-cost realized P&L from spread capture.

Hostile deterministic events are **deferred model scope**: API.md:183-185 explicitly says price jumps, halts and stale/gapped data need runner scenario events not yet defined. `/service` exposes only interactive synthetic runs; real-time protocol mode is unavailable here. Existing exchange/gateway halt/feed infrastructure does not demonstrate deterministic research scenario support. Adjustable feed latency tests delayed data, not packet loss, feed gaps, recovery or strategy staleness timeout behavior. Existing broad synthetic/model warnings are good; add a concise setup capability note identifying which stresses are supported versus deferred. Do not market this study as hostile-market readiness. No new strategy or fault engine is demanded as a fix for an existing advertised UI control.

Decisive objections before clearance:

1. A cancel-path latency experiment must not conceal the replacements that use the path and lose races.
2. An explanation of FIFO priority must include priority-losing replacements and their new queue position.
3. Exposure/markout denominators and order identity must be explicit enough to prevent a trader from interpreting a different question than the simulator measured.

## Coverage, artifacts and next action

Covered: baseline creation/review/launch in browser; 15 completed native runs, fixed-seed comparisons for width/skew/feed/cancel; browser skew comparison; native first fill; own-order selection and fill-before-replace race; independent full-fill baseline accounting and markout eligibility reconciliation; targeted API/source/spec tracing. Browser transcripts, screenshots, native JSON and three review scripts live in `docs/reviews/PERSONA_MM_EVIDENCE_2026-11-08/`.

Not covered: fresh builds, sanitizer/performance suites, restart/durability, concurrency/load, mobile/accessibility completion, full browser variant-form workflow, strategy size-scaling treatment, all seed outcomes' individual fills, all rejection causes, targeted configured risk breach, forced cancellation-race loss, scenario interventions, stale-data shutoff, hostile jumps/halts/gaps, deployed services or human first-use. No server operation or release clearance is claimed.

Parent next action: fix MM-01..05, retain these native receipts/repro paths, start a new independent recheck against the rebuilt disposable pair, and complete that non-human fix/recheck cycle before stopping for human first-use validation. Recheck must observe labels and exact counts in UI/API, select both original/replaced fills, and exercise a queue-reset case with nonzero quantity ahead. Human first-use success remains untested.
