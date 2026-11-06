# Research workflow reliability checkpoint

## Scope

The existing Bun/Next.js/TypeScript/Tailwind/shadcn workstation now handles interrupted seed launches, large research lists and request failures more explicitly. Matching, strategies, accounting and simulated time remain in C++. This checkpoint has no new C++ engine/performance claim and is not deployed yet.

## Fixed behavior and evidence

| Area | Behavior | Regression evidence |
| --- | --- | --- |
| Partial batches | Save the experiment separately from launching seeds. If a launch fails, open the saved experiment; its panel identifies seeds with no recorded run and starts only those. Failed/cancelled runs are preserved. | Browser accepts seed two but loses its reply, then continues the missing third seed. Exactly three runs remain. |
| Initial-run retries | SHA-256 of the full exact experiment/seed pair produces one stable key across callers. Read all run pages before launching; concurrent callers with the same body replay the same seed mutation. | Unit/service tests cover two simultaneous callers, an accepted mutation with lost reply and a fresh-client retry, uint64 seeds and long opaque identities. |
| Control retries | An unchanged failed control keeps its key in the mounted run view. Duplicate clicks are guarded synchronously. Step submissions enforce 1..100000 before both click and keyboard dispatch. | Browser loses an accepted step reply, retries with the same key and observes no second advancement. Out-of-range keyboard submission produces no POST. |
| Variant research | Copy a saved config exactly and retain the parent experiment. Expose both existing market-making models and their parameters. Unchanged re-review retains its creation key. | Browser compares the complete preview body to the saved parent, then changes to inventory-aware quoting and checks the submitted skew. |
| Complete lists | Experiment library/selectors and experiment run lists follow opaque cursors with unchanged filters. A failed later page or cursor cycle produces a failure, never a successful partial list. Overlapping records are deduplicated by exact identity. | Forced unit cases cover empty intermediate pages, overlapping IDs, cursor cycles and later-page failure. |
| Bounded inspection | Fill and order tables show 200-record pages with visited-cursor navigation and explicit page status. Selected fill/lifecycle state stays associated with its run and refreshes as the run advances, including off-page selections. | Browser forces a two-record page, visits next/previous fill and order pages, and retains the selected order explanation. |
| Error handling | Read failures expose a retry action; malformed error envelopes fall back to a safe structured error. Missing activity counts are unavailable, not zero. Book failures cannot masquerade as perpetual loading. | Unit malformed-error cases and browser offline/recovery check. |
| Proxy boundary | JSON-only, origin-checked mutations; actual body bytes limited to 1 MiB; 120 s upstream deadline and caller abort propagation; body-read failures return a structured 503 without internal network/path details. Forwarded TLS host and idempotency headers are preserved. | Five focused route tests reject form/cross-site/opaque-origin requests, over-limit UTF-8 bodies, broken response streams and non-v1 paths, and check valid forwarding. |
| Accessibility/responsiveness | Card titles are section headings. Fill/order tables have accessible names; paging and failure recovery have named controls. Mobile routes retain document width. | Browser checks 390 px library/create/compare/inspect routes and reports no browser exception. Author checks, not an independent accessibility audit. |

## Repeatable checks

From `web/`:

```sh
bun install --frozen-lockfile
bun test ./tests
bun run typecheck
bun run lint --max-warnings 0
bun run build
bunx playwright install chromium --only-shell
bun run test:browser
```

`test:browser` copies current static/public assets into the standalone build, runs it only on loopback 3762, uses one worker and closes its server and browser contexts. Each test intercepts every research API call with a separate in-process fixture. It never calls the installed research service or uses private run data. Playwright failure traces are local ignored output. Node and Bun can both load the schema validator; its path uses `fileURLToPath(import.meta.url)` rather than a Bun-only import-meta extension.

Evidence at this checkpoint:

- Frontend invariant tests: 73/73, 3.01 s, `/tmp/exsim-quality-web-tests.log`.
- Focused proxy/workflow/error subset: 14/14, 295 ms, `/tmp/exsim-quality-unit.log`.
- Typecheck and warning-free lint: `/tmp/exsim-quality-{types,lint}.log`.
- Production build: `/tmp/exsim-quality-build.log`.
- Browser invariants: 6/6, 4.3 s, `/tmp/exsim-quality-browser.log`.
- Integrated delivery script tests from child implementation `eca0f55`, parent `7fbf6c0`: 40/40, 5.596 s, `/tmp/exsim-quality-delivery-tests.log`.

CI now has a separate research-web job for frozen Bun dependencies, generated-type drift, types/lint/tests, production build, browser recovery invariants, the actual C++ HTTP contract, and the delivery fixture tests. The workflow itself has not run on GitHub yet. Existing C++ matrix, determinism and benchmark jobs are preserved.

## Limits and next gates

This is implementation validation, not independent review. The browser tests use API fixtures; existing real-binary contract evidence remains historical until the final integrated backend changes are checked. Control retry state intentionally lasts only for the mounted run view; reloading is not an automatic replay. A conflicting initial-run body from another tab produces a conflict rather than silently changing its paused/running choice. Initial keys depend on server retention and persistence, whose restart/I/O behavior is being repaired by a separate task. Cursor lists are refreshed reads, not transactional snapshots while other clients append new experiments. Arbitrary reruns and replacement of failed seeds are not implied by the missing-seed action.

API execution integration, durability fixes, independent review, final production promotion and deployment readiness are still pending in PLAN_2026-11-07.md. Production services and private data remain untouched by this checkpoint.
