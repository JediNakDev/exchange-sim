# Trader research and UX review plan

Date: 2026-11-08. Status: source audit and scoped workflow implementation complete. See TRADER_WORKFLOW_IMPLEMENTATION_2026-11-08.md for findings, acceptance evidence and remaining research capabilities. The original exploratory checklist below is retained; unchecked items are not silently certified by implementation.

## Objective and boundaries

Judge the simulator as a quant firm's execution and market-making research tool. Test whether a trader can formulate a hypothesis, configure an honest experiment, compare alternatives and explain an outcome without a tutorial. Existing engineering checks establish scoped behavior, not intuitive UX or research usefulness.

A complete flow redesign is in scope for recommendations. This phase produces evidence, design recommendations and an implementation backlog. It does not implement, deploy or change existing research data. Preserve .claude/, output/, interrupted worktrees and live services. No new compilation or broad test run is needed for this review.

Initial context: docs/WORK_PLAN.md, docs/PRODUCT.md and docs/quality/PLAN_2026-11-07.md. Current source routes cover experiments, creation, experiment details, comparison, run details, inspection and connections. Verify deployed source/build identity before attributing source observations to the live product.

## Ordered task list

- [x] T01. Read current state and product intent; identify the shipped workflow and deferred capabilities.
- [x] T02. Define personas, tasks, evidence standards and first-use acceptance criteria.
- [ ] T03. Identify the review build, viewport, dataset and browser access. Observe the first session before reading UI implementation or help. Record any unavoidable prior product knowledge.
- [ ] T04. Map every user-facing capability to a visible entry point and a useful trader outcome. Identify dead ends, duplicate concepts and hidden features.
- [ ] T05. Walk the execution research task: buy a parent order, compare immediate execution with TWAP under equivalent assumptions, explain cost and residual quantity, inspect one fill, save a candidate.
- [ ] T06. Walk the market-making task: compare quote widths and inventory skew, distinguish spread capture from inventory gains, inspect toxic fills and test latency/inventory sensitivity.
- [ ] T07. Walk the research-validation task: repeat baseline/candidate on paired seeds, expose changed assumptions, inspect uncertainty and failed/missing runs, reproduce and export the result.
- [ ] T08. Audit charts, tables and metrics against the decisions in T05-T07. Specify missing data and exact semantics, units, denominators, sign conventions and unavailable observations.
- [ ] T09. Test navigation and state comprehension: empty and populated screens, creation defaults, progress, pause/step, completion, failed runs, partial seed launch, retry, selection, paging and back navigation. Use isolated fixtures for mutations and failure probes.
- [ ] T10. Audit research realism: synthetic market limitations, participant behavior, queue/latency visibility, market impact, fees, risk, censored markouts and terminal valuation. Consult architecture and normative C++ specs only where needed to substantiate a finding.
- [ ] T11. Review readability, information hierarchy, keyboard use, focus, contrast, loading states and desktop density. Check a smaller viewport for essential task access.
- [ ] T12. Synthesize evidence-backed findings and conflicts between personas. Identify what should be retained, removed, renamed, regrouped or rebuilt.
- [ ] T13. Design the proposed end-to-end flow and screen responsibilities. Include a mapping from existing capabilities to the new flow and explicit deferred capabilities.
- [ ] T14. Produce a prioritized implementation backlog with dependencies, frontend/API/C++ ownership, acceptance criteria and a cheap relevant verification approach.
- [ ] T15. Publish the review, evidence paths and next actions; update WORK_PLAN.md. No implementation-complete claims from design work.

## Persona lenses

| Lens | Decision to support | Required challenge |
| --- | --- | --- |
| Execution quant | Which schedule reduces cost at acceptable completion risk? | Side-aware implementation shortfall, residual treatment, benchmark definition, matched market assumptions, participation and impact limits |
| Market-making quant | Does quoting earn spread after adverse selection, fees and inventory risk? | P&L attribution, signed markouts, inventory exposure, queue/fill evidence, cancels and latency |
| Research lead / skeptical validator | Is the improvement real, reproducible and relevant? | Paired seed differences, sample counts and dispersion, comparability, failure selection bias, synthetic model limitations, provenance/export |
| First-use trader | Can I discover and complete the important work unaided? | Predictable labels, task-oriented entry points, visible defaults/units, obvious next action and recovery |

Optional separate persona delegates may be commissioned after the unaided parent walkthrough. Delegates should critique the same build and tasks independently before receiving the parent's findings. Use GPT with high reasoning. Read-only research needs no independent implementation checkout; any implementation would require harness worktree isolation. Record actual task IDs, links, settings, reports and checkouts if used. No delegate has been launched for this plan.

## First-use test protocol

1. Predeclare task instructions and the capability inventory before scoring. Give goals, not click instructions. Use no tutorial, README, code or coaching during the observation phase.
2. For each capability, record whether the trader found it, completed a meaningful task, and interpreted the result correctly. Finding a button alone is not success.
3. Measure discovery/completion time, wrong turns, assistance and interpretation errors. Suggested targets: useful first experiment launched within five minutes; baseline comparison and explanation of one outcome within fifteen minutes. These are proposed targets, not current measurements.
4. Interpret the user's 80% requirement as at least 80% of predeclared core capabilities completed and correctly interpreted unaided. Also require every critical task to pass: valid setup, valid comparison, correct cost/risk interpretation and explainable outcome. Peripheral features cannot offset a critical failure.
5. Separate observed walkthrough evidence from expert heuristic judgments. An AI reviewer already exposed to product docs cannot certify human first-use success. Validate the redesign with representative traders unfamiliar with the product before claiming the target is met.

## Evidence and severity

Each finding contains: ID; persona/task; build and route; observable behavior or screenshot; trader consequence; proposed change; required data/engine support; severity; effort/dependencies; acceptance criterion. Label findings as observed, source-confirmed, inferred or untested. Separate absent UI from absent API/engine capability. Do not equate a synthetic-market result with an estimate of real-market profitability.

Severity is based on research harm: P0 can produce a materially wrong conclusion or lose work; P1 prevents a core task or makes its result ambiguous; P2 adds substantial friction or hides useful evidence; P3 is polish. Rank implementation by severity, task coverage and dependency, independently of effort. Treat new strategy families and historical backtesting as scope proposals unless the current task requires them.

For analytics recommendations specify the trader question, proposed chart/table, axes and units, comparison basis, interaction/drilldown, underlying data, unavailable/censored behavior and acceptance criterion. Prefer a decision-relevant chart over a decorative dashboard. Examples to evaluate, not findings: cumulative execution versus schedule, signed cost decomposition, P&L/inventory alignment, markout curves with eligible counts, paired-seed difference distributions and latency sensitivity.

## Deliverables

1. Strict evidence-backed review with the most consequential objections first and clear limits on coverage.
2. Current versus proposed workflow, with full restructuring if existing navigation obstructs the research loop.
3. Screen and analytics specification connecting every proposed surface to a trader question.
4. Dependency-ordered task backlog suitable for implementation, with an explicit smallest useful release and later work.

Next action: start T03-T04, observe the actual browser workflow, then perform the three strategy/research task passes. This plan contains no verdict on the current UI's usability.
