# Working state and implementation plan

Updated 2026-11-03. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## User decisions and constraints

- Goal remains a low-latency C++ exchange simulator with realistic protocols, strong testing, and no allocations on declared hot paths.
- First trader workflow: **execution and market-making research**.
- Primary research UI: **Bun + Next.js + TypeScript + Tailwind CSS + shadcn/ui**, following `/Users/jedi/AGENTS.md`. Prefer existing components and meaningful names. C++ owns simulation/backend behavior.
- API recommendation: C++ HTTP/OpenAPI with generated TS types; omit a tRPC wrapper initially. The user asked us to assess fit after reopening tRPC, so this is a reasoned recommendation rather than an explicit final veto.
- Resumed/new delegates use Claude Opus 5.5 and high or xhigh reasoning (user decision 2026-11-03). No Ollama while the user works here. Do not add co-author attribution to commits.
- Full product rationale, causal simulation requirements, realism assumptions, accounting, and UI workflow: [PRODUCT.md](PRODUCT.md).

## Verified repository state (2026-11-03)

- Branch `exchange-simulator`. Integrated and verified on this branch:
  - foundation + repaired contracts `9a2eaa5`; codecs `57bfe2e`; networking `2ee9b5a`; admin parser `5bd83ca`; position ledger `bacb58c`; bots `3125553`, `52b750f`;
  - oracle, differential fuzzer, scenario DSL, canonical journal/replay `52a241f` (from `5abebec`);
  - engine variant B (sorted level vectors) `d9cffc9` (from `66fe553`) as the current production engine.
- Checks on `d9cffc9`: `make test EXTRA_CXXFLAGS=-DEXSIM_TEST_PRODUCTION` 271/271 release, 269/269 ASan/UBSan, warning-free; `make tools`; `exsim_fuzz` 200 seeds x 10000 steps, mixed and tiny profiles, no divergence. Logs: `/tmp/exsim-int-release.log`, `/tmp/exsim-int-debug.log`. Long campaign 1000 seeds x 20000 steps per profile (40M commands) passed with no divergence: `/tmp/exsim-fuzz-long/`.
- Not built yet: protocol sessions (OUCH/SoupBinTCP, FIX), market-data publisher/retransmission/feed handler, server binary and end-to-end tests, deterministic research runner, manifests/artifacts, HTTP/OpenAPI API, frontend.
- Delegates: all on Claude Opus 5.5 since 2026-11-03 (user decision). Do not add co-author lines to commits.

## Milestones

- [x] Recover original requirements, interrupted work, completed codec result, and independent review.
- [x] Confirm trader focus and frontend conventions; record product/UI/backend boundary.
- [x] Repair reviewed public contracts and foundation defects (`9a2eaa5`).
- [x] Integrate codecs, networking, admin parser, ledger, bots.
- [x] Integrate oracle/fuzzer/scenarios/journal; validate and integrate engine B.
- [ ] Finish engines A and C (resumed on Claude), validate with the oracle, benchmark all three on one machine/run, keep the fastest correct one.
- [ ] Protocol sessions: OUCH 4.2/SoupBinTCP and FIX 4.4 (server and client).
- [ ] Market data: ITCH/MoldUDP64 publisher, retransmission server, client receiver and book builder, feed-correctness test under packet faults.
- [ ] Server binary (reactor, gateways, MD, admin, faults, bots, journal) and end-to-end loopback tests; client tool; wire-to-wire latency benchmark.
- [ ] Deterministic event runner with latency model and compiled baseline strategies (fixed-spread MM, inventory-aware MM, TWAP), accounting, manifests/artifacts, CLI.
- [ ] C++ HTTP/OpenAPI research interface and generated TypeScript types.
- [ ] Next.js experiment/compare/inspect vertical slice on real artifacts.
- [ ] Independent review (correctness, causality, accounting, protocol recovery, UI); resolve findings; README/docs with measured numbers.

## Existing worktrees and continuation ownership

| Task | Checkout | State |
| --- | --- | --- |
| Ladder engine A | `/Users/jedi/.t3/worktrees/lob/t3-eaaf2612` | Resumed on Claude 2026-11-03 (gate: merge `5abebec`, production tests, fuzz, bench). |
| Arena engine C | `/Users/jedi/.t3/worktrees/lob/t3-2d9b3f09` | Resumed on Claude 2026-11-03 (same gate). |
| Vector engine B | `/Users/jedi/.t3/worktrees/lob/t3-3500e31b` | Done, integrated. Untracked `tests/exsim/engine/vector_reference_model.hpp` is the redundant second oracle; not merged. |
| Oracle/fuzzer/journal/scenarios | `/Users/jedi/.t3/worktrees/lob/t3-0fb59d8d` | Done, integrated. Disabled shim stays untracked there. |
| Networking, codecs, bots | see registry | Done, integrated. |

Wave-2 task IDs and branches are recorded in [delegation-registry.json](delegation-registry.json) when launched.

## T3 features evaluated and selected

| Capability | Use here |
| --- | --- |
| Incremental thread reads and individual-item reads | Retrieve specific briefs/final findings with small limits; do not reload whole tool transcripts. |
| Provider/model configuration | Migrate existing failed children to Claude Opus 5.5 in their actual checkouts, preserving partial files. |
| App-owned delegate tasks and isolated worktrees | Independent scopes and concise completion reports; new worktrees only via the harness. |
| Completion notifications | Resume dependent work on completion; no polling loops or scheduled status checks. |
| Native plan/checklist plus repository checkpoint | Short live milestones in the plan; durable decisions/dependencies/task pointers in these files survive context and provider changes. No separate T3 todo tool was exposed in the inventory. |
| Saved project scripts | The project had no scripts. Added named C++ release and ASan/UBSan test actions; neither runs on worktree creation. |
| Context fork/merge transfers | Available, but not needed for ordinary continuation. Do not create top-level threads without a user request. |
| Scheduled tasks and PR watches | Useful for unattended schedules/PR monitoring when required; not a context-management substitute for current development. |
| Inline HTML preview/render | Available for design review when a visual clarifies a decision; do not start a dev server just to show a diagram. |

For each completion record: task ID, actual branch/commit, changed scope, tests and log path, unresolved risks, and the next dependent action. Ask children for concise reports, not copies of source files or full logs. When public interfaces change, synchronize those changes before building dependent branches.

## Immediate next action

Running on Claude (task IDs in [delegation-registry.json](delegation-registry.json)): engines A and C, OUCH gateway, FIX gateway, market-data publisher/feed, research runner, exchange core, legacy lob fixes, and the research API contract plus Next.js UI on a mock server (`web/`, `docs/api/openapi.yaml`, `docs/API.md`). Integrate each completion on `exchange-simulator` with the full gate. When gateways, publisher, and core land, launch the server binary and end-to-end tests; when the runner and contract land, launch the C++ HTTP/OpenAPI API on real artifacts and point the UI at it. Do not poll running tasks.
