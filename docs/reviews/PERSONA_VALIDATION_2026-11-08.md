# Independent skeptical research-lead review

Date: 2026-11-08. Base `d61c329b2a8cb77aeb163fb4e8aaf0395a725aaa`.
Checkout: `/Users/jedi/.t3/worktrees/lob/t3-826df8a9`.
Lens/prefix: `VALIDATION-REVIEW`, reproducibility and research validity.

## Verdict and boundaries

**Do not declare all non-human review/fix/recheck steps complete.** The current native workflow completes a useful paired experiment and provides strong fill evidence, but has three P1 audit/reproduction gaps below. Parent must fix or explicitly narrow unsupported product claims, then commission a NEW independent recheck. This reviewer made no production changes. Human unaided first-use validation remains outstanding and cannot be inferred from this expert walkthrough.

No P0 demonstrated: observed paired numerical differences were consistent with fills and fees. Severity: P0 research harm, P1 core task, P2 friction, P3 polish. Findings concern the existing research workflow, not requests for new strategies, calibrated impact, historical backtesting or external live execution.

Only the authorized disposable native pair was used: UI `http://127.0.0.1:45772`, API `http://127.0.0.1:45773`, supplied frontend BUILD_ID `ZbCMVWqStD4QzLA8AwzWD`; binary unchanged by this reviewer. No build, compiler, installation, broad suite, server startup, private service, delegate or production operation. No other persona's experiment was changed. Shared library contents can change independently.

Prior knowledge recorded: knew the task brief, ports/build identity, C++ ownership, requested probes and the required WORK_PLAN recovery rollup. Read WORK_PLAN before observation as instructed; it contains prior implementation claims, so this is not an unprimed human study. Had not read UI code, README, PRODUCT or implementation before the browser pass. Initial library appeared empty. First observed library, setup, experiment overview, variant, comparison, run Summary/Manifest/State history, fill inspector and Connections, then inspected source/API. No README read.

Browser: preview_status returned `available:false`; mandated preview_open with `reuseExistingTab:false` succeeded. Owned tab `tab_4_e40125e3-02a4-4ae8-98a3-dbbb53b4744f`. Snapshots sometimes returned only URL/screenshot, so owned-tab DOM text was read with preview_evaluate; actions used a snapshot ref for the name input and observed selectors thereafter. No Playwright fallback required. Closed owned tab before handoff.

## Native end-to-end evidence

Baseline: `exp_9cf39bf838c23b72`, name `VALIDATION-REVIEW baseline`, TWAP, seeds 101/102/103, 60 seconds, 500-share buy, 30-second execution window, 5-tick protection, taker fee 0.0030 USD/share. Browser reviewed service assumptions and created all three runs successfully. Config digest `bd6a8d7af8d16528d802cdbff9387305ac316b0cdc88367afb23b5625a10ea03`.

Candidate: `exp_b475fe9282596e9b`, name `VALIDATION-REVIEW candidate`, cloned with parent lineage. Protection 1 tick, intentional confound taker fee 0.0060, extra seed 104. Browser created four completed runs. Config digest `6363112c2078f96ded7d19140612ab651b10745f85a83a0c0f93ff25cb1a6a6f`.

Comparison route: `/compare?baseline=exp_9cf39bf838c23b72&candidate=exp_b475fe9282596e9b`. Correctly shows paired 101/102/103, candidate-only 104, all three assumption differences, explicit mixed-effects warning, failed/incomplete/unpaired exclusion and selection-bias warning. Candidate-minus-baseline executed cost +1.466000 USD, sample SD 0.029445, SE 0.017000, n=3, worse 3/3. Completion difference zero, underlying completion 1.000000/0.966000/0.966000. Exact fee changes +1.50000/+1.44900/+1.44900 agree with filled quantities 500/483/483 times 0.0030. Strategy protection happened not to change fills in these seeds; this is not evidence that protection never matters.

Seed 101 original run `run_e284f2e19edbfc9e`: 22,420 events, 24 fills, 500 shares, final position 500, no open orders. Canonical digest `7cbb9aea4f742fa9`; ledger `1e86abeac24f9cfd`. Native API repeat `run_b23eda11f9f605da` matched both digests exactly. API manifest digest differs because enclosing API run identity changes; no false claim of identical API envelopes. Repeating the POST with the same idempotency key returned the original run with `Idempotent-Replayed:true`, not another execution. Comparison after the repeat selects its new run ID without increasing n.

Inspector `/runs/run_e284f2e19edbfc9e/inspect`: selected first fill, order 95, event 383, match 63. Shows submit 359, arrival 381, accept 382, fill 383, acknowledgment 385, fill report 386; taker 16 at 99.9900, fee +0.0480, cash 1000000 to 998400.1120. Explicitly says order never rested and has no queue position. Separate delivered book and exchange truth are visible. Aggressor-arrival field says not observed for this taker while its own arrival is shown; potentially confusing wording, not claimed absent evidence.

API paused/step/cancel probe `run_1eabacaf9169d405` seed 102: step requested 500 canonical events and processed 502 at a complete event boundary; cancel preserves position 16, unexecuted 484, no open orders. Browser labels cancelled and execution window cut short. 1s/5s markouts censored (one fill each), totals unavailable, not zero. Successful original seed 102 remains selected by comparison.

Event-bound experiment `exp_ffe1b89f1f82d39d` (`VALIDATION-REVIEW event bound`), run `run_78d54dd7472192ee`: bound 100 fails explicitly at 101 records; browser reports `failed`, exact bound reason, parent not declared, costs unavailable. No eligible pair and `comparable:false` against baseline. No silent successful-completion label found.

Large-seed experiment `exp_99bcb8545291fe54` (`VALIDATION-REVIEW large`): 0, 9007199254740993 and 18446744073709551615 all complete, exact seed strings retained. Disjoint-seed comparison rejects paired inference. Duplicate seeds [101,101] return 422 with the second field identified; uint64 overflow 18446744073709551616 returns 422. Large-seed browser input/paired chart was not separately exercised.

Reproducer: `docs/reviews/PERSONA_VALIDATION_probe.py`; run succeeded, exit 0, full raw request/response evidence `/tmp/PERSONA_VALIDATION_probe.json` (1,793,632 bytes). It creates only prefixed disposable experiments and runs. Re-running creates additional prefixed review observations. Later one-off log summarization hit an IndexError on an empty metric list; that was the summarizer, not the probe or service. No broad tests run. The script itself uses actual native API outputs, not mocks. JSON endpoint responses were saved; these are not a complete native audit export.

## Findings

### VALIDATION-01, P1: provenance promises stronger reproduction than it identifies

**Observed and source-confirmed.** Route `/runs/run_e284f2e19edbfc9e`, Manifest tab: “Everything needed to reproduce this run byte for byte.” Engine `exsim.matching_engine/canonical_encoding=1`, runner `exsim.research.runner/1`, strategy `twap_executor/1` identify format/implementation families, not the executable revision. Neither UI manifest nor its API response identifies an exact source commit or binary digest. This matters even though same-process rerun and comparison reproduction passed.

Source: `web/src/components/research/run-detail-view.tsx:277`; `src/exsim/api/research_run.cpp:299` uses constant IDs; `include/exsim/research/run_artifacts.hpp:45` defines these fixed labels. Native `BuildInfo` supports executable fingerprinting, but `src/exsim/api/research_run.cpp:563` calls `current_build_info("")`, so its executable file cannot be opened and the default fingerprint remains unavailable (`src/exsim/research/run_artifacts.cpp:105`). `research_run_views.cpp:1049` reuses recorded build labels for comparison. Reproduction by the current executable is real, but it does not authenticate or recover the original executable. This is not a claim that native manifests or replay are absent.

Trader consequence: archived results cannot identify the exact implementation needed after upgrades; the unconditional copy invites unjustified audit certainty. Fix: persist source/release identity and a content digest of the running engine/runner artifact, expose it in the API and manifest, and state precisely which artifacts are deterministic and which identity/wall-clock envelopes differ. Until available, narrow the copy to recorded assumptions and current-build verification limits.

Acceptance: two different implementation builds produce distinct persisted build identities while identical same-build runs match deterministic event/ledger artifacts. Exported metadata identifies the executable required for replay; no byte-for-byte claim for cancelled execution or API envelopes. New independent reviewer verifies this across an actual build change; this review did not build anything.

### VALIDATION-02, P1: research UI has no actionable reproduction/export handoff

**Observed and source-confirmed workflow gap.** Same run Manifest/State history and experiment overview offer inspection, variant and comparison, but no download, native artifact locator, rerun-this-seed or reproducibility instructions. A trader can read JSON but cannot take the promised audit package away from the browser. Reproduction required manual API research and the reviewer-written script. UI clicks alone did not complete this core task.

Source: `ManifestPanel`, `run-detail-view.tsx:271-315`, renders definitions and a JSON pre only. `docs/api/openapi.yaml:387-650` offers translated manifest/summary/series/event/order/fill/book reads, not a native artifact bundle/export route. Backend support DOES exist: `docs/RESEARCH_RUNNER.md:29-38` documents CLI verify, compare and native artifacts; `include/exsim/research/run_artifacts.hpp:50` lists the native audit files. `/v1/runs` can create another seed run and comparison independently reproduces native artifacts (`research_run_views.cpp:1024-1084`). Therefore do not characterize this as no backend reproducibility.

Trader consequence: independent handoff, archiving and rerun are expert-only operations requiring service/filesystem knowledge. Fix: expose a discoverable reproducibility action with exact seed/config/build identity, deterministic artifact package and integrity inventory, plus a supported local verification command or action. Export must include full-resolution evidence, not plotted buckets or just the first event page. If export remains deferred, explicitly label the limitation and remove the claim of a complete browser research handoff.

Acceptance: from this completed run, a fresh reviewer downloads the package, verifies config/build/artifact identity and reproduces event/ledger results with the supported route. More than 1,000 events remain complete across export; censored values and failure/cancel diagnostics survive. No export button was offered, so no nonexistent export was claimed successful.

### VALIDATION-03, P1: generic bias disclosure cannot audit actual inclusion or retries

**Observed native behavior and source-confirmed.** Comparison route above, before/after API repeat and cancellation: n stays 3, which is correct, but selected baseline seed 101 silently changes from `run_e284f2e19edbfc9e` to `run_b23eda11f9f605da`; later cancelled seed 102 is ignored in favor of its older completed run. Comparison gives no list/count/reason for that cancelled attempt or superseded completed attempt. A failed-only experiment shows no eligible pair, but its comparison does not identify the failed run/reason. General exclusion/bias text is good and insufficient for an audit.

Source: `src/exsim/api/research_service.cpp:875-885` explicitly lets the later eligible completed run win. `research_run_views.cpp:1024-1084` returns nullopt for noncompleted, mismatched or unreadable evidence; reasons are discarded. `research_service.cpp:904-925` exposes eligible seed lists, without attempted runs, excluded-run records or selection policy. `experiment-compare-view.tsx:191-214` shows only these lists and the generic warning. The native CLI comparison has a richer excluded-run concept (`src/exsim/research/run_compare.cpp:179-194`), so this is an HTTP/UI evidence gap.

Trader consequence: analysts cannot tell whether missing seeds were never launched, failed, cancelled or failed reproduction without reconciling another page manually. A saved experiment comparison URL can select different attempts later without an explicit cohort change. Deterministic repeat values matched here; no actual outcome cherry-picking or wrong numeric result is alleged.

Fix: show an eligibility ledger per planned seed with attempted run IDs, states, selected attempt and exclusion reasons; declare retry selection policy. Allow an immutable comparison receipt identifying selected runs/digests so a published result cannot silently change its evidence cohort. Keep the generic bias warning.

Acceptance: repeat seed 101, cancel a later seed 102, and fail a run. Comparison still counts each seed once, displays all attempts and reasons, explains the older completed seed 102 selection and identifies selected run IDs without hover. A saved receipt remains bound to the original selected artifacts after another retry. Include explicit reproduction-error reasons, not only no-pair messaging.

### VALIDATION-04, P2: baseline/candidate context disappears at the inspection decision

**Observed and source-confirmed friction.** Follow either baseline/candidate cell from comparison into `/runs/run_e284f2e19edbfc9e` or `/inspect`. Heading becomes only “Seed 101”; experiment is an opaque `exp_...` and inspector starts with `run_... / seed 101`. Both alternatives have the same seed and near-identical fills. Recognizing which side is open requires decoding IDs or returning to the experiment.

Source: `web/src/components/research/run-detail-view.tsx:338-346` deliberately renders seed and opaque experiment ID; comparison links at `paired-seed-analysis.tsx:69-70` pass only runId. IDs are valid identities, but poor research labels.

Trader consequence: raises the chance of explaining the wrong alternative while comparing two same-seed executions. Fix: show experiment name, strategy/config digest abbreviation and run ID on summary and inspector, retain a comparison backlink with side designation when entered from comparison. Do not replace exact IDs entirely.

Acceptance: opening either side of the same-seed pair visibly identifies `VALIDATION-REVIEW baseline` versus `VALIDATION-REVIEW candidate`, seed and comparison direction without hovering or another click. Direct run URLs retain experiment name too.

## Capability map and task completion

| Discovered surface | Actual capability / verdict |
| --- | --- |
| `/experiments` | Library, execution/MM task entry, name/task filter, ordered baseline/candidate selection; baseline entry used. Shared global library is expected. No deletion attempted. |
| `/experiments/new?intent=execution` | TWAP/sweep setup, human display units, seed set, latency, fees/risk, scenario/accounting disclosures, service preview and hash; baseline launched through browser, pass. |
| `/experiments/new?intent=market-making` | MM entry and fixed-spread baselines discovered in setup; not independently traded under this validation lens. Other personas must validate strategy/risk behavior. |
| `/experiments/new?from=...` | Config clone/parent lineage, changed-assumption preview; candidate launched, pass. Custom fee rate retains old schedule name, but exact rate is visible and diffed, so no hidden fee defect alleged. |
| `/experiments/[id]` | Planned/completed/active/failed-cancelled/missing counts, run table, assumptions hash, variant/comparison actions; observed 3/3 and 4/4, pass for completion tracking. Repeat and cancelled rows remain separate observations. |
| `/compare` | Baseline/candidate selectors, ordered URL identities, assumptions diff, common seeds, cost/completion cards, per-seed chart/table, aligned equity/inventory/drawdown, metric SD/SE/better-worse; numeric task passes. Eligibility audit incomplete, VALIDATION-03. No claim of live profitability. |
| `/runs/[id]` Summary | Final/partial label, terminal diagnostics, cost/completion/residual, ledger, order denominators, markout eligibility/censoring and latency counts; observed completed/cancelled/failed, pass for interpretation. |
| `/runs/[id]` Manifest | Full translated manifest, derived seeds, models and hashes; inspect assumptions pass, exact build provenance incomplete, VALIDATION-01. |
| `/runs/[id]` State history / controls | Transition reasons, event/time positions and digests. Paused/step/cancel exercised through native API; summary/history browser read. UI resume/pause race not independently exercised. |
| `/runs/[id]/inspect` | Event cursor, exchange vs delivered book, own-order/fill pages, lifecycle timing/FIFO and fill ledger/markouts; real taker fill successfully explained. Passive queue/missed fill/cancel race not independently validated here. |
| `/connections` | Reports real-time protocol `not_running`, no sessions/feed/fault records; read-only, faults external. Correct capability boundary for this research service. Source `research_service.cpp:832-844` returns explicit not_running/empty arrays; this is not evidence of live protocol connectivity. No startup or fault operation attempted. |
| Native API / CLI | Actual JSON artifact reads, exact large seed handling, idempotent run create, controls, native reproduction during comparison. Same-seed repeat accomplished via API. CLI verify documented/source traced, not executed. Browser artifact export/handoff incomplete, VALIDATION-02. |

## Charts, units, denominators and limits

The observed comparison uses candidate-minus-baseline throughout, costs lower-is-better, completion higher-is-better, and directionless position metrics. It reports n, n-1 sample SD, SE and warns SE is not a confidence interval. Exact table strings accompany approximate plots. Zero completion differences are tied and visually have no bar; the y-axis defaults to 0..4 ratio, which wastes resolution but does not alter the explicit zero table. No P0 inferred from this polish issue.

Run summary states filled-quantity cost limits, unavailable residual opportunity cost, USD versus USD/share and the own-trades-inclusive participation denominator. Fill rate definition uses accepted shares including replace increases/cancelled orders. Markout summary gives available/no-mark/censored fill counts and excludes censored totals. Comparison markout rows show total USD and pair n, but do not inline eligible-fill/share counts for each side; add these as a useful P2 evidence improvement when editing comparison, especially for unequal completion. Do not relabel total markouts as per-share alpha.

Aligned means are derived and bucketed, not distribution bands; per-seed selection/table is available. Source `research_service.cpp:1112-1126` averages only present bucket values, without publishing contributing n per bucket. This review's paired completed runs all share the same end bound; changing-cohort behavior was source-inspected, not independently reproduced. Require per-bucket n or fixed-cohort semantics before interpreting curves across differing bounds/missing marks. This is a follow-up boundary probe, not an observed numeric failure.

Not covered: cross-build restart reproduction, native filesystem corruption, cancellation replay, complete native export, compiler/platform determinism, large-seed browser chart, actual real-time gateway/feed faults, historical worlds, sell-side costs, passive MM/risk extremes, multi-user operation, mobile/keyboard/accessibility/unaided usability. No server restart authorized. Source/API inspection used PRODUCT, ARCHITECTURE, API/OpenAPI and RESEARCH_RUNNER only where relevant; no matching/protocol changes proposed, so no broad engine-spec audit claimed.

## Evidence artifacts and handoff

Screenshots saved by preview, under `/Users/jedi/.t3/userdata/browser-artifacts/`:

- `browser-screenshot-127-0-0-1-mw6ptjc6-ad6ad13c.png`: initial empty library.
- `browser-screenshot-127-0-0-1-mw6pv5kp-723f5df3.png`: first completed paired comparison.
- `browser-screenshot-127-0-0-1-mw6py8j5-cc759a0a.png`: later comparison including retry selection.
- `browser-screenshot-127-0-0-1-mw6py8l9-ac0c992a.png`: zero-difference table and aligned chart surface, visually inspected.

`mw6pxynf-d69ad67d.png` captures transient “Computing comparison”, not final results; excluded as completion evidence.

Coordination receipt: review-only report/reproducer committed in this harness checkout; no WORK_PLAN edit because ownership was explicitly limited to PERSONA reports/reproducers. No delegate created. Parent next actions: prioritize VALIDATION-01/02/03, fix VALIDATION-04 while touching run context, preserve native arithmetic/eligibility behavior, run focused checks, commission new independent recheck, then stop for human first-use validation. This review is not that recheck and is not human first-use success.
