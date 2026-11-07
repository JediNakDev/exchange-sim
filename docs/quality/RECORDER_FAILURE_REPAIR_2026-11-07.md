# Recorder failure containment and bounded inspection caches

Independent integrated API review `05c0f71`, merged as `df01ab7`, reproduced
IAR-01: a failed intermediate native recorder drain left a run actionable and
could starve later runs in the executor. SC-01 identified allocation from an
untrusted cache count before checking the file length. The original report and
reproducer remain unchanged in [the review](../reviews/INTEGRATED_API_REVIEW_2026-11-07.md).

## Resulting behavior

- Unexpected startup, simulation, recorder or control-persistence errors make
  the affected run terminal and release its driver and event builder. Failure
  handling never drains or finishes the failed native writer again. Available
  accounting, progress, order state, digests and the original cause are saved
  while the simulation still exists. Saving that evidence is best effort if
  storage also rejects the failure record.
- The executor gives every healthy running run its slice before reporting the
  first exception. A failed run frees its simulation admission slot. Existing
  uncertain mutation keys stay reserved; a new step/resume cannot re-enter a
  failed recorder.
- A small saved summary can survive failure to write larger inspection caches.
  On restart, its accounting and the saved run history remain available. Missing
  event evidence produces an error rather than an empty successful log. If no
  summary survived, the summary endpoint returns `409 artifact_not_ready`.
- Cancellation preserves the first native writer error when API cache writes
  also fail. A queued cancellation whose simulation cannot even start becomes
  failed as well.
- Binary cache readers check the opened file's actual length and record size
  against its count before allocating. Truncation, trailing bytes and counts
  near 1 TiB or `UINT64_MAX` fail without allocating from that count. Matching
  files above the supported 256 MiB payload limit are rejected. Writers enforce
  the same limit before creating a temporary cache file. [API contract](../API.md).

## Focused verification

The focused binary contains `tests/main.cpp` and the five `tests/exsim/api/*.cpp`
files. All 71 registered cases run without a filter, including the existing
17 durability and 11 execution cases plus five new I/O and two cache-bound cases.
Builds use `-j2 -Werror`; no old assertion was weakened.

| Gate | Result | Exact log |
| --- | --- | --- |
| Release API invariants | 71/71; summed case timings 3.137 s | `/tmp/exsim-iar01-release.log` |
| ASan/UBSan API invariants | 71/71; summed case timings 5.452 s | `/tmp/exsim-iar01-debug.log` |
| Release / sanitizer compilation | Passed, zero warnings | `/tmp/exsim-iar01-build.log`, `/tmp/exsim-iar01-debug-build.log` |
| Production API binary | Built, zero warnings | `/tmp/exsim-iar01-api-build.log` |
| Empty filter | Exit 2 | `/tmp/exsim-iar01-empty-filter.log` |
| Original independent HTTP reproducer | Original defect assertion fails with terminal `failed` state | `/tmp/exsim-iar01-original-after.log` |

The unchanged independent HTTP probe now observes 116,364 processed canonical
events, simulation time 162,500,155,372 ns, residual position 500, one open order,
no allowed controls and `command journal write failed` diagnostics. It exits 1
at its original assertion that the broken run remains paused. This is the
expected invalidation of that defect assertion, not a claim that the rest of
that script ran or that a fresh independent review has cleared the repair.
Its disposable server/data and raw replies are retained at
`/tmp/exsim-iar01-original-after/`.

The tested API executable SHA-256 is
`b6f020104ebf1cdf38015c4004187024bce92a17d58e937945623c5e2b2f9b8a`.
The seven new regressions exercise actual filesystem and native recorder
failures. Child-only `RLIMIT_FSIZE` and ignored `SIGXFSZ` leave the test runner's
limits unchanged. The post-clear stream failure specifically checks its native
JSON write error; the small journal fixture exercises the earlier drain error.
The oversized file fixture is sparse and does not construct a large payload.

## Remaining checks and limits

Fresh independent review `2a2bd1b`, integrated as `5c97846`, cleared IAR-01 and
SC-01 within its stated scope, with no new confirmed defect. Its private HTTP
probes independently reconcile fill-based accounting, restart evidence, uncertain
keys, native journal/post-clear failures and safe malformed-cache rejection. The
original positive accounting/idempotency assertions also pass unchanged.
Same-pass fairness and pause-persistence failure have source-level independent
coverage; the parent's native tests cover same-pass fairness at runtime. The
review reproduces the accepted partial-terminal-cache startup rejection rather
than claiming arbitrary storage failures always permit restart. See
[independent report](../reviews/RECORDER_FAILURE_REPAIR_RECHECK_2026-11-07.md).

Fresh [Linux GCC/Clang checks](LINUX_RECORDER_REPAIR_2026-11-07.md) now pass
71/71 each with warning-free API builds and failed empty selections.
The earlier Linux64 receipts apply to `7ca1014`, not this changed backend. The
frontend82 checks remain evidence for its unchanged source; frontend independent
selected-fill clearance is still pending after the last reviewer hit a limit.

No full engine/protocol suite, new performance measurement, production restart,
real-data mutation or deployment occurred in this repair. There is still no
power-loss transaction, multi-writer support, reconstruction of a lost live
simulation or automatic uncertain-key reconciliation. An incompletely persisted
terminal cache set can still require operator intervention at startup. The old
copied `7ca1014` candidate must not be promoted; a fresh paired release is needed.
