# Working state and implementation plan

Updated 2026-11-02. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## User decisions and constraints

- Goal remains a low-latency C++ exchange simulator with realistic protocols, strong testing, and no allocations on declared hot paths.
- First trader workflow: **execution and market-making research**.
- Primary research UI: **Bun + Next.js + TypeScript + Tailwind CSS + shadcn/ui**, following `/Users/jedi/AGENTS.md`. Prefer existing components and meaningful names. C++ owns simulation/backend behavior.
- API recommendation: C++ HTTP/OpenAPI with generated TS types; omit a tRPC wrapper initially. The user asked us to assess fit after reopening tRPC, so this is a reasoned recommendation rather than an explicit final veto.
- Resumed/new delegates use GPT and high reasoning. No Ollama while the user works here. Do not add co-author attribution to commits.
- Full product rationale, causal simulation requirements, realism assumptions, accounting, and UI workflow: [PRODUCT.md](PRODUCT.md).

## Verified repository state at recovery

- Branch: `exchange-simulator`; last code/CI commit `83b17c7` before this design work.
- `35da879`: foundation/spec/types/test framework; `120523e`: config/token tables/sequenced store/packet faults; `5fe0add`: server contract; `83b17c7`: CI matrix.
- Earlier parent report: 45/45 release and ASan/UBSan tests, plus Linux compiler checks. These are historical results, not freshly repeated checks for the complete platform.
- The production MatchingEngine is declared but no implementation is merged into this checkout. Protocol sessions, integrated server, deterministic research runner, research API, accounting, and frontend remain unfinished.
- Root untracked `include/exsim/server/admin.hpp`, `src/exsim/server/admin.cpp`, and `tests/exsim/server/admin_tests.cpp` are interrupted parent work. Preserve and verify them before committing. Preserve unrelated `.claude/` and `output/`.
- Codec task completed: branch `t3/exchange-protocol-codecs`, commit `3a8bd64657a35404a80995e3b2307f8595a60fb4`, checkout `/Users/jedi/.t3/worktrees/lob/t3-f657635f`. Agent reports 117/117 tests in release/debug, 93 new protocol tests; not yet integrated.
- Independent foundation review completed with 38 findings. Full report: [FOUNDATION_REVIEW.md](FOUNDATION_REVIEW.md). Do not equate passing existing tests with resolving this review.

## Milestones

- [x] Recover original requirements, interrupted work, completed codec result, and independent review.
- [x] Explore T3 harness tools before selecting a workflow.
- [x] Confirm trader focus and find global frontend conventions.
- [x] Record product/UI/backend boundary and first useful vertical slice.
- [x] Switch six interrupted direct delegates and the nested vector-oracle child to GPT-6.1 Sol at high reasoning. Partial files stay in their original worktrees. Launch status is recorded separately from configuration.
- [ ] Repair reviewed public contracts and foundation defects before resuming dependent engines/oracle.
- [ ] Finish independent networking and background-bot components.
- [ ] Resume three existing engine alternatives and the oracle/fuzzer against the corrected shared contract.
- [ ] Validate alternatives with the independent oracle, allocation guard, and fair benchmarks; keep the fastest correct design and record results.
- [ ] Integrate codecs, selected engine, networking, bots, admin, journal and fault support.
- [ ] Build protocol sessions, server/client, publisher/recovery, and end-to-end checks.
- [ ] Add deterministic event runner and compiled baseline strategy host, separate from real-time network mode.
- [ ] Add versioned run manifest/artifacts and reconciled fixed-point accounting/metrics.
- [ ] Define C++ HTTP/OpenAPI research interface and generated TypeScript types.
- [ ] Build the Next.js experiment/compare/inspect vertical slice on real artifacts.
- [ ] Independently review correctness, causality, accounting, protocol recovery, and UI behavior; resolve findings and run appropriate checks.

## Existing worktrees and continuation ownership

| Task | Checkout | Recovery state |
| --- | --- | --- |
| Ladder engine | `/Users/jedi/.t3/worktrees/lob/t3-eaaf2612` | Failed at Claude usage limit; partial untracked implementation/tests. Wait for shared contract repair. |
| Sorted-vector engine | `/Users/jedi/.t3/worktrees/lob/t3-3500e31b` | Failed at Claude usage limit; partial untracked implementation/tests. Wait for shared contract repair. |
| Arena engine | `/Users/jedi/.t3/worktrees/lob/t3-2d9b3f09` | Failed at Claude usage limit; partial untracked implementation/tests. Wait for shared contract repair. |
| Main reference/fuzzer/journal/scenarios | `/Users/jedi/.t3/worktrees/lob/t3-0fb59d8d` | Failed at usage limit. Contains `reference_shim_DO_NOT_MERGE.cpp`; never merge that as a production engine. Wait for corrected canonical contract. |
| Networking | `/Users/jedi/.t3/worktrees/lob/t3-bbf9dbf6` | Failed at usage limit; partial net headers. Independent of engine internals. |
| Background bots | `/Users/jedi/.t3/worktrees/lob/t3-b900e409` | Failed at usage limit; partial common/context/order-table headers. These generate market conditions; they are distinct from the trader's evaluated strategy. |
| Protocol codecs | `/Users/jedi/.t3/worktrees/lob/t3-f657635f` | Completed GPT/Astra work; commit above. |

Exact child thread IDs/links are in [delegation-registry.json](delegation-registry.json). Read the relevant child before acting. The vector task also spawned a failed reference-model child; do not resume a second competing oracle by accident. Preserve its work and record whether it adds useful coverage after the main oracle returns.

## T3 features evaluated and selected

| Capability | Use here |
| --- | --- |
| Incremental thread reads and individual-item reads | Retrieve specific briefs/final findings with small limits; do not reload whole tool transcripts. |
| Provider/model configuration | Migrate existing failed children to GPT in their actual checkouts, preserving partial files. |
| App-owned delegate tasks and isolated worktrees | Independent scopes and concise completion reports; new worktrees only via the harness. |
| Completion notifications | Resume dependent work on completion; no polling loops or scheduled status checks. |
| Native plan/checklist plus repository checkpoint | Short live milestones in the plan; durable decisions/dependencies/task pointers in these files survive context and provider changes. No separate T3 todo tool was exposed in the inventory. |
| Saved project scripts | The project had no scripts. Added named C++ release and ASan/UBSan test actions; neither runs on worktree creation. |
| Context fork/merge transfers | Available, but not needed for ordinary continuation. Do not create top-level threads without a user request. |
| Scheduled tasks and PR watches | Useful for unattended schedules/PR monitoring when required; not a context-management substitute for current development. |
| Inline HTML preview/render | Available for design review when a visual clarifies a decision; do not start a dev server just to show a diagram. |

For each completion record: task ID, actual branch/commit, changed scope, tests and log path, unresolved risks, and the next dependent action. Ask children for concise reports, not copies of source files or full logs. When public interfaces change, synchronize those changes before building dependent branches.

## Immediate next action

Repair the canonical contract and reviewed foundation defects, then apply that commit to dependent worktrees before resuming engines and the main oracle. Network and background-bot recovery can proceed independently with ownership limits. The next integration must keep design intent separate from verified implementation status.
