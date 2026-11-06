# Independent frontend recovery review, 2026-11-07

Report-only review of integrated `4595fa0`, compared with `6d5c29f`, in harness checkout `/Users/jedi/.t3/worktrees/lob/t3-d6afb8a8`, branch `t3/frontend-quality-review`. The intervening delivery commit is background, not a reviewed backend implementation. Read project AGENTS, WORK_PLAN, PRODUCT, quality PLAN and the parent's reliability evidence. Implementation reports were treated as claims.

**Disposition:** four confirmed P2 findings, one introduced variant-navigation regression and three retained recovery defects. No P0/P1 defect established within this scope. This is not browser, accessibility, backend durability or release clearance. The checked recovery acceptance items are too broad for the behavior below.

## Findings

### FR-01 / P2 / confirmed: ordinary numeric-field editing crashes the creation form

- **Locations:** `web/src/components/research/experiment-create-view.tsx:367`, `:416`, `:417`, `:418`, `:419`; `web/src/lib/exact-decimal-format.ts:16` and `:129`. Retained from the base, still present in the changed creation workflow.
- **Trigger:** start a new experiment, select all text in Requote interval, Feed, Order entry, Acknowledgment or Cancel, then press Backspace before entering the replacement. Pasting non-decimal text also triggers it. These are text inputs; `inputMode="numeric"` does not validate input.
- **Failure and impact:** the draft immediately accepts `""`, then the next component render calls `formatDurationNs("")`, which throws `not an exact decimal string: ""`. The whole form fails before the service can validate or display field issues. Reloading discards the unsaved research configuration. Simulation end already guards invalid text, but the other duration fields do not.
- **Evidence:** opt-in reproducer `FR-01` invokes the actual TextField callback and the next actual component render; its no-throw assertion fails with this exception. No browser crash recording was made.
- **Fix:** keep arbitrary draft text editable. Validate before computing display descriptions, showing an absent/invalid description until valid. Keep the strict formatter strict for valid API values. Add a real keyboard clear/type/recovery regression for each described duration input.

### FR-02 / P2 / confirmed: query navigation can create a variant from the previous source

- **Locations:** `web/src/components/research/experiment-create-view.tsx:174`, `:179`, `:188` to `:199`; `web/src/app/experiments/new/page.tsx:8`. Introduced by the new `from` workflow.
- **Trigger:** visit `/experiments/new?from=exp_fixedspreadw2`, edit Name or any configuration field, then navigate within the same page to `/experiments/new?from=exp_fixedspreadw4` (a client Link or same-page query history navigation, retaining the component instance).
- **Failure and impact:** the resource key changes and fetches the second parent, but `editedDraft` takes unconditional precedence over that parent. The form still contains the first parent's config and lineage. A previously valid `previewOutcome` also remains current if its request text is unchanged. The new URL can therefore launch a fully reviewed but wrong-source research variant without requiring review of the requested second source. This is an intent/lineage error, not a precision error.
- **Evidence:** opt-in `FR-02` edits the actual first form, changes the mocked Next query while retaining hook state, and supplies the actual second fixture parent. Expected second-parent name is `Fixed spread 4 ticks (candidate) variant`; received `Edited variant A`. Source inspection establishes that config, lineage and preview state have the same lifetime. Actual Next/browser history preservation was not exercised here.
- **Fix:** put all mutable creation state, including preview, keys, pending flags and launch choice, in a child keyed by source identity (`from` or a distinct no-source value). Alternatively store/check the source identity with the draft and invalidate all associated intent state on change. Do not merely change the displayed parent label. Add same-route query navigation coverage after editing and after previewing.

### FR-03 / P2 / confirmed: a disconnect after first load silently leaves a live-looking run and actionable controls

- **Locations:** `web/src/hooks/use-api-resource.ts:48`; `web/src/components/research/run-detail-view.tsx:327`, `:350`; `web/src/components/research/run-inspect-view.tsx:452`. Retained behavior, not repaired by the new initial-load retry action.
- **Trigger:** load a nonterminal run successfully, then make subsequent `/v1/runs/{runId}` reads return 503 or lose connectivity. Other artifact reads can remain cached and successful.
- **Failure and impact:** the resource hook intentionally retains last data alongside failure, but both views display the failure only when there is no run. Once loaded, no warning or retry control appears. Summary, event count, run badge and allowed actions remain based on an old observation while the actual run may advance, finish or be changed by another client. A trader cannot distinguish a paused/stationary simulation from an unavailable status feed. Control submission may eventually return a state conflict, but that does not repair the false observation before submission.
- **Evidence:** two opt-in `FR-03` assertions supply the exact retained-data-plus-failure state produced by the hook to the actual summary and inspector component bodies. Neither returns an ApiFailureAlert. These are component-level reproductions, not a network/browser run.
- **Fix:** render run refresh failures even with retained data; label last-known state as stale and provide reload. Define what controls remain usable against stale state, with an explicit status check before new intent where appropriate. Preserve the same idempotency key when resolving an uncertain mutation outcome. Add success-then-failure-then-recovery browser coverage on both views.

### FR-04 / P2 / confirmed: several critical one-shot evidence reads have no in-place recovery

- **Locations:** `web/src/components/research/run-inspect-view.tsx:196`, `:411`; `web/src/components/research/experiment-detail-view.tsx:53`, `:191`; `web/src/components/research/run-detail-view.tsx:325` and `:388`; creation parent-selector resource at `experiment-create-view.tsx:177`, `:258`.
- **Trigger:** a single lifecycle request fails after selecting a fill/order on a terminal or otherwise stationary run. Restore connectivity, then select the same fill/order again. Related cases: a one-shot experiment/assumptions read or run-series request fails; parent-list fetching fails on the creation page.
- **Failure and impact:** lifecycle failure returns an alert with no retry callback. Selecting the same order does not change its resource key, so the request is not rerun. A trader must select some other order first or reload and lose their investigation context. Experiment and assumptions reads likewise expose no in-place retry. Run-series failure is not rendered at all, so charts silently disappear; parent-selector list failure is also not rendered, so available lineage choices silently vanish. These are distinct missing consumers of the existing resource error/reload contract.
- **Evidence:** opt-in `FR-04` selects a lifecycle state, supplies failure, invokes the actual FocusPanel and asserts that its ApiFailureAlert offers retry. The assertion fails. Remaining locations are source-confirmed; no browser reproducer was run for them. Other inspect artifact pages/books and comparison errors already offer retry.
- **Fix:** pass lifecycle reload through FocusPanel; render the missing series/list failures; wire the existing experiment/assumptions reload callbacks. Retain selected run/order/event across retries. This requires no new state or fetching framework. Regress stationary-run retry, not only a run whose next progress update happens to change the key.

## Design and bounded-work objections

**O1 / confirmed unbounded algorithm, plausible capacity impact:** `run-inspect-view.tsx:417` to `:422` adds a hidden full enumeration of every fill for the selected order using `readAllPages`, on each processed-event-count change. Visible tables request bounded 200-record pages, but selecting one fill can still issue arbitrarily many sequential requests and retain all order fills. Order replacement identities can span a long logical order history under the contract. An unrelated later-page failure also prevents publication of the refreshed selected fill even if it was on the first page. `useApiResource` cancels publication, not the outstanding pagination reads, when selection or run progress changes. No memory/latency stress measurement was run, and this is not a confirmed browser hang. Prefer a bounded lookup for the selected fill identity, or a cancellable selected-fill search that stops when found and discards other records. A dedicated identity lookup can be a later coordinated API change; do not duplicate fill-accounting logic in TypeScript to avoid it.

The shared complete-list loader, stable SHA-256 seed key, cursor-history hook and synchronous control guard are appropriately small for the requirements. No objection to adding Playwright with one worker or to keeping exact arithmetic outside chart floats. Do not respond to these findings with a global mutation registry, a second API layer or a new state framework. Source-keyed form state and consistent consumption of the existing resource contract are sufficient for the confirmed findings.

Mounted-view control retries and backend idempotency retention are explicitly limited in the parent's report. I do not recast a reload as an automatic replay requirement. Conflicting `startPaused` bodies share a seed key and should fail explicitly rather than create a second run; backend retention, restart and in-flight reservation need the separate durability gate. No durability-branch claim is made here.

## Highest-risk inspected areas without an additional confirmed defect

- **Seed batch and mutation identity:** `research-workflow.ts:37` to `:57`, create save/launch split at `experiment-create-view.tsx:236` to `:250`, detail missing-seed launch at `experiment-detail-view.tsx:56` to `:72`. Full exact experiment/seed pair is hashed, not truncated; all run pages are read before launching; recorded failed/cancelled seeds are preserved; a run-launch failure does not discard the saved experiment. Two same-body simultaneous launchers and accepted/lost replies pass the existing focused tests against MockResearchService. This does not prove the native service's persistence/concurrency behavior.
- **Control dispatch:** `run-detail-view.tsx:40` to `:115`. Ref guard is synchronous; unchanged retryable action retains its key; fingerprint includes run, action and step count; step validation occurs before Number conversion and on form dispatch; count is safely bounded to 100000. No further arithmetic or duplicate-dispatch defect established. Dynamic allowed-actions/stale data still need FR-03.
- **Preview intent:** request-text equality prevents creating from an edited, unreviewed body; unchanged re-review reuses the key; fieldset disables ordinary edits during preview/save. First-load parent cloning preserves exact config and lineage. FR-02 concerns a later source-identity change, not the normal clone path. No actual create lost-response browser test is present.
- **Complete experiment/run lists:** `readAllPages` follows opaque cursors, keeps filters unchanged through wrappers, deduplicates exact IDs, follows empty intermediate pages, rejects cycles and rejects later-page failures rather than returning a partial success. Existing focused cases pass. Lists are refreshed reads, not append-consistent snapshots; that limitation is disclosed.
- **Inspect identity and freshness:** RunInspection is keyed by runId; cursors/focus reset on run navigation; fill selection uses event position plus order ID, not match ID alone; selected-order and lifecycle keys advance with processed count; visited fill/order cursors request 200 per page. No cross-run selection leak found. O1 limits the bounded-work claim; stationary failure recovery remains FR-04. Off-page live markout and late lifecycle freshness are not behaviorally proved by the existing browser test.
- **Exact values:** inspected create request construction, seed-key input, exact decimal/BigInt formatters, event stepping, comparison sign handling and chart conversion/tooltips. Chart numbers are plot positions; values shown in tooltips use retained strings. Existing decimal tests pass at large integer boundaries. FR-01 is invalid draft handling, not a reason to weaken exact parsing.
- **Proxy:** `route.ts:23` to `:86` rejects cross-site/form/opaque-origin POSTs; validates the v1 prefix; encodes segments; forwards only named headers; counts actual streamed request bytes; combines caller abort with a 120 s upstream timeout; catches body-read/network failure into a generic structured 503. Existing five boundary tests pass. Same-origin validation compares host, not full scheme/origin; deployment Host trust and browser Fetch Metadata enforcement were not independently exercised. Request-body read time and upstream response size are not bounded by this code. No demonstrated exploit or current-data endpoint contact is claimed.
- **Error envelope:** malformed error shapes are checked before UI rendering, preserving a structured fallback. Retained resources are deliberately keyed and late results are suppressed by the hook cleanup. Consumer omissions are FR-03/04.

## CI and browser-test sufficiency

Reviewed all six `web/browser-tests/research-workflow.browser.ts` cases, `playwright.config.ts`, asset preparation, and `.github/workflows/ci.yml:30` to `:77`. Standalone launch, one worker, fresh per-test fixture service, type drift, frozen Bun install, lint/types, production build, browser failure traces and separate native HTTP contract are useful gates. Asset preparation copies both static and public directories into standalone output. No packaging flaw was established by static inspection. The GitHub job has not been run by this review.

The six cases are not sufficient to close the broad acceptance items:

1. No invalid/intermediate duration editing, same-route variant-query change, run-to-run client navigation after selection, slow/stale request races, or post-success run polling failure. All four findings evade them.
2. The variant test checks preview bodies and skew but never creates the variant. No accepted experiment-create lost reply, delayed preview with attempted edits, unchanged re-review key comparison, or conflicting multi-tab launch choice in a browser.
3. The pagination case forces two records/page and checks lifecycle heading retention, not all pages, full experiment/run selector pagination, off-page markout updates, later lifecycle reports, selected-read failures or cursor errors. It does not verify selection refresh while a run advances.
4. The offline case fails the initial experiment list only. It does not test stale data after success, or one-shot evidence recovery.
5. The mobile test often waits only for a heading, which can precede API-loaded table/chart content. It is not an overflow audit of every fully populated route. There is no accessibility engine or screen-reader check, focus/error-announcement verification, focus persistence after paging, or keyboard traversal of portal selectors and disabled forms. Row Enter handlers and named fill/order tables are useful but not equivalent to an accessibility audit.
6. All browser research requests are intercepted before the Next proxy and served by the TypeScript mock. Route unit tests and direct C++ contract tests are separate gates; no integrated browser -> Next proxy -> native API path is established. The native contract gate is valuable, but cannot by itself prove React mutation/navigation behavior against that backend.
7. Proxy tests assert that a signal exists, not that timeout/body-stream cancellation and caller abort actually terminate work. No deadline-duration wait was added here; use controlled signal/fake-clock or bounded loopback-stream tests rather than a 120-second sleep.

Before promotion, parent should add focused browser regressions for FR-01 to FR-04 and exercise the integrated native path after the durability/execution work lands. Keep the existing mock cases for deterministic transport-failure injection. Do not substitute this review's shallow component harness for React scheduling, DOM accessibility or real backend checks.

## Reproducers, checks and limits

Added only `web/review-tests/frontend-recovery.review.test.tsx`. It invokes actual component bodies and callbacks using isolated hook slots plus mocked navigation/resource state. It is a report-only reproducer, not a React renderer. It intentionally lives outside `web/tests` and must run alone because of module mocks. The five known-defect assertions are skipped without the opt-in flag; convert them to ordinary post-fix regressions in an appropriate real renderer/browser rather than adding this mock harness to normal CI.

Commands run from this checkout's `web/`:

```sh
bun install --frozen-lockfile --ignore-scripts
bun test ./tests/research-workflow.test.ts ./tests/proxy-boundaries.test.ts ./tests/api-errors.test.ts ./tests/exact-decimal-format.test.ts
EXSIM_RUN_FRONTEND_REVIEW_FAILURES=1 bun test ./review-tests/frontend-recovery.review.test.tsx
bunx eslint review-tests/frontend-recovery.review.test.tsx
bunx tsc --noEmit --incremental false
```

- Frozen installation: passed; no dependency/lock changes. Log `/tmp/exsim-frontend-review-install.log`.
- Existing focused tests: **29 passed, 0 failed**, 103 assertions, 174 ms. Log `/tmp/exsim-frontend-review-focused.log`.
- Report-only opt-in reproducers: **5 expected failures**, four findings, 517 ms. Log `/tmp/exsim-frontend-review-reproducers.log`. These failures are the evidence, not a successful regression gate.
- Default reproducer invocation: 5 skipped, 0 failures, `/tmp/exsim-frontend-review-default.log`.
- Scoped new-file ESLint and frontend typecheck: passed, empty success logs `/tmp/exsim-frontend-review-lint.log` and `/tmp/exsim-frontend-review-types.log`.

No production code edit, dependency/lock update, browser/preview run, build, full suite, C++ compile, Docker job, benchmark, service start, production endpoint/data access, deployment, push, PR or merge. The checkout had no standalone build, so an actual browser reproducer would first need the parent's serial build slot. Ready existing browser-gate commands, for the parent to run after adding the above cases:

```sh
cd /Users/jedi/.t3/worktrees/lob/t3-d6afb8a8/web
bun run build
bun scripts/prepare-browser-assets.ts
bunx playwright test
```

Use the parent's actual integrated checkout instead after applying fixes; these commands refer to this reviewed harness checkout. No new delegate was started. Unfinished execution/durability branches and live research data were outside scope. Findings remain open for the parent to fix. Any subsequent independent re-check must be a new task.
