# Market-making diagnostics UI implementation, 2026-11-08

Implementation handoff only. No independent clearance or human evaluation is claimed.

Checkout: `/Users/jedi/.t3/worktrees/lob/t3-91b14a38`. Base: `b707c87`, the TypeScript consumer follow-up on native integration `50d5ed4`. Read WORK_PLAN, PRODUCT, EXECUTION_PERSONA_REPAIR and the independent execution, market-making and validation persona findings. No C++, schema, API, global CSS, run-detail, comparison, mock-service or WORK_PLAN edits. Parent retains those files and coordination records. No delegates, browser, servers, installs, builds, production operations or pushes. Dependencies use a symlink to the parent's existing `/Volumes/PortableSSD/Documents/programming/lob/web/node_modules`.

## Delivered behavior

- `RequestDiagnosticsPanel({diagnostics}: {diagnostics: ApiSchemas["RequestDiagnostic"][] | undefined})` is exported from `web/src/components/research/request-diagnostics-panel.tsx`. Parent mounts it with `summary.orders.requestDiagnostics`. Separate new/cancel/replace submitted and exchange-rejected counts, per-kind denominators, exact rejection reasons/counts and arrival sample/min/max/mean ns appear in the existing shadcn card/table system. Labels distinguish outbound requests from pre-trade host risk blocks and explain that observed arrivals include rejected requests. Missing diagnostics, request kinds or optional transit data remain unavailable, never inferred zero.
- Inspector mounts dedicated replacement transit and FIFO evidence tables. Exact submit, arrival and actual fill-before-arrival event positions have button jump controls. Only a non-null native related fill produces the observed causal fill-race label; unknown-order alone does not. Missing legacy transit evidence differs from an observed empty list.
- FIFO renders every returned row, including reset and kept-priority replacements, exact exchange/old IDs, price, priority, shares/orders ahead and total shares. Unavailable queue position at reset remains unavailable until subsequent rest, whose nonzero queue is shown directly. Explicit native truncation flag distinguishes the 500-row cap, no truncation and legacy unknown status. No frontend slicing or fabricated zero queue.
- Lifecycle stages show their own exchange IDs and replaced-old IDs. Selected-fill identity and latest-chain/as-of labels remain separate. Existing fill-resource identities, processed-event freshness keys, paginated selected-fill lookup and cursor controls remain intact.
- Initial `?order=decimal` selects its lifecycle without Number conversion. Canonical unsigned decimal IDs remain exact through uint64 values. Malformed strings are ignored. A direct link has a real return-to-list focus target; row selections retain the existing Return to selected row control and focus/scroll behavior, with list fallback when the origin was removed by paging. Read explanation focus and live status remain available.
- Trader outcomes expose exact optional `availableShares` beside eligible fill/no-mark/censored counts. Formula is sum(signed markout per share × eligible fill shares) / available shares. Observed exchange-mid eligibility, signs and initial-edge inclusion remain explicit, with no invented alpha decomposition. Missing old-data shares are unavailable. Simulation timestamps display all nine fractional second digits; latency and evidence show exact ns, and horizon labels retain raw ns alongside readable units.

## Verification

All checks ran locally without a server or build. Logs are retained at exact paths:

| Check | Result | Log |
| --- | --- | --- |
| `bun test ./tests/mm-diagnostics-ui.test.tsx ./tests/research-workflow.test.ts ./tests/trader-analytics.test.tsx` from web | 26 pass, 0 fail, 132 assertions, 669 ms | `/tmp/mm-diagnostics-ui-tests.log` |
| `bun run typecheck` from web | pass | `/tmp/mm-diagnostics-ui-types.log` |
| `bun x --no-install eslint src/components/research/run-inspect-view.tsx src/components/research/trader-run-outcomes.tsx src/components/research/request-diagnostics-panel.tsx src/components/research/run-inspect-evidence.tsx src/lib/inspection-order-link.ts tests/mm-diagnostics-ui.test.tsx` from web | pass | `/tmp/mm-diagnostics-ui-lint.log` |
| `git diff --check` | pass | terminal result |

Seven new renderer/helper cases cover 505 replacement versus 3 new rejects and 0 cancel rejects; exact arrival statistics and a submitted count above safe integer precision; unavailable legacy data; nonzero reset/rest plus kept reduction and truncation; actual related fill versus no-fill replacement; exact decimal deep-link parsing; measured markout shares/formula/legacy absence; original order7/event45/match4 versus replaced order12/event75/match5 alongside final chain totals. Fixtures test rendering semantics, not a new native scenario execution. Existing workflow cases retain cursor-cycle, later-page failure, selected-fill paging and abort/missing-evidence coverage. Renderer tests verify jump labels/targets; real click/focus and URL integration await parent browser validation.

During implementation, typecheck caught two misspelled test enum literals, corrected to the generated `replace_priority_*` values. A later check caught a removed formatter import still needed for fill horizons; restored before the final passing checks. No unresolved scoped failure remains.

## Parent next actions

Mount the request diagnostics panel in owned run-detail with `summary.orders.requestDiagnostics`; finish owned comparison/markout and provenance/export work. Integrate this commit and run the integrated disposable native browser checks, including direct trace-to-order deep links, exact jump events, focus return after paging, original/replaced fills, nonzero FIFO reset and the seed103 slow-replace 505/3 rejection case. Commission a NEW independent reviewer on the integrated result and finish any resulting non-human fix/recheck cycle. Stop before human evaluation. This branch does not claim those parent steps are complete.
