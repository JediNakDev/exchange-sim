# Trader workspace revamp proposal

Status: original proposal. A current-contract structural implementation is now running for user review; see WORKSPACE_REVAMP_IMPLEMENTATION_2026-11-08.md for exact delivered scope, evidence and deferred capabilities. This proposal is not blanket implementation or independent usability clearance. Responds to the user's clarification that a structural revamp was requested. Earlier correctness repairs remain useful but do not establish that the existing information architecture satisfies that request.

## Design authority and boundaries

Follow frontend-design and vercel-react-best-practices. docs/UI_DESIGN.md is the visual source of truth: exact ledger, green primary action and favourable signs, olive neutrals, square flat surfaces, Inter interface, Geist Mono data, fixed chart roles, existing shared research components and shadcn primitives. Do not invent another palette, typography system, decorative dashboard or bespoke primitives.

Section4 currently fixes three primary destinations. The navigation below is a proposed explicit amendment to that section, not permission to silently contradict it. No existing design document or implementation is changed by this proposal. Uncommitted PostgreSQL/worker work and other owners' files remain untouched.

Scope remains single-venue synthetic execution and market-making research. Strategy upload, historical counterfactual backtesting, calibrated impact, remote deployment and portfolio research are not implied by new labels. Distinguish implemented data, new derived views, new durable entities and genuinely new native evidence.

## The researcher's job

A useful research loop is: choose a strategy and market assumptions, state a hypothesis, define a controlled comparison, run common seeds, assess performance and failure coverage, explain a specific mechanism, save the finding and choose the next test.

The product should retain that question and comparison context across every screen. A list of experiments and IDs does not provide this research memory.

| User | Concrete question | Decision evidence | Follow-up |
| --- | --- | --- | --- |
| Execution quant | Can I reduce buy500 execution costs without accepting more residual risk? | Completion/residual alongside side-aware filled-quantity cost, quantity/benchmark scope, paired seed outcomes | Inspect a late/missing slice and the delivered market state |
| MM quant | Does wider quoting improve net returns at tolerable inventory and adverse selection? | Net P&L, drawdown, inventory extremes, eligible-share markouts and fill exposure | Inspect toxic fills or quote-update races |
| Research lead | Is the effect consistent, fairly compared and reproducible? | Changed assumptions, selected/all attempts, metric-specific paired counts, distributions, exact provenance | Pin evidence, export, record conclusion or request more seeds |
| Operator/researcher | Which study needs attention, and can I recover without duplicating work? | Queue/active/paused/failed states, retained partial artifacts and actual recovery actions | Resume, retry missing work or inspect failure |

## Proposed navigation

Five primary destinations, each justified by a repeated job. No extra destination merely to increase menu count.

| Destination | What belongs here | What does not |
| --- | --- | --- |
| Research | Studies: hypothesis, baseline/candidates, progress, findings, next action | A decorative overview of total run counts |
| Strategies | Supported built-in strategies, immutable versions/configuration presets, parameter meanings, linked studies | A fake code editor or unsupported upload capability |
| Scenarios | Synthetic market presets, participants, latency/fees/risk assumptions, declared limitations | Historical-data labels without a fill/reconstruction model |
| Runs | Cross-study queue and searchable result records, failures and retained partial work | Another independently editable experiment configuration |
| Compare | Saved/pinned comparisons and cross-study selection with compatibility review | A blank pair of experiment-ID selectors |

Connections and Settings are secondary utility navigation. Protocol connectivity should not compete with the main research loop. Inspection/replay is contextual to a selected run, fill or divergence, not a global empty destination. Exports/provenance belong to the evidence they describe; risk belongs beside strategy outcomes.

Research is the landing page. With no studies, show two explicit starts: execution research and market-making research. With existing work, show active work needing attention and a dense study table: question/name, baseline/candidate, changed factor, observed/eligible/planned seeds, result summary, last activity and next action. Counts of all historical runs are secondary.

## Research object hierarchy

Study -> immutable experiment configurations -> seed runs/attempts -> event/fill/order evidence.

A study groups a question and its variants; an experiment fixes one executable configuration; a run is one execution attempt. Do not ask first-time users to learn all three nouns before starting. Create the study/configuration together from the primary workflow; reveal exact identities in context.

Strategy and scenario presets are reusable starting points, never mutable references that relabel old runs. Saved findings bind to a pinned cohort. New retries do not silently change a saved conclusion.

Study tabs: Setup, Results, Runs, Notes and evidence. Results switches between Overview, Compare and Sensitivity within the same study. Selecting Compare in the sidebar opens a cross-study workspace or a saved comparison, not a second disconnected comparison implementation.

## End-to-end flow

1. New study: choose execution or market making, name the research question, select supported strategy and scenario presets. A concise supported-model summary stays visible.
2. Define the hypothesis: baseline plus one changed factor by default. Example: quote half-spread1 ->2 ticks. Multiple factors are allowed but visibly described as a combined treatment.
3. Define evaluation: common seeds, simulation bounds, primary outcome and explicit constraints. Completion and inventory constraints are not collapsed into a single arbitrary score. Defaults are visible, editable and physically meaningful.
4. Review: baseline/candidate difference table, fixed assumptions, planned configuration x seed count, observation horizons and declared model limits. Block invalid input; distinguish a planned censored run from an eligible full-window comparison. Show scheduling/resource estimates only if measured support exists.
5. Launch: a durable activity view with completed/failed/paused/unlaunched counts and per-run recovery. Do not turn results into a fake live market terminal. UI disconnect does not imply simulation failure.
6. Results: evidence quality and practical trade-off first, then distributions and explanations. Keep the same selected cohort across tabs and links.
7. Investigate: select a bad seed, divergence, fill or missed decision. Open its linked evidence at the exact time/order/event, preserving comparison, filters, selected seed and return location.
8. Save finding: researcher-written conclusion, constraints, selected cohort, config differences and evidence links. Next action: change factor, add fresh seeds, investigate failure, or export. Saving a note does not certify a strategy.

Wizard screens are for initial formulation. Returning researchers get direct, grouped setup editing with a persistent diff and review state, rather than repeatedly clicking through a wizard. Launch actions state exactly how many runs they create.

## Information hierarchy

Every result answers these in order:

1. What am I looking at? Strategy/scenario, treatment, baseline/candidate direction, run/cohort state and any interpretation-blocking evidence problem.
2. What happened? Four or five strategy-specific figures, quantities and denominators together. No universal P&L-first page for execution.
3. Where did it happen? A primary chart chosen for that strategy's question.
4. Is the outcome consistent? Per-seed paired evidence, dispersion and failure/censor coverage.
5. Why did it happen? Linked schedule, inventory, markout, fill and lifecycle evidence.
6. Can someone reproduce it? Exact assumptions, provenance, pinned attempt ledger and full artifacts.

Censoring, missing valuation and failed attempts are visible at the first relevant level, not buried with raw diagnostics. Raw manifests are secondary. Routine result interpretation must not require reading JSON.

## Execution result workspace

Header: study question, baseline/candidate identity, common market assumptions, selected metric/seed cohort and observed/planned count. Persistent context strip carries model and benchmark scope without occupying half the page.

First figures: completion ratio, filled/residual shares, executed shortfall including fees, and eligible pair count. Explain positive signed cost as worse for either buy or sell. Filled-quantity costs never imply a valuation of residual shares.

| View | Axes / information | Interaction and boundary |
| --- | --- | --- |
| Primary: execution progress | Simulated time in seconds; cumulative target and executed shares, with parent-window deadline | Select a seed; click a shortfall interval to open the exact decision/report range. Exchange fills and delivered known fills are separately named, not silently merged. Native trace currently paginated; a whole-run view needs a bounded server-derived series, not first-page extrapolation. |
| Cost versus completion | Completion ratio versus executed USD/share cost; baseline/candidate seed marks linked where paired | Quantities, cost scope and residual in tooltip/exact table. Do not rank incomplete orders solely by their filled-share costs. Explicit common scale/units. |
| Paired differences | One row per common seed, candidate minus baseline, zero reference | Metric selector, metric-specific n, unmatched/excluded attempts visible. No fabricated precision or unsupported confidence claim. |
| Cost details | Decision benchmark slippage plus net fees equals executed shortfall | Only additive terms in an arithmetic bridge. Residual opportunity cost stays unavailable unless a declared valuation model is added. |

Secondary decisions table: decision time, target, delivered known fills/touch, outstanding reservation, requested/submitted shares, reason and child link. Lead with the decision that explains the selected residual, rather than dumping all event rows.

## Market-making result workspace

First figures: net P&L after fees, maximum drawdown, inventory extremes and measured markout coverage. Ending inventory/mark convention remain visible. No win badge based on profit alone.

| View | Axes / information | Interaction and boundary |
| --- | --- | --- |
| Primary: linked P&L and inventory | Two stacked plots sharing simulation-time cursor: net P&L USD, inventory shares with configured limits | Brush an interval; retain exact values in accessible sample tables. Separate y-axes in separate plots, never ambiguous dual-axis overlays. |
| Paired performance | Per-seed net P&L differences with zero reference | Pair actual eligible attempts; drill into a bad seed. Single path examples and cohort summaries are not interchangeable. |
| Post-fill quality | Horizon versus eligible-share weighted signed markout USD/share | Show measured shares/fills and censored/unavailable counts at each point. Zero remains measured zero; missing horizons are gaps. Do not imply a continuous response between configured horizons. |
| Accounting bridge | Realized P&L + unrealized P&L - net fees = net P&L | A true accounting identity. Spread capture is a separate diagnostic, never another additive profit term. |
| Quote behavior | Request kinds/reject reasons, transit evidence, exposure and actual observed causal races | Link to the specific replace/fill/arrival sequence. A rejected update alone is not a race. |

A useful toxicity investigation starts from a poor markout cohort or adverse P&L interval, narrows to eligible fills, then opens order evidence. Filtering that cohort requires actual per-fill marks/time data; summary-only data cannot pretend to support it.

## Comparison workspace

Choose the decision and baseline first; show readable candidate names and actual configuration differences. Default to a selected study's baseline/candidates. Support cross-study comparison only with visible compatibility checks; no synthetic/historical blend.

Above the charts: same/different assumptions, matched seed coverage, selected-attempt policy and critical evidence gaps. Then the primary treatment trade-off, paired differences, absolute values and distributions. Aggregate means never replace absolute per-seed execution quantities or MM risk.

Allow comparing several candidates eventually, but keep one baseline fixed and series identities consistent. This requires expanding the current pairwise contract honestly. More than five visible chart series must use selection/facets rather than cycling the design palette. Filters distinguish an intentional subset from missing/failed evidence and are included in pinned links/exports.

Sensitivity is a study mode, not another giant top-level application. A parameter grid can show quote width on x, cancel latency on y, a named aggregate metric in each cell and usable/planned seed counts. Click a cell to reach the same result/compare/inspect flow. Use comparable completion/risk constraints; no best-parameter recommendation from one seed. Sweeps require orchestration, bounded concurrency, cancellation and durable partial results beyond a frontend heatmap. Independent seed evaluation after selection should be explicit; seed holdout roles are new product state, not relabeling old evidence.

## Investigation workspace

Keep it one contextual workspace: selected outcome/question and seed at top; time/event controls; linked books; decision/fill/order list; explanation pane. Simulated exchange truth and strategy-delivered state are distinct and time-aligned. On narrower screens show a task-focused pane with explicit switches, not a shrunk desktop grid.

The default fill explanation leads with selected fill identity and inventory/cash effect, then submit/arrival/report chronology, queue priority changes and causal race evidence. Raw event encoding and all fields are expandable. A missed fill must remain a hypothesis unless queue/decision evidence establishes the cause.

Keyboard selection and stepping preserve focus. Read explanation and return actions have truthful targets after paging/filtering. Back restores the comparison and selected interval, not an empty comparison page.

## UX and implementation requirements

- Keep the exact-ledger visual system. Primary green is action/selection; favourable signs use SignedFigure; fixed chart1 candidate/chart2 baseline roles stay unchanged. Neutral deltas are not coloured as wins.
- Task names and visible choices support recognition, with advanced details disclosed in context. Essential denominators, censoring and model scope are never hidden as advanced settings.
- Predictable next actions: vary this factor, compare with baseline, inspect this outcome, add seeds, retry missing work. Raw IDs are available without becoming the main navigation vocabulary.
- Stable URLs contain selected study, comparison, pin, seed, interval and filters where meaningful. Back/refresh preserve research context. New working selection is distinct from saved evidence.
- Initial results load useful summary/context independently of heavy inspection. Fetch independent resources concurrently. Keep static shell server-rendered where supported; isolate interactive leaves. Lazy-load replay/heavy optional charts, not the primary chart needed to answer the question.
- Deduplicate active status subscriptions; terminal immutable results do not need continuous polling. Page/window exact evidence and use bounded chart series. Never load every event to render a study row.
- Follow React skill rules at actual implementation boundaries; do not add SWR or abstractions merely because a skill lists them. Preserve existing project stack and exact decimal boundaries.
- Accessibility: logical headings, named charts, exact sample alternatives, keyboard selection/return, labelled controls, non-colour state cues and390px document containment. Complex chart selection also needs a table alternative.

## Dependency-ordered task list

- [ ] R01: Amend UI_DESIGN section4 and PRODUCT with study/preset terminology, navigation ownership and screen responsibilities. Preserve all visual tokens and shared primitives.
- [ ] R02: Inventory contracts against each proposed view: current evidence, derivable bounded series, new metadata, new C++ observations. Specify units, exactness, coverage and unsupported states before building charts.
- [ ] R03: Build two complete workflow prototypes with representative populated, empty, failed and censored states: execution study and MM study. Review first-use discovery and density in the existing green system. Clearly label prototype-only data.
- [ ] R04: Add durable studies, preset versions, baseline/candidate relationships, saved findings and comparison state. Coordinate with the existing PostgreSQL/worker owners; do not fork a second metadata store.
- [ ] R05: Replace the shell and landing/library hierarchy; add contextual tabs and cross-study run search. Preserve deep links to existing evidence and exact identities.
- [ ] R06: Rebuild formulation as a controlled hypothesis setup and returning-user editor. Declare planned run count, fixed variables and interpretation constraints.
- [ ] R07: Implement execution results as progress -> completion/cost trade-off -> paired evidence -> causal decision drilldown. Add bounded native-derived whole-run series where required.
- [ ] R08: Implement MM results as P&L/inventory -> paired robustness -> coverage-aware markouts -> toxic-fill/replace investigation. Define new per-fill evidence where required.
- [ ] R09: Unify in-study and cross-study comparison state, cohort pins, absolute evidence, filters and saved findings. Multi-candidate contract follows pairwise correctness.
- [ ] R10: Add durable parameter sweeps and sensitivity matrix only after orchestration and parameter validation exist. No clickable empty heatmap promises.
- [ ] R11: Validate correctness, targeted performance, keyboard and responsive behavior on real native data. New independent trader workflow review must evaluate the new structure, not certify it using earlier three-menu screenshots.
- [ ] R12: Run actual unaided human evaluation against predeclared research tasks. Target16/20 core capabilities plus every critical task; record timings and interpretation, not just clicks. Revamp readiness cannot inherit earlier UI acceptance automatically.

Smallest coherent release is R01-R09 plus R11-R12, with supported preset strategies/scenarios and pairwise comparisons. R10 and multi-candidate analysis follow explicit native/API support. Prototype and implementation are separate milestones; this proposal does not claim any task completed.

## UX references

Nielsen Norman Group: recognition rather than recall, https://www.nngroup.com/articles/recognition-and-recall/ ; progressive disclosure, https://www.nngroup.com/articles/progressive-disclosure/ . These support visible choices and contextual complexity management; the exsim navigation and analytics decisions above are product-specific proposals, not conclusions proven by those articles.
