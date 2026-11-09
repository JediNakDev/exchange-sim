# Deploy with Docker Compose

The repository's existing `docker/Dockerfile` remains the C++ development image.
`docker/api.Dockerfile` and `docker/web.Dockerfile` build the deployable runtime
images. Compose runs PostgreSQL 18, the C++ API and a production Next.js frontend.
The API container runs `exsim_apid`. Each active run launches separate
`exsim_exchanged` and `exsim_traderd` processes. Their private Unix-domain socket
carries actual strategy observations and actions; a separate control socket
connects the exchange to the API. All three binaries are shipped in the image.
They do not require Docker access or separate containers per run. See
[process ownership and wire contract](WORKERS.md).

## Start

Requires a current Docker Compose plugin supporting `up --wait` and dependency
restart rules, plus `openssl` on the host to generate local credentials. From
the repository root:

```sh
sh docker/create-secrets.sh
docker compose up -d --build --wait
```

Open **http://localhost:3761**. Only the frontend is published, bound to loopback
by default. The API and database are reachable on the private Compose network
through service names; your host PostgreSQL and installed app are not replaced.
The API and frontend run as non-root users. PostgreSQL creates an `exsim` login
which owns only the application database and is not a superuser.

To select a different frontend port:

```sh
EXSIM_WEB_PORT=8080 docker compose up -d --build --wait
```

Use the same Compose project name and configuration on subsequent commands.
An optional `.env` at the repository root can retain `EXSIM_WEB_PORT=8080` and
`COMPOSE_PROJECT_NAME=exsim`; it is ignored by Git and Docker builds. Default
volume names are scoped by the Compose project. Changing that project selects
a different dataset.

Credentials are generated once into `.secrets/`, are excluded from Git/build
contexts, and are mounted only into the services that need them. The helper
preserves existing credential bytes. The host directory is private; mounted
files are readable by the non-root service accounts. Keep this directory for
restarts. Editing password files does not rotate passwords already stored in
an initialized database; coordinate an explicit SQL password change if needed.

The app has no user authentication yet. Keep loopback/private ingress. Set
`EXSIM_BIND_ADDRESS` to another host address only behind an access-controlled
HTTPS proxy or private network. No ingress or certificate configuration is
silently installed by Compose.

## Storage and updates

Two named volumes persist independently of containers:

| Volume | Container path | Contents |
| --- | --- | --- |
| `postgres_data` | `/var/lib/postgresql` | PostgreSQL 18 cluster, including authoritative metadata |
| `run_artifacts` | `/var/lib/exsim` | Manifests, journals, fills, summaries and diagnostic evidence |

The PostgreSQL 18 volume target intentionally uses the major-version layout
under `/var/lib/postgresql`. Do not reuse an older PostgreSQL cluster volume
without a deliberate database upgrade.

```sh
docker compose ps
docker compose logs --tail 100 api web db
docker compose stop
docker compose up -d --wait
```

`docker compose down` removes containers/network while retaining the named
volumes. **`docker compose down --volumes` deletes the database and research
artifacts.** It is only suitable for intentionally discarding a dataset.

For a source update, let active work finish or cancel it, then use
`docker compose up -d --build --wait`. Active workers stop with the API. Queued
runs require their original executable/release identity, so changing the image
can fail an old queued run explicitly rather than executing it under misleading
provenance. Completed runs remain queryable. Do not run two API replicas against
the same database/artifact volume; the local ownership locks enforce one owner.

Startup waits for PostgreSQL TCP readiness and the API HTTP check. These checks
are process/HTTP readiness, not a full experiment or database-session integrity
check. The API deliberately does not reconnect a lost PostgreSQL session. After
a database restart use `docker compose restart api` (or restart `db api` together).
Compose's dependency restart rule handles explicit Compose dependency updates;
it is not an automatic unhealthy-container recovery controller.

## Backup

Back up the database and artifacts together, with all application writers
stopped. Use a new private backup directory:

```sh
mkdir -m 700 backup
docker compose stop web api
docker compose exec -T db pg_dump -U postgres -Fc exsim > backup/metadata.dump
docker compose cp api:/var/lib/exsim backup/artifacts
docker compose up -d --wait
```

Retain both parts and checksums, along with the built image identities. Test a
restore into a fresh project before accepting the backup. A restore needs the
application database dump and artifact tree with ownership UID/GID 10001, at
`/var/lib/exsim`, plus the app database role/credentials. Never restore a copied
live PostgreSQL directory by treating it as a logical dump. The legacy
file-only backup tool intentionally rejects PostgreSQL artifact directories.

This stack starts a fresh dataset. Importing existing host research data is an
explicit offline operation described in [POSTGRES.md](POSTGRES.md); no automatic
host-data migration happens on `compose up`.

## Validation

The deployment check uses a disposable project and a random published port,
drives a real worker-backed run through the frontend proxy, checks its artifacts
and idempotent replay, then recreates containers and checks persistence. Only
that validation project's temporary volumes are removed afterward. See the
Compose receipt in `docs/quality/` for the build/check results and exact logs.

```sh
python3 tests/integration/compose_secrets_test.py
python3 tests/integration/compose_deployment_test.py
```

The second command expects `exsim-api:local` and `exsim-web:local` to have been
built first. It creates its own randomly named project, then deletes only that
project's temporary volumes. CI builds both images and runs these checks.

Relevant upstream behavior:
[Compose startup ordering](https://docs.docker.com/compose/how-tos/startup-order/),
[PostgreSQL official image](https://hub.docker.com/_/postgres).
