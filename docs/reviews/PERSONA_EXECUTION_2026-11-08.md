# Execution quant persona review, 2026-11-08

Decision: request changes before human first-use validation. No P0 found. Native execution, variants, pairing and fill inspection work; the current comparison can still misstate eligibility and neutral outcomes, and cannot explain the schedule that produced a residual. This is an expert review, not a human usability pass.

## Scope and prior knowledge

- Reviewer checkout: `/Users/jedi/.t3/worktrees/lob/t3-ddf6da87`, base `d61c329b2a8cb77aeb163fb4e8aaf0395a725aaa`. Reviewer only; no production edits, build, install, broad suite, server startup, delegates or production operations.
- Authorized disposable native service: frontend `http://127.0.0.1:45772`, API `http://127.0.0.1:45773`; supplied frontend BUILD_ID `ZbCMVWqStD4QzLA8AwzWD`, supplied unchanged binary `build/release/exsim_api`. Build identity is supplied by parent, not independently attested here. Timestamps in API receipts are UTC on November 7; review date is November 8 Singapore time.
- Prior knowledge before UI observation: user brief, execution-research concepts, and required `docs/WORK_PLAN.md` recovery/current-state rollup. No UI source, README, product/spec or previous persona findings read before observing the app. Thus independent expert observation, not blind first-use evidence.
- `preview_status` initially said `available:false`; required `preview_open(reuseExistingTab:false)` succeeded. Owned tab `tab_6_e40125e3-02a4-4ae8-98a3-dbbb53b4744f` was used throughout and closed before handoff. No fallback needed. Snapshot refs used where returned; CSS selectors used where snapshots returned no refs. All mutations used this disposable service and `EXEC-REVIEW` names. Other personas' records were not changed.
- After observation, consulted PRODUCT, ARCHITECTURE, relevant OpenAPI normative schemas, and focused UI/native source. No C++ change was made; exchange-protocol audit was unnecessary.

## Actual task and receipts

Browser: library -> Research execution -> buy 500 TWAP, 1 s start, 30 s window, 1 s interval, 60 s simulation end; seeds 101,102,103. Review assumptions, create/start all three. Create variant, change only method to immediate sweep, review/start, Compare with parent, inspect seed 102 and click fill match 62. Fees, risk, market, latency and seeds remained identical. Comparison lists only method and removed TWAP-only fields as differences.

Buy baseline `exp_b8d6039b9e647a2e`; sweep `exp_8c3586e643695c8d`. Run IDs and exact complete responses are in [native-api.json](PERSONA_EXECUTION_evidence/native-api.json). Browser comparison and selected-fill texts: [compare-ui.json](PERSONA_EXECUTION_evidence/compare-ui.json), [fill-ui.json](PERSONA_EXECUTION_evidence/fill-ui.json). Screenshots: [paired.png](PERSONA_EXECUTION_evidence/paired.png), [fill.png](PERSONA_EXECUTION_evidence/fill.png). Screenshots are viewport evidence; JSON retains below-fold text.

Additional direct native HTTP probes use a review-only standard-library script, [probe.py](PERSONA_EXECUTION_evidence/probe.py). It creates only prefixed disposable experiments, runs six sell observations over the same three seeds plus cap/censor probes, and checks signed slippage and fee identities with Decimal. It is a mutating reproducer, not a broad test suite; rerunning creates new prefixed records. Output: [probe-summary.json](PERSONA_EXECUTION_evidence/probe-summary.json). Completed without assertion failure in approximately 0.5 s on this small scenario. This is not a large-workload performance benchmark.

| Side / seed | TWAP filled / requested | Sweep filled / requested | TWAP price cost USD | Sweep price cost USD |
| --- | --- | --- | --- | --- |
| buy / 101 | 500 / 500 | 500 / 500 | -22.34000 | 8.88000 |
| buy / 102 | 483 / 500 | 500 / 500 | -11.53000 | 14.00000 |
| buy / 103 | 483 / 500 | 500 / 500 | 33.22000 | 11.31000 |
| sell / 101 | 500 / 500 | 500 / 500 | 31.38000 | 11.04000 |
| sell / 102 | 450 / 500 | 500 / 500 | 18.68000 | 4.00000 |
| sell / 103 | 500 / 500 | 500 / 500 | -16.88000 | 10.11000 |

Buy sweep minus TWAP: mean completion +0.022667, residual -11.333333 shares, executed cost including fees +11.647333 USD; n=3. TWAP's lower mean executed cost is not an unconditional improvement: it leaves 17 shares on two seeds, total costs cover different quantities and residual opportunity cost is unavailable. Even per-filled-share cost conditions on which shares filled. Ending mark-to-market P&L also mixes holding-time exposure with execution quality.

Decision benchmark is the reference exchange mid at parent declaration, not each child's arrival mid. UI names the exchange and delivered decision mids and explains the distinction; no separate child-arrival cost attribution is supplied. Source: `research_run_views.cpp:185`, `simulation_core.cpp:1472`, OpenAPI ExecutionSummary. For buy, price cost is notional minus benchmark times filled shares; for sell its negative. Net fees are added on either side. Native sell and cap/censor assertions confirm these formulas. Fees were 0.003 USD per filled share; rebate behavior was not exercised. Participation is parent filled shares divided by all market traded shares from declaration through last parent fill, including own trades, not a fixed-window participation target. Seed 102 buy TWAP is 483 / 101967 = 0.004737 rounded. Denominator independently read, not reconstructed trade-by-trade.

Fill 62 / order 92: submitted event 350 at 1 s, arrival 372, accepted 373, fill 374 at 1.000123569 s, accepted report 376, fill report 377. Buy 16 at 100.0300; fee 0.0480; position 0 -> 16; cash 1000000 -> 998399.4720. IOC aggressor never rested, so queue position is correctly unavailable. Browser and lifecycle API agree. This verifies one fill, not all lifecycle/race combinations.

## Findings

### EXEC-01, P1 core task: censored execution is silently eligible in comparison

**Observed and source-confirmed.** Route `/compare?baseline=exp_b8d6039b9e647a2e&candidate=exp_f75d802c5e345a44`. Repro: clone baseline, keep 30 s execution window, set simulation end to 5 s, run seed 101 to native `completed`. Summary correctly reports `censored_run_end`, filled 66/500. Comparison pairs seed 101 and offers normal directional outcomes without showing execution-window censoring; its text says incomplete runs are excluded. The changed bound appears in assumptions, so it is not hidden there. Evidence: [censored-compare-ui.json](PERSONA_EXECUTION_evidence/censored-compare-ui.json), [censored-comparison.json](PERSONA_EXECUTION_evidence/censored-comparison.json), probe-summary run `run_b36ae89c51f88166`.

Source: `ResearchRun::comparison_inputs`, `research_run_views.cpp:1022` gates on API Completed and replay verification, not parent observation status. `research_service.cpp:879-904` pairs those inputs; comparable checks market world and shared seeds. Compare/paired-seed components do not receive/display window eligibility.

Consequence: an intentionally unfinished observation can be treated as completed execution evidence, and short exposure can look like better fees/drawdown/P&L. Comparing truncated windows is valid exploratory research only when clearly identified, not implicit strategy-quality evidence.

Fix: carry per-run observation status and horizon into paired evidence; show censored counts and explicit warnings at the decision cards. Either exclude censored observations from full-window execution conclusions or provide a deliberate exploratory inclusion mode. Replace ambiguous “incomplete” copy with precise run-state and observation eligibility rules. Acceptance: this exact pair visibly identifies the censored candidate before any directional conclusion; each metric reports eligible n and exclusion reasons; ordinary observed partial completion (483/500) remains eligible and distinct from censoring.

### EXEC-02, P1 core task: no schedule evidence to explain unexecuted shares

**Observed UI gap, source-confirmed evidence gap.** Route `/runs/run_2f94393d568b1cf1` and `/inspect`. Parent residual is 17, blocked orders 0, accepted-order share fill rate 1.0. Fill list jumps from 1 s to 3 s and ends at 29 s. It cannot tell the trader which slices were skipped, whether they lacked a delivered touch, were in flight, were capped, or had IOC remainder. The cap10 probe `run_7ed5993975ae118a` fills 240/500, also demonstrating that risk-blocked count is not a cap-attribution count.

Source: `strategies/twap_executor.hpp:40-48` computes cumulative targets and ignores the boolean child-send result; `parent_order_child.hpp:33-49` returns false for in-flight/no-touch/zero headroom and clamps quantity before submit. No per-slice target/skip/cap record is returned by the current OpenAPI event/summary schemas. This does not assert that matching or raw order evidence is absent. Existing native schedule policy is clearly documented and the residual itself is legitimate.

Consequence: explaining a missed parent target requires reconstructing internal strategy state from thousands of unrelated events, and even that lacks explicit skip attribution. This blocks the “why this residual?” execution-research task.

Fix: record a compact C++ decision trace for each scheduled slice/retry: due time, cumulative target, known fills, outstanding quantity, visible touch/event age, proposed/capped/sent shares, skip reason, child ID and final result. Display target versus filled cumulative shares and residual over simulated seconds, with deadline and decision markers; link rows to lifecycle evidence. Preserve exact values in a table/export. Acceptance: seed102's 17 shares and cap10's 260 shares are explained by attributable schedule decisions without guessing from absence of fills. This adds evidence to the existing strategy, not a new execution model.

### EXEC-03, P1 core interpretation: nonzero neutral differences reported as ties

**Observed and source-confirmed.** Main three-seed buy comparison: Ending position differences are 0,+17,+17 but better/worse/tied is 0/0/3. Filled shares, fills, order counts and participation exhibit the same error. See compare-ui and native-api comparison response.

Source: `research_service.cpp:1043` sets tiedCount to sample count whenever betterDirection is neither, overriding actual zero-difference count. `experiment-compare-view.tsx:105` displays these under “Better / worse / tied.” Lack of preference is not equality.

Consequence: the supporting evidence falsely reports unchanged execution exposure/activity, particularly material when completion and participation differ. Exact per-seed numbers do expose the contradiction, but should not require the trader to resolve it.

Fix: preserve actual equality count; for neutral metrics show positive/negative/zero counts, or explicitly mark preference counts not applicable. Coordinate schema semantics before changing API consumers. Acceptance: ending-position example has one true zero and two positive changes, never three ties; neutral metrics do not acquire an invented “better” direction.

### EXEC-04, P2 friction: semantic error does not identify the editable field

**Observed and source-confirmed.** On `/experiments/new?from=exp_b8d6039b9e647a2e`, set Slice interval to 0, Review assumptions. Launch is correctly disabled, but error is `/config rejected by the research runner: slice_interval must be positive`, with no error beside Slice interval. Evidence: [invalid-interval-ui.json](PERSONA_EXECUTION_evidence/invalid-interval-ui.json). No invalid run was created.

Source: `experiment_config.cpp:578` throws a generic check; translation `experiment_config_translation.cpp:672` attaches non-line semantic failures to root. UI field errors require matching paths.

Consequence: trader must map internal snake_case to a visible input and search a long form. Fix semantic validation to return `/config/strategy/sliceIntervalNs`, human label, minimum and unit. Acceptance: zero interval highlights/focuses Slice interval and explains “must be greater than 0 s”; invalid configurations remain unlaunchable.

### EXEC-05, P2 research friction: comparison omits benchmark and denominator definitions

**Observed UI gap, source-confirmed.** Main comparison shows cost/participation differences and directions, but its page text never identifies the decision-mid benchmark, filled-only cost scope, residual opportunity-cost absence, or participation's last-fill-dependent horizon. These are explained well on individual run pages, not at the comparison decision point. See compare-ui versus `execution-summary-panel.tsx:59` and `paired-seed-analysis.tsx:31`.

Consequence: comparison-only readers can read executed cost as whole-parent shortfall or participation as a common-window rate. Fix: put concise benchmark, fee, sign, filled-only scope and participation definition beside the execution comparisons; show baseline and candidate absolute filled/residual quantities together with costs, not only one metric selected at a time. Acceptance: a trader can state benchmark, USD versus USD/share, buy/sell sign, quantity coverage and volume horizon from `/compare` alone. No child-arrival benchmark model is required to fix the description.

## What works and decisive limits

- Task entry, human time units, explicit IOC protection, scenario disclosure, review-before-launch, variant lineage, exact assumptions and paired seed selection all supported the end-to-end task. Source configs retained the market/fees/risk/latency. Default child cap 200 is exposed. Sweep is repeated protected IOC execution across delivered updates, not guaranteed instantaneous liquidation; do not market it as guaranteed one-shot completion.
- Run analysis prominently separates parent residual from filled inventory, declares positive costs worse on both sides, labels fee inclusion and unavailable opportunity cost. Window censoring and markout availability are represented natively. A fully filled accepted child order is not full parent completion, and the UI supplies both denominators.
- Comparison provides n, dispersion, per-seed exact values and explicit seed exclusions. Standard error is not presented as a confidence interval. Aligned equity/inventory/drawdown charts have units; inventory is a useful filled-share proxy only for these single-sided flat-start probes, not a replacement for schedule evidence.
- This estimates outcomes under one seeded, interactive synthetic liquidity/noise model and protected built-in strategies. It does not establish calibrated market impact, live capacity, a production execution recommendation, historical counterfactual fills, or an opportunity-cost-complete implementation shortfall. Those are model scope, not bugs to silently “fix.”
- Slow/large workflow was source-reviewed only: comparisons synchronously re-execute each eligible run and verify replay (`comparison_inputs`); a page refresh can repeat work. No large workload was launched and no measured scalability defect is claimed. Future acceptance should include progress/cancellation or cached verified comparisons and a bounded large-run receipt; this review supplies none.
- Untested: rebates, many instruments, non-unit multiplier, extreme exact-value limits, protocol sessions, failed/cancelled eligibility, passive queue races, mobile/assistive interaction, broad stress/restart/recovery, all markout censor identities and all fills' arithmetic. No new broad test/build clearance is implied. Direct API probes supplement, not replace, the browser task.

## Parent continuation

Review artifacts only are owned here, including the reproducer and captured disposable responses. Existing WORK_PLAN is intentionally not edited under this review's ownership constraint; parent should record this report, resulting commit, task link and checks there. No other person's experiments were mutated. No production services were touched and no server was launched/stopped. Browser tab is closed.

Parent should fix the above, run focused checks, then launch a **new independent recheck task** against the fixed native build. Do not treat this report or source declarations as recheck clearance. Complete all remaining non-human review/fix/recheck steps before stopping for the user's human first-use validation. This reviewer does not claim that validation succeeded.
