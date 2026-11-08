# Facilitator answer key and environment checklist

FROZEN FOR HUMAN EVALUATION. All reported current-workflow objections are independently closed within documented scopes. This is facilitator-only material. Do not give it to participants. Actual human results: NOT RUN.

## Freeze and separation

Frozen artifact: `/tmp/exsim-persona-review-75cf28f`, native source360ad69529aa57b5e8856391812cc3d1d93c6896, frontend source75cf28fcdbee8e82bd71ba20264604ff9dfdac15, Next build `efg6fmC6FKFd5gxu0E1Hb`, Nodev24.22.1. Delivery SHA256 `2c1dee66607947a282313f451b5b5827ad61e35a3132846f75fd6023f00f6d1c`; API executable SHA256 `10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5`. Complete inventory verifies after expert use. An exact durable copy, including the same delivery.json hash, is retained at `output/releases/human-review-75cf28f`; use that copy for new sessions.

The previous temporary empty session was stopped without any participant activity. Participant01 is running at `http://127.0.0.1:46072/experiments`, API46073, new session `output/human-participant-01-75cf28f`. Its library was verified empty, without creating any participant record. SupervisorPID29157, service PIDs/session metadata in `session.json`. To stop, first verify `ps -p 30473 -o command=` names scripts/start_human_review.py with that exact session, then `kill -TERM 30473`. This retains data/logs. If already stopped, use a NEW directory with the packet launcher command, not that old PID. Each participant requires a new browser profile and session. Expert45872/45873/45882 and reference45992/45993 services were stopped; their records are retained and must not be reused for participant first use. Installed production is unchanged.

Use a clean browser profile, 1440x900 desktop, then a390x844 viewport task. Keyboard task uses the same browser; record whether an actual screen reader was used separately. No tutorial, repository files or facilitator click coaching. Keep viewport/browser/version, source/binary hashes, package identity, participant identifier and session timestamps with the score sheet. Recruit at least one execution researcher and one market-making researcher unfamiliar with this UI; score separately.

## Task answers and error boundaries

1. Execution setup must describe buy500 shares, seeds101/102/103, a60s simulation, parent start1s/window30s and TWAP1s slices, using a service baseline. Record all other defaults from the immutable manifest. A variant changes execution method only; preserving fees, risk, market and seeds is necessary to isolate that treatment. Review must be current before launch. Mark assistance if the facilitator names a control.
2. Completed simulation is distinct from fully observed execution window, and both differ from full parent completion. A window extending beyond run end is censored, not a normal full-window comparison observation. For a fully observed partial parent, lower filled-only USD cost does not resolve residual opportunity cost. Positive signed cost is worse on either buy or sell; slippage excludes fees, executed shortfall includes them. Benchmark is exchange mid at declaration, not each child arrival.
3. Explain a residual using native decision times, cumulative target, delivered touch, known fills, outstanding reservation, child/risk caps, submitted quantities, skip reasons and delivered reports. Never infer a cause solely from absent fills. TWAP final target time differs from observation deadline; no extra retry at the deadline is implied. A trace chart covers only its current record page and decision rows. Request/report-only zero fields are not decisions.
4. Correct fill explanation identifies selected order/event/match and quantity, separate from final replacement-chain quantities/ID/as-of event. Exchange truth can precede delivered strategy knowledge. Replacement-race attribution requires an actual related fill between submit and arrival, not unknown_order alone. FIFO priority reset and subsequent rest have distinct queue availability; a size reduction can keep priority.
5. MM net P&L is gross realized + unrealized - net fees; a negative fee is a rebate. Spread capture is not an additive synonym for profit. Inventory, drawdown and conditional markout evidence must accompany a strategy preference. Market-making quote-width comparison changes only half-spread; inventory-skew and latency treatments use separate variants.
6. Signed markout is buy future exchange-mid minus fill price, or sell fill price minus future mid. It includes initial edge, so it is not pure future-mid drift. Per-share mean weights eligible SHARES, not fill count. No-mark/censored horizons are excluded and unavailable if no measured shares; unobserved is not measured zero or a tie. Metric n counts eligible seed pairs, distinct from per-horizon eligible shares/fills.
7. New/cancel/replace outbound and rejection counts have different denominators. Host risk blocks precede submission. Replacements use the configured cancel/replace path; a zero cancel-race count does not establish absence of quote-update races. Compare the request kind, reasons and observed transit, and inspect lifecycle evidence.
8. Paired differences are candidate minus baseline, n is metric-specific; SD/SE require enough eligible pairs and SE is not a confidence interval. Neutral metrics have no optimization direction; nonzero changes are not ties. Mean charts are sampled/derived and can conceal per-seed behavior. Assumption differences can confound attribution. A synthetic result cannot establish live profitability, historical counterfactual accuracy or calibrated impact.
9. Comparison attempt ledger must identify selected, superseded eligible, ineligible and unlaunched work, with reasons/digests. Pinning binds selected run IDs across later retries; an ineligible/corrupt pinned run must not silently substitute another attempt. Downloaded receipt preserves cohort query and evidence, not authentication of all inputs.
10. Export succeeds only when all present inventory artifacts transfer at exact sizes and match SHA-256. Browser audit JSON unpacks with scripts/research_audit.py into a NEW directory. Integrity verification is distinct from native reproduction and external authenticity. Completed runs can be re-executed with exsim_research verify; failed/cancelled timing is not claimed reproducible from config. New API envelopes/run IDs/wall clocks can differ while canonical event and ledger digests match. Exact executable provenance and supplied source/release metadata must remain recorded, with unavailable legacy metadata disclosed.
11. Partial launch recovery keeps existing runs and starts missing seeds. Paused step advances complete event boundaries, so processed count may exceed a requested boundary count. Cancelled runs retain residual position/open orders/diagnostics; cancelling a simulation is not liquidation. Repeating a seed is a new attempt with a reusable idempotency key for uncertain replies.
12. Connections is a read-only boundary for a separate protocol service. Empty sessions/feeds/faults do not mean browser research is broken or that this UI can launch a gateway. Delayed data is not packet-loss/recovery, and deterministic jumps/halts/gaps are not claimed implemented.

## Scoring and evidence

Use the20 capabilities in HUMAN_FIRST_USE_TEST_2026-11-08.md, all critical passes plus at least16/20 unaided successes. Success requires discovery, meaningful completion and correct interpretation. Record exact interpretation quotes, time, wrong turns, help, failed requests and recovery behavior. Record first-use launch<=5min and comparison+one explained outcome<=15min separately. Do not convert expert review time into a human timing prediction. Do not score screenshots or automation as a participant pass.

Reference scenarios below were regenerated with the exact frozen API in separate facilitator data, after independent closure. They are not answers to feed participants. If participant inputs differ, inspect their actual immutable manifests and evidence. Any newly demonstrated critical defect returns to repair/recheck, rather than lowering the denominator after the session.

## Frozen reference receipts

[Reference receipts](HUMAN_REFERENCE_2026-11-08/receipt.json), [binary/package identity](HUMAN_REFERENCE_2026-11-08/identity.json), complete immutable manifests/summaries and paired comparisons are in HUMAN_REFERENCE_2026-11-08/. These are20 completed native study runs, plus separately identified control fixtures. They are automated preparation, not participant success. Source/config differences are preserved exactly. Reference supervisor/session was stopped after generation and all data/logs retained.

Execution uses service TWAP baseline buy500, seeds101/102/103, start1s/window30s, slices1s, end60s. Sweep changes only method and removes TWAP-only fields; capped changes only child cap200 ->10 with seed101; censored changes only end60s ->5s with seed101. Signed cost includes fees on filled shares only, not residual opportunity cost.

| Method / seed | Filled shares | Residual shares | Signed executed cost USD |
| --- | --- | --- | --- |
| twap / 101 | 500 | 0 | -20.84000 |
| twap / 102 | 483 | 17 | -10.08100 |
| twap / 103 | 483 | 17 | 26.45900 |
| sweep / 101 | 500 | 0 | 10.38000 |
| sweep / 102 | 500 | 0 | 15.50000 |
| sweep / 103 | 500 | 0 | 4.60000 |
| capped / 101 | 240 | 260 | -9.68000 |
| censored / 101 | 66 | 434 | -0.80200 |

The observed TWAP/sweep cohort has three pairs. Capped seed101 is fully observed despite260 residual shares and remains eligible. The censored run completed its simulation with434 residual, but5s <31s deadline excludes it from a full-window comparison. TWAP102 trace has72 records: at final target30s, known filled483, outstanding0, residual17, requested17, submitted0, reason no_delivered_touch. Observation deadline is31s. This explains that run; it does not establish why every unrelated partial parent failed.

MM uses the current fixed-spread service baseline with halfSpread1 and otherwise unchanged defaults. Width changes only halfSpread1 ->2. Inventory changes only to inventory-aware skew1 tick/100 shares, scalingfalse. Latency changes only cancelLatency80000ns ->100000000ns. Each has three common seeds; hypotheses are separate. These configurations need not match earlier diagnostic fixture numbers.

| MM treatment / seed | Net P&L USD | Ending position | Max long / short | Max drawdown USD |
| --- | --- | --- | --- | --- |
| mm / 101 | 1677.06400 | -163 | 500 / -500 | 5.00000 |
| mm / 102 | 1794.82000 | 130 | 500 / -500 | 5.00000 |
| mm / 103 | 1749.72100 | -199 | 500 / -500 | 6.38400 |
| width / 101 | 1423.24400 | 217 | 500 / -500 | 8.88000 |
| width / 102 | 1457.59800 | 202 | 500 / -500 | 7.50000 |
| width / 103 | 1468.82400 | 438 | 500 / -500 | 6.09400 |
| inventory / 101 | -7711.57900 | -81 | 467 / -456 | 7714.73800 |
| inventory / 102 | -2654.84600 | -140 | 500 / -500 | 2977.02800 |
| inventory / 103 | -11970.41600 | 48 | 500 / -449 | 11992.45900 |
| latency / 101 | 837.52100 | 129 | 500 / -500 | 5.30000 |
| latency / 102 | 924.60200 | -1 | 500 / -500 | 3.70400 |
| latency / 103 | 931.87900 | 277 | 376 / -500 | 4.61700 |

These few synthetic outcomes do not establish live profitability. Widening quotes reduced P&L in these seeds; inventory-aware configuration losses are real observed reference outcomes, not a profitable-strategy promise. Inventory, markout eligibility and fees must accompany interpretation. Inspect per-seed comparisons/manifests before attributing any change.

[Browser preparation](HUMAN_REFERENCE_2026-11-08/browser.json) verifies Parent start1s ->1000000000ns ->1000ms ->1s without changing assumptions. A reference run created paused is stepped: native accepted100 events, then a test-only browser route interrupted the response. Clicking Step again reused the exact key and processed count stayed100, then Resume completed. Zero page errors. The injected network error is intentional. This is an automated uncertainty/idempotency check, not a human recovery result. It does not count facilitator-created pause state as a participant's successful Pause action.

## Controlled human task6 provisioning

Keep the participant library empty initially. Only AFTER the initial unaided research tasks, prepare the control task from a separate facilitator terminal:

```sh
python3 scripts/prepare_human_control_task.py --session output/human-participant-01-75cf28f
```

Use that participant's actual session path. This adds a named paused control experiment/run and a deliberately event-bounded failed experiment/run, both with seed602 unlaunched. Exact IDs are retained in control-task.json. It refuses duplicate completed preparation; interrupted calls use stable operation keys. It was exercised only in facilitator data; it has NOT been run against participant01. Fixture creation is never scored as participant discovery, creation, Pause or recovery success. Ask the participant to find the named control/recovery exercise without giving click directions.

For the temporary connection-error task, facilitator opens browser developer tools in a separate window and selects Network Offline immediately before the participant's first Step request. After the UI shows the error, restore Online without changing application state or naming a recovery control. Record the fault onset/end and the participant's independent recovery. This human protocol is an outage before delivery; the stricter ambiguous-after-acceptance case was separately automated above. Compare native processed counts and idempotency IDs if any action is repeated. Network injection is facilitator setup, not a product feature. Participant must decide how to recover and resume. Do not claim that the Offline interaction itself was tested by a human.

For capability15, a supplied paused fixture is not proof the participant can Pause. Count the capability only if they actually pause, step and resume unaided; if rapid deterministic completion prevents Pause, record failure/unavailable for that capability. It is noncritical and the fixed denominator remains20. Do not coach, skip or mark it passed on the basis of a button being present. Actual screen-reader operation remains untested; record separately if used.

## Recreate references, only if needed

Start the frozen release with a NEW facilitator session directory on45992/45993, using scripts/start_human_review.py explicit ports. Then run prepare.py --api http://127.0.0.1:45993 --output NEW_OUTPUT_DIRECTORY, followed by HUMAN_REFERENCE_OUTPUT=NEW_OUTPUT_DIRECTORY node browser.cjs, with scripts under HUMAN_REFERENCE_2026-11-08/. The preparer requires an empty API. Keep reference data separate from participant data. All source outputs are strings where exact IDs, prices, quantities or ns matter. Frozen recorded receipts are retained here; rerunning creates new envelopes/IDs while canonical outcomes may match.

Human results: NOT RUN. This key and the packet are ready for the last human step, with documented coverage limits and no80% success claim.
