# Trader workflow implementation

Status: integration in progress, 2026-11-08. Source review, not unaided human usability certification.

## Findings and response

| Priority | Source-confirmed obstacle | Research consequence | Change |
| --- | --- | --- | --- |
| P1 | Library starts with records and generic creation | New users must discover which baseline serves execution versus quoting | Two research-task entry points with outcome descriptions and matching setup intent |
| P1 | Comparing library entries requires opening records and remembering identities | Baseline/candidate selection is detached from the research library | Ordered checkbox selection, named baseline/candidate summary, direct comparison and clear selection |
| P1 | Run outcome hierarchy is identical across strategies | Execution cost can be judged without completion; quoting P&L can hide inventory risk | Execution-first completion/residual/cost; MM P&L attribution, inventory risk and markout eligibility |
| P1 | Non-comparable results still expose paired charts | Traders can overrule a warning by reading misleading evidence below it | Suppress paired statistical surfaces for service-declared non-comparability |
| P2 | Comparison statistics require scanning wide tables | Mean improvement can hide unfavorable seeds or missing observations | Selectable zero-referenced per-seed difference chart with exact linked evidence and eligible sample counts |
| P2 | Experiment details show runs without an analysis-oriented overview | Completion, interruption and missing work are easy to conflate | Distinct completed-seed/active/interrupted/missing counters, analysis/monitor/recovery links and stale-state disclosure |

Library comparison order is explicit: first selection is baseline, second is candidate. Filtering does not discard the selection; the named selection summary remains visible. Comparison validity remains the API's responsibility. Keyboard activation within a checkbox no longer activates the parent row.

## Design and workflow

Keep the established harbor/canvas/paper/ink/teal/bronze palette and IBM Plex typography documented in UI_DESIGN.md. Add hierarchy through task-oriented entry, grouped configuration and decision-first evidence. No decorative market data or live-profit claims.

Intended workflow: choose execution or market making -> configure a baseline -> review assumptions -> launch a seed set -> analyze completion/cost or P&L/risk -> inspect the fill -> create a variant -> select baseline/candidate -> inspect paired differences -> open the responsible runs.

## Boundaries and remaining work

Current simulated world remains single-venue interactive synthetic. No historical counterfactual backtest, calibrated market impact, arbitrary uploaded strategy, confidence interval or unexecuted opportunity-cost model was added. Those require explicit engine/API design and validation rather than UI-only estimates.

Source inspection preceded an interactive walkthrough, so no unaided-use claim is possible. The initial shared preview on port 3000 returned “Upgrade Required”, not the product. Follow-up verification must use an identified isolated app instance. Human first-use testing is still needed to measure the user's 80% goal.

## Verification record

Initial integrated analytics + parent checks: typecheck, lint; frontend invariants 95 passed, 5 existing skipped, 0 failed, 13098 assertions (4.38s). Logs: /tmp/trader-parent-types.log, /tmp/trader-parent-lint.log, /tmp/trader-parent-tests.log. New browser regressions cover library selection -> paired evidence and overview -> MM outcomes. Setup integration, final production build and browser checks pending.

Parent checkpoint: production build passed; five selected browser cases passed in 4.7s, including both new navigation cases and existing offline, paging and mobile checks. Logs /tmp/trader-parent-build.log and /tmp/trader-parent-browser.log. Shared preview successfully loaded the isolated production app at port 3766 with in-memory mock fixtures on 3767; library rendering inspected at 1280x800, with clear task entry, named controls and bounded table. This is fixture visual evidence, not native simulation evidence.

Initial native execution browser run: inventory-aware flow passed; TWAP/sweep stopped at a renamed heading expected by the existing invariant. Restored the stable “Parent-order execution” heading in the implementation, preserving the new completion-first headline and benchmark descriptions. A rebuilt native rerun is required after setup integration. Initial failing receipt /tmp/trader-parent-native-browser.log.
