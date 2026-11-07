# Production research service

The research deployment consists of the C++ HTTP service and a production Next.js frontend. It does not start the separate real-time exchange protocol listeners.

## Build and package

Build on the intended host OS/architecture with the Node version that will run the frontend. These commands are for a planned release, not a service restart:

```sh
make -j4 build/release/exsim_api EXTRA_CXXFLAGS=-Werror
(cd web && bun install --frozen-lockfile && bun run build)
```

`next.config.ts` enables standalone output and disables ISR disk flushing so runtime route caches do not add files inside the verified release. Node.js runs the standalone Next.js server; Bun manages dependencies and builds. Keep run data outside releases. A release is source-checkout independent, not portable across arbitrary OS/architecture/Node versions. Native dependencies and the API must match the target. The host still needs its own Node runtime and service configuration.

Use the stdlib-only Python 3.9+ tool from the repository root to copy **already validated** artifacts. Set `API_REVISION` and `WEB_REVISION` to their actual full 40-character source commit SHAs from the build evidence. They can differ. Setting them to the current checkout SHA does not establish the provenance of an older binary. Set `RELEASE` to a new name under an existing release parent directory.

```sh
python3 scripts/deployment/research_delivery.py package \
  --api build/release/exsim_api \
  --standalone web/.next/standalone --static web/.next/static \
  --api-revision "$API_REVISION" --web-revision "$WEB_REVISION" \
  --target "$(uname -s)-$(uname -m)" --node-version "$(node --version)" \
  --output "$RELEASE"
python3 scripts/deployment/research_delivery.py verify "$RELEASE"
```

If this build has `web/public`, add `--public web/public` to `package`. The tool replaces copied static/public directories in its staging copy, so old assets cannot accumulate. Without `--public`, any public directory in the standalone input is removed. It never edits build inputs or an existing release. The result contains `api/exsim_api`, `frontend/server.js`, complete standalone dependencies, static/public assets, and `delivery.json`. The manifest records both revisions, Next build ID, target, declared Node version and every entry's SHA-256/size/executable bit or relative symlink. It verifies copied bytes before publishing. Absolute, dangling or escaping symlinks, special files, `.git`, `.env*` and common package credential files are rejected. Review inputs for other secrets; this is not a secret scanner. Do not bundle host plists or data.

Outputs and their parents must be disjoint from inputs. Existing destinations are refused, including an empty directory. Operations stage in the destination parent and rename on success; ordinary failures remove staging and leave previous outputs untouched. A `.delivery-lock` name prevents concurrent tool writers to that destination. After abrupt process death, inspect and remove only that operation's abandoned lock/staging directory before retrying with a new name. These are local filesystem operations with no power-loss/fsync guarantee. Checksums detect changes against the manifest, not malicious replacement of the manifest; keep trusted copies and private backups outside the source repository.

## Run on loopback

In one terminal, from the repository root:

```sh
./build/release/exsim_api --bind 127.0.0.1 --port 7600 \
  --data-dir "$HOME/.local/share/exsim/research-data"
```

In another, from `web/`:

```sh
NODE_ENV=production HOSTNAME=127.0.0.1 PORT=3760 \
  EXSIM_API_BASE_URL=http://127.0.0.1:7600 \
  node .next/standalone/server.js
```

The browser calls `/api/exsim/v1/...`; Next.js forwards to the loopback C++ service. `/` redirects to `/experiments`. For a packaged release use `"$RELEASE/api/exsim_api"` and `node "$RELEASE/frontend/server.js"` instead of the source paths, and keep the same loopback binding/data/proxy settings. Configure supervisors with absolute executable, Node, working-directory and data paths rather than depending on a shell PATH or an external-drive checkout.

After an operator starts the intended pair, run this read-only gate:

```sh
python3 scripts/deployment/research_delivery.py ready --release "$RELEASE"
```

It verifies the local manifest, then uses bounded GETs to compare direct and proxied `/v1/service` responses, schema header/version and executable research capabilities, serve `/experiments` as HTML, and match one packaged static JavaScript file's bytes. Redirects, non-200 responses and invalid JSON fail. A successful process launch or HTTP 200 alone is insufficient. The gate does not create runs, test browser hydration, check data writability, reproduce native results, or prove active executable provenance. The API currently reports the constant `exsim_api/1`, not a commit identity. Check supervisor executable paths against the manifest pair separately. Real-binary contract and UI/browser checks belong in the release evidence before promotion.

## Private Tailscale access

On a machine already signed in to Tailscale, use a free Serve port:

```sh
tailscale serve --bg --https=10000 http://127.0.0.1:3760
tailscale serve status
```

Use the HTTPS URL printed by Serve from a device in the same tailnet. This is tailnet-only access; Funnel is not enabled. Existing Serve handlers on other ports should be preserved. The app has no separate user authentication, so anyone your tailnet policy lets reach this endpoint can control its research runs. The C++ port remains loopback-only.

Remove this endpoint with `tailscale serve --https=10000 off`.

After loopback readiness, repeat `ready --release "$RELEASE" --frontend-url "https://<host>.<tailnet>.ts.net:10000"` using the actual Serve origin. It still checks the API directly on loopback and validates TLS normally. No URL credentials, paths or query strings are accepted. A local pass does not establish tailnet reachability or policy from a second device. Keep Serve configuration and ACL verification in the host runbook; the tool does not modify them.

## Installed macOS deployment

The 2026-11-07 paired release runs copied artifacts under
`$HOME/Library/Application Support/exsim/releases/production-20261107-a0b3c8d/`,
independent of the external-drive checkout. The API is `api/exsim_api` from source
`8ee86f1`; the frontend is `frontend/server.js` from source `a0b3c8d`, Next build
`1S7lB4_DnycLm77V_UTS_`, running on Node v24.22.1. The previous 2026-11-06 pair
is retained. Persistent data stays at
`$HOME/Library/Application Support/exsim/research-data/`. Offline backup, restored
data compatibility, local/private readiness and exact provenance are recorded in
[the release receipt](quality/RELEASE_2026-11-07.md). The earlier delivery audit
itself did not perform this promotion.

Two LaunchAgents start at user login and restart on exit:

- `com.jedinak.exsim.api`: C++ service, loopback 7600.
- `com.jedinak.exsim.frontend`: standalone Next.js, loopback 3760.

Their property lists are in `$HOME/Library/LaunchAgents/`; logs are `$HOME/Library/Logs/exsim/{api,frontend}.log`. Host-specific property lists and run data are not committed.

```sh
launchctl print gui/$(id -u)/com.jedinak.exsim.api
launchctl print gui/$(id -u)/com.jedinak.exsim.frontend

# Restart only after deliberately selecting a verified release in the plists:
launchctl kickstart -k gui/$(id -u)/com.jedinak.exsim.api
launchctl kickstart -k gui/$(id -u)/com.jedinak.exsim.frontend
```

Stop the jobs with `launchctl bootout gui/$(id -u)/com.jedinak.exsim.api` and the corresponding frontend label. Wait for `launchctl print` to report each job absent and its process to exit: `bootout` can return while unloading is still in progress. Their property lists remain available for a later `launchctl bootstrap gui/$(id -u) <plist-path>`.

Completed runs persist across service restarts. Runs interrupted while active are marked failed when the service reloads them. Browser comparisons re-execute the expected configuration/seed and do not use unverified outer metric or series caches.

## Offline backup and restore drill

There is no API-wide snapshot/quiescence endpoint or supervisor lock honored by the C++ writer. Individual atomic file replacements do not make a live multi-file copy consistent. Paused simulations are still live and other HTTP mutations can write experiments, run state and idempotency. **Do not use this workflow against active data.**

For an operator-authorized maintenance window, block ingress, let runs finish/cancel as intended, then boot out both LaunchAgents and confirm they have exited and cannot restart. On other hosts disable the equivalent supervisors. Confirm no manually started API or other writer uses this directory. A port refusing connections alone is not proof that all writers stopped. Keep all writers stopped through backup completion. The tool does not stop jobs; the actual release maintenance is documented separately.

Set `DATA` to the exact API data root, `BACKUP` to a new directory on private backup storage, and `RESTORED_DATA` to a new disposable destination whose parent exists:

```sh
python3 scripts/deployment/research_delivery.py backup \
  --data "$DATA" --output "$BACKUP" --writers-stopped
python3 scripts/deployment/research_delivery.py verify "$BACKUP"
python3 scripts/deployment/research_delivery.py restore \
  --backup "$BACKUP" --output "$RESTORED_DATA"
```

`--writers-stopped` is the operator's attestation that **all** writers and automatic restarts remain disabled; the script cannot verify that claim. The manifest says `operator-declared-writers-stopped`, never automatically certified consistent. Full inventories before/after copy and of the copy must match, catching observed changes but not proving quiescence. Copy the entire data root including `idempotency.jsonl` when present, native run artifacts, diagnostics, unknown files and empty directories. A new empty API data root may not yet have an idempotency file. Symlinks/special files are rejected. Nothing is filtered, repaired or silently omitted. A backup faithfully preserves existing corruption too.

Restore verifies the backup and the restored bytes, and refuses any existing target. It does not parse/rewrite records or start an API. For an application-level drill, use a **second restored copy**, the exact recorded API binary and separate loopback ports under an operator-approved runbook. Startup can mark previously active runs failed and write data. Verify expected experiments/runs/counts, native manifests/comparisons, cached idempotent responses, and startup logs for skipped records. Do not run the drill on the original backup or real data. The 2026-11-07 release performed this drill for saved read responses and native comparison; fixture tests separately verify idempotent-response replay. Keep backups private, access-restricted and off the source disk as appropriate; deployment logs/manifests can also contain private paths and identifiers.

## Promotion, rollback and logs

1. Save the previous API/frontend executable paths, Node version, plist copies and release evidence privately. Keep the previous immutable release pair. A failed candidate package/check requires no service change; discard only its new directory after diagnosis.
2. Run the offline backup above before an API change. Check the planned data format against the previous API. Package the intended pair under a new name and run `verify`. Do not modify a live release in place. Test the candidate using disposable restored data on separate loopback ports before selecting it.
3. In the maintenance window, edit the host plists to the new absolute API/frontend pair and external data path, then bootstrap the selected jobs. With changed ProgramArguments, `kickstart` alone does not reload the plist: boot out then bootstrap it. Repeat local readiness, private Serve readiness and the product checks required by the release evidence. Record both selected paths, revision pair, build ID, result and log locations. Nothing in `research_delivery.py` promotes automatically.
4. On a failed promotion, block ingress and boot out the candidate jobs. Retain their logs and data for diagnosis. Restore saved plist paths to the previous release pair, bootstrap, and repeat readiness. Binary rollback against current data is allowed only when backward compatibility is known. Otherwise restore the pre-change backup to a **new** directory and point the old API at it. Preserve failed-candidate data. A data rollback loses mutations after the backup; decide that explicitly before reopening access. The script never replaces a data directory or switches supervisors.

LaunchAgent logs are files, not a health gate. There is no committed rotation/retention policy or log collector. Inspect startup errors, skipped-record messages and restart loops; monitor free disk space for data, backup and log volumes. During maintenance with writers stopped, archive the configured stdout/stderr logs to a private timestamped location and restart with new files, or configure an operator-managed rotation policy that reopens files safely. Moving a live log file does not force a process to reopen it. Do not commit logs containing private experiment names, idempotency keys, Serve URLs or filesystem paths. The [durability repair](reviews/API_DURABILITY_2026-11-07.md) propagates persistence failures and retains uncertain reservations; absence of a log error alone remains insufficient evidence of persistence.

Focused, disposable tooling checks (no build or real service/data access):

```sh
python3 -B scripts/deployment/test_research_delivery.py
```

The `research-web` CI job now runs frozen frontend installation, generated-type
drift checks, types/lint/invariants, a production build, browser recovery and real
Next-proxy/C++ workflows, the native HTTP contract and the 40 delivery fixtures.
The existing jobs cover C++ tests/tools and research determinism. CI does not
promote a release or perform a real-host data restore. The historical audit and
its original evidence limits are preserved in
[DELIVERY_AUDIT_2026-11-07.md](reviews/DELIVERY_AUDIT_2026-11-07.md).


## 2026-11-07 storage upgrade precautions

The new API reads legacy `idempotency.jsonl` and writes per-key records in `idempotency/`. Keep both stores in every full-data backup. An older JSONL-only executable ignores new replay protection, so do not perform a writable binary-only downgrade after new mutations. Use a reader compatible with both stores, or an explicit offline data rollback that preserves the later directory separately and acknowledges which later work it would discard.

Before promotion, check for active runs and arrange quiescence rather than destroying their in-memory simulations. Stop all data writers and automatic restart jobs before using the offline backup tool. Validate a restored disposable copy with the new API before switching the installed data directory. Never start two APIs against the same directory. Malformed records or incomplete terminal caches now fail startup; preserve the diagnostic and original bytes rather than deleting records to make readiness green. An uncertain reserved key is intentionally not retried automatically and has no reconciliation endpoint. These guarantees concern single-writer process restart, not an end-to-end power-loss transaction.
