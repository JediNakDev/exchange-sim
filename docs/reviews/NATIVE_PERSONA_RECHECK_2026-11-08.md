# Independent native persona recheck, 2026-11-08

Decision: **request changes**. MM01 is not closed on the reviewed implementation. A separate CLI censoring gap and partial-trace corruption issue remain. This is an independent review of the original integrated native repairs, not clearance of the parent's later fix, frontend, deployment, or human first-use criterion.

## Reviewed identity and independence

Review checkout `/Users/jedi/.t3/worktrees/lob/t3-ff6e6207`, base `8e5cb45f0b28d2b72bb18ecd64110c297f897bba`; implementation `5394b84410c4d31d4212163e72b6b99888ae998c`, source `db43a42c8df4b5794c1ebf5a4217cf4d01f52102`. `git diff` against both source commits and the parent checkout head produced no differences for `src`, `include`, `tools`, and Makefile before probes. Parent uncommitted native diff was empty at that point. Read current WORK_PLAN rollup/persona checkpoints, all four PERSONA reports, repair handoff, and relevant PRODUCT/ARCHITECTURE/SPEC/RESEARCH_RUNNER/API/OpenAPI contracts. No shared WORK_PLAN edit.

Actual original parent binaries:

- API SHA256 `7da1c8685bf5f4abf023da9ac993dd8054359599e04c864b2e12c4b92e752a6b`.
- Research CLI SHA256 `62486d9fdd0eef7c16223b97f92505cb024666c027869866e4a6cdcf611eff21`.
- Distinct actual implementation API in original implementation checkout `t3-961565e2`: SHA256 `14c2dc0e97a62c3773525f626735ecd32feaaf56060a8950b4d9d9bed8cc11ff`. Its committed native source equals the reviewed source. This is a second actual build of the implementation, not `/bin/ls`, changed labels, or a changed family ID. No independent rebuild or toolchain reproducibility attestation is claimed.

After parent notified review of its MM01 discovery, preserved exact original API/CLI copies and verified hashes before further launches. Parent's new patch and separate `build/persona-race` binaries were never reviewed or used. MM01 discovery was parent-coordinated, then independently reproduced here. Other findings below were independently discovered.

## Findings

### NATIVE-R1 / P1 / MM01: real fill-before-replace is omitted

Actual slow cancel/replace seed103, own order7: submit event48 at63099715ns references order7; fill69 at105073965ns fills its final39 shares; arrival104 at163120271ns has no exchange order ID; rejection105 is `unknown_order`. Lifecycle reports transit100020556ns but `fillBeforeArrivalEventPosition:null`.

Source `src/exsim/api/request_diagnostics.cpp:51` compares fill identities with the arrival's zero `order_id`, rather than submitted old order7. Zero also must never match an unused zero fill leg. Thus correct505 replace rejects and correct transport statistics do not establish the required causal explanation. Implementation synthetic coverage used nonzero arrival identity and missed the actual native projection.

Repro: fresh slow variant from the recorded MM baseline, change only `latency.cancelLatencyNs` to100000000, run seed103, GET `/orders/7/lifecycle` and events48..105. `mm_probe.py` and committed `evidence.json` preserve exact responses. Expected related fill69, actual null. Require both this actual arrival-ID-zero case and no-related-fill unknown-order case in the NEW fix recheck. Parent reports a fix, but this report does not clear it.

### NATIVE-R2 / P1 / EXEC01 native CLI: censored parent still enters full-window comparison

API correctly excludes the 5s completed run with a31s execution observation deadline. However the actual original CLI command:

```sh
exsim_research compare CENSORED_NATIVE_DIR FULL_WINDOW_CAP10_NATIVE_DIR --json
```

returns exit0, `paired_seeds:["101"]`, `excluded_runs:[]`, `reproduction_checked:true`. Completion has pairs1, means0.132 vs0.48; residual has pairs1, means434 vs260. This promotes truncated observation into the same ordinary completed cohort that the API repair explicitly excludes. Config differences disclose duration, but there is no execution observation exclusion or deliberate exploratory inclusion mode.

Source `src/exsim/research/run_compare.cpp:185` admission checks state/replay/reproduction only; no parent deadline check. Native reproduction is artifact equivalence and cannot establish that a parent window was fully observed. Keep genuine full-window partial completion eligible. Repair CLI admission or explicitly introduce a separately labeled exploratory mode. `run_review.py` reproduces the CLI result after creating its own runs; full JSON retained in scratch. This finding concerns CLI contracts, not browser layout.

### NATIVE-R3 / P2 / EXEC02 partial persistence: corruption acceptance depends on page

For a fresh cancelled TWAP with3 recorded trace rows, append `{invalid-json}` plus newline to its own disposable sidecar. GET `execution-trace?limit=1` returns200, `verification:recorded_terminal_partial`, `totalCount:"4"`; GET with limit1000 returns409 `evidence_unavailable`. Source `src/exsim/api/execution_trace.cpp:29` skips parsing all off-page rows before counting them as evidence.

The endpoint already scans the entire file to count rows, yet a client sees an apparently valid terminal evidence inventory until it reaches the corrupt page. Validate the scanned evidence consistently, or explicitly qualify page-local validation and separately report invalid snapshot integrity. Do not claim native reproduction for partial runs. `restart_probe.py` demonstrates both responses and restores original bytes. Committed `evidence.json` contains the exact counterexample. This is a corruption boundary, not a claim that ordinary cancellation corrupts files.

## Demonstrated behavior and remaining limits

| Requirement | Independent result on original build |
| --- | --- |
| EXEC01 API eligibility | Censored5s parent excluded. Full-window cap10 partial remains paired. Unlaunched seeds and cancelled/failed attempts appear in ledgers. CLI objection R2 remains. |
| EXEC02 actual residual causality | seed102 trace72 rows:30 decisions,21 submitted,9 missing-touch skips; last30s decision target500, known483, headroom17, missing touch, submitted0. Cap10 seed101 trace78 rows:24 ten-share submits,6 missing-touch skips. Last decision knows230, submits10; subsequent delivered report raises known fills to240, residual260. Known-fill accumulation checked against delivered fill reports row by row. Final target30s differs from observation deadline31s. |
| Trace bounds and history | Pages of13 have exact contiguous indices and total counts; sweep1987 rows in pages1000 has in-flight skips and delivered source time no later than decision. Limit1001 rejected422. Removing completed trace sidecar still obtains reproduced trace. Cancelled trace uses recorded partial label and survives restart. Corruption objection R3 remains. |
| EXEC03/MM05 | Actual nonzero neutral ending-position difference -260: better0/worse0/tied0, sample1. Source ties only exact zero. No all-ties shortcut. |
| EXEC04 | Interval0 preview is HTTP200 valid=false with exact `/config/strategy/sliceIntervalNs` and positive/minimum1ns message. Preview200 is intentional, not launch success. |
| FIRSTUSE01 | Fully observed short parents, both5s horizons unavailable: paired markout n0, null mean, tied0. Eligible100ms identical observations remain paired/tied. Baseline MM quantity-weighted totals independently divided by measured shares within six-decimal rounding. A fresh simulation whose aggregate measured markout is exactly0 was not independently constructed; measured-zero helper semantics were source-inspected only. |
| VALIDATION01 | Real byte digest matches persisted provenance; omitted source/release null. Restart and a different actual implementation API build reproduce old selected runs without relabeling old provenance. New run under distinct build persists its actual different digest and explicitly supplied source/release. This is equivalence across two actual same-source builds, not two different releases. |
| VALIDATION02 | Complete13382-event native audit downloaded through indexed chunks using research_audit.py; inventory hashes/size and actual `exsim_research verify PACKAGE/runner` pass. Cancelled and failed packages retain diagnostics, reproductionSupported=false. Invalid offset400; symlink409; sparse file2GiB+1 rejected413. No truncation. Browser unpack not exercised here; parent browser recheck owns that path. |
| VALIDATION03 | Newest eligible selection changes after retry; old pin stable across retry and restart. Ledger contains planned unlaunched, failed and cancelled attempts with reasons. Corrupt pinned native events return409, no substitute retry; original bytes restored. Selected persisted digest receipts are exposed. No malicious rewrite of all native evidence plus metadata was tested; hashes are integrity, not authentication. |
| MM01 | Actual new/cancel/replace rejected counts3/0/505 and replace reasons504 unknown_order +1 post_only reproduced. Request counters/transit fields present. Causality objection R1 prevents closure. |
| MM02 | Real baseline order7 lifecycle now includes rest and priority-reset rows. Original targeted FIFO fixture passes nonzero reset shares ahead, kept reduction and explicit500-row truncation. These last three are implementation fixture corroboration, not independently constructed actual native scenarios; no complete independent MM02 clearance claimed. |
| MM04 | Actual baseline seed103 measured shares135665/133167/124664 reproduced. Summary per-share matches total/eligible shares. Comparison exposes per-share/coverage fields. Not an empirical profitability or capacity claim. |
| Recorder bounds/allocation | Source shows preallocated decision buffer included in flush/clear accounting; push overflow fails recorder_overflow. Original targeted overflow fixture1/1 and allocation guards2/2 pass. No newly instrumented allocation oracle or large-memory stress. HTTP comparison reconstruction retains samples/request evidence and re-executes synchronously; bounded transfer does not mean constant-memory whole-request processing. |

Evidence distinguishes independent actual process probes, source inspection and existing implementation fixtures. Passing fixture assertions alone do not clear unexercised requirements. Further native repair/recheck must address the three findings and any remaining required actual-scenario gaps before claiming all non-human steps complete.

## Reproduction, logs and cleanup

Review-only scripts and compact evidence: `docs/reviews/NATIVE_PERSONA_RECHECK_2026-11-08/`. Reproduce with original immutable binaries:

```sh
python3 docs/reviews/NATIVE_PERSONA_RECHECK_2026-11-08/run_review.py ORIGINAL_API ORIGINAL_CLI DISTINCT_IMPLEMENTATION_API
```

This wrapper copies original binaries, starts ephemeral loopback services with fresh disposable storage, runs all review phases, terminates owned processes in finally, and writes timings/hashes. Finding reproductions assert the observed original bug behavior; a repaired build requires NEW review, not treating this command as a universal pass gate.

Final reproducibility run: `/var/folders/99/cjkytqh130n5xq3vzsr3f17c0000gn/T/native-persona-recheck-vze8ztor`. Five phases exit0: main2.705s, restart1.384s, supplemental0.197s, MM0.049s, cross-build0.541s; subsequent actual CLI compare exit0 demonstrates R2. Root `review-receipt.json`, per-script `*.log`, `*-responses.json`, `mm-original.json`, `cli-censored-comparison.json`, downloaded packages and native-verify.log retain full evidence. Earlier independent scratch `/var/folders/99/cjkytqh130n5xq3vzsr3f17c0000gn/T/native-recheck-final-jcct1nrz` retained; compact original findings committed as evidence.json. `/tmp/native-persona-recheck-reproducible.log` names final scratch. Selected logs and receipt copied alongside probes.

Cheap existing test commands: original implementation checkout `build/release/run_tests research_allocations`2/2 (about8.55ms test time), `... fifo_replace_reset_nonzero_ahead_kept_priority_and_truncation_are_exact`1/1 (1.40ms), `... execution_trace_overflow_fails_instead_of_dropping_evidence`1/1 (3.94ms). No77-case rerun. First parent test binary attempt matched no allocation tests (exit2); switched to verified original implementation fixture binary. This supplies no independent-build claim.

Initial probe development corrected assumed run-create202 to actual201, nonexistent residual summary key to filled quantity, preview422 to actual valid=false200, nested dispersion access and limit-bound400 to actual422. Initial background child was cleaned up by the exec lifetime; final wrapper owns/waits processes within one invocation. A cross-build probe argument-count error was corrected; final complete wrapper rerun passes. No failed attempt is counted as passing evidence.

No production edits, schema changes, installs, Docker, benchmarks, broad suite, new compilation, delegates, installed/private service access or parent output rebuilds. Only review-owned loopback processes/data used, and every owned server stopped. Compiler slot was never needed and is released. No browser/frontend certification, actual human80% test, performance assurance, live-protocol calibration or deployed-service clearance.

Report/reproducer commit is the commit containing this file; exact SHA is returned in review handoff. No co-author attribution.
