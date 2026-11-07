# Trader analytics implementation

Date: 2026-11-08. Scope: source audit and implementation in harness checkout `/Users/jedi/.t3/worktrees/lob/t3-686c35ac`. Parent integrates serially. No delegates, production operations, C++ or API changes.

## Audit and decision

Read WORK_PLAN current rollup, PRODUCT, frontend-design skill, and the parent's uncommitted TRADER_UX_REVIEW_PLAN_2026-11-08.md from `/Volumes/PortableSSD/Documents/programming/lob`. Findings below are source-confirmed, not browser observations or human usability evidence.

| Finding | Trader consequence | Implemented response |
| --- | --- | --- |
| A1: Run result strip always leads with net P&L, position and filled shares | Parent execution completion/residual and signed cost tradeoff is below time-series charts | Strategy-specific outcome section above charts: parent filled/requested, completion ratio, residual shares, filled shortfall including fees and slippage excluding fees, with units and residual opportunity-cost limitation |
| A2: MM markout counts and values are buried in a summary table | Spread capture or inventory gains can be mistaken for robust quoting returns | Lead with P&L attribution and inventory/drawdown; show signed per-share markouts at every reported horizon with eligible/no-mark/censored fill counts |
| A3: Comparison requires scanning a very wide statistics table to interpret candidate impact | Mean differences can hide unfavorable seeds and missing observations | Service-supplied priority mean changes, direction-aware text and counts, then selectable paired-seed bar chart and linked baseline/candidate evidence table |
| A4: Non-comparable experiments still render charts and metric statistics below the warning | Warning can be overlooked while treating descriptive differences as paired evidence | Keep pairing/assumption evidence and incompatibility reasons; suppress paired result surfaces when comparable is false |
| A5: Summary refresh failure appears only in the summary tab below headline outcomes | Retained data can be read without seeing its failed refresh | Move summary failure/retry immediately before headline outcomes; preserve keyed-resource freshness behavior |
| A6: Run detail offers inspection but no direct variant/comparison continuation | Result analysis ends without a clear next research action | Persistent inspection, create variant and compare-as-baseline links; comparison with no candidate offers creation from baseline |

## Design plan and implementation

Use the existing research palette: background `#eef3f7`, text `#203849`, surface `#ffffff`, primary `#17676c`, chart baseline `#157c80`, candidate `#ba7339`. No global token changes. IBM Plex Sans carries headings and explanations; IBM Plex Mono carries exact quantities and costs. Left-aligned responsive grids keep comparable values adjacent.

Layout: `run identity / reliability -> outcome and interpretation -> next decision -> time series -> full ledger and evidence`. Comparison: `selectors -> pairing and assumption differences -> candidate impact / metric selector / seed differences / linked evidence -> time series -> dense statistics`.

The distinctive element is the zero-referenced seed difference chart, paired with exact run evidence. No decorative hero, new fonts, animation or tutorial. Existing shadcn Card, Field, Select, Table, ChartContainer and Recharts handle the interaction. Review against the brief removed generic P&L-first execution framing and the blanket “exact” seed-chart label, because recorded series can be derived.

## Data and semantics

- The chart uses `MetricComparison.perSeed.difference` from the API, never a browser-computed difference, ranking, confidence interval or significance claim. X is categorical master seed; Y is candidate minus baseline in the selected metric's unit. Zero is a tie. Null is absent, not zero.
- Plot positions alone pass through the existing approximate chart conversion helper. Tooltips and tables format original decimal strings. Seed identifiers remain strings. Tests include values above the JavaScript safe integer limit and a tiny signed decimal that cannot safely be interpreted through ordinary floating-point magnitude.
- Mean changes, preferred direction, better/worse/tied counts and available sample counts come directly from the service. Missing pairs are visible in the evidence table; n is metric-specific. Fewer than two observations does not supply dispersion. No significance or real-market profitability claim is made.
- Completion is a ratio, residual is shares, cost totals are USD. Positive signed cost is worse for either side. Filled-quantity cost does not value unexecuted shares. Existing exactness/unavailable details remain in full metric evidence.
- MM markout means are derived USD/share. Eligible fill counts are not share counts. No-mark and censored observations are separate; no eligible fills display unavailable even if the backend's empty sum is zero. Full markout totals remain in the detailed table when eligible observations exist.
- Existing run control, idempotency, polling, resource identity and API requests are preserved. Non-comparability is gated by the service flag, not a new client validity rule. Assumption differences still warn about mixed effects, and exclusion of incomplete/failed/unpaired runs is explicit.

## Verification

- `cd web && bun run typecheck`: pass. Log `/tmp/trader-analytics-typecheck.log`.
- `cd web && bun test tests/trader-analytics.test.tsx tests/execution-presentation.test.tsx tests/compare-math.test.ts tests/exact-decimal-format.test.ts`: 32 pass, 0 fail, 3936 assertions, about 1 second. Log `/tmp/trader-analytics-tests.log`.
- Seven new helper/actual-renderer cases cover service direction and exact sign, priority selection, exact/missing paired evidence and drilldown links, non-comparable suppression, zero available pairs, unavailable MM valuation/censored markouts, and execution-first cost/residual framing.
- Existing execution presentation, independent comparison arithmetic and decimal-format checks remain passing. `git diff --check` passed.
- The isolated checkout lacked dependencies. An ignored `web/node_modules` symlink uses the parent's existing dependency installation; no packages or lockfiles were changed. The initial typecheck could not launch before linking it. One new assertion initially expected capitalized disclosure text and was corrected before the final passing run.

## Actual limitations and parent next actions

No build, browser suite, visual screenshot, live-data mutation, deployment or independent review was run. Server rendering verifies output semantics, not interactive Recharts tooltips, selection, responsive layout or keyboard behavior. Parent must integrate and check those interactions; human first-use success remains unmeasured.

The API does not supply failure/missing-run counts to this comparison response, unexecuted opportunity cost, confidence intervals or a calibrated market-impact estimate. This implementation discloses those limits rather than deriving replacements. No execution schedule trajectory is invented. The comparison headline includes one markout horizon initially; the selector and full tables expose all reported horizons. Run detail exposes all reported markout horizons.

Changed implementation files: the three owned views, `paired-seed-analysis.tsx`, `trader-run-outcomes.tsx`, `web/src/lib/trader-analytics.ts`, and `web/tests/trader-analytics.test.tsx`. This report and the WORK_PLAN handoff entry record scope and evidence. Commit identifiers are recorded in the handoff entry and completion response.
