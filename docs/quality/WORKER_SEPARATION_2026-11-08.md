# Local worker separation receipt

User request: separate workers and keep expensive IPC off the hot path. Local
artifacts remain the chosen storage. Implementation is uncommitted in the parent
checkout, on top of the prior PostgreSQL changes. No installed service, original
dataset, release selection, PR or deployment was changed. Concurrent frontend
workspace changes were preserved and are outside this native implementation.

## Implemented behavior

The default API executable supervises one C++ child per active run, bounded by
`--max-live-runs` including paused runs. Each child's own executor advances all
simulation stages locally. IPC contains bootstrap, explicit controls, requested
views and periodic status; there is no per-event/order/fill/scheduler-step IPC.
Control results and snapshots are captured at one boundary; socket I/O happens
after control/run locks are released. PostgreSQL is accessed only by the API.

Cold comparison verification, completed execution traces and book replay also
run in query children, with one cold query slot. The diagnostic
`--in-process-runs` path remains explicit, with no silent fallback on failure.

Launch is reserved durably before spawn. Children verify their actual build and
release provenance. The inherited root lock fences all writers after API death.
EOF stops orphaned workers; a stopped orphan retains ownership and prevents a
replacement API from writing. Started run IDs are never automatically retried.
Channels are poisoned on transport/decode failure, preventing a late response
from acknowledging another command. Worker failure is reaped before capacity
is reused. Incomplete caches are preserved in `interrupted_*` directories,
including during startup recovery, rather than advertised as complete results.

## Verification

- macOS PostgreSQL 18.6 real native HTTP/process suite: **27/27 release and
  27/27 ASan/UBSan**. Covers worker PIDs/limits, independent controls, crash
  recovery, orphan fencing, partial caches, child provenance checks, timeout
  poisoning, API suspension with a still-running worker, cold queries,
  metadata/audit recovery, import preservation and paired backup/restore.
  Logs: `/tmp/exsim-worker-integration.log`,
  `/tmp/exsim-worker-sanitizer-integration.log`.
- Strong independent-progress and child-provenance probes pass separately in
  both builds. The progress probe asserts the worker is still running after
  API suspension before waiting for completion without any API IPC consumption.
  Logs: `/tmp/exsim-worker-independent-progress.log`,
  `/tmp/exsim-worker-independent-progress-sanitizer.log`.
- Existing native API plus new frame tests: **99/99**. Allocation guards:
  **25/25**. Logs: `/tmp/exsim-worker-api-tests.log`,
  `/tmp/exsim-worker-allocation-tests.log`.
- Real OpenAPI HTTP contract: **9/9**, all 23 operations, with the default
  worker-based CLI. Log: `/tmp/exsim-worker-http-contract.log`.
- Linux aarch64 GCC `-Werror`, file-backed build: **99/99 API and 25/25
  allocation checks**. Final source log: `/tmp/exsim-worker-linux-final.log`.
  A separate actual Linux process smoke exercised two isolated runs,
  pause/step/resume, cold book replay, audit and paired comparison.
  Log: `/tmp/exsim-worker-linux-smoke.log`.
- PostgreSQL release/sanitizer and separate file-only API builds pass without
  warnings. Logs: `/tmp/exsim-worker-build.log`,
  `/tmp/exsim-worker-sanitizer-build.log`,
  `/tmp/exsim-worker-file-only-build.log`.
- Delivery tests: **41/41**, `/tmp/exsim-worker-delivery-tests.log`.
- Deterministic native journals and evidence match the diagnostic in-process
  path byte-for-byte. Public inspection field values match too. Existing binary
  inspection caches contain native struct padding and are not treated as a
  canonical byte encoding by this comparison.

Read-only GPT/high QA task `/root/worker_review`, shared parent checkout,
completed with three blockers: parent snapshot writer confusion in file mode,
child provenance validation bypass, and reusable channels after transport
failure. Parent fixed each and added real-process regressions. Review found no
per-event IPC. Subsequent acknowledgment capture, terminal publication and
partial-cache recovery repairs are parent-reviewed with the checks above; a
fresh independent clearance of final edits is not claimed.

All validation APIs and task-owned Linux containers were stopped/removed.
Temporary integration databases were cleaned up. Original Docker images,
PostgreSQL service, `exsim_local`, installed services and research data remain.

## Limits

This is local process isolation under one exclusive API owner. API death stops
active workers and those runs fail explicitly; queued runs survive under
matching provenance. Distributed leases, workers surviving API replacement,
automatic retry attempts, OS memory quotas, multi-user authentication and
production monitoring are not implemented. IPC deadlines and cold-query bounds
are documented in [WORKERS.md](../architecture/WORKERS.md). No TSan, throughput
benchmark, full exchange E2E or browser UX run is claimed for this change.

Next action: review/commit the local PostgreSQL and worker source when desired,
then use a separate verified rollout/migration procedure for an installed
deployment. No live migration is implied by successful local tests.
