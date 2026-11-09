# Docker Compose deployment receipt

Completed 2026-11-09, from the uncommitted parent workspace. The existing C++
development Dockerfile remains unchanged. Added separate multi-stage API and
Next.js runtime images, a Compose stack, credential generation and deployment
checks. Concurrent UI work was preserved; no standalone UI behavior review is
claimed beyond the production build and deployment HTTP checks.

## Deployment

- PostgreSQL 18 on a persistent named volume at `/var/lib/postgresql`.
- Non-superuser `exsim` application role, separate from the database admin.
- API as UID 10001, artifacts at `/var/lib/exsim` on a separate named volume.
- Production standalone Next.js as the `node` user, built with Bun 1.4.4 and
  running on Node 24.
- Only the frontend is published, on host loopback port 3761 by default.
- Native simulation/query workers remain inside the API container; no Docker
  socket, privileged container or per-event network IPC is introduced.
- Credentials live in ignored `.secrets/` files and Compose secret mounts,
  outside both image build contexts. Existing credential bytes are preserved.

## Checks

- `docker compose build` succeeds for both application images. Log:
  `/tmp/exsim-compose-build.log`.
- `docker compose config --quiet` passes.
- The real disposable Compose deployment passes: production HTML/static assets,
  frontend proxy, non-root service users, non-superuser database role, private
  API/DB ports, actual child worker, pause/step/resume, completed artifacts and
  PostgreSQL metadata. All containers are then removed/recreated with volumes
  retained; exact idempotent response replay and the saved summary match.
  Log: `/tmp/exsim-compose-integration.log`.
- Credential helper 4/4: generation/preservation, failed-generation cleanup,
  empty-file rejection and symlink rejection. Log:
  `/tmp/exsim-compose-secrets-tests.log`.
- Git ignores the credential files; Docker ignores credentials, dependency and
  build output trees. Final diff whitespace check passes.

Checked local image identities (linux/arm64):

```text
exsim-api:local sha256:c430f0dbe60f965fad1ca7e48a3234c1c0731522b7bd3564f596910daf8e365c
exsim-web:local sha256:c6250b98e889adfcf16e6a507d88915e0f7a677354f41a38ba63cf18290decf9
```

Read-only GPT/high QA task `/root/compose_review`, shared checkout, completed
without blocking findings. Parent addressed its small credential-generation
robustness finding using atomic non-overwriting publication and the isolated
failure tests above. No fresh independent re-check of that helper-only repair
is claimed.

CI now builds the runtime images and runs credential and real Compose checks.
That workflow is configured, not claimed to have run remotely in this task.

## State and limits

All disposable deployment containers, networks and test volumes were removed,
including the initial PostgreSQL bootstrap project. Built application images
and private local credential files remain available for the user's deployment.
The host PostgreSQL, `exsim_local`, installed app and existing research data were
not changed. No source commit, image push or installed-service promotion.

HTTP health is not a database-session integrity probe. The API requires restart
after a lost PostgreSQL session; this is documented rather than hidden behind
an unsafe reconnect. Authentication, HTTPS ingress, automated recovery and
distributed workers remain separate production work. This stack is private by
default. Backup needs the database and artifacts together; container recreation
was tested, but a separate Compose backup restore drill was not performed.

Runbook and startup commands: [DOCKER.md](../architecture/DOCKER.md).
