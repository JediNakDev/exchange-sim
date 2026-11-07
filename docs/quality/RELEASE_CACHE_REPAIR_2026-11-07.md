# Post-use release inventory repair

The first deployed frontend remained functional, but post-use delivery
verification found **16 added entries** under `frontend/.next/server/route-cache`.
No manifest entry was removed or changed. Next 16.4.4 generated HTML/RSC/metadata
cache files while serving dynamic route families. The inventory check correctly
rejected the additions; checksumming a bundle only before navigation was
insufficient to establish an immutable deployment.

The initial compound shell command ended with a successful whitespace check,
masking readiness's nonzero exit status until its saved log was read. This
corrects the earlier release checkpoint's post-use readiness claim. Its failed
receipt remains `/tmp/exsim-release-8ee86f1-final-ready.log`; it is not pass evidence.
The final deployment driver propagates failures and the final readiness receipt
is checked directly.

## Repair and regression

Frontend commit `a0b3c8d4734b3ef209d784e1459ade2af457ceb4` sets
`experimental.isrFlushToDisk: false`. The installed Next 16.4.4 declarations accept
this setting, and both route-cache/incremental-cache construction paths consume
it. Route responses can be regenerated in process without changing the copied
release tree. The C++ binary, HTTP contract, accounting and UI components are
unchanged.

The real TWAP/sweep browser case now inventories the entire standalone tree
before navigation and after its full create/run/inspect/compare workflow. It
compares file hashes, executable bits, directory entries and symlink targets.
This covers the actual filesystem contract, rather than asserting a config
property. Both real C++ browser cases pass **2/2** in 4.3 s on a fresh Node 24 build;
types and scoped zero-warning lint pass. The five selected-fill async/navigation edge cases also pass 5/5 in 7.1 s on this corrected artifact. Existing independent UI/backend review
clearance remains scoped to their unchanged behavior; it is not an independent
review of this operational setting.

## Final artifact and handoff

- API source stays `8ee86f1`; SHA-256
  `b6f020104ebf1cdf38015c4004187024bce92a17d58e937945623c5e2b2f9b8a`.
- Frontend source is `a0b3c8d`; build `1S7lB4_DnycLm77V_UTS_`; Node v24.22.1.
- Fresh copied manifest contains **1,762 entries**. Private dynamic-route warmup
  and verification pass both before and after stopping its test processes.
- A second full offline backup includes all **13** completed runs and the new
  per-key idempotency store. Both supervisors were unloaded and absence of
  another API writer checked. A restored copy passes **101 exact saved read
  responses**, including native comparison, with no metric filtering needed.
  Warming all saved experiment/run HTML routes leaves its package unchanged.
- Before promotion, the current data tree again matches this latest backup
  byte for byte. Both supervisors now use the copied pair at
  `$HOME/Library/Application Support/exsim/releases/production-20261107-a0b3c8d/`.
- The actual live read/preview tour is repeated against this final pair. Full
  package/private HTTPS readiness passes **after** it; no unexpected entry,
  changed asset or log error is accepted. The refreshed MP4 uses this final
  recording. The tour creates no additional runs.

The private backup is
`$HOME/Library/Application Support/exsim/backups/research-pre-cachefix-20261107/`.
Original plists, restored data, expected responses and startup-log deltas are at
`$HOME/Library/Application Support/exsim/maintenance/production-20261107-cachefix/`.
The earlier backups and bundles are retained. The first v1 frontend's package
inventory is known to contain runtime additions and is not current pass evidence.
The data-format/writable-downgrade restrictions remain unchanged.

## Exact receipts

- `/tmp/exsim-cache-fix-{build,browser,types,lint}.log`: build and focused gates.
- `/tmp/exsim-cache-fix-package.log`: final source/build identity and manifest.
- `/tmp/exsim-cachefix-fill.log`: five async/navigation edge cases on the final build.
- `/tmp/exsim-cachefix-preflight.log`: private warmup, before/after verification.
- `/tmp/exsim-cachefix-backup.log`: offline full backup/verify/restore.
- `/tmp/exsim-cachefix-restored-preflight.log`: 101 matching responses and warmup.
- `/tmp/exsim-cachefix-promote.log`: final selection and loopback readiness.
- `/tmp/exsim-execution-tour-cachefix.log`: actual production read/preview tour.
- `/tmp/exsim-cachefix-final-ready.log`: **`ready: OK` after the live tour**.
- `/tmp/exsim-cachefix-video-{encode,verify}.log`: encoded video and full decode.

The first promotion command unnecessarily tried to stop already-unloaded jobs
and exited before selecting a new release. It was resumed from the verified
offline-backup checkpoint, preserving its failed log separately. There was no
data rollback or discarded run. [Final release scope](RELEASE_2026-11-07.md).
