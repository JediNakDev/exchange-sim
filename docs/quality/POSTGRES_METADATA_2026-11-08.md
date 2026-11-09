# PostgreSQL metadata implementation receipt

Scope: opt-in PostgreSQL metadata, transactional experiment/run creation,
durable queued state, local artifact preservation, explicit file import,
authoritative audit export and development/CI wiring. This is the first
production architecture milestone, not worker separation or multi-user hosting.

Implementation is uncommitted in the parent checkout. No live installed release,
LaunchAgent, original research dataset or PR was modified. Created `exsim_local`
and bound it to `output/postgres-local`; the bootstrap service was stopped.

## Checks

- PostgreSQL 18.6 native HTTP integration: 15/15, release and ASan/UBSan.
  Includes real process termination/restart, explicit import, failed submission
  rollback, connection loss, release mismatch, audit record hashing/chunking, and
  an offline pg_dump + paired artifact restore into a new disposable database.
  Logs: `/tmp/exsim-postgres-integration-final.log`,
  `/tmp/exsim-postgres-sanitizer-integration-final.log`.
- Existing native API selection: 96/96, release, including file-backed durability.
  Log: `/tmp/exsim-postgres-api-tests-final.log`.
- Delivery tests: 41/41. File-only backups now refuse PostgreSQL datasets.
  Log: `/tmp/exsim-postgres-delivery-tests.log`.
- PostgreSQL release and sanitizer API builds and a separate file-only API build
  pass without warnings. Logs: `/tmp/exsim-postgres-build-verified.log`,
  `/tmp/exsim-postgres-sanitizer-build.log`,
  `/tmp/exsim-postgres-file-only-build.log`.
- Frontend TypeScript and scoped development-launcher lint pass.
  Logs: `/tmp/exsim-postgres-web-types.log`, `/tmp/exsim-postgres-web-lint.log`.
- Local empty-database bootstrap verified HTTP experiment listing and schema
  binding. Logs: `/tmp/exsim-postgres-local-bootstrap.log`,
  `/tmp/exsim-postgres-local-api.log`.

Read-only GPT/high QA task `/root/postgres_review` reviewed the uncommitted
integration and reproduced two defects: queued-run provenance loss and omitted
authoritative audit metadata. It also identified marker durability. Parent
fixed all three and added provenance/audit regressions; marker and root are
explicitly synced. Reviewer completed with no further original-scope findings.
The final repairs are parent-reviewed and verified by the tests above; no fresh
independent clearance of those final edits is claimed.

## Limits and next actions

Separate workers, leases/fencing, retry attempts, workspace ownership, identity,
quotas, remote exposure and production monitoring are not implemented. Live
simulations remain inside the API. Queued runs recover under identical build
provenance; interrupted running/paused runs fail explicitly. Connection loss
requires process restart, with no unsafe automatic session reconnection.

The coordinated restore drill uses disposable test data. Existing user data has
not been imported or restore-tested with PostgreSQL. A live rollout needs its
own offline backup, migration and acceptance window. Retain local artifacts per
user direction. Operational and next-milestone details:
[PostgreSQL guide](../architecture/POSTGRES.md).
