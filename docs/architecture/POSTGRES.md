# PostgreSQL research metadata

PostgreSQL and local worker milestones, 2026-11-08. This is an opt-in local
PostgreSQL deployment with isolated simulation processes, not a completed
multi-user production deployment.

## Ownership

The C++ API owns PostgreSQL access. Next.js continues to proxy the existing
OpenAPI HTTP API. Matching, simulated time, accounting and strategies are
unchanged. There are no database calls inside matching or simulation steps.

PostgreSQL stores experiments, run records (including the queued state), and
idempotency reservations/responses in the `exsim` schema. Versioned record text
preserves exact encodings and response bytes. Generated relational keys, foreign
keys, state constraints and experiment/state indexes support integrity and
scheduling. Event journals, manifests, fills, samples, summaries and native
reproduction evidence remain local artifacts. Audit export synthesizes `run.json`
from the authoritative database, ignoring any file retained by import.

Creation of an experiment or queued run and its successful replay response share
one transaction, following a separately committed idempotency reservation.
Known validation rejections roll back and release the reservation. Unexpected
failures keep it reserved. A failed creation transaction makes the API instance
unavailable until restart, avoiding continued use of uncertain in-memory state.
There is no automatic reconciliation of an uncertain key.

The executor cannot activate an uncommitted submission. Queued runs survive
shutdown and restart under the same executable/release provenance. Queued runs
from a different build or release become failed with an explanation, rather than
running with a misleading immutable manifest. Running/paused simulations still
become failed when interrupted. The API now supervises a separate process per
active run. Matching, strategy execution, accounting and the event scheduler all
remain local to that process. See [WORKERS.md](WORKERS.md) for ownership, control
acknowledgments, crash behavior and the boundary that keeps IPC out of the hot
path. Distributed leases and retries under an existing run ID are not implemented.

## Local use

Requires the PostgreSQL server, client utilities, and libpq development headers.
`pg_config` must match the intended libpq installation; override `PG_CONFIG` if
needed. Standalone/default simulator builds remain database-independent.

```sh
make -j1 WITH_POSTGRES=1 build/release/exsim_api
EXSIM_DATABASE_URL='dbname=exsim_local' \
  build/release/exsim_api --data-dir output/postgres-local --port 7601
```

`exsim_local` was created locally for this work. Its artifact binding is
`output/postgres-local` in this checkout. Existing installed file-backed services
and their original data directories are not switched or migrated by this work.

To run the development frontend and API together, from `web/`:

```sh
EXSIM_DATABASE_URL='dbname=exsim_local' \
EXSIM_API_DATA_DIR="$PWD/../output/postgres-local" \
EXSIM_API_PORT=7601 bun run dev
```

The development launcher selects `WITH_POSTGRES=1` when the connection setting is
present. The connection string is read from the environment, not a command-line
flag or committed credentials file. Standard libpq settings and `.pgpass` also
work. Do not expose this service remotely: authentication and workspace
authorization are not implemented yet.

Startup applies schema version 1 transactionally and checks its artifact-root
binding. An unknown version or a different root fails startup. One API owns a
database and artifact root at a time, using a session advisory lock plus a local
file lock. The local lock remains held after database-session loss. There is no
automatic connection reset, which could silently lose exclusive ownership.
Restart the process after connection failure.

The `.postgres-metadata` marker is flushed before serving or committing an import.
New binaries refuse file fallback for that directory. Do not point an older,
unaware binary at it. Completion flushes local artifacts and their directories
before publishing a terminal database record. This does not provide replication
or protect against loss of the artifact disk.

## Import existing file metadata

Stop all writers and disable automatic restarts. Take a verified offline copy
with the original file-backed backup procedure first. Import into a fresh
database, using the original artifact root:

```sh
createdb exsim_import
EXSIM_DATABASE_URL='dbname=exsim_import' \
  build/release/exsim_api --data-dir /absolute/existing/data \
  --import-file-metadata --port 7601
```

Import checks original identities and filename hashes, supports legacy JSONL and
per-key idempotency records, and validates the complete metadata/artifact graph
before committing. A failed import rolls back all imported rows. Committed JSON
and binary artifacts are preserved; PostgreSQL becomes authoritative. Incomplete
inspection caches from interrupted executions are retained in an `interrupted_*`
subdirectory instead of being presented as complete results. Import
cannot merge into a populated database or silently replace existing data.

## Backup and restore

The existing file-only delivery backup/restore commands reject PostgreSQL data
because copying the artifact directory omits authoritative metadata.

For this local stage, stop all writers and keep them stopped while taking both
a custom-format `pg_dump` and a complete copy of the bound artifact directory.
For example, with a new private backup directory:

```sh
pg_dump -Fc -d exsim_local -f /private/backup/metadata.dump
cp -a output/postgres-local /private/backup/artifacts
```

Use checksums for both parts. Restore the dump into a new empty database with
`pg_restore --exit-on-error`, and restore the paired artifact tree. The dump
retains the absolute artifact-root binding. If the restored tree has a different
absolute root, verify the pair and deliberately update
`exsim.schema_version.artifact_root` to that canonical root before startup.
Never change the original database's binding as part of a restore drill. Check
run inspection, audit export, idempotency replay and native comparison in the
restored instance. A backup is not accepted until that restore has been tested.

Production hosting still needs a least-privilege application role, managed
credentials, encryption/TLS appropriate to its network, monitoring, automated
paired backups and agreed recovery targets. Separate application migrations from
normal startup when introducing a non-DDL runtime role. No production rollout or
restore drill of the existing user dataset is claimed here.

## Verification

```sh
python3 tests/integration/postgres_api_test.py
python3 tests/integration/postgres_api_test.py --binary build/debug/exsim_api
build/release/run_tests api
python3 -B scripts/deployment/test_research_delivery.py
```

The PostgreSQL suite creates and drops only random `exsim_it_<hex>` databases.
Its local test user needs CREATEDB. It covers exact restart replay, local
artifacts/controls, queued recovery and build mismatch, authoritative audit
metadata, exclusive ownership, database loss, explicit import preservation,
transaction rollback and unknown schema rejection. CI runs the integration suite
against PostgreSQL 16, matching Ubuntu 24.04's dump/restore client version.
Local verification uses PostgreSQL 18.6. Existing non-PostgreSQL CI remains enabled.

## Following milestones

1. Extend the implemented local supervisor to independent worker hosts only when
   needed. That requires distributed leases, fenced attempt tokens and resource
   quotas. Current local workers share one exclusive API/artifact owner; an API
   death terminates them and fails active runs rather than retrying them.
2. Add identity, workspace ownership/authorization and quotas before remote use.
3. Add metrics, failure alerts and automated recovery drills. Retain local
   artifacts as requested; introduce object storage only when selected.
