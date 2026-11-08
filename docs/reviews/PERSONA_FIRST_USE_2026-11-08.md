# Independent fourth persona: unfamiliar trader

Decision: request changes before human evaluation. Expert heuristic/browser review only, not a human 80% success result. The basic execution flow works; it does not need replacement. Repair the misleading unavailable-observation comparison and recovery/discoverability gaps below, then commission a NEW independent recheck of the actual repaired build. This report cannot clear future changes.

## Identity, independence and boundaries

- Checkout `/Users/jedi/.t3/worktrees/lob/t3-111b79c5`, base `a43cb8b0a638105afc9db24afce323b09400df1b`. Documentation and review evidence only. Parent should add this task/commit to WORK_PLAN; that shared file was not edited under the exclusive report ownership instruction.
- Target only `http://127.0.0.1:45772`, isolated native API 45773. Supplied served source `d61c329`, build `ZbCMVWqStD4QzLA8AwzWD`. Identity is supplied, not independently attested. Source references below are from checkout a43cb8b and are separately labeled; parent uncommitted fixes were not served or reviewed.
- Prior knowledge: task brief, trader domain knowledge and required WORK_PLAN. Accidentally read 180 lines of WORK_PLAN rather than only its current rollup, including historical integration records, but no persona findings there. No prior persona report, product/spec, source or human packet read before initial UI observation. Names of other personas' experiments were necessarily visible in the library; their results were not opened or changed. PRODUCT, relevant source, execution persona report and parent's draft human packet were consulted only after the initial UI pass ended.
- Browser observation began at library, then execution setup, invalid review, repair and launch, results, fill/order inspection, variant and parent comparison. Also MM setup, paused/partial/cancelled state, keyboard and narrow viewport. All creations and run mutations used UI only. No source edits, deployment, installed data access, compiler/build/install, new server, broad suite, or delegates.
- Created `FIRSTUSE-REVIEW TWAP` exp_fd2f08f19ca37165, run_3725454d3f7f6f3f, seed 901; `FIRSTUSE-REVIEW wider protection` exp_050f81a6558521b2, run_7dfa62b014586417, seed 901; `FIRSTUSE-REVIEW MM paused` exp_637e1eed01b1ded8, run_ae5052bf56a0bdd3, seed 902. Only the last run was stepped/cancelled. Other persona records preserved.
- Device coverage: 1280x800, 1024x768 desktop, 390x844 CSS viewport. Mouse, Tab/Enter, ArrowRight and Shift+ArrowRight, skip link. No real mobile device, screen reader or human participant.

## Findings

### FIRSTUSE-01 / P1 / observed + source-confirmed: censored markouts become measured ties

Repro: create TWAP with 500 shares, start 1 s, window 1 s, interval 0.1 s, end 3 s, seed 901. Create variant changing only protection 5 to 6 ticks. Open Compare with parent. Run summary says 5 s horizon has 0 available, 8 censored fills and total unavailable. Comparison Metric differences instead shows Markout total @ 5 s: n=1, mean 0, tied 1. Run All metrics also displays that unavailable total as exact zero.

Evidence: `evidence/firstuse-2026-11-08/05-run.json`, `08-compare-mobile.json`. Source: `src/exsim/api/research_run_views.cpp:173` unconditionally emits markout totals; `web/src/components/research/run-detail-view.tsx:199` guards the dedicated table with availableFillCount but the general metrics/comparison path does not preserve that distinction. This duplicates EXEC-01, discovered before reading that report.

Consequence: a trader can call two entirely unobserved adverse-selection outcomes equal. Fix: propagate eligibility into general metric and paired comparison values, excluding unavailable horizon pairs; retain available/censored counts. Acceptance: these two own runs show n=0/unavailable for 5 s, no tied observation or zero mean; genuinely measured zero remains n=1/tied. Partially eligible horizons disclose coverage. No new engine capability is required.

### FIRSTUSE-02 / P2 / observed + source-confirmed: semantic validation does not locate the input

Repro: execution setup, enter Slice interval 0, Review assumptions. Service rejects it as `/config rejected by the research runner: slice_interval must be positive`. No input has aria-invalid or a linked field issue. The generic error lives in the assumptions area below the form; internal snake_case differs from the visible label. Restore 0.1 s and review: valid, launch succeeds.

Evidence: `03-validation-settled.json`, `preview-validation.png`, `01-validation.json` (transient Reviewing state). Source: `experiment_config_translation.cpp:672` generic exception maps to root, while `experiment-create-view.tsx:230` groups by exact fieldPath and `:447` expects `/config/strategy/sliceIntervalNs`. This independently duplicates EXEC-04.

Consequence: recovery needs internal-name translation and hunting through a long form. Fix: return a specific field path with plain label/unit, focus or link the first invalid input and associate its error text. Acceptance: zero is unlaunchable; Slice interval is marked invalid, announced and reachable from summary; fixing it clears the error without losing other assumptions.

### FIRSTUSE-03 / P2 / observed + source-confirmed: selecting a fill leaves its explanation above the user's focus

Repro: own TWAP Inspect events, scroll to fill list, click match 64; Tab reaches match 71, Enter updates event position to 479 and explanation to Order 119/Fill 71. Focus stays in the bottom table. The explanation is inserted above that list, beyond the current viewport; there is no live announcement or explicit jump action. Mobile compounds this with a long event/lifecycle page.

Evidence: `06-inspect.json`, `07-keyboard-fill.json`, `preview-inspect-mobile.png`. Source: `run-inspect-view.tsx:570` fill rows only set selected data/cursor via focusFill; no scrollIntoView or focus/announcement of explanation. Keyboard activation itself succeeds and is not reported broken.

Consequence: a first-time trader may think selection only changed the event counter or must scroll backward to discover the substantive answer. Fix: provide a visible selected-fill summary with a focusable “Read explanation” action, or move/focus the explanation in a predictable accessible region. Acceptance: keyboard selection announces the selected fill and makes its explanation reachable in one explicit action while retaining a route back to the list. Do not force a disruptive focus jump on every event step.

### FIRSTUSE-04 / P3 / observed + source-confirmed: Connections is an empty boundary with little recovery guidance

Repro: Connections on this isolated service. Header reports `real time protocol service not_running`, read only. Order entry sessions and faults render headers with no rows. Fault text correctly says configuration is outside the UI, but there is no explicit empty-state explanation or next action.

Evidence: `10-connections-actual.json`. Source: `connections-view.tsx` maps empty arrays directly into table bodies.

Consequence: novice cannot distinguish “not running here”, “no clients” and “unsupported in research mode” from an incomplete load, or discover where to proceed. Fix: explicit no-sessions/no-faults states, distinguish research from real-time protocol mode and link to relevant setup documentation or back to research. Acceptance: isolated not_running service explains why tables are empty and how to continue without implying the UI can launch a gateway. No missing engine/protocol capability is inferred.

### FIRSTUSE-05 / P2 / observed DOM + source-confirmed: identical unnamed chart applications

Repro: run summary keyboard sequence reaches three SVG role=application elements, each tabindex=0 and no aria-label. Visible headings distinguish Equity, Inventory and Drawdown but the interactive chart roots have no explicit accessible name. This was DOM inspection, not a screen-reader usability test.

Evidence: `20-mobile-run.png`, source `series-line-chart.tsx` renders a titled Card but does not associate that title with LineChart. Run chart output contains no exact time-series table alternative. Axis values/units and bucket aggregation are otherwise visible and useful.

Consequence: assistive users must infer which application they entered from reading order. Fix: give each chart a stable name linked to its title and unit/aggregation description; offer exact sampled values in a keyboard-readable alternative when needed. Acceptance: accessibility tree uniquely names each chart and keyboard can inspect a sample's time/value. Do not claim screen-reader compatibility solely from tabindex.

## Capability and coverage map

Evidence files below are under `docs/reviews/evidence/firstuse-2026-11-08/`. Observed means this expert exercised the path, not human success. A single seed is deliberately a small demonstration.

| Capability from human packet | Entry / outcome / coverage |
| --- | --- |
| Execution setup and seeds | Library Research execution -> labeled time/share fields; exercised 500 shares and seed 901. |
| Review and launch | Review assumptions -> service summary -> Create and start; exercised invalid then valid launch. |
| Variant preserving assumptions | Results Create variant copies source; changed protection only; comparison confirms one difference. |
| Baseline/candidate direction | Variant Compare with parent prefills original baseline/candidate correctly; no ID memorization. |
| Completion/residual/cost signs | Results prominently 500/500, residual 0, $5 incl fees; benchmark and filled-only limitation explained. Expert read, no human interpretation score. |
| Censored windows | Markout censoring observed; parent execution-window censoring not exercised. General metric bug FIRSTUSE-01. |
| Residual schedule explanation | Not exercised; no claim of schedule decision coverage. Prior execution persona reports missing schedule evidence, read after observation. |
| Fill lifecycle / delivered truth | Click fill 64 and keyboard fill 71; submit/arrival/ack/fill reports, ledger, truth/delivered book and censored markouts shown. Own orders tab and order 109 also selected. |
| MM quote width comparison | Library MM route and form discovered; comparison between widths not run. |
| MM P&L / inventory / eligibility | Own paused/stepped/cancelled MM run shows partial accounting, unavailable marks, retained -85 shares and 1 open order. Final completed MM inference untested. |
| Inventory-aware skew | Quoting-model entry visible; fields source-confirmed; not exercised. |
| Latency sensitivity | Named collapsed Latency section exposes separate paths with human units; source-reviewed, no sensitivity run. |
| Pairing / dispersion / confounds | Compare shows seed 901, n=1, one changed assumption, no SE for one pair; exercised. |
| Failed/missing work recovery | Semantic validation repaired; network-error and failed-run recovery not injected. |
| Pause / step / resume | Created paused, Step events progressed 0 -> 103, Cancel preserved residuals. Resume visible but not activated. |
| Export / reproduce / share | Manifest tab visible, no dedicated export/reproduce path verified. Critical packet task remains unproven. |
| Model boundaries | Synthetic single-venue warnings in library/setup/results; Connections correctly read-only; historical/live research not implied. |
| Back to experiment/comparison | Results use opaque experiment-ID link instead of name, but link exists; variant parent comparison works without memorization. |
| Timing unit changes | Human units displayed (s/ms/µs); conversion preserved 100 ms in variant. Explicit unit-switch round trip not tested; markout horizon input still raw comma-separated ns under Accounting. |
| Keyboard key tasks | Skip link focuses research-content; fill Tab/Enter; stepper ArrowRight 0 ->1 and Shift+ArrowRight ->26. Entire setup/compare keyboard-only completion untested. |

Useful hierarchy: execution/MM goals are visible before the library table; advanced assumptions have meaningful sections; review and launch are distinct; results prioritize task-specific metrics and next decisions. At 1280 desktop chart units and scale are readable. At 1024 and 390 layouts stack; tables scroll locally without observed document-wide overflow. Narrow inspection is long and needs horizontal table exploration, but controls wrap rather than disappearing. No generic cosmetic redesign is requested.

## Human packet readiness

After observation, read parent `/Volumes/PortableSSD/Documents/programming/lob/docs/reviews/HUMAN_FIRST_USE_TEST_2026-11-08.md`: draft, 20 capabilities, 16/20 plus every critical capability, NOT RUN. Its blocked status is correct. It cannot honestly be marked ready from this pass. FIRSTUSE-01 undermines critical markout interpretation; FIRSTUSE-02 undermines safe recovery. The packet's critical schedule explanation and export/provenance/reproduction tasks need demonstrated UI paths and a facilitator answer key before denominator freeze. Absence of a verified export UI does not prove absence of API artifacts. Scope must be implemented or explicitly revised with the user, never silently counted as a pass. The packet gives goals rather than clicks, which preserves unaided observation. Viewport and keyboard expectations should be fixed in the protocol; no expert timing here predicts a human five-minute launch.

Complete ALL outstanding non-human persona/fix/recheck steps before human evaluation. Parent source fixes must be served in a new isolated build and independently rechecked. This report neither measures nor certifies the 80% human criterion.

## Evidence, automation limits and checks

Preview status initially returned `available:false`; required open with reuseExistingTab:false succeeded. Owned tab `tab_7_e40125e3-02a4-4ae8-98a3-dbbb53b4744f`. Snapshots returned only URL/screenshotPath, no accessibility refs, so inspected DOM selectors were used. Preview evaluate once failed: `Preview automation evaluate produced a result larger than 64000 bytes.` Retried with a bounded focus description successfully (`02-validation-settled.json` preserves failure, `03` the observation).

Some preview actions returned errors that were initially not printed because return values were discarded. They are not claimed successful; subsequent snapshots exposed unchanged routes. A nonexistent guessed comparison selector failed; actual Compare with parent worked. Mobile navigation selectors needed disambiguation. The two explicitly captured failures were:

1. `Preview automation click received an invalid selector (30 characters). selector takes CSS; use locator for aria-ref=<ref>, role, or text targets.`
2. `Preview automation click timed out after 2000ms. The click may have happened and removed its target or navigated; check the page before retrying. No element matches "a:has-text('Research market making'):visible" after the timeout; take a fresh snapshot and use its locator.`

Fallback used existing `/tmp/exsim-browser-tools/node_modules/playwright`, an isolated new Chromium context. No install. Initial root require failed MODULE_NOT_FOUND; ES-module import failed `The requested module './index.js' does not provide an export named 'default'`; createRequire succeeded. No browser app defect inferred from automation failures. Both owned preview tab and Playwright context/browser closed before handoff.

`01` through `08` JSON files capture preview observations; `09-connections.json` is a mislabeled unchanged comparison snapshot, retained as a failed-navigation receipt, not connections evidence. `10-connections-actual.json` is the actual boundary observation. `11` through `20` text/PNG pairs capture fallback checkpoints. `14-step` and `16-cancel-action` are immediate action checkpoints; tool reads immediately afterward observed transient Loading summary. `15-partial` and `17-cancelled` were captured after waiting for that text to disappear. Text and PNG capture are sequential rather than atomic and may span an asynchronous update. PNGs are viewport captures, text captures full body. Copied preview PNGs document desktop results, validation, narrow inspection and comparison. Review evidence is local and does not attest production.

Checks: scoped native browser interactions only; no build, install or suite run. No induced network errors, service downtime, retries/idempotency mutation, failed run, library-empty dataset, screen-reader, all-fill arithmetic, calibration, protocol client, seed sweep robustness or production readiness clearance. Frozen initial observations precede source reads; later source checks are explanatory, not browser rechecks of parent fixes.
