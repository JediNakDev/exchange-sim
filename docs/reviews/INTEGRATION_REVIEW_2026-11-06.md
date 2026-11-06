# Independent integration review, 2026-11-06

Target: `69bb968999a2e6fde1aacef3ac67fbe0dde9bf6e` on exchange-simulator. This is a new report-only review of `db8c710` and `96e75c3` and their integrated callers. Findings below are confirmed by source inspection, with minimal manual triggers specified; **none of those triggers was executed in this checkout**.

## Result

| ID | Severity | Scope | Finding |
| --- | --- | --- | --- |
| IR-01 | P1 | HTTP comparison after restart | Verified native artifacts do not protect the summary metrics or series actually returned. |
| IR-02 | P2 | HTTP comparison provenance | A self-consistent native directory is not bound to the enclosing API run's config or seed. |
| IR-03 | P2 | Runtime shutdown | Blocking log output can prevent shutdown from reaching its journal deadline. |

P1 means incorrect research results can be presented as verified paired evidence. P2 means a narrower correctness or bounded-shutdown guarantee fails under the stated trigger. IR-01/02 require local persisted-data corruption, accidental replacement, or stale cache files; they are not remote-authentication findings. IR-03 predates the journal patch but prevents a general bounded-reactor/shutdown claim.

## IR-01: native reproduction does not validate comparison caches

Evidence:

- `src/exsim/api/research_run_views.cpp:888-920`: `comparison_inputs()` verifies only `run_directory/runner`, then parses `final_summary_json_` for metrics and copies `artifacts_.samples` for aligned series. It never compares either source with the reproduced native result.
- `src/exsim/api/research_run.cpp:396-417`: the outer `summary.json` and `samples.bin` are separate persisted API caches. Binary loading checks header magic, record size/count and successful reads (`:143-153`), with no checksum or semantic relation to native `series.csv`.
- `src/exsim/api/research_run.cpp:420-475`: restart restores the master seed, final simulation time and summary from the outer API files. Terminal artifacts load lazily. It does not reconstruct the summary or samples from native evidence.
- `src/exsim/research/run_verification.cpp:54-129`: reproduction compares files inside the supplied native directory only. Outer API files are outside its input and comparison set.
- `src/exsim/api/research_service.cpp:670-689`, `:742-800`, `:856-920`: seed admission, paired differences, dispersion and aligned values consume these inputs without further integrity checks.
- `web/src/components/research/experiment-compare-view.tsx:139-145`, `:269-294` and `web/src/lib/chart-derived-floats.ts:33-47`: the real UI directly displays the returned metric comparisons and aligned series. There is no downstream native-artifact check.

Minimal metric trigger: complete baseline and candidate for one shared seed, stop the service, change only the candidate's **outer** `runs/<API-ID>/summary.json` `metrics` entry for `net_pnl` to another valid decimal string, and restart. Leave `runner/` untouched. GET the comparison. Native reproduction still succeeds; `comparable` remains true and candidateValue/difference use the edited string. Editing a flat metric can also make it disagree with the structured summary sections.

Minimal series trigger: after stopping the service, replace only `samples.bin` with another run's valid binary cache of the same layout, or alter a record's position/equity while preserving the header and length. Restart and GET the comparison. The unchanged native directory passes reproduction, while aligned inventory/equity/drawdown use the replacement cache. No checksum rejects this change. End-time corruption in outer `run.json` can likewise alter alignment without changing native verification.

Impact: the feature verifies evidence adjacent to the values it presents, rather than those values themselves. This breaks the PRODUCT requirement that browser and CLI read the same results and comparisons use trustworthy completed evidence. A malformed cache can also yield an error instead of exclusion, but the finding does not depend on malformed input or a crash.

Fix direction: derive comparison inputs from reproduced native evidence, or rebuild and validate every consumed API cache from it before admitting the run. A digest stored beside a mutable cache alone is insufficient. Add restart tests changing outer summary and samples while keeping native artifacts valid. The new test at `tests/exsim/api/research_service_tests.cpp:485-506` corrupts only the inner native summary and therefore does not exercise this gap.

## IR-02: canonical native IDs are generated correctly but never bound on read

Evidence:

- `src/exsim/api/research_run.cpp:519-530` now generates the native ID using the same name/config-hash/master-seed inputs as CLI `src/exsim/research/experiment_runner.cpp:25-35`. Separate outer API IDs are intentional and correct.
- `src/exsim/research/run_verification.cpp:72-95`: reproduction obtains configuration and master seed from the native directory's own `experiment.conf` and manifest. It takes no expected API identity/config/seed argument.
- `src/exsim/api/research_run_views.cpp:895-903`: only the reproduction boolean is checked; the returned comparison identity and seed come from `parameters_`. No expected native ID, config hash, seed or digest is checked.
- `src/exsim/api/research_service.cpp:302-309` associates a restarted run with an experiment via outer `run.json`, and `:674-676` pairs by the outer seed. Neither association is verified against the native manifest.

Minimal trigger: produce completed runs for seeds 1 and 2, then replace the seed-1 API run's `runner/` directory with an intact copy of seed 2's native directory. A comparison request, including without a service restart, reproduces seed 2 successfully but admits the enclosing API run as seed 1. An intact native directory from another configuration also passes this check. Replacing outer caches along with it can present seed-2 results as a verified seed-1 pair.

Impact: native self-consistency does not establish provenance of the API run being paired. Even repairing IR-01 by reading native values would still pair the wrong seed/config unless this binding is checked too. This is separate from a native-ID generation defect: initial generation is consistent with the CLI.

Fix direction: check the expected normalized native config, seed and canonical identity against the enclosing experiment/run before reproduction and admission. Add a valid-directory substitution test; malformed-file tests cannot prove this property.

## IR-03: shutdown still performs blocking stdio before enforcing the deadline

Evidence:

- `include/exsim/server/server_runtime.hpp:64`: logging defaults to `stderr`.
- `:248-257`: first shutdown progress sets the deadline and calls `log("end of day: closing every session...")` synchronously.
- `:263-273`: journal drain/deadline evaluation happens only after that call returns. Timeout logging is synchronous too.
- `:293-298`: `log()` uses `fputs` and `fflush` on the reactor thread. A blocking pipe or stalled filesystem sink has no deadline here.

Minimal trigger: configure `options.log` as a blocking pipe-backed FILE, complete startup while draining it, then fill the pipe and stop its reader. Request shutdown and execute `poll_once()`. The shutdown log blocks before journal drain progresses; the configured session deadline and worker's extra 100 ms teardown cannot interrupt it. Redirecting default stderr to a stalled pipe has the same effect. Keep the reader alive so this is backpressure rather than EPIPE.

Impact: moving journal I/O off-thread fixes the journal stall path, but does not make every reactor operation or shutdown bounded. This is a confirmed synchronous blocking path from static inspection; its wall-clock behavior was not measured here. It is not a new steady-state throughput claim or a journal queue/lifetime bug.

Fix direction: use bounded/nonblocking diagnostic delivery or keep blocking sinks off the reactor. Validate shutdown against a full log pipe with a bounded child-process reproducer. `log = nullptr` avoids this path for an embedding, but the normal executable uses the blocking default.

## Journal boundary assessment

No additional confirmed journal ordering, replay or use-after-free defect was found in the inspected code.

- `JournalWorker::append/flush/enqueue` (`include/exsim/server/runtime/journal_worker.hpp:42-87`) does bounded producer work with no file operations. The sole producer owns counters and flush coalescing. Capacity 16385 gives 16384 usable entries. Overflow latches failure and stopping; further entries are refused.
- `lob::engine::SPSCQueue` (`include/lob/engine/spsc_queue.hpp:16-45`) release-publishes entries and acquire-consumes them in FIFO order. Flush markers share that queue. Worker acknowledgement is published only after a successful writer flush (`journal_worker.hpp:89-112`); append after a flush resets coalescing. The stopping path rechecks the queue after observing stopping.
- `ExchangeCore` has a synchronous default fourth policy and runtime explicitly selects the worker (`exchange_core.hpp:96`, `server_runtime.hpp:71`). Applied commands are appended before dispatch; overflow does not roll back an already-applied command (`exchange_core.hpp:603-649`). First failure is counted/latches once; timer health observation (`:261-263`) handles idle I/O failures even with periodic flush disabled. Continue/shutdown behavior matches the documented policies.
- Shutdown requests a final flush and checks drain on later iterations (`server_runtime.hpp:264-273`). Once the core knows persistence failed, `journal_drained()` permits exit as a failed capture, not as successful persistence (`exchange_core.hpp:448-464`); the executable derives its exit status from that latched state (`tools/exsim_server/main.cpp:147-152`). Failed flushes cannot publish a new successful acknowledgement.
- The worker captures shared State containing its writer and queue, not the runtime (`journal_worker.hpp:22-36`, `:68-75`). Writer reset/close occur on the worker (`:110-112`). Detached I/O can outlive runtime destruction without referring to core, endpoints or config. The 100 ms wait is an intended bound plus scheduling overhead, not a hard real-time guarantee.
- Canonical writer `src/exsim/sim/journal.cpp:38-49` opens without truncation, fstats the actual descriptor, rejects nonregular files, then truncates. A regular-file symlink target is permitted by the stated “resolve to regular files” contract. Startup regular-file I/O can block by explicit contract. Runtime seeks, buffered writes, footer writes and close stay on the worker. Offline research writer callers remain deliberately synchronous.
- `journal.cpp:54-78` preserves canonical append/footer overwrite behavior; reader `:107-124` checks footer/count, complete record alignment, sequence, checksums and re-encoding before exposing commands. The worker reuses this format rather than introducing a second serialization.

Accepted limits: successful footer writes are not fsync durability; overflow or I/O failure may leave an incomplete capture; continued trading after failure is explicitly configurable; detached I/O may persist until process exit. None is reported as a new bug. A `drained()` acknowledgement is of footer writing, not close completion. Disk failure or process loss after successful writes is outside that acknowledgement.

Remaining uncertainty: no new stress, sanitizer, Linux, real-binary or blocked-sink execution was performed. Scheduler-dependent latency in observing worker failure, close-time errors, and detached I/O during process teardown remain unverified here. A worker failure is observed on the next timer or failed enqueue, not necessarily before every already-ready inbound command. This report does not claim a stricter instantaneous-stop guarantee.

## Review scope and evidence

Read project AGENTS.md and WORK_PLAN, PRODUCT, ARCHITECTURE, SERVER, SERVER_RUNTIME, EXCHANGE_CORE and API contracts. Traced production JournalWriter users (runtime core and offline RunArtifactWriter), core append/flush/timer/shutdown/status/destructor paths, server exit status and endpoint shutdown, queue implementation, journal replay validation, native ID generation/reproduction, API persistence/restart/comparison aggregation, and real UI comparison consumers. Examined existing journal worker tests, core/review callers and the new native-corruption test. Broader protocol and mutation-corpus correctness was not re-audited.

The parent release/sanitizer/TSan/frontend/HTTP results in the brief are inherited evidence, not checks rerun by this reviewer. No build products were available under this checkout's `build/release`; static evidence suffices for the findings above, so no full or replacement build was started. No Docker, benchmarks, delegates, production/API edits, existing-reproducer edits, pushes or parent merges. No WORK_PLAN edit because ownership for this task is restricted to this report and an optional new reproducer. Untracked `output/` was preserved.

Exact inspection log: `/tmp/exsim-integration-review-2026-11-06/source-evidence.log` (numbered production source snapshot at the target HEAD, not a runtime test log). No new reproducer was added. Report path: `docs/reviews/INTEGRATION_REVIEW_2026-11-06.md`. The scoped documentation commit is the commit carrying this report; its SHA is returned in the review handoff. Next action: parent routes the three findings for fixes and runs focused restart/substitution/full-log-pipe regressions under its validation budget.
