# API durability and restart integrity, 2026-11-07

Implementation task: API-DURABILITY-2026-11-07. Harness checkout:
`/Users/jedi/.t3/worktrees/lob/t3-a3d1cea4`, branch
`t3/api-durability-idempotency`, base
`6d5c29f647399a52596741b380bbabee2a2bd373` on exchange-simulator.

This task implements and tests service persistence/reload fixes. It does not
claim an integrated parent feature or a deployed release. The delivery audit's
DA-01 and DA-02 were independently reproduced using disposable directories.
DA-05's distinction between process restart and power-loss durability remains.
No production data, service lifecycle, frontend, OpenAPI, matching engine,
runner implementation, strategy translation, native driver switches, benchmark,
push, PR or parent merge was touched. No delegates were launched.

## Confirmed defects and changes

| Finding | Before | Implemented behavior and regression |
| --- | --- | --- |
| DUR-01: mutation before replay protection | The append stream's open/write/close state was unchecked and exceptions were suppressed after resource creation. A failed append followed by retry after restart created two experiments. | Persist a key reservation before executing. Save the successful response by atomic replacement of that key's file. Both stages check open, flush and close, then fsync the temporary file, rename and fsync its containing directory. A pre-execution store failure returns 503 without creating anything. A failure after execution retains the reservation and returns 503. Tests obstruct the store and inject an actual `RLIMIT_FSIZE` write failure after experiment creation, then use `_exit` without service shutdown and instantiate a fresh service. |
| DUR-02: missing/malformed state | Missing members could take a null dereference path. Parse/load failures were skipped while the service started normally. Torn legacy journal lines were discarded, potentially freeing an already-used key. | Typed field and schema/identity checks reject malformed metadata. Legacy JSONL is read without rewriting it; malformed or conflicting records, including torn tails, fail startup with diagnostics. New files validate key hashes and response shape. Successful responses must reference an existing resource. Child-process probes isolate the old loader's crash behavior and verify preservation of the corrupt bytes. |
| DUR-03: capacity on reload | The configured key bound was checked only for new requests, after an unbounded retained-key reload. | Reservations and successful keys count toward the same bound. Reload refuses a configuration smaller than its retained key set instead of dropping keys or silently exceeding capacity. Existing keys still replay when capacity is full. |
| DUR-04: suppressed control persistence | A step could return 200 after its run record write failed. The success was saved as an idempotent response although that step's persisted history was stale. | Transition and step persistence errors propagate to the service, which keeps the reservation and returns 503. An obstructed `run.json.tmp` proves that the first step applies in memory, its retry does not advance again, and retry after restart remains blocked. |
| DUR-05: terminal publication and errors | Terminal state was persisted before inspection caches. Cache write errors were suppressed, so cancellation/completion could appear successful with partial files. Missing `events.bin` could cause reload to rewrite a completed record as failed. | Write inspection caches before publishing the terminal record. A failure changes the in-memory run to failed with explicit persistence diagnostics, attempts to save the failure record, releases its simulation and propagates the original failure. Synchronous requests return 503; executor failures are observable as failed runs. Startup refuses incomplete completed/cancelled caches without rewriting their records. Injected `events.bin.tmp` failures exercise both paths. |
| DUR-06: failed diagnostics rewritten on every restart | A failed run without inspection caches was treated as newly interrupted on every load, replacing diagnostics and appending more failure history. | A saved failed run without caches retains its original diagnostics/history. A terminal-write-failure test reopens the directory twice and checks identical saved record bytes and diagnostic values. |

Request validation and invalid-transition rejections release their reservation
before returning the original 4xx error. Unexpected failures retain it. A key
with an uncertain outcome returns 503 for the same fingerprint and 409 for a
different request. It consumes capacity until explicit reconciliation; there is
no automatic expiry or unsafe retry. Successful experiment and run responses
replay byte for byte across restart, including the original creation state.
The current state remains available through GET.

## Interfaces, storage and integration coordination

No public C++ signatures, HTTP schemas, OpenAPI operations, error codes,
capacity defaults, canonical ordering or native comparison rules changed.
`ResearchRun` controls and executor methods now propagate persistence errors
instead of suppressing them; comments document this exception behavior.
`ResearchService::shutdown` catches per-run persistence exceptions so destruction
still completes. Existing in-memory accounting is retained on terminal write
failure rather than being replaced by an empty summary.

Private service helper `append_idempotency_record` is replaced by
`persist_idempotency_record`, with `remove_idempotency_reservation` for known
request rejections. The existing response structure uses status zero internally
for a pending reservation, never as an HTTP response.

New records live at `idempotency/<sha256(key)>.json`, schema
`exsim.api.idempotency-record/1`, containing `key`, `fingerprint`, `status` and
`body`. Each key has one committed file; `.tmp` files are uncommitted and ignored
on reload. An incomplete response replacement leaves its reservation intact.
The legacy `idempotency.jsonl` remains readable and unchanged. There is no bulk
migration, deletion, guessed key reconstruction or rewriting of old experiments.
Corruption stops startup instead of presenting a healthy partial restore.
Operators must retain a copy and reconcile the diagnosed record; blindly deleting
it would discard duplicate protection.

The execution API task owns capability/baseline additions and strategy driver
switches. This change leaves those sections intact. Following the parent's
2026-11-07 audit steering, `research_run.cpp` edits are restricted to load,
transition, terminal/step persistence and abandon cleanup, all below the driver
switches. The service changes cover persistence, mutation sequencing and shutdown
error handling. Parent should cherry-pick these hunks together, then rerun focused
API checks with the execution task integrated.

## Actual evidence

All tests use `mkdtemp` directories, the existing public HTTP router and the
manual executor seam. Filesystem obstacles require no permission assumptions.
The file-size limit is applied only in a forked child; the parent waits for its
exit and reopens the same directory with a new service object. The child does not
run the service destructor. The test advances the parent's fork-copied identifier
generator so the old retry cannot mask duplication by reusing the same random ID.
No external filesystem watcher, production path or new failure-injection API is
used.

Baseline reconstruction compiles the two original persistence translation units
and headers from `6d5c29f`, linking them to the unchanged library objects and the
same new regression tests. Before: **4/17 pass, 13 expected regression failures**.
The post-creation write-failure test observes two experiments on the old source;
the fixed implementation retains one and refuses to re-execute the key. The
missing-cache probe also demonstrates the old completed record being overwritten.

Final macOS release focused gate: **53/53 API tests pass**, including **17/17 new
durability tests** and the existing state-machine, restart, canonical/native
comparison, artifact-substitution, HTTP and translation checks. Wall time:
**2.38 seconds**. The 17 durability cases total **71.605 ms** as reported by the
test runner. Builds use at most `-j2`; baseline reconstruction compiles serially.
No compiler warnings were emitted. `git diff --check` passes.

Exact evidence root:
`/Users/jedi/.t3/worktrees/lob/t3-a3d1cea4/output/api-durability/`.

- `before-final.log`: full final regression set on the original source, including injected failures and before-state mutation evidence.
- `baseline-final-build.log`, `baseline-research_service.cpp`, `baseline-research_run.cpp`, `baseline-include/`, `before-tests`: original-source reconstruction and disposable reproducer binary.
- `all-api-build.log`: final focused release build.
- `final-api.log`: authoritative final 53/53 run, per-case timings and process wall time.
- `before.log`, `after-initial.log`, `service-initial.log`, `combined-intermediate.log`: retained intermediate evidence. The initial service check exposed an overly strict check for legitimate shutdown-failed runs without caches; the final implementation explicitly supports those records. These intermediate counts are not final validation.

Reproduce the focused release gate:

```sh
make -j2 build/release/run_tests TEST_SRCS="tests/main.cpp $(rg --files tests/exsim/api -g '*.cpp' | tr '\n' ' ')"
build/release/run_tests api_
```

No full suite, sanitizer, TSan, Docker or benchmark was run. Ready command for a
parent-authorized serial sanitizer slot, not executed here:

```sh
make -j2 MODE=debug build/debug/run_tests TEST_SRCS='tests/main.cpp tests/exsim/api/research_service_durability_tests.cpp tests/exsim/api/research_service_tests.cpp'
build/debug/run_tests api_
```

## Remaining limits

- This is at-most-once execution under the supported single-process data-directory and process-restart model. It is not an exactly-once completion/reconciliation mechanism. A pending key may represent a crash before execution or after a partial/complete mutation; it intentionally requires inspection rather than repeating the operation. There is no reconciliation endpoint in this change.
- Native/artifact writers outside the key store still use flush and rename without file/directory fsync. There is no multi-file transaction or full power-loss guarantee, no faulted-disk recovery certification, and no Linux validation in this task. The reservation/result fsync calls alone do not establish those stronger properties.
- A crash loses the in-memory simulation and partial inspection indexes. Interrupted live runs fail on restart. Failed runs with no caches preserve diagnostics but cannot reconstruct the missing inspection/accounting history; this change does not invent it. A partial terminal cache set may require offline reconciliation before startup can proceed.
- Startup validates enclosing metadata, retained-key capacity, resource references and terminal cache presence. It does not exhaustively scan binary cache contents or perform canonical native reproduction during startup. Existing artifact readers and fresh canonical native comparison verification remain responsible for those checks; the existing corruption/substitution regressions still pass.
- Historical mutations whose old JSONL append was already lost cannot have their keys recovered from the remaining resource alone. No automatic association is guessed. Back up the entire directory, including both old and new idempotency stores. A writable downgrade to the old service is unsafe because it ignores the new store; binary rollback requires a compatible reader or an explicit data-reconciliation plan.
- Failure-record persistence itself can fail. The original exception is propagated or logged by the executor/shutdown path; in-memory failure diagnostics remain available while that process lives. There is no promise that an unwritable disk saves its own failure report.
