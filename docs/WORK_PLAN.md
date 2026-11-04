# Working state and implementation plan

Updated 2026-11-05. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## User decisions and constraints

- Goal remains a low-latency C++ exchange simulator with realistic protocols, strong testing, and no allocations on declared hot paths.
- First trader workflow: **execution and market-making research**.
- Primary research UI: **Bun + Next.js + TypeScript + Tailwind CSS + shadcn/ui**, following `/Users/jedi/AGENTS.md`. Prefer existing components and meaningful names. C++ owns simulation/backend behavior.
- API recommendation: C++ HTTP/OpenAPI with generated TS types; omit a tRPC wrapper initially. The user asked us to assess fit after reopening tRPC, so this is a reasoned recommendation rather than an explicit final veto.
- Resumed/new delegates use Claude Opus 5.5 with high reasoning (user decision 2026-11-05, "continue as claude", after GPT hit its usage limit; GPT was used 2026-11-04 20:45 to 2026-11-05 00:10 +08). No Ollama while the user works here. Do not add co-author attribution to commits.
- Full product rationale, causal simulation requirements, realism assumptions, accounting, and UI workflow: [PRODUCT.md](PRODUCT.md).

## Verified repository state (2026-11-05)

- Branch `exchange-simulator`. Integrated and verified on this branch:
  - foundation and repaired contracts `9a2eaa5`; codecs `57bfe2e`; networking `2ee9b5a`; admin parser `5bd83ca`; position ledger `bacb58c`; bots `3125553`, `52b750f`; legacy lob fixes `7035f89`;
  - oracle, differential fuzzer, scenario DSL, canonical journal and replay `52a241f`; SPEC conformance audit and scenarios `6ee8a50` (via `integrate-engine-c`);
  - production engine C (index arena) `b874ce5` (from `integrate-engine-c` `02f23d7`), after its independent review: per-instrument order lists fix the EndOfDay and imbalance scans; engine B is superseded and engine A (`b77d206`) was evaluated and not selected (`docs/engine-variants/A_TICK_LADDER.md`);
  - exchange core `a8c8e5a` (from `4948efe`); OUCH 4.2/SoupBinTCP gateway `1023f60` (from `b64057e`); ITCH 5.0/MoldUDP64 publisher, retransmission, receiver and book builder `4a8927a` (from `79dbd73`); OpenAPI research contract and Next.js research UI on a mock service `f2c0b57` (from `a5bc883`); coverage-guided fuzzing `4f44239` (from `e866bce`).
- Checks on the integrated tree plus the 2026-11-05 review fixes (FIX checksum framing, server config limits, price overflow, scenario book lines, engine-agnostic dispatch capacity test, fuzz model without known-bug steering): macOS release 613/613, ASan/UBSan 611/611, `make all`, warning-free (`/tmp/exsim-int2-{release,debug,all}.log`); Linux g++ and clang++ release 613/613, 0 warnings (`/tmp/exsim-int2-linux.log`). Engine C fuzz: 1000 seeds x 20000 steps per profile, 40M commands, no divergence.
- Not integrated yet: FIX 4.4 gateway, SBE over FIXP gateway, deterministic research runner (all three resumed on Claude in their own worktrees); server binary and end-to-end tests; C++ HTTP/OpenAPI API; UI on real artifacts; final independent review.

## Milestones

- [x] Recover original requirements, interrupted work, completed codec result, and independent review.
- [x] Confirm trader focus and frontend conventions; record product/UI/backend boundary.
- [x] Repair reviewed public contracts and foundation defects (`9a2eaa5`).
- [x] Integrate codecs, networking, admin parser, ledger, bots.
- [x] Integrate oracle/fuzzer/scenarios/journal; validate and integrate engine B.
- [x] Finish engines A and C, validate with the oracle, benchmark on one machine, keep the fastest correct one (C; `b874ce5`).
- [ ] Protocol sessions: OUCH 4.2/SoupBinTCP (done, `1023f60`) and FIX 4.4 (resumed on Claude).
- [ ] Third order-entry protocol: SBE over FIXP (FIX Trading Community open standards, exsim-owned schema), codec, session, gateway per SERVER.md section 2, client.
- [x] Market data: ITCH/MoldUDP64 publisher, retransmission server, client receiver and book builder, feed-correctness test under packet faults (`4a8927a`).
- [ ] Server binary (reactor, gateways, MD, admin, faults, bots, journal) and end-to-end loopback tests; client tool; wire-to-wire latency benchmark.
- [ ] Deterministic event runner with latency model and compiled baseline strategies (fixed-spread MM, inventory-aware MM, TWAP), accounting, manifests/artifacts, CLI.
- [ ] C++ HTTP/OpenAPI research interface and generated TypeScript types.
- [ ] Next.js experiment/compare/inspect vertical slice on real artifacts (UI and contract on a mock service merged, `f2c0b57`).
- [ ] Independent review (correctness, causality, accounting, protocol recovery, UI); resolve findings; README/docs with measured numbers.

## Existing worktrees and continuation ownership

| Task | Checkout | State |
| --- | --- | --- |
| FIX 4.4 gateway | `/Users/jedi/.t3/worktrees/lob/t3-49268324`, branch `t3/fix44-order-entry-gateway` | Resumed on Claude 2026-11-05: merge `exchange-simulator`, finish verification, commit. |
| Research runner | `/Users/jedi/.t3/worktrees/lob/t3-e1e148f1`, branch `t3/deterministic-research-runner` (`b06a276`, `e44b5c6`) | Resumed on Claude 2026-11-05. Must not commit `docs/WORK_PLAN.md`; notes go in `docs/RESEARCH_RUNNER.md`. |
| SBE over FIXP | `/Users/jedi/.t3/worktrees/lob/t3-dd94b274`, branch `t3/add-fixp-sbe-gateway` (`f2a6a83`, `547ba68`) | Resumed on Claude 2026-11-05 (GCC constexpr fix, merge, commit). |
| Server binary and E2E | harness worktree (task `...delegate-task%3A08e286a4-cae1-4949-b6f0-83a32243c0a3`) | Running on Claude since 2026-11-05, from `e2cdd11`. |
| HTTP/OpenAPI API and UI | harness worktree (task `...delegate-task%3Ae28022a3-cc29-4d4a-86c4-4cfec8e4fb7b`) | Running on Claude since 2026-11-05, from `e2cdd11` plus the committed runner branch; re-check against the finished runner before merge. |
| Ladder engine A | `/Users/jedi/.t3/worktrees/lob/t3-eaaf2612` (`b77d206`) | Done; evaluated, not selected. |
| Engine C integration | scratch worktree `engine-c-integration`, branch `integrate-engine-c` | Merged (`b874ce5`). |
| Vector engine B | `/Users/jedi/.t3/worktrees/lob/t3-3500e31b` | Done, superseded by C. Untracked redundant second oracle not merged. |
| Oracle, networking, codecs, bots, core, OUCH, MD, API+UI, fuzzing | see registry | Done, integrated. |

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

2026-11-05: GPT hit its usage limit after merging engine C, OUCH, MD, API+UI and fuzzing; the user said "continue as claude". The orchestrator verified and committed GPT's uncommitted review fixes, then resumed FIX, runner and SBE on Claude in their own worktrees and launched two new delegates: server binary plus end-to-end tests, and the C++ HTTP/OpenAPI API with the UI on real runs (started in parallel against the committed runner).

Next, on completion notifications (do not poll):
1. FIX, SBE: review, merge into `exchange-simulator`, full gate (macOS release and ASan/UBSan, Linux g++/clang++), then plug them into the server binary.
2. Runner: review and merge first; then reconcile the API delegate's adapter with the finished runner, review and merge the API and UI.
3. Server binary: review, merge, run the E2E and wire-to-wire benchmark gates.
4. Final independent review (correctness, causality, accounting, protocol recovery, UI) by separate QA delegates; fix findings; README with measured numbers.
