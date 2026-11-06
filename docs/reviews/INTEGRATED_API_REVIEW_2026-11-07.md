# Independent integrated API review, 2026-11-07

Reviewed source: `7ca10141852a56a6a784cb4c5aa58213efa57d96`, including durability `795876d` (source `f6b0304`) and execution `bc07370` (source `944ef81`). Harness checkout: `/Users/jedi/.t3/worktrees/lob/t3-755fbb91`, branch `t3/review-exchange-simulator`. Report-only task; no implementation fix is included.

## Result

| ID | Severity | Evidence | Result |
| --- | --- | --- | --- |
| IAR-01 | P1 | Reproduced in private native processes | An intermediate native recorder write failure leaves its run live and can indefinitely starve every later running run. |
| SC-01 | P2 concern | Static only; allocation deliberately not attempted | A corrupt binary-cache count can request nearly 1 TiB before the reader checks whether the file contains those records. |

P1 reflects service-wide execution loss after a persistence error, not duplicate mutation execution. SC-01 is a bounded-reader concern, not a reproduced OOM or exploit. The execution translation and metric paths produced no additional confirmed finding.

## IAR-01: contain intermediate writer failures at the run boundary

**Locations:** `src/exsim/api/research_run.cpp:587-590`, `:721-735`, `:750-763`; `src/exsim/api/research_service.cpp:279-290`. Native dependency: `src/exsim/research/run_artifacts.cpp:477-491`.

`step_driver_locked()` copies the recorder records into API indexes and calls `driver_->drain()` on `FlushRequired`. An exception escapes without marking the run failed. Neither `advance_running_slice()` nor synchronous `step()` catches it. The executor catches only around the *entire* `run_executor_pass()`, logs, waits 200 ms and restarts from the first run. A journal writer whose failure is latched throws again on the next drain. The run retains its simulation slot, live state and allowed actions, with null `terminalDiagnostics`. A broken running run before a healthy run in creation order throws before the healthy run gets its slice on every pass.

This differs from `finish_locked()` at `research_run.cpp:695-715`, which handles terminal API-cache persistence failure, records diagnostics and releases the driver. The existing `executor_terminal_write_failure_does_not_claim_completion` test obstructs `events.bin.tmp` at termination, not an intermediate native drain.

**Executed trigger:** the checked-in `reproducers/integrated_api_2026_11_07/probe.py` launches only private loopback port-0 processes with disposable data. For the failure cases, it ignores SIGXFSZ and sets that child's `RLIMIT_FSIZE` to 65,536 bytes. Metadata fits. A normal baseline with a 600-second simulated bound reaches a recorder flush before the end; `commands.journal` hits the limit and `JournalWriter::append()` fails. This models native journal write failure without filling a disk or changing permissions.

**Observed results:**

- Paused case: the first step advances 100,000 canonical events successfully. The next step fails at 116,364 events / 162,500,155,372 ns with HTTP 503 and `command journal write failed` in stderr. GET still reports `paused`, actions `resume`, `step`, `cancel`, and null terminal diagnostics. A different-key step fails again. The run did not reach its 600,000,000,000 ns bound.
- Background case: create two paused runs before resuming the first. Wait for its native drain failure, then resume the second. After three executor retry intervals, both report `running`; the second still has **0 canonical events and simulation time 0**. Stderr repeatedly reports `executor pass failed: command journal write failed`.
- The uncertain step key correctly remains reserved after restart: same body 503; different body 409. This is **not** an idempotency finding. Releasing that key would be an incorrect repair.

**User effect:** a storage error becomes an indefinitely actionable/stuck run without a terminal I/O-failure diagnosis. One such running run blocks later research runs in the service, including runs whose own simulation/files are healthy. Retrying or cancelling may invoke the broken writer again. Restart loses live simulations under the declared restart contract.

**Remedy:** make unexpected step/drain failures terminal for the affected run under its mutex. Preserve the first exception, accurate available accounting/progress/residual orders and a best-effort failure summary/record before releasing driver/builder. Do not resume or redrain a failed writer. Continue the executor pass for other runs even if saving this failure diagnosis fails. Keep mutation reservations uncertain on post-effect exceptions. Cover synchronous step/cancel and background advancement; preserve these original reproducers and add assertions that later healthy runs advance after repair.

**Related static risk, not executed:** native drain clears the recorder before checking JSON stream health (`run_artifacts.cpp:484-485`). If that check throws, consumed offsets are not reset because `research_run.cpp:589` is skipped. Resuming can call `.subspan(consumed_*)` on a cleared recorder at `:568-585`, violating its precondition. The journal-failure fixture throws before `recorder.clear()`; no crash or sanitizer result for the clear-then-throw variant is claimed. Terminal containment should cover both paths.

## SC-01: validate cache length before allocating from its header

**Location:** `src/exsim/api/research_run.cpp:154-165`, particularly `:159-163`.

The count check permits `header[2] <= (1ULL << 40) / sizeof(Record)`. `std::vector<Record>(header[2])` then value-initializes those records before a read discovers truncation. A valid 24-byte header with a corrupted large count and no body can request an allocation approaching 1 TiB when an inspection endpoint lazily opens it. A thrown `bad_alloc` may become HTTP 500, but process memory exhaustion/termination is also possible. The trigger requires local persisted-data corruption/replacement, not an HTTP count parameter.

No large allocation was attempted; the platform failure mode is unverified. The source confirms allocation ordering. This predates the two child commits but lies on the explicitly requested reload/capacity boundary. The declared absence of exhaustive startup scans does not require trusting a lazy reader's allocation count.

**Remedy:** before allocation, use checked arithmetic to compare count/record size with actual file length, then apply a supported per-artifact memory/count limit. A cheap native regression can write only the header and assert immediate rejection. No new native test requiring compilation was added here.

## High-risk paths inspected with no additional finding

| Area | Independent assessment and evidence |
| --- | --- |
| At-most-once ordering | `research_service.cpp:1122-1204` serializes mutations, persists a reservation before effects, and replaces it with a response before success. Unexpected/5xx errors retain it; known pre-effect rejections release it. Response-write failure leaves memory pending. Key fsync/rename ordering is explicit at `:323-352`. Eight concurrent identical-key experiment creations produced one resource and identical bodies. A validation rejection freed its key; success replayed after restart. |
| Pending/restart | A real failed step's key stayed 503 after process restart and conflicted with a changed body. Pending keys requiring inspection are intentional, not a defect. The new store cannot recover keys already lost by historical old JSONL failures. |
| Startup/capacity | `research_service.cpp:355-524` reads legacy/new keys before run rewrites, rejects conflicting records, enforces key capacity, validates experiment/run IDs, seed membership, history tail state, manifest resource identity, successful-response resource references and required terminal cache presence. Admission rejects key exhaustion before mutation. No new bypass found statically. Exhaustive binary-content validation is deferred; see SC-01 for allocation safety. |
| Existing data | Legacy fingerprints use the same path/stable-body rule as the new store. A legacy-format successful key replayed without rewriting its JSONL bytes. A terminal summary with additive `execution` removed remained readable after restart. Optional execution schedule fields preserve native defaults. Writable old-binary downgrade remains unsupported. |
| Terminal publication | `research_run.cpp:623-719` finishes native artifacts, computes outcome, writes API caches, then publishes the terminal record. Cache-write failure becomes failed diagnostics in memory and attempts a failure record. Partial cache sets may intentionally block startup; no multi-file transaction is claimed. IAR-01 concerns the earlier drain. |
| Concurrency | Mutation serialization is separate from per-run locks; immutable config references survive lookup. No reverse run-to-service-state lock acquisition found in these callers. Responses cannot be observed halfway through publication. `render_experiment()` holds the state mutex while waiting for run state (`research_service.cpp:660-666`), so long comparison/replay work may delay unrelated calls. No latency bound or TSan validation claimed. |
| Translation/identity | `experiment_config_translation.cpp:482-524` maps sides, quantity, start, duration, protection, TWAP interval and limit-only children to native config. Shared risk quantity is the child cap. Native validation checks lot/schedule and arithmetic bounds. Actual executor/build identity is selected at `research_run.cpp:51-84`, `:285`; four buy/sell probes reported the expected identity. |
| Causality/window | Inspected executor/parent-child integration, not matching algorithms. Children use delivered opposite touch, compute/order delays, IOC and one in-flight child. Filled-quantity tracking uses delivered reports. Parent metrics use engine-truth fills/reference mid at declaration, separate from delivered mid. Window coverage uses configured start plus duration independently of completion. All four short probes were censored with observed completion/residual. TWAP stops at its final slice; sweep retries on subsequent delivered data in its window. Sent orders can arrive later due to latency. |
| Exact costs/fees/participation | `research_run_views.cpp:177-203`, `:482-527` preserve doubled integer cost totals and signed fees; six-place ratios/means are derived. Independent Decimal arithmetic summed HTTP fills and reconciled notional, fees, signed costs, shortfall/per-share cost, residual and participation ratio with negative taker fees. TWAP buy: 334 shares, price cost -2.00000, fees -1.0020; TWAP sell: 500, 17.00000, -1.5000; sweep buy: 500, 3.25000, -1.5000; sweep sell: 500, 16.30000, -1.5000. Denominator mapping was traced to market volume at declaration/last fill (`simulation_core.cpp:948-957`, `:1472-1484`); no independent event-by-event volume reconstruction performed. |
| Null/censored values | Before declaration, parent fill values are null. A completed no-liquidity TWAP reported declared parent, zero fills/completion, full residual, null benchmark/cost/participation with `no_parent_fills`. Truncated terminal windows were censored. Markout counters/reference-mid carry behavior were inspected, not independently rebuilt from every event. |
| Comparison/substitution | `research_run_views.cpp:1019-1086` re-executes expected enclosing config/seed, checks canonical native ID/exact config text, verifies replay and compares deterministic native files. Metrics/samples/time come from the fresh core. A same-seed wrong-strategy native directory was excluded immediately and after restart. Existing wrong-seed/config/corruption tests were inspected as claims and their protected paths traced, not rerun. |
| OpenAPI/types | Strategy/metric enums and optional schedule/summary members are additive; old required fields remain. Five-place cost totals use metric `ExactDecimal`; four-place notional/fees and five-place nullable mids stay distinct. Execution is optional for old summaries. Generated types mirror variants/null-only member. No frontend/type build or independent frontend re-check performed. |

## Evidence, commands and limits

Read AGENTS, WORK_PLAN, PRODUCT, ARCHITECTURE, relevant SPEC/SERVER boundaries, API/OpenAPI and runner contracts, integration checkpoint, child reports, scoped source/diffs and durability/execution tests. Old broad protocol/engine findings were not re-audited.

This checkout had no build directory. Probes used a **copy** of the existing integration binary `/Volumes/PortableSSD/Documents/programming/lob/build/release/exsim_api`, copied to `/tmp/exsim-integrated-api-review/exsim_api`. The four scoped production `.cpp` files there matched this reviewed checkout byte-for-byte before copying. Build log: `/tmp/exsim-quality-api-binary-build.log`. Binary SHA-256: `07189df9d380e786b55972a83fc09cd1cd3d7f43dee91393752b3578db720b61`. This reviewer did not compile the checkout or independently certify the complete binary build provenance.

Authoritative command, exit **0** (original-defect assertions intentionally succeed while the defect exists):

```sh
python3 docs/reviews/reproducers/integrated_api_2026_11_07/probe.py --binary /tmp/exsim-integrated-api-review/exsim_api --output /tmp/exsim-integrated-api-review/final
```

Use a new nonexistent output path for a repeat. The harness accepts no external endpoint/data directory. It binds private loopback port 0, closes processes/connections, and preserves fixtures/raw responses. A repair should make the original IAR-01 defect assertions fail; do not weaken them to certify repair. Positive checks follow them in the same script.

Evidence:

- `/tmp/exsim-integrated-api-review/final.log`: authoritative output, also committed as `reproducers/integrated_api_2026_11_07/observed.log`.
- `/tmp/exsim-integrated-api-review/final/*/responses.jsonl`: paths/keys, statuses, parsed/raw bodies and replay headers.
- `/tmp/exsim-integrated-api-review/final/*/server.stderr`: native failure/shutdown messages.
- `/tmp/exsim-integrated-api-review/final/*/data/`: actual synthetic fixtures, native streams/keys, about 4.7 MiB total.
- `/tmp/exsim-integrated-api-review/source-evidence.log`: numbered source snapshot.
- Earlier incremental probe logs/data remain at `/tmp/exsim-integrated-api-review/probe-{1,2,3,4}{,.log}`. These are not independent re-checks or replacement assertions.

Parent API64 release/ASan, HTTP9 and browser13 remain inherited claims, not reviewer reruns. No compilation, heavy/full suite, sanitizer, Linux check, frontend re-check, Docker, benchmark, new delegate, production action, push, PR or merge occurred. Half-price-unit formatting and overflow guards were inspected; probes do not cover every maximum arithmetic bound. No OOM, stream-clear crash, abrupt-kill timing sweep or physical power-loss test claimed.

Accepted limits: one writer per directory; uncertain keys require inspection; active simulation reconstruction is unavailable; no full multi-file power-loss transaction; old executables cannot safely write the new store; startup does not exhaustively validate binary cache contents. These limits alone are not findings.

Next: parent repairs IAR-01, decides/addresses SC-01, runs focused gates serially, and requests any independent re-check as a new delegate. Preserve this report and the original reproducer as evidence.
