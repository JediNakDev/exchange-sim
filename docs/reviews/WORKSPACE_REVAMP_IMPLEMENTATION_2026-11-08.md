# Trader workspace revamp implementation

Status: running for user review, uncommitted. This is a structural frontend revamp with real native research data, not an independently certified human usability result. The earlier three-menu human-test handoff is superseded for evaluating this new interface.

## Open and run

- Development UI: http://127.0.0.1:46172/research
- Native research API: http://127.0.0.1:46173
- Supervisor PID14570. Exact owned PIDs, URLs and paths: `output/revamp-review/dev-session.json`.
- Data: `output/revamp-dev-data`. Logs: `output/revamp-review/{supervisor,api-dev,next-dev}.log`.
- Restart from the repository root after stopping this supervisor: `python3 scripts/run_workspace_dev.py`.
- Stop with Ctrl+C in its terminal, or verify PID14570 still names that exact script/session and send SIGTERM. Research data is retained. Do not start a second writer on the same data directory.

The supervisor is detached from the short tool command so normal handoff does not intentionally tear down the development service. It binds loopback and starts Next development mode with hot reload. It deliberately uses the already-reviewed native binary at `output/releases/human-review-75cf28f/api/exsim_api`, source360ad69. This avoids rebuilding or interfering with concurrently changing PostgreSQL/worker source. No installed production service, database, other development data or other owner's backend file was changed.

The visible native examples contain execution baseline/sweep, MM baseline/wider quotes, and one deliberately interrupted configuration. Five configurations form three studies, with actual completed and failed attempts. They are generated synthetic results, not performance promises.

## Implemented workflow

1. Research is now the landing page. Baselines and nested variants group by persisted parent lineage. Questions/descriptions, attention states, concrete next actions and study search replace historical activity counters as the main hierarchy.
2. Strategies lists supported built-in C++ strategies by the decision they answer, relevant evidence and useful treatments. A chosen preset/model opens the correct editor. Unsupported future strategy kinds are not offered as editable models.
3. Scenarios exposes actual synthetic participants, parameter assumptions, fees, latency, risk, model boundaries and service presets. No historical backtest capability is implied.
4. Runs is a cross-study attempt queue with active/paused/interrupted/completed filters, exact seed/run search, retained failure messages and direct control/result entry points. Its search uses optimistic transition state to avoid dropping typed characters while URLs update.
5. Compare is a study-pair library with receipt/finding reopening and cross-study selection. It no longer silently opens an arbitrary newest baseline when entered without context.
6. Study configuration pages group their family, exact changes and baseline direction, then expose Results, Runs and recovery, and Assumptions. Results show absolute per-seed outcomes, not just run counts. Root/child/nested lineage remains the API's persisted evidence rather than a second client metadata store.
7. Setup leads with the question/name and starting preset. Changing presets retains the typed question and study lineage; variants still copy immutable source assumptions. Engine preset and research baseline have distinct labels.
8. Execution Overview prioritizes completion/residual/cost, a bounded target-versus-known-fill decision plot, and observation status. Explain execution retains exact native schedules, reports and lifecycle links. Charts do not manufacture a complete exchange-fill trajectory from callback samples.
9. MM Overview prioritizes native net P&L, inventory/drawdown and coverage, then stacked time-aligned equity-change/inventory/drawdown plots and per-horizon markout bars. Equity change is an explicitly derived display offset using exact decimal strings, not recomputed valuation. No unmeasured horizon becomes zero.
10. Diagnostics and provenance/export remain explicit secondary run sections. Raw IDs/manifests do not lead ordinary result interpretation. Keyboard inspection and return targets remain truthful after paging.
11. Comparison starts with trade-offs and paired results, including completion versus executed USD/share cost for execution. Assumptions/eligibility and time paths have dedicated sections. Absolute seed evidence, native cohort selection, exact units and missing-data semantics remain available.
12. Finding exports bind a researcher-written conclusion to the selected native cohort and metrics. The UI explicitly says notes are local downloads, not server-saved notes. Drafts are keyed by cohort, survive section switching/pinning the same cohort, and do not transfer silently to another selected cohort. Receipts reopen from the Compare library.
13. Study/run/comparison sections and comparison seed selection are URL state. Pinning preserves section context. Refresh and back navigation retain the selected analysis section.

## Design and engineering alignment

Read frontend-design, vercel-react-best-practices, docs/UI_DESIGN.md, web/AGENTS.md and installed Next directive documentation before implementation. Amended UI_DESIGN navigation responsibilities explicitly while retaining the green exact-ledger system: Inter/Geist Mono, token colours, square flat surfaces, shared research components, shadcn primitives, original-value tables and baseline/candidate chart identities.

Independent metadata requests load concurrently. Optional markout plotting loads dynamically. Exact values remain strings; approximate chart coordinates use chart-derived-floats. Libraries do not fetch event streams. The new pages use the existing C++ HTTP/OpenAPI contract; no extra simulation/accounting implementation or schema/store was created.

Current-contract boundaries: studies are lineage groups, not a newly introduced editable database entity. Findings export as files. Libraries expose service presets, not a custom strategy-code editor or independent scenario-version CRUD. Durable editable notebooks, multi-candidate statistical comparisons and parameter-sweep orchestration remain separate API/worker capability work. None is presented as an active empty feature in this workspace.

## Review loops and repairs

Native browser loop:15 checks. Actual UI execution creation, hypothesis preservation, three-seed variant lineage, native paired comparison, finding export/pin identity, four configurations grouped into two studies before the deliberate failure study, native MM outcomes, run-queue step/resume, failed-work discovery, seven390px routes and zero page errors.

Visual and accessibility loop:23 checks. Removed duplicate variant actions and reduced metadata/formula blocks that pushed the main MM plot below the useful viewport. Verified inventory-aware library/editor selection, preset question retention, plot position, export discoverability, draft persistence across sections, keyboard read/return and paging fallback, seven routes in each light/dark390px theme, zero page errors.

State/recovery loop:7 checks. Found and repaired the Compare landing's null-parent auto-selection and run-search character loss. Reopening an actual downloaded finding preserves its exact native pin. Study/run/comparison sections survive refresh, typed search is complete and filters the intended results, no page errors.

Final smoke:4 checks on the supervised dev pair. Pinning retains selected section and same-cohort note, invalid JSON receipt gets an actionable message without a crash, no page errors. Direct UI/API readiness and retained native configuration counts pass after moving to the supervisor.

These are parent expert/automated checks, not independent blind traders or a human80% score. Evidence, scripts, logs and inspected screenshots are in [WORKSPACE_REVAMP_2026-11-08](WORKSPACE_REVAMP_2026-11-08/). The first native mutating probe expects fresh isolated data; do not rerun it against the populated user review environment. Later loops use those recorded review-owned IDs.

Probe-development failures were retained in temporary logs while repairing the probe: two wrong preset/container locators, using context rather than page emulateMedia, and reloading before asynchronous URL navigation settled. Those are not counted as product successes or product defects. Genuine discovered defects were fixed in source before the successful final loops.

## Verification receipts

| Check | Result | Log |
| --- | --- | --- |
| TypeScript | pass | `/tmp/revamp-types.log` |
| ESLint | pass | `/tmp/revamp-lint.log` |
| Ordinary tests |127 pass,5 pre-existing review skips,0 fail;13,330 assertions,3.92s | `/tmp/revamp-tests.log` |
| Production build | pass,15 static generation targets | `/tmp/revamp-build.log` |
| Focused native browser |4/4 pass,10.8s | `/tmp/revamp-native-browser.log` |
| Targeted recovery/accessibility browser |9/9 pass,6.0s | `/tmp/revamp-workflow-browser.log` |
| Python launcher syntax / diff whitespace | pass | checked in parent |

The four native cases cover execution costs/residuals, inventory-aware C++ execution, censoring/pins/complete audit download -> unpack -> native reproduce, and actual MM replacement/FIFO evidence. Two initial native test failures were old metric-table selectors now intentionally behind Paths over time; updated the tests to enter that explicit section, retaining the original numeric assertions. The scoped UI tests now assert the study/preset/section design documented in UI_DESIGN, not the removed counters/checkbox navigation.

No new C++ compilation, broad engine suite, benchmark, full historical browser-suite run, independent redesign audit, screen reader or actual unaided human test is claimed. Use the application to review the new workflow; human intuitiveness remains an observation to collect, not a property inferred solely from these automated passes.
