# Trader workflow implementation

Status: scoped implementation integrated and verified, 2026-11-08. Source review, not unaided human usability certification.

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

## Final integrated result

Setup source `22f4091c95fa5e96c280400fa32868a7516854e8` from `/Users/jedi/.t3/worktrees/lob/t3-4af7b2d3` integrated as `ca9193a`. Parent follow-up improves the selected-task appearance, chooses readable default timing units and preserves partial decimal typing. Requests remain whole-nanosecond strings. The first implementation normalized valid partial decimals too early; the final browser test types 0.000001 one character at a time and checks exact ns requests, sub-ns rejection and review invalidation. Display-only unit changes retain a valid review.

Final scoped implementation is complete locally:

- Task-oriented library and setup honor execution/market-making intent and service capabilities. Saved variant configuration takes precedence over intent.
- Strategy, run plan and hypothesis are primary; secondary scenario, latency, fees/risk and accounting settings use disclosure. Service field issues expose secondary settings.
- Always-visible assumption summary and review/launch state explain what will run. Exact timing entry supports ns, microseconds, ms and seconds with readable defaults.
- Experiment overview distinguishes completed seeds, live work, failed/cancelled runs and missing seeds, with direct continuation links.
- Run outcomes are strategy-specific; result pages link to inspection, variants and comparison.
- Paired comparison exposes assumption changes, seed eligibility, direction-aware mean differences and a selectable seed-difference chart with exact linked values. Non-comparable statistical surfaces are suppressed.

### Final checks

| Check | Result | Receipt |
| --- | --- | --- |
| Frontend invariants after setup integration | 103 passed, 5 pre-existing skips, 0 failed; 13158 assertions; 4.28s | /tmp/trader-final-tests.log |
| Typecheck, full frontend lint, production build after final input fix | Passed | /tmp/trader-final-types.log, /tmp/trader-final-lint.log, /tmp/trader-final-build.log |
| Selected integrated browser cases on final production artifact | 12 passed, 0 failed; 11.8s | /tmp/trader-final-browser.log |
| Visual review | Library, setup/review layout and paired-difference chart inspected at 1280x800 using shared preview and mock fixtures | Shared preview session; no screenshot artifact retained |
| Diff whitespace | Passed | git diff --check |

The 12 browser cases include two native C++ flows with isolated data: TWAP/sweep launch, residual/cost evidence, variant and same-seed comparison, plus inventory-aware execution. The TWAP flow now enters via execution intent. The remaining cases use isolated in-memory fixtures: partial seed-launch recovery, saved variants/capability gating, offline count recovery, mobile width/page exceptions, invalid duration drafting, source-change review reset, unknown capabilities, ordered library selection, overview continuation and exact timing. Existing native artifact-inventory assertions remain unchanged and passed. Test navigation now expands fees/risk before editing child caps and latency before editing delays; timing tests explicitly choose ns where raw ns entry is intended. Behavioral assertions were retained.

The 103-case invariant receipt precedes the small final input-draft/display-default fix; final type/lint/build and browser checks cover that fix. No full native test suite, benchmark, fresh independent audit or deployment was run. Installed services and research data were untouched; isolated verification processes and shared preview tabs were closed.

### Follow-up backlog beyond this implementation

1. Human first-use evaluation: predeclare core tasks, measure unaided completion and correct interpretation, and require all critical research tasks plus 80% overall capability success. Existing automated evidence cannot certify this.
2. Execution schedule trajectory and unexecuted opportunity cost: add C++/API evidence and benchmark semantics before drawing schedule/cost charts or claiming total parent-order cost. Existing results disclose the limitation.
3. Controlled hostile scenarios and sensitivity sweeps: define executable market models and paired experiment orchestration before adding broad stress/parameter-grid UI.
4. Comparison eligibility audit: expose failed/incomplete/excluded run counts directly in comparison responses, with reason-specific drilldown. Current views disclose exclusions and link to experiment run status.
5. Historical counterfactual replay and calibrated impact: separate model contracts, data validation and accounting work. No UI-only claim of support.

These are explicit remaining capabilities, not silent claims that the broader review plan's exploratory tasks or every possible research feature are complete.
