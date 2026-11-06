# Execution and recovery integration checkpoint

This is a verified implementation checkpoint, not independent release clearance or a new deployment. The user requested continued product completion and authorized isolated GPT delegates. The first workflow remains single-venue synthetic execution and market-making research. Matching, scheduling, strategies and accounting remain C++.

## Integrated sources

- Delivery tools `eca0f55` integrated as `7fbf6c0`; 40 disposable fixture tests passed on the parent. No live-host inspection or actual restore is implied.
- API durability `f6b0304` integrated as `795876d`. Seventeen new tests cover I/O failure, uncertain mutations, malformed state, capacity, restart and terminal persistence. The child demonstrated 13 failures on the original code, including an injected post-mutation write failure and abrupt restart.
- Execution API `944ef81` integrated as `bc07370`. Both sides of TWAP and immediate-sweep execution use existing runner strategies, native artifact verification and exact parent metrics. See [execution contract](../reviews/EXECUTION_API_2026-11-07.md).
- Independent frontend review `9769191` integrated as `c7c73cc`. Its four findings and selected-evidence work objection are addressed in the parent changes below. The report is preserved unchanged; no old assertion was weakened to certify the repairs.

## Trader behavior

The creation form intersects known editors with service capabilities and filters the baseline catalogue accordingly. It exposes fixed-spread and inventory-aware market making, TWAP and immediate sweep. Copying a saved execution experiment preserves scenario, seed set, fees, latency, limits and parent lineage; switching its execution method changes only the strategy fields.

Execution controls cover parent side/quantity, start, observation window, protection distance and TWAP interval. Children are price-protected IOC limit orders, not unprotected market orders. The shared risk quantity is explicitly labeled as the child cap. The form explains cumulative TWAP targets, skipped/capped slices, lack of an extra final slice, and sweep retries on delivered market updates. Parent completion is distinct from the simulation's completed state.

The summary displays declared/filled quantity, benchmark and delivered mids, decision/first/last fill times, fees/notional, completion/residual, signed executed costs and participation denominator. Missing values remain unavailable, and a truncated observation window remains censored. Costs cover executed quantity; residual opportunity cost is explicitly unavailable. Labels cover all new metrics in per-run and paired comparison views. Old summaries may omit the optional execution block.

The real-browser test configures a sell TWAP with a child cap that guarantees a residual, runs it against C++, creates a sweep variant with the same assumptions/seed, and compares their reproduced native evidence. A second real-browser case reviews and executes inventory-aware quoting. Both use the actual standalone Next proxy and private C++ test process, without route interception. The remaining browser cases use isolated fixtures for deterministic failures.

## Independent frontend findings repaired

- **FR-01:** incomplete duration drafts are checked before display formatting; strict exact formatters remain strict. Keyboard clearing, invalid intermediate text and replacement are covered in the browser.
- **FR-02:** all creation intent is owned by an editor keyed to the `from` source. Same-route query changes reset draft, preview, keys, pending flags and launch choice. A browser changes the source after editing/reviewing and verifies the new source and invalidated preview.
- **FR-03:** retained run data is accompanied by a stale-status alert and retry. Native controls and dispatch handlers reject new intent until status recovers. Existing uncertain-action keys remain mounted. Browser checks success, polling failure and recovery on both summary and inspector.
- **FR-04:** lifecycle, experiment, assumptions, series and parent-list failures consume their reload callbacks. Missing series/list failures are rendered. Stationary lifecycle and series failures recover without changing selection or reloading the page.
- **O1:** the selected-fill read no longer retains/enumerates every fill. It searches ordered pages, stops at the match or when past its position, discards other records, checks cancellation and forwards AbortSignal to fetch. Resource cleanup now aborts cooperative loaders. Missing selected evidence becomes an explicit failure. A very late fill can still require multiple pages because there is no direct fill-by-identity endpoint; this is not an O(1) lookup claim.

The new real execution flow also exposed identity controls losing rendered layout after a baseline switch. The form DOM is keyed to baseline identity, so switching to a new baseline resets native input/portal state; its outer disabled fieldset uses normal flex layout, and styling targets only actual shadcn field groups. The browser returns from the schedule fields and successfully edits Name before creation. This is tested behavior; no claim about a particular browser engine's internal cause is made.

## Client contract and test gates

The installed openapi-typescript-helpers 0.1.0 Readable mapper removed required null-only fields because NonNullable<null> is never. The new explicit `unexecutedOpportunityCost: null` exposed it at the generated-client boundary. A minimal Bun patch retains such keys in both declaration formats. The generated contract is unchanged; a compile-time client response test would fail without the patch. Frozen installation applies the checked-in patch, and generated schema output remains byte-identical.

The C++ test runner previously returned success for an unmatched filter (0/0). It now returns exit code 2. The before and after CLI logs are `/tmp/exsim-quality-empty-filter-{before,after}.log`. Listing tests remains read-only behavior. No matching, strategy or journal algorithm changed for this fix.

CI now builds the native API before browser cases, checks generated types, uses frozen Bun installation, runs frontend invariants/type/lint/build, real HTTP contract and browser recovery cases, and tests delivery tools. Native tests use an ephemeral data directory and loopback 3763; the standalone test frontend uses loopback 3762. Browser contexts/processes are closed after each run; nothing uses installed service ports or private research data.

## Actual checks

| Check | Result | Exact evidence |
| --- | --- | --- |
| Combined focused C++ API release | 64/64, including 17 durability and 11 execution cases | `/tmp/exsim-quality-api-{build,release}.log` |
| Combined focused C++ API ASan/UBSan | 64/64, no sanitizer diagnostics | `/tmp/exsim-quality-api-debug{,-build}.log` |
| Native API binary build | Passed, -j2 -Werror | `/tmp/exsim-quality-api-binary-build.log` |
| Frontend invariants | 76/76, 2.58 s | `/tmp/exsim-execution-web-tests.log` |
| Actual-binary HTTP contract | 9/9, all 23 operations, 284 assertions, 644 ms | `/tmp/exsim-execution-contract.log` |
| Browser recovery and execution | 13/13, 13.3 s; two actual Next-proxy/native paths and eleven isolated fixture cases | `/tmp/exsim-execution-browser.log` |
| Types and zero-warning lint | Passed | `/tmp/exsim-execution-web-{types,lint}.log` |
| Production standalone build | Passed | `/tmp/exsim-execution-web-build.log` |
| Frozen patched installation and regenerated types | Passed, generated declaration unchanged | `/tmp/exsim-execution-lockcheck.log`, `/tmp/exsim-execution-types-generation.log` |

The ASan binary covered the integrated backend before the later test-runner empty-selection guard; the release gate includes that guard. No full C++ suite, benchmark or new latency comparison was run. Linux and independent review of the new backend remain pending. Original frontend reproducer harnesses are report-only; actual browser regressions exercise the fixes instead. The prior production deployment remains untouched and no current tailnet claim is made for these new commits.

## Storage and rollback boundary

The API persists an idempotency reservation before mutation and atomically saves its completed response afterward. Unexpected failures keep the reservation: retry returns 503 rather than executing again; a changed fingerprint conflicts. Known validation/transition errors release their reservation. New records are in `idempotency/`, with the legacy JSONL retained as readable input. Malformed metadata, missing terminal evidence or conflicting replay protection prevent healthy startup rather than silently dropping state. Persistence errors propagate; failed diagnostics remain visible where they can be saved.

This is at-most-once execution under single-writer process restart, not guaranteed completion, automatic reconciliation, lost-simulation reconstruction or a multi-file power-loss transaction. An uncertain key requires inspection; no reconciliation endpoint exists. An old executable does not read the new key store, so writable binary downgrade is unsafe. Promotion requires an offline full-data backup, validation of a restored copy with the new service, and a compatible-reader or explicit data-rollback plan. No active run should be destroyed by an automatic upgrade.

Next gates: independent backend review, independent frontend re-check, focused Linux GCC/Clang validation, resolve findings, then promote a validated copied release and update delivery/media evidence. This checkpoint is not permission to skip those gates.
