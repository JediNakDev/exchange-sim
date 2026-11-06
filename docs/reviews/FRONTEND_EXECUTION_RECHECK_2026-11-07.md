# Independent frontend execution re-check, 2026-11-07

Reviewed `7ca10141852a56a6a784cb4c5aa58213efa57d96` in harness checkout `/Users/jedi/.t3/worktrees/lob/t3-a4a5cb93`, branch/task `t3/frontend-execution-recheck`. This is a new independent review, not a continuation of the recovery reviewer. Read AGENTS, WORK_PLAN, PRODUCT, the frontend-design skill, FRONTEND_RECOVERY_REVIEW_2026-11-07 and quality/INTEGRATION_2026-11-07. Backend execution math and persistence are excluded and remain with their separate reviewer.

Harness task ID: `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A064d0df2-8d57-4f6c-932c-13df7a3a7c3d`.

**Disposition: one confirmed P2 regression, FER-01.** FR-01 through FR-04 are repaired for their reported triggers, with four independently rerun real-browser regressions passing. O1's search is cancellable and stops early, but its integration now aliases two fills of the same order. No P0/P1 defect established in the reviewed frontend scope. This is not deployment, accessibility, screen-reader or overall release clearance.

## FER-01 / P2 / confirmed: selecting another fill of the same order retains the previous fill's explanation

- **Location:** `web/src/components/research/run-inspect-view.tsx:420` omits `selectedFill.eventPosition` from the resource key, while the loader at `:421` now reads exactly that one fill. The unconditional preference for `selectedFillResource.data` at `:424` then displays it without checking that it matches the current selection. The resource effect only restarts for its explicit dependencies, `web/src/hooks/use-api-resource.ts:60`.
- **Trigger:** on a completed or otherwise stationary run, select fill A; let its selected evidence load; select fill B of the **same order**. A multi-fill order is sufficient. No disconnect, pagination, malformed response or timing race is required.
- **Observed failure:** the second row becomes selected and the event stepper moves from event 55 to 90, but the explanation heading stays `Fill 4` instead of changing to `Fill 7`. Both fixture fills are buys of order 10; their position-after values differ, 100 versus 200. The old fill's ledger, markouts and related-event links stay preferred over B's currently displayed table record. With a fixed processed-event count there is no automatic recovery until another selection changes the key or the page reloads.
- **Impact:** a trader can inspect the right event/row while attributing the previous fill's accounting and market evidence to it. This undermines the core explain-a-fill workflow. It is an introduced regression: the old resource contained all fills of the order and the consumer searched that array by event position; the replacement resource contains one fill but retained the old order-level identity.
- **Evidence:** `web/review-tests/frontend-execution.browser.ts:7` runs the actual standalone React UI in Chromium, intercepting all research requests with a synthetic two-fill fixture. It does not mock React, the resource hook or component bodies. The normal fixture has one fill per order, so its existing pagination test misses this case. Expected `Fill 7` is absent after selecting the second row; the recorded DOM still contains `Fill 4`. This test is intentionally failing against the reviewed build and is outside the normal browser-test directory.
- **Fix recommendation:** include the selected fill's event position in its resource key, alongside run ID, order ID and progress. Also check the returned fill's identity before giving it precedence over the selected table row. Changing fill identity must clean up/abort the previous search. Add the sibling-fill regression to ordinary browser coverage after fixing it, including selection while the previous fill request is pending and an off-page selected fill.
- **Confidence boundary:** the stationary selection defect is browser-confirmed. The same incomplete key also allows an in-flight A search to complete after selecting B without cancellation; that timing variant is source-supported, not independently reproduced here. There is no claim that the fixture proves native matching/accounting behavior.

Final browser log: `/tmp/exsim-frontend-execution-recheck-browser.log`. DOM failure context: `/tmp/exsim-frontend-execution-recheck-browser-results/review-tests-frontend-exec-2d855-es-the-selected-explanation/error-context.md`. Corresponding trace: the adjacent `trace.zip`.

## Re-check of the prior findings

| Area | Current behavior and evidence | Assessment |
| --- | --- | --- |
| FR-01, duration drafts | `experiment-create-view.tsx:30`, `:384`, `:469` through `:472` validate text before strict duration formatting. The real browser clears, temporarily invalidates and restores all five previously crashing fields, then successfully reviews. | Repaired for the reported trigger. Strict API-value formatters remain strict. |
| FR-02, source query intent | `experiment-create-view.tsx:177` through `:194` put draft, preview, key, request failure, pending flags and paused launch choice inside the source-keyed editor. The same-route history change replaces the edited/reviewed A form with B and disables creation pending a new review. | Repaired. No reuse of A's creation state across a different `from` identity. |
| FR-03, stale status | `run-detail-view.tsx:354`, `:378` disclose retained data; `:50` blocks dispatch and `:85` disables native controls. Run-keyed controls retain uncertain-action keys across status failure/retry. `run-inspect-view.tsx:474` identifies the last known range. Browser verifies success, subsequent failure, blocked summary control and recovery on both routes. | Repaired for observed polling failure. This does not imply instantaneous disconnect detection or cancellation of a previously dispatched mutation. |
| FR-04, stationary evidence | Lifecycle consumes its retry callback at `run-inspect-view.tsx:198` and `:533`; run series at `run-detail-view.tsx:379`; saved experiment/assumptions at `experiment-detail-view.tsx:53` and `:191`; parent list at `experiment-create-view.tsx:278`. Browser independently recovers stationary series and lifecycle without changing selection. | All named consumers now wire reload. Browser rerun covers series/lifecycle; the other paths were inspected, not independently failure-injected. |
| O1, selected-evidence work | `research-workflow.ts:67` through `:82` searches pages without retaining unrelated fills, forwards AbortSignal, checks it before each page, stops at a match/passed position, and rejects missing evidence/cursor cycles. `use-api-resource.ts:57` aborts cooperative loaders on cleanup. Pure probes verify in-flight signal propagation, exact positions above 2^53, early stopping and cursor-cycle bounds. | Algorithmic concern addressed, subject to FER-01's missing identity/cancellation boundary. Late matches can still require many pages; no O(1) claim. |

Baseline form identity is also addressed: `experiment-create-view.tsx:281` keys the native form to baseline identity; `:288` gives the disabled fieldset its own layout. CSS scopes form grouping to actual field groups. Parent browser `execution-api.browser.ts:56` through `:82` selects TWAP, edits schedule fields, returns to Name and creates successfully. I inspected that real-renderer test and its successful parent log; I did not independently rerun its native path or make a new layout-engine causation claim.

## Execution, capability and contract areas inspected without another confirmed finding

- **Capability intersection and unknown kinds:** `experiment-create-view.tsx:53`, `:196` through `:197` intersect known editors with advertised executable kinds before choosing/displaying baselines. Method/quoting selectors filter by the same intersection (`:376`, `:418` through `:419`), and creation is disabled for an unsupported current strategy (`:512`). A saved unsupported source cannot be launched through this form. The parent unknown-kind browser case injects an unknown capability and baseline, then verifies it is absent from options. Empty known intersections render an explicit no-baselines state. No arbitrary fallback strategy was found.
- **Execution variants preserve assumptions:** source drafts clone the complete stored config, seed set and horizons and retain lineage (`:200` through `:208`). Switching TWAP/sweep replaces only `config.strategy` (`:411` through `:415`), carrying parent side/quantity, start, window and protection. Scenario, fees, latency, risk, initial cash, seeds and bounds stay in that draft. Choosing a different catalogue baseline intentionally replaces the complete baseline config. The parent real proxy/native flow executes sell TWAP, creates a sweep variant and verifies paired seed 11; it does not deeply assert every copied cost/scenario field in the browser, so the preservation conclusion also depends on source inspection.
- **Execution controls/disclosures:** fields expose side, quantity, start, observation window, protection and TWAP interval (`:424` through `:436`). Copy explains IOC price protection, child risk caps, catch-up/skipped slices, no extra final slice, sweep retries and simulation completion versus parent completion. Shared risk quantity is labeled as a child cap (`:490`). No JS strategy simulation was added.
- **Exact values and unavailable evidence:** `execution-summary-panel.tsx:17` through `:28`, `:44` through `:51` format strings without converting money/IDs to Number, retain null as unavailable and show missing-metric reasons. Totals and per-share metrics use separate labels/units in summary and paired comparison. The panel distinguishes a censored window, missing decision/fills, decision exchange mid versus delivered mid, positive adverse costs, executed-only costs, unavailable residual opportunity cost and the participation denominator including own trades. Six focused probes include actual React DOM server rendering of exact five-decimal money above 2^53, a negative tiny fee, null versus zero, censored text and an omitted legacy execution block. This verifies presentation only, not backend formulas. Timestamp display intentionally rounds to its stated display precision.
- **Generated-client boundary and minimal patch:** both Readable declaration variants retain null-only keys before applying the write-only exclusion. The generated OpenAPI declaration is unchanged and independently regenerated output byte-compares equal. The compile-time client assignment in `tests/api-null-fields.typecheck.ts` is included by the project's tsconfig and passes with a frozen patched installation. Writable mapping and runtime client behavior are unchanged by the declaration patch. No handwritten execution response substitute was introduced.
- **CI and isolation:** `.github/workflows/ci.yml:42` through `:68` freezes patched dependencies, checks generated drift, runs types/lint/invariants/build, builds the native API **before** browser cases, then runs HTTP contract checks. Standalone browser setup copies static assets and uses one worker. The two native browser cases launch a private disposable C++ API on loopback 3763 and use the Next proxy on 3762; the other eleven cases intercept research requests. Reviewed parent logs corroborate frontend76, HTTP9/all23operations/284assertions and browser13/two native paths. These are real logged parent gates, not thirteen native browser tests, and not a GitHub CI execution claim.

## Checks and artifact provenance

No new native or Next/browser build was performed. For independent browser reproduction I copied the existing parent standalone bundle read-only into `/tmp/exsim-frontend-execution-recheck-bundle.hOCo9q`. Parent source HEAD was `1dddc49`; its frontend source/config/dependencies had no diff from reviewed `7ca1014`. Existing build ID: `AELSM7AQE1ZMBctebXnUM`. The copied inspector chunk `0d8ruue_jjow0.js` SHA-256 is `e24994987a55e2cc95ff603399a3e83a95191e162167cf3eb23e66842803cdd4`. This is reuse of the parent's artifact, not an independently rebuilt provenance guarantee. Static review and type/probe checks use this review checkout's source.

The copied server uses private loopback 3862; its upstream is explicitly loopback port 9 and every research API request is intercepted. No native API or installed service was started/contacted by these browser checks. Playwright closes its disposable server/context after the run.

| Independent check | Result | Exact log |
| --- | --- | --- |
| Frozen patched install, scripts ignored | Passed; dependency/lock files unchanged | `/tmp/exsim-frontend-execution-recheck-install.log` |
| Workflow/decimal/error/proxy subset | 32 passed, 108 assertions, 244 ms | `/tmp/exsim-frontend-execution-recheck-focused.log` |
| New pure/helper and actual static-renderer probes | 6 passed, 19 assertions, 281 ms | `/tmp/exsim-frontend-execution-recheck-probes.log` |
| Four original recovery browser regressions plus FER-01 | Four passed; one expected defect failure | `/tmp/exsim-frontend-execution-recheck-browser.log` |
| Full frontend TypeScript check, no incremental output | Passed, including new review files and null-only contract test | `/tmp/exsim-frontend-execution-recheck-types.log` |
| Scoped review-file lint | Passed, zero warnings | `/tmp/exsim-frontend-execution-recheck-lint.log` |
| Regenerate schema to /tmp and byte-compare | Passed | `/tmp/exsim-frontend-execution-recheck-schema.log` |

An intermediate browser run used my review config's 1.5-second assertion deadline, shorter than the inspector's 2-second polling interval. That produced an additional stale-status timeout, not a product finding. I restored the ordinary 5-second deadline and the stale-status test passed. Historical intermediate log: `/tmp/exsim-frontend-execution-recheck-browser-short-timeout.log`. The final log above is authoritative for the five focused cases.

Repeat cheap checks from `web/`:

```sh
bun test ./tests/research-workflow.test.ts ./tests/exact-decimal-format.test.ts ./tests/api-errors.test.ts ./tests/proxy-boundaries.test.ts
bun test ./review-tests/frontend-execution.review.test.tsx
bunx tsc --noEmit --incremental false
bunx eslint review-tests/frontend-execution.browser.ts review-tests/frontend-execution.playwright.config.ts review-tests/frontend-execution.review.test.tsx --max-warnings 0
```

The focused browser reproducer can use the copied artifact while it exists:

```sh
EXSIM_REVIEW_STANDALONE=/tmp/exsim-frontend-execution-recheck-bundle.hOCo9q/server.js bunx playwright test --config review-tests/frontend-execution.playwright.config.ts review-tests/frontend-execution.browser.ts
```

After a production fix, the parent must use a newly built artifact, not this old copied bundle. In its serial build slot, from its integrated `web/` checkout:

```sh
bun run build
bun scripts/prepare-browser-assets.ts
bunx playwright test --config review-tests/frontend-execution.playwright.config.ts --grep 'FER-01|duration drafts|changing variant|loaded run status|stationary lifecycle'
```

Expected post-fix result: all five pass; B's Fill heading/ledger replace A's, A disappears, and two selected-resource reads occur. Browser installation is an environment prerequisite, not a reason to start another compiler.

## Remaining coverage limits and next action

- The new tests do not validate screen readers, focus restoration, accessibility announcements or every populated mobile execution layout. Existing mobile heading/width checks remain narrower than that.
- The paired comparison has labels/units/sample counts but no per-run observation-status badge or unavailable-reason display. The detailed window/cost conventions are clearest on the run summary. No wrong numeric result is established here; a browser test comparing censored/undeclared versus observed windows and checking the trader's route to the explanation is still missing.
- Source-switch navigation during a pending preview/save, lost experiment-create replies followed by unchanged re-review, same-order selection during an in-flight search, stationary failed selected evidence off-page and a very late multi-page selection are not covered by this independent browser rerun. Signal propagation tests do not prove the Next proxy/native request stops working on disconnect. No bounded timeout/body-stream cancellation audit was added.
- The two actual proxy/native execution browser flows, production build and HTTP gate were read from the parent's source/logs, not rerun here. Backend math, persistence, Linux validation, copied release/restore readiness, real production data and deployment remain excluded.

Parent should fix FER-01 and run its focused renderer regression against the updated standalone output. Any subsequent independent re-check must be a new task. This review changes only its report, focused review files and the WORK_PLAN completion record; no production edit, dependency/lock change, full suite, Docker job, benchmark, new delegate, push, PR, merge or promotion.
