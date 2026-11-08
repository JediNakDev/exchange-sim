# Native execution trace UI implementation, 2026-11-08

Implementation handoff only. No independent clearance, integrated browser result, or human evaluation is claimed.

Checkout: `/Users/jedi/.t3/worktrees/lob/t3-daf09b37`, branch `t3/execution-trace-persona-ui`. Requested parent baseline `50d5ed4`; actual harness starting commit `b707c87` includes the TypeScript consumer follow-up. Read WORK_PLAN, PRODUCT, native repair contract and relevant independent execution/validation findings. No delegates were created.

## Owned changes

- `web/src/components/research/execution-trace-panel.tsx` exports `ExecutionTracePanel({run}: {run: ApiSchemas["Run"]})` for the parent to mount beside execution summaries. It does not fetch summary data or infer whether a strategy is an execution strategy.
- `web/src/components/research/execution-trace-data.ts` contains a one-page native reader, decision-only chart projection and lifecycle URL construction.
- `web/tests/execution-trace-ui.test.tsx` contains browser-independent renderer, projection, transport and source-wiring regressions.

No existing files, schema/API/C++ code, generated types, mock service, global CSS or WORK_PLAN were changed. The parent owns those surfaces. Existing parent dependencies were linked at `web/node_modules` from `/Volumes/PortableSSD/Documents/programming/lob/web/node_modules`; no install or build ran.

## Behavior

The native endpoint is read with limit100 and one opaque cursor per request. No fetch-all loop or automatic polling exists. `useCursorHistory` and `ArtifactPageControls` handle visited pages. `useApiResource` receives the request AbortSignal and a key including run ID/state, cursor and refresh generation. Run/state changes remount pagination; manual refresh clears displayed evidence until success. Failed responses display the native API error/retry action, no retained stale chart, and an explicit unknown end-of-trace statement. Previous remains available after a failed later-page request. Empty successful pages disclose emptiness with refresh/previous guidance.

The panel separates exact decision and delivered-report tables. Decisions expose callback time, cumulative target due, strategy-known fills/residual/outstanding, requested quantity before cap/lot rounding, child cap, position headroom, actually submitted quantity, raw reason/skip, client ID, delivered source sequence/time/touch and limit. Reports expose delivery versus exchange time, kind, exact client/exchange/match IDs, quantities/leaves/cumulative, rejection/cancellation reasons and trace reason. Exchange order IDs link to `/runs/{runId}/inspect?order={id}` while preserving baseline/candidate comparison context. Missing exchange IDs explicitly fall back to client ID/delivery time. Decision rows themselves have only client IDs in the contract, so no exchange identity is invented.

Existing `SeriesLineChart`, chart tokens and decimal-to-chart helpers plot cumulative target versus known fills over simulated callback time. Only `kind=decision` enters the chart. Report-only request zeros cannot fabricate target/fill decisions. Original strings remain available in tooltip data and exact tables, including values beyond JavaScript's integer range and fractional prices. The page's total/current record counts and decision/report counts are explicit. The chart covers only current-page decision rows, with no synthesized final-target or deadline point and no implication that the page covers the run.

Final target quantity/time and observation deadline are distinct exact fields. Full-window versus censored status is explained separately from simulation completion and residual. TWAP's final target time is not represented as a deadline retry. Completed trace verification uses the native `native_reproduction_and_replay_verified` marker; `recorded_terminal_partial` visibly says it is not reproduction verified. Missing verification makes no reproduction claim. Active runs do not request trace evidence and instruct the user to resume/finish to terminal and refresh run status.

## Checks

All final checks passed, without servers, browser, production operations, installs or builds:

- `cd web && /Users/jedi/.bun/bin/bun test tests/execution-trace-ui.test.tsx tests/execution-presentation.test.tsx`: 14 pass, 0 fail, 69 assertions; `/tmp/execution-trace-ui-tests.log`.
- `cd web && /Users/jedi/.bun/bin/bun run typecheck`: pass; `/tmp/execution-trace-ui-typecheck.log`.
- `cd web && /Users/jedi/.bun/bin/bun x --no-install eslint src/components/research/execution-trace-panel.tsx src/components/research/execution-trace-data.ts tests/execution-trace-ui.test.tsx`: pass; `/tmp/execution-trace-ui-lint.log`.
- `git diff --check`: pass before receipt/commit.

New tests cover observed/censored/partial/missing verification; no synthetic chart rows from reports; exact decimal, nanosecond, ID and maximum uint64 seed strings; empty pages and active guidance; lifecycle comparison-context encoding; one bounded native request despite a next cursor; cancellation propagation; native unavailable errors; and source wiring for keyed refresh/pagination. Renderer tests validate markup and data, not browser effects, layout or interactive navigation. Refresh/cursor effect behavior relies on the existing shared hook and awaits integrated browser checking.

## Parent next actions

Mount `ExecutionTracePanel` only for execution summaries. Add inspection `order` query support as planned. Verify native multi-page next/previous, refresh/failure, report lifecycle links, partial-terminal and censored cases in the integrated browser. Then commission the required NEW independent persona repair reviewer. Completed native trace pages synchronously reproduce/replay on the backend, so no responsiveness/scalability claim is made. No human evaluation was run; stop before that stage after the remaining non-human integration/recheck work.
