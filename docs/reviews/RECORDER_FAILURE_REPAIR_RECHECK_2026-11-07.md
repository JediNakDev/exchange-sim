# Independent recorder repair recheck, 2026-11-07

Reviewed source: `8ee86f1e2ee9fbe10d916b2bcf1f7e60184412de`. Checkout: `/Users/jedi/.t3/worktrees/lob/t3-8c2c07d0`, branch `t3/review-exchange-simulator-repair`. Task: `exsim-20261107-iar01-repair-review-8ee86f1` (full harness ID in WORK_PLAN). GPT/xhigh; no delegates. Report and independent HTTP assertions only.

## Conclusions

- **IAR-01: repaired for the reviewed failure paths.** Native journal and post-clear JSON failures become terminal, retain accurate available evidence, prohibit recorder re-entry and release simulation admission. A younger healthy run advances after the older run fails. The executor's per-run catch and deferred rethrow also support same-pass fairness by source inspection.
- **SC-01: addressed.** The opened file's length, record size and count are validated before vector construction. The 256 MiB payload bound precedes allocation; the writer checks the equivalent count bound before opening a temporary file. Independent malformed-header and sparse oversized-file probes reject safely.
- **No new confirmed implementation defect found in this scope.** Partial terminal caches still require operator recovery and can prevent service startup. That documented limit was independently reproduced below; this is not whole-project or deployment clearance.

The [original review](INTEGRATED_API_REVIEW_2026-11-07.md), its reproducer and its recorded evidence are unchanged. Running that original script still exits **1**, specifically at its old paused-state defect assertion, with a terminal `failed` run. Its positive tail was separately executed verbatim by the new recheck and passed.

## Independently executed evidence

Authoritative command, exit **0**, ending `ALL_REPAIR_RECHECKS_PASSED`:

```sh
python3 docs/reviews/reproducers/recorder_repair_2026_11_07/recheck.py --binary /Volumes/PortableSSD/Documents/programming/lob/build/release/exsim_api --output /tmp/exsim-recorder-independent-8ee86f1-complete
```

The harness pins the executable hash and accepts only a new disposable output directory. All servers bind private loopback port 0. Only the child API process receives `RLIMIT_FSIZE` and ignored `SIGXFSZ`.

| Trigger | Observed repair assertion |
| --- | --- |
| 65,536-byte child file limit; step the 600-second baseline through its first native flush | HTTP 503; `failed`, no actions, original `command journal write failed`. Progress is 116,364 canonical events / 162,500,155,372 ns. Independent arithmetic over all 5,472 HTTP fills gives position **500**, cash **954658.7460**, net fees **-730.8160**; one accepted open order. Summary, diagnostics and history agree. New step/resume/cancel keys return 409 and leave the run unchanged. |
| Older background run fails under that limit, then a pre-created younger short run resumes | Younger run completes with **228 events / 200,000,000 ns**. This directly replaces the original starvation assertion with healthy progress. HTTP timing does not prove both runs received slices in one exact executor pass. |
| 16 MiB child file limit; same long run | Native JSON `write failed in ...` after `recorder.clear()`, rather than journal append failure. Terminal containment, matching accounting/progress and 409 for new step/resume all pass. |
| Cancel after 100,000 events under the 64 KiB limit | Native final-drain error precedes secondary API-cache error in diagnostics; no actions remain. All 4,702 fills reconcile to position **42**, cash **999770.6320**, fees **-629.2120**, two open orders, time **139,400,426,431 ns**. |
| Directory obstructs `run.json.tmp` during paused step or resume | 503, terminal failure, original path in diagnosis, accurate fill-based accounting, same-key retry remains 503. |
| Eight occupied admission slots; queued cancel/activation each has a regular file obstructing `runner/` | Queued cancel returns 503 and becomes failed; activation also becomes failed. A control-write failure frees an occupied slot, the failed queued startup does not consume it, and a third queued healthy run activates paused. |
| Restart journal-failed run with saved summary but no `events.bin` | Run response, cause, digests, progress, history and summary equal their pre-restart values. Events return 500 rather than an empty success. |
| Remove that disposable failed run's saved summary, restart again | Summary returns **409 `artifact_not_ready`**, before and after an events read fails. No zero-accounting reconstruction is published. |
| Header-only near-1-TiB count and `UINT64_MAX`; truncated payload, trailing byte, incorrect record size | Events return 500 with length/layout errors; service remains responsive. No large payload or allocation attempt is constructed from either count. |
| Matching sparse `events.bin`, 268,435,624 bytes including header | Rejected with `256 MiB payload limit`, before vector construction. Only a sparse file is used, not a large in-memory record vector. |

The former reviewer's unchanged positive tail additionally passes all four TWAP/sweep buy/sell exact-cost and negative-fee cases, wrong-strategy/same-seed substitution exclusion before/after restart, legacy summary compatibility, uncertain step-key reservation after restart (503 same body / 409 changed body), concurrent same-key creation, validation-key release, successful response replay, legacy JSONL replay without rewrite, and no-liquidity null/censored metrics. Those assertions are loaded from the original file, not copied and weakened.

## Static review and residual concerns

- **Cleanup:** `research_run.cpp:649-679` sets terminal state before allocating diagnostics; formatting, cache saving and record saving have catch-all protection. Driver and builder resets are outside those guarded attempts. Typed driver destroys its writer before its simulation; `RunArtifactWriter` has a default destructor, `JournalWriter::~JournalWriter()` only closes its descriptor, and standard streams use their default nonthrowing destruction. No hidden call to `drain()` or `finish()` was found during this cleanup. Close may discard unwritten buffered evidence, consistent with the failed-writer contract. Metadata can still be incomplete if allocation itself fails; no allocation-failure injection was performed.
- **Finalization:** `research_run.cpp:681-778` records the native writer exception, then preserves it before a later cache error when terminal state was already failed. Outer step/cancel/advance catches contain earlier finalization exceptions while a driver remains; they avoid a second failure handler after the inner path has released it. Both actual native drain and native summary-finalization errors were exercised. Nonstandard exceptions and allocation failures were inspected, not injected. Pause has the same guarded transition pattern as resume at `:820-848`; pause persistence failure was not deterministically executed over HTTP.
- **Fairness:** `research_service.cpp:266-294` catches each run separately, visits the remaining running runs, then rethrows the first exception. Failed startup does not increment admission. The next pass recounts live drivers. Reporting an error can still add the existing 200 ms retry wait; this is bounded delay, not the old repeated starvation. No scheduling-latency guarantee or mutex fairness claim is made.
- **Restart availability, reproduced accepted limit:** obstruct native `runner/summary.json.tmp` and API `fills.bin.tmp`, then cancel a run stepped 1,000 events. Native cause remains first, all 49 fills reconcile to position **33** / cash **996740.2000** / fees **-6.3900**, and `events.bin` exists while later caches/summary do not. Private restart fails with `terminal run is missing summary.json`, without rewriting `run.json`. `research_service.cpp:505-509` deliberately requires the full cache set when a failed run has `events.bin`. Operator recovery is still necessary; failure containment does not provide automatic restart availability for arbitrary partial persistence. No repair of this fixture was attempted.
- **SC-01 bounds:** `research_run.cpp:159-181` compares count to division of actual payload length, avoiding count multiplication overflow. The cap applies to the shared reader/writer template used by all five cache types; runtime corruption cases here target `events.bin`. Writer overflow refusal and exact-limit acceptance are source-verified only. A 256 MiB *per-cache* cap is not a total-process RAM cap: multiple caches/runs and rebuilt indexes add memory, and live artifact accumulation is separate. No memory-budget or OOM guarantee is inferred.

## Identity, logs and limits

Executable, read in place without modifying parent build output:
`/Volumes/PortableSSD/Documents/programming/lob/build/release/exsim_api`.
SHA-256 verified before execution, inside the new harness and after execution:
`b6f020104ebf1cdf38015c4004187024bce92a17d58e937945623c5e2b2f9b8a`.
All **162 tracked files** under `src/`, `include/`, `tools/exsim_api/` plus `Makefile` match the parent working tree byte-for-byte. Parent HEAD at comparison was `3d932dd32e14f843c76f8f3f9d85d1fa5799f42d` (documentation checkpoint); reviewed source remains `8ee86f1`. This reviewer did not rebuild or independently establish compiler/link provenance. Parent release71, ASan/UBSan71 and `-Werror` remain inherited receipts, not reviewer reruns.

- Authoritative log: `/tmp/exsim-recorder-independent-8ee86f1-complete.log`, also [committed receipt](reproducers/recorder_repair_2026_11_07/observed.log).
- Raw requests, replies, stderr, synthetic data and partial-cache restart rejection: `/tmp/exsim-recorder-independent-8ee86f1-complete/`.
- Source identity manifest: `/tmp/exsim-recorder-independent-source-identity.log`.
- Unchanged original defect probe: `/tmp/exsim-recorder-original-8ee86f1.log`, fixtures `/tmp/exsim-recorder-original-8ee86f1/`; exit 1 at original line 115.
- Earlier full recheck without the added admission/partial-cache cases passed: `/tmp/exsim-recorder-independent-8ee86f1-final.log`. Initial attempt `/tmp/exsim-recorder-independent-8ee86f1.log` stopped at an HTTP pause fixture race: background finalization acquired the run before pause and failed on its obstructed record. That attempt is not pause-path evidence. The unreliable fixture was removed; no original assertion changed.

Read WORK_PLAN, repair/original reports, PRODUCT, ARCHITECTURE, API/OpenAPI, runner contracts and relevant SPEC/SERVER boundaries; inspected scoped changes and parent regression assertions. No compilation, sanitizer/full-suite repetition, Linux/Docker, benchmark, frontend/protocol change, installed-service action, private-data mutation, push, PR or merge. No abrupt-kill/power-loss timing sweep, physical disk-full test, global memory measurement or allocation fault injection. Single-writer and uncertain-key operator reconciliation limits remain.

Next: parent reviews this scoped clearance, completes independent platform/frontend gates, and decides any operational recovery follow-up. Do not promote the old copied candidate or infer whole-project completion from this report.
