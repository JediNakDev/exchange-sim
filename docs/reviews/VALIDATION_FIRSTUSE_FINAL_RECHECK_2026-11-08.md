# Independent validation and first-use final recheck, 2026-11-08

Decision: **request changes before the human test**. No P0/P1 counterexample was demonstrated in this scope. Two P2 objections and one P3 wording objection remain below. This is a NEW expert review of the corrected immutable pair, not implementation continuation, a human study, or clearance of later repairs. Parent should fix the small findings, then commission a NEW affected-scope review. This reviewer changes only this report and review evidence/probes.

## Identity and method

Review base `360ad69529aa57b5e8856391812cc3d1d93c6896`, checkout `/Users/jedi/.t3/worktrees/lob/t3-fa5a42aa`. Actual UI `http://127.0.0.1:45872`, API `http://127.0.0.1:45873`, immutable release `/tmp/exsim-persona-review-360ad69`. Delivery identifies native source `360ad69529aa57b5e8856391812cc3d1d93c6896`, frontend `bc089ea6eba59497dcf0b5034ec89ef2ffe354e4`, BUILD_ID `Q-wBK50aSIFE_xNPhvB0e`, Node `v24.22.1`. Independently hashed copied API: `10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5`; recorded run manifest matches. BUILD_ID bytes hash `6b83cbbfc1d49d4c429770a3b5f599465bf962eecb4c38b5887ffa7200da5bb6`. These checks identify the supplied pair, not independently attested source-to-binary compilation.

Read WORK_PLAN first, PRODUCT, original VALIDATION/FIRST_USE reports, native recheck, repair ledger, native repair and trace/MM UI handoffs, human packet; relevant UI source was used to explain behavior after observation. Other persona results are context, not this review's clearance. This is deliberately informed expert rechecking, not unfamiliar/blind human testing. No new delegates, builds, C++ changes/compilation, installs, Docker, broad suite, benchmarks, deployed/private services, or service/.next restart/stop. All mutations used own `VALIDATION-RECHECK` experiments. No other persona's runs changed.

Preview status returned available=false; mandated preview_open(reuseExistingTab:false) created owned tab `tab_8_e40125e3-02a4-4ae8-98a3-dbbb53b4744f`. Both snapshot calls failed: `Preview automation snapshot timed out after 15000ms.` Switched to authorized existing `/tmp/exsim-browser-tools/node_modules/playwright`, fresh headless Chromium context, 1280x800 and 390x844 CSS viewports. Used accessible role/label locators and keyboard actions. The preview tab and final owned fallback context/browser were closed. A node_repl timeout reset the first fallback kernel during an initial run-navigation attempt; no success is attributed to that attempt. Reopened an isolated context and repeated the route successfully. Final process inventory found no headless-shell user-data-dir process. No server lifecycle was changed.

Evidence directory: `docs/reviews/VALIDATION_FIRSTUSE_FINAL_RECHECK_2026-11-08/`. Text/PNG checkpoints `01` through `30`, full API request/response receipts in `evidence.json`, exact comparison receipt, three compressed complete browser audit downloads, independent evidence verifier, read-only native-browser reproducer and `checks.json`. Text and screenshot are sequential captures, not atomic;29/30 are text-only context checkpoints. DOM text captures strip trailing layout whitespace only; artifact archives and API numeric strings are unchanged. Early create responses intentionally remain queued receipts; later comparisons/summary establish completion.

## Scoped dispositions

| Finding | Status | Independent evidence and limits |
| --- | --- | --- |
| VALIDATION01 exact provenance | Closed for corrected-pair UI/persistence; legacy native migration untested here | `05-manifest`, manifest API receipt, copied API hash all match exact source/release/executable digest. UI distinguishes family/format labels from executable identity and API envelopes. Missing-provenance rendering says unavailable in isolated interception `19`; that is presentation evidence, not an actual historical dataset or second-build experiment. Native cross-build coverage remains the separate review's responsibility. |
| VALIDATION02 complete audit handoff | Closed in bounded completed/partial browser path | Actual browser download -> project unpack -> independent all-present SHA/size verification -> required native CLI verify pass. Full native events, not chart buckets or first API page. Partial package retains evidence and explicitly disclaims reproduction. Limit source-check below; native huge browser transfer untested. |
| VALIDATION03 inclusion/retries/pinning | Objection R1, P2, although original P1 cohort gap is repaired | Three paired seeds, explicit selection policy, exact ledger IDs/digests, superseded/cancelled/failed/unlaunched records, stable old pin after actual browser retry and receipt query binding. R1 falsely classifies unavailable execution deadlines as no execution window. No silent new attempt substitution observed. |
| VALIDATION04 context retention | Closed | `04`, `12`, `14`: visible baseline role, named experiment, TWAP, config `ea7ababd69fb`, exact IDs; run -> inspector -> comparison retains the old runSelection. Candidate summary/inspector also independently show Candidate, named experiment, immediate sweep and its config digest (`29`,`30`); `candidate-context.json` confirms its return preserves the pin. |
| FIRSTUSE01 unavailable versus measured zero | Closed | Short observed parent run Summary + All metrics + comparison distinguish no measured shares from zero. Actual independently constructed measured-zero native pair remains n1/tied1, details below. |
| FIRSTUSE02 semantic field recovery | Closed | Actual native interval0 error focuses visible Slice interval, aria-invalid=true with linked help/error text. At390px input top405.734375/bottom437.734375 inside844px viewport. Repair preserves name/seed; settled review enables launch and clears invalid state. `01`, `21`, `27`. |
| FIRSTUSE03 keyboard explanation | Objection R3, P3; primary repair works | Native fill63/order95/event383 announced; Enter leaves row focused, Tab then Enter reads explanation, return restores row. Event arrow step retains stepper focus; direct order95 link returns to artifact list. Native paging fallback does focus list, but old selected-row wording becomes inaccurate. `12`, `15`, `26`. |
| FIRSTUSE04 Connections guidance | Closed | Actual not_running mode explains separate OUCH/FIX/ITCH service, all three empty states and next actions; research continuation exists. `16`. No protocol service was started or claimed working. |
| FIRSTUSE05 chart accessibility | Closed for DOM names/table path; screen reader untested | Native applications uniquely named Equity (USD), Inventory (shares), Drawdown (USD), plus current-page execution decisions. Associated descriptions give units/aggregation. Keyboard Enter opens exact original sampled Equity table (`10`); comparison paired chart named `Paired difference: Parent completion (ratio)` with paired-count description. Other sample alternatives are present but not each keyboard-walked. Narrow run Summary objection R2 remains independently. |

## Remaining objections

### R1 / P2 / VALIDATION03: unavailable execution evidence is labeled “Not an execution window”

Reproduce comparison baseline `exp_59aba9ec14e219e2`, candidate `exp_6f9635c0ec5d56de`, open Inspect every planned seed and run attempt. Candidate TWAP failed run `run_fc7e28b757f16efa` and baseline cancelled TWAP `run_7ff458b7fbcbf284` both show `unavailable` followed by `Unavailable / Not an execution window`. Both configurations have a parent instruction with start `1000000000` and duration `30000000000`. The failure occurs before declaration; the latter cancellation follows an actual parent fill. Neither becomes a non-execution experiment because its observation metadata is unavailable.

`08-failed-unlaunched.txt`, `browser-repro.json` and API receipts retain exact evidence. Source `web/src/components/research/comparison-audit-evidence.tsx:23` maps every null windowEndTimeNs to the categorical “Not an execution window”, ignoring unavailable status. Selection exclusion itself remains correct. Repair the label to distinguish unavailable/not-yet-observed execution evidence from a genuinely non-parent strategy. Do not infer a window or classify unknown as not applicable. NEW recheck should exercise both failed-before-declaration and cancelled-after-fill rows plus an actual MM row.

### R2 / P2 / narrow execution results: document overflow hides result content

At390x844, direct `/runs/run_b132d7f2f6b40c15` Summary yields document.scrollWidth `967`, innerWidth `390`, reproducible with fresh isolated browser. Parent-order execution and outbound diagnostics cards extend to x967.4375. This is page-wide overflow, not a table scrolling inside a constrained card. Some other cards stay narrow, increasing the inconsistent reading/navigation burden.

`20-run-narrow`, `23-overflow-repro`, `28-overflow-at-summary`, `evidence.json.overflow`, and fresh read-only `browser-repro.json` establish it. Setup, inspector, comparison and Connections all measured document width390. Source run Summary contains responsive `xl:col-span-2` children and large request diagnostics inside its grid; measured overflow identifies the actual impacted cards without claiming a fully isolated CSS root cause. Repair containing grid/card sizing so wide tables scroll locally and all primary result text stays within the document viewport. NEW recheck should cover Summary including native diagnostics, not only inspector/mobile mock fixtures.

### R3 / P3 / FIRSTUSE03 paging fallback: status and return label promise a row no longer on the page

Own native12s MM run `run_7dade5dfdd439f21`: select first fill4/order7/event45 by keyboard, Next fills, then inspect status and return action. The selected row is no longer rendered. Status still says “Read explanation is available on the selected row”; button still says “Return to selected row”. Clicking it actually focuses `Fills and own orders`, which is a sound fallback, but its promised target and Read location are inaccurate.

`26-paging-return`, `browser-repro.json`; source `run-inspect-view.tsx:519,525` uses retained origin state/text while actual focus action checks isConnected. Keep the good fallback and make its label/status truthful when origin is absent. No new navigation feature is requested. Fresh same-page/direct-link checks pass as described above; this report does not self-clear any future wording fix.

## Native research evidence

Browser-created baseline `exp_59aba9ec14e219e2`, candidate `exp_67f44feeb1b1212f`. Baseline seeds101/102/103/105; candidate101/102/103/104/106. Paired exactly101/102/103. Candidate changes TWAP to immediate sweep and taker fee0.0030 ->0.0060; UI lists all five configuration differences including strategy-specific fields/seeds and warns that multiple changes mix effects. No claim that observed differences isolate strategy alone. Baseline config SHA256 `ea7ababd69fb7bb50bb278ff19f69b4418a03a3c4fc13180a28f1ba04ab1ec7b`.

Original seed101 `run_b132d7f2f6b40c15`, browser Repeat this seed creates `run_5769bf8845ba17e9`. Canonical digest `7cbb9aea4f742fa9`, ledger `1e86abeac24f9cfd` match; API manifest digests differ as expected with new envelopes. UI explicitly explains new API identities and comparing event/ledger digests. Same idempotency-key run POST returned same paused run `run_866d04015e206d3d` with replay header, not a duplicate; that attempt was stepped/cancelled through UI. Unpinned comparison selects newest eligible seed101 once; old pinned receipt keeps original. Receipt `query.runSelection` exactly matches pin URL. Old pinned cohort is visible in `06`, including its selected IDs; later attempts remain in ledger. No restart/pin corruption test here, covered only by separate native review.

Cancellation after500 requested canonical events: own `run_7ff458b7fbcbf284`, seed102, stepped through native API then Cancel in browser, retains fill/accounting evidence. `17` explicitly says `Recorded terminal partial evidence. Not reproduction verified.` Downloaded partial audit unpacks with reproductionSupported=false. Early cancelled seed103 package also says false. Failed bound100 run records101 processed canonical events and exact failed_event_bound_exceeded message; candidate seeds102/103 never launched. `08` captured after reload to terminal, not the initial queued transient. Ledger gives “not completed” exclusion plus failed/cancelled state, not the detailed terminal cause; detailed cause is retained in run API. No false success inference from the initial queued capture.

Complete browser export has22420 API canonical timeline records in events.bin and12851 native matching-engine events.jsonl rows. These are distinct event domains; do not equate their counts. Both are complete inventory entries, not the plotted sample or a1000-row truncation. All present files across completed and both partial packages independently match SHA256 and decimal-string size. `checks.json` records actual CLI `/Volumes/PortableSSD/Documents/programming/lob/build/persona-race/exsim_research verify /tmp/VALIDATION-RECHECK-unpacked/runner`, exit0, replay_verified=true, reproduced=true. CLI SHA256 `598ff719788963c03d3d94ddaf3afaea7025434dfea14e53574b7e5cb8ecc442`. The native package includes manifest/commands/events/ledger/summary needed by the verifier; no partial native reproduction is claimed.

`web/src/lib/audit-download.ts` source independently confirms32MiB summed present-byte bound before chunk reads, actionable streaming local command message and no saved package on rejection. The actual UI local-command panel was opened (`18`) and supplies streaming download/unpack/verify instructions. This is source/presentation evidence of the limit, NOT a native >32MiB browser experiment. No huge synthetic allocation or intercepted huge success is used as native clearance.

## Unobserved and genuine-zero markouts

Actual seed901 short baseline `run_cf1893aae6eda7b3`, candidate `run_2b8b4db5fc18ef64`: parent start1s, duration1s, slice100ms, simulation end3s,500shares; only protection5 ->6 changes. Parent observation window is fully observed and8 fills total500shares. At5s horizon eligible fills0/shares0, censored8, total and mean null. Summary says unavailable; All metrics says no_measured_markout_shares; comparison has n0, mean null, better/worse/tied0 and n/a. At100ms both measured totals-6.00000, difference0, n1/tied1. `09`, `11`, complete API receipts.

A genuine absolute zero was independently constructed, not inferred from a renderer fixture: one-share TWAP seed901 fills at `1900115489`ns at100.0200; choose horizon `1099884511`ns so reference lands exactly at3s run end. Run `run_b1771ba83140d3d6` has one eligible fill/share, no censoring, total `0.00000`, mean `0.000000`. Identical independently launched twin `run_efdb674e5ba00a0c` compares with n1, mean0, tied1. Both Summary/All metrics and comparison keep measured zero (`24`,`25`). Two exploratory100ms/1s one-share runs were nonzero and retained in evidence; they are not called measured-zero successes. Horizon chosen using exact BigInt arithmetic from native fill time, no Number conversion of IDs/times. This establishes the boundary case, not a robustness/profitability claim.

## Human packet and20-capability support audit

The tracked `HUMAN_FIRST_USE_TEST_2026-11-08.md` remains DRAFT/blocked, results NOT RUN. No separate finalized facilitator key exists among tracked review artifacts. This report is not that key. Do not freeze denominator, recruit, score80%, or infer human timing/success from these expert actions. Remaining objections and the other persona/native repair rechecks must finish first. Current route support is assessed below; unexercised paths are not passed from handoff/test declarations.

| Packet capability | Current evidence / limit |
| --- | --- |
|1 execution setup/seeds | Actual browser config and paired launches. |
|2 review/launch | Invalid native review then valid review/launch exercised. |
|3 variant | Browser clone preserved assumptions except deliberate strategy/fee/seed changes. |
|4 baseline/candidate direction | Browser Compare with parent plus receipt/pinned links exercised. |
|5 completion/residual/cost | Actual absolute per-seed values and definitions visible; no independent all-fill cost reconstruction in this scope. |
|6 censored observation | Full short parent plus unobserved markouts exercised; long-window censor admission belongs to native/execution recheck, not cleared here. |
|7 residual schedule explanation | Native trace decisions and child links visible in actual baseline; complete independent residual attribution belongs to execution reviewer. |
|8 fill/truth versus delivered | Native fill63 lifecycle/explanation, distinct book panels, keyboard and direct order link exercised. |
|9 MM width comparison | Setup/handoff/source support exists; paired native width experiment untested here. |
|10 MM P&L/risk/markouts | Actual MM runs used for paging; no independent MM numeric reconciliation here. |
|11 inventory skew | Source/handoff path only; untested here. |
|12 latency sensitivity | Source/handoff path only; untested here. |
|13 paired samples/dispersion/confounds | Three pairs, unmatched seeds, fee confound, exact tables and named chart exercised. |
|14 failed/missing/recovery | Native field recovery, failed bound, unlaunched/cancelled ledger and idempotent replay exercised; induced service-error recovery untested. R1 applies. |
|15 pause/step/resume | Pause/step/cancel exercised, Resume button observed; actual resume untested here. |
|16 export/provenance/reproduce | Actual complete browser download and native verifier pass. |
|17 model limits/unavailable | Synthetic disclaimers and Connections separate-mode explanation exercised. |
|18 return navigation | Context/pin run -> inspect -> comparison and direct-link list fallback exercised. |
|19 timing unit switching | Human units/review retained; explicit unit-switch round trip untested here. |
|20 keyboard tasks | Native row/read/return, exact-sample table and event stepper exercised; complete keyboard-only setup flow/screen reader untested. R2/R3 apply. |

This map identifies usable paths and remaining evidence ownership. It does not establish all20 tasks or every critical task passed. Facilitator key still needs exact final-build expected outcomes and controlled service-error/pause/resume scenarios before an actual human session.

## Checks and reproducibility

From checkout root:

```sh
python3 docs/reviews/VALIDATION_FIRSTUSE_FINAL_RECHECK_2026-11-08/verify_evidence.py
node docs/reviews/VALIDATION_FIRSTUSE_FINAL_RECHECK_2026-11-08/reproduce_browser.cjs
```

Both exit0. First independently validates all present artifact SHA/size, native row count, old-cohort receipt versus latest retry, idempotency identities, null versus genuine zero, settled recovery and recorded overflow. It verifies recorded evidence, not future code. Second is read-only, only opens own existing records on the immutable pair and prints the three objections. No mock renderer, installation, server startup or run mutation. `browser-repro.json` is its successful fresh result.

To repeat unpack/native verification, gunzip audit.json.gz to a fresh file, then `python3 scripts/research_audit.py unpack FILE NEW_DIRECTORY` and the required native CLI `verify NEW_DIRECTORY/runner`. Partial package integrity passes with reproductionSupported=false. Original unpacked directories remain `/tmp/VALIDATION-RECHECK-unpacked` and `/tmp/VALIDATION-RECHECK-partial`. The archives are full byte evidence compressed for repository size, not sampled output.

Probe-development correction: initial independent assertion mistakenly expected22420 native engine JSONL rows, confusing API timeline versus engine events. Actual count12851 was checked directly and the assertion corrected; all size/hash checks and native CLI verify pass. The first recovery checkpoint captured a reviewing transient with disabled launch; `27` and recoverySettled confirm eventual enabled launch. MM3s and6s paging probes had106 and182 fills and correctly no next page; clicking disabled Next on6s timed out and is not a pass.12s native run produced real pagination and demonstrated R3. These are automation/probe corrections, not product fixes.

No full test suite/lint/build rerun was needed for review-only evidence. No second-release provenance test, actual historical missing-metadata data, native huge browser export, screen reader, real mobile device, broad seed sweep, all accounting arithmetic, service disconnection, performance, protocol-client connectivity or actual human success certification. Final report/probes commit SHA is returned in handoff. No production, schema or WORK_PLAN edits.
