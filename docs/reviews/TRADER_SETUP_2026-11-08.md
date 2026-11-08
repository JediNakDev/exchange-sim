# Trader experiment setup overhaul

Date: 2026-11-08. Scope: source audit and implementation in the isolated harness checkout `/Users/jedi/.t3/worktrees/lob/t3-4af7b2d3`. Parent integrates serially. No deployment or live data mutation.

## Evidence and findings

Read the current WORK_PLAN rollup, PRODUCT, and the parent's uncommitted review plan at `/Volumes/PortableSSD/Documents/programming/lob/docs/reviews/TRADER_UX_REVIEW_PLAN_2026-11-08.md`. The plan is absent from this checkout. These are source-confirmed findings, not observed first-use trader results.

| Finding | Consequence | Implementation |
| --- | --- | --- |
| Setup selected the first executable baseline without reading research intent. | An execution launch could start on a market-making form. | Honor `intent=execution` and `intent=market-making` against service-provided executable baselines. Task buttons expose the two decisions. Missing task capability has an explicit unavailable state. |
| Identity and all background-agent parameters preceded the strategy. | The trading decision was buried in a long configuration editor. | Task, identity/hypothesis, strategy and run plan are visible first. Scenario, latency, fees/risk and accounting are expandable sections. Validation issues expand secondary sections. |
| Execution timing inputs were raw nanoseconds; duration descriptions could round away nanoseconds. | Traders had to count zeros and could misread exact delays. | Scalar ns timing fields support exact ns/µs/ms/s display and entry. Default ns mode preserves existing fill behavior. Decimal shifts retain every nanosecond, including uint64 values above Number precision. Invalid fractional input remains visible and sends an invalid empty duration for service validation. No truncation or rounding. |
| Assumptions summary was empty until service review. | Costs, risk and research interpretation were absent while configuring. | Always show market world, instrument rules, strategy/parent or quote parameters, seed strings, fees, delays, position cap and mark convention. Explain synthetic-model limits and task-specific outcome interpretation. |
| Launch controls appeared at the end of the form with little lifecycle context. | It was hard to see the next action or whether review was stale. | Dedicated review/launch panel shows draft, reviewing, changed, invalid, ready and submitting states. One run per seed and simulated duration are visible. Full service assumptions remain available with changed-row count, digest, estimates and validation evidence. |

## Design decisions

Use the existing IBM Plex Sans for hierarchy and IBM Plex Mono for exact numeric inputs. Retain inherited semantic colors rather than introducing global tokens: the existing light palette uses background `#eef3f7`, foreground `#203849`, card `#ffffff`, primary `#17676c`, muted foreground `#5b7180`, and border `#dbe4ea`, with the existing amber changed-assumption treatment. Actual theme variables remain authoritative. No globals changed.

The layout is a working research ticket: a broad decision/configuration column and a narrower review column, stacked on smaller viewports. Distinction comes from task-specific outcomes and readable assumptions, rather than decorative metrics. Existing shadcn fields, selects, buttons, cards and tables remain in use; native details/summary provide keyboard-operable disclosure. No tutorial, wizard gate or additional dependency.

## Acceptance and verification

| Criterion | Evidence |
| --- | --- |
| Intent selects the matching strategy family without inventing a config or silently falling back to another task. | Focused helper tests cover both intents, absent capabilities, unknown/default intent, all four editable strategy kinds and unchanged baseline configuration. Source wiring filters baselines through service capabilities before selection. |
| `from=` preserves exact saved configuration even with an intent query. | Parent draft takes precedence over intent-selected baseline in the editor. Dedicated variant helper test covers deep copy, lineage, seeds beyond Number precision, exact latency and small signed fees. Query wiring and precedence are source-reviewed, not browser-tested. |
| Timing conversions preserve exact strings at request boundaries. | Round-trip tests across all four units cover zero, one ns, fractional milliseconds/seconds, Number-overflow values and full uint64 maximum. Malformed values and sub-ns precision are rejected. Changing display units alone never calls the draft updater. |
| Launch remains gated by a current valid service preview and executable strategy. | Both disabled UI controls and the launch handler enforce validity/capability/current-preview state. Existing idempotency key reuse, partial launch recovery, source lineage and paused starts remain. Workflow helper tests pass. |
| A changed draft invalidates the prior review. | Existing full request serialization equality remains the review identity. Exact input changes update the draft, while display-unit-only changes leave configuration untouched. Source-reviewed. |
| Review errors remain discoverable. | Secondary sections expand when field issues exist; service errors and the authoritative assumptions table remain accessible beside launch. Source-reviewed. |

Checks from `web/`, all passing:

- `bun run typecheck`: `/tmp/trader-setup-typecheck.log`.
- `bun test tests/experiment-setup.test.ts tests/exact-decimal-format.test.ts tests/research-workflow.test.ts`: 33 passed, 0 failed, 134 assertions. `/tmp/trader-setup-tests.log`.
- `bunx eslint src/components/research/experiment-create-view.tsx src/components/research/experiment-setup.ts tests/experiment-setup.test.ts`: `/tmp/trader-setup-lint.log`.
- `git diff --check`.

Dependencies were accessed through a temporary ignored `web/node_modules` symlink to the parent's installed dependencies. No install, build, C++, API or browser suite was run.

## Limits and integration actions

This is a completed setup implementation with focused source-level verification, not a measured trader usability pass or an integrated release. No browser layout, focus traversal, interactive unit-switching or real-service launch was exercised. No first-use completion rate is claimed. Parent should visually check the setup and both intent entry links after serial integration.

Most existing input labels and ids remain. Timing labels now say `stored as ns`; raw ns remains the default. Existing browser checks that fill latency, scenario, fee/risk or accounting fields must expand the corresponding disclosure first. Strategy, parent-order fields, simulation end and master seeds remain directly visible. Markout horizon lists remain exact comma-separated ns strings with readable per-horizon descriptions, rather than a multi-unit list editor.

The form does not add execution metrics, new calibrated market models, seed statistics, a comparison experiment, instrument editing or backend validation rules. A sweep or TWAP can retain residual quantity. One synthetic seed is not robust evidence of an improvement. Estimated work and full canonical assumptions still come from the service, not client simulation logic.

Owned files: `web/src/components/research/experiment-create-view.tsx`, new `web/src/components/research/experiment-setup.ts`, new `web/tests/experiment-setup.test.ts`, and this report. WORK_PLAN is left to the parent because it is outside this task's file ownership. Next action: parent cherry-picks the reported commit and integrates the library intent launch cards serially.
