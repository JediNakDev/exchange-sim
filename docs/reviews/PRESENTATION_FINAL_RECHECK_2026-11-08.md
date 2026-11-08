# Independent presentation final recheck, 2026-11-08

Decision: **R1 closed, R2 closed, R3 closed on the exact supplied artifact**. No request-changes finding was demonstrated in this affected scope. This is a NEW independent review, not implementation continuation, certification of future changes, or a human first-use result. Parent may use these scoped closures with the separate native and trading reviews when preparing the human environment. This report does not itself certify overall readiness or an 80% human outcome.

## Artifact and method

Review base and frontend source `75cf28fcdbee8e82bd71ba20264604ff9dfdac15`, checkout `/Users/jedi/.t3/worktrees/lob/t3-da213789`. UI `http://127.0.0.1:45882`, immutable package `/tmp/exsim-persona-review-75cf28f`, Next BUILD_ID `efg6fmC6FKFd5gxu0E1Hb`. Native remains `http://127.0.0.1:45873`, source `360ad69529aa57b5e8856391812cc3d1d93c6896`. Superseded UI45872 was not used.

Independently verified size and SHA256 for all1490 file entries in delivery.json; delivery SHA256 `2c1dee66607947a282313f451b5b5827ad61e35a3132846f75fd6023f00f6d1c`. Copied API SHA256 `10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5`. Actual existing run manifest matches that native source and executable digest. All23 script assets loaded from the served run route matched the copied package bytes. Delivery declares Node `v24.22.1`; this review did not independently rebuild or attest source-to-binary compilation. Full identity, asset hashes and manifest are in `PRESENTATION_FINAL_RECHECK_2026-11-08/evidence.json`.

Read WORK_PLAN recent rollup, PRODUCT, VALIDATION_FIRSTUSE_FINAL_RECHECK and its evidence/reproducer, repair ledger, and relevant prior trading comparison receipts. Relevant presentation source was consulted to understand controls after initial observation. Prior native final review closes its native R1/R2/R3 and actual nonzero FIFO/kept reduction/zero gaps; those are context, not checks rerun or clearance awarded here.

Required preview_status returned available=false. preview_open created owned tab `tab_a_e40125e3-02a4-4ae8-98a3-dbbb53b4744f`; both subsequent snapshots failed with `Preview automation snapshot timed out after 15000ms.` Closed that owned tab and used the permitted existing `/tmp/exsim-browser-tools/node_modules/playwright`, fresh headless Chromium context. Context/browser closed in finally. No service stop, restart, package mutation, production change, schema change, WORK_PLAN edit, delegate, build, install, Docker, native compilation, broad suite or deployed access. All native/API operations were GETs on existing expert records. No new experiments or runs were needed, and no records were mutated.

## Dispositions and adjacency

| Objection | Verdict | Exact evidence |
| --- | --- | --- |
| R1 unavailable deadline mislabeled not applicable | **Closed** | Failed-before-declaration `run_fc7e28b757f16efa` and cancelled-after-fill `run_7ff458b7fbcbf284` both retain native `windowEndTimeNs:null`, `observationStatus:unavailable`. Their actual ledger rows say `Unavailable / Deadline unavailable`, never `Not an execution window`. Six actual MM rows remain `Not an execution window`. |
| R2 Summary overflows390px document | **Closed** | Execution `run_b132d7f2f6b40c15` Summary, including native outbound diagnostics, now has document width390 at390x844. Every measured card has x16, width358, right374. Primary labels, quantities and explanation text wrap inside the card; screenshots inspected. Wide tables retain local horizontal scrolling to final columns. |
| R3 paging retains false row-return/status promise | **Closed** | MM `run_7dade5dfdd439f21`: keyboard selection of first fill4/order7/event45 retains row focus; Tab reaches Read explanation; Enter focuses explanation region; keyboard Return focuses originating TR. Next fills changes page, retains exact selected-fill explanation, and changes both status and return label to the actual list fallback. Return focuses `Fills and own orders`. |

R1 adjacency: native trading comparison attempt ledgers and runSelection tokens match their prior recorded receipts exactly for both censor and width comparisons. Observed parent rows remain60s/31s; actual completed censored run `run_0d1471c29bd157e0` remains5s/31s, excluded. The old validation pin retains the exact runSelection URL and still selects `run_b132d7f2f6b40c15` despite the later retry. Unpinned ledger still selects the later eligible attempt and excludes failed/cancelled attempts. This is read-only persistence/selection evidence, not a fresh restart test.

R2 adjacency: actual MM Summary at390x844 also has document width390 and cards x16/width358/right374. At1440x900 both execution and MM documents measure1440; full-width diagnostics align at x256/width1152/right1408, while Ledger and Orders/risk form568px columns at x256 and840. Charts retain three aligned columns. Native diagnostics table is913px scroll width inside a320px local container on mobile; scrolling593px exposes final `Mean (ns)` column. Markouts, latency and All metrics also scroll locally to their final columns. Exact floating-point bounds, per-table widths and final-column visibility are recorded. Desktop narrow markout cards retain local scrolling too. Primary mobile text and desktop layout were visually inspected; no claim is made about every viewport or a real mobile device.

R3 adjacency: after Next, status is exactly `Selected fill 4, order 7, event 45. Its explanation is above. Return goes to fills and own orders.` Label is `Return to fills and own orders`, focus lands on the labeled DIV list container. Previous restores the first page and retains truthful list fallback until explicit reselection; reselection restores `Return to selected row`. Switching to Own orders clears row-origin language while retaining the selected-fill explanation, and Return again focuses the list. Switching back/reselecting works. Direct `/inspect?order=7` says `Selected order 7. Its lifecycle explanation is above.` and returns to the list. Next event retains focus on the `Next event` BUTTON; no forced focus to explanation was observed. These are DOM/keyboard focus assertions, not a screen-reader test.

## Evidence and checks

Evidence directory: [PRESENTATION_FINAL_RECHECK_2026-11-08](PRESENTATION_FINAL_RECHECK_2026-11-08/). `evidence.json` contains identities, raw unavailable comparison, exact comparison ledgers/tokens, layout geometry, scroll positions, selected explanations, statuses and focus targets. Numbered text/PNG checkpoints show unavailable/MM/censored ledgers, both mobile/desktop summaries, same-page selection, paged fallback and direct-order navigation. PNG and text captures are sequential, not atomic. Text checkpoints strip trailing layout whitespace only. Mobile primary PNGs are viewport captures positioned below the sticky header; full-page Summary PNGs retain the complete page.

From checkout root:

```sh
node docs/reviews/PRESENTATION_FINAL_RECHECK_2026-11-08/probe.cjs
```

Final run exited0 in7.3s:53 assertions passed, zero page errors, zero observed browser responses with HTTP status>=400. Exact output is `probe.log`. Inventory/served-byte checks additionally assert each file/asset directly. The count of local-table assertions can vary as unrelated trace data settles; each observed overflow table must reach its final column. Probe is read-only against these records and writes only its owned evidence directory. It does not launch or stop a server.

Probe-development correction: initial direct native manifest request used `/api/runs/...` and returned404. Corrected to the documented native `/v1/runs/...` route before successful checks; that attempt is not a product defect or pass. Initial element screenshots could include the sticky header over the top of a long card; final primary viewport captures were positioned below it. No product was changed to make the probes pass.

No new export/native reproduction, broad numeric reconstruction, zero/null/core-validation suite, screen reader, human participant, performance test, real mobile device or complete capability audit was run. Those independent earlier scopes are preserved, not silently rerun or expanded. All three current presentation objections are closed only for this frontend/native pair. Later fixes need their own affected-scope review; parent owns future implementation and rechecks.
