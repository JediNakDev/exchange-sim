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
- FIX 4.4 gateway merged `4a636ca` (from `d136ff6`): 76 gateway tests, macOS release 689/689 and ASan/UBSan 687/687, Linux g++ and clang++ 689/689 (`/tmp/exsim-fix-*.log`, `/tmp/exsim-int3-release.log`).
- SBE 1.0 over FIXP 1.0 gateway merged `340a949` (from `02b3430`); server config `protocol = fixp_sbe` and `fixp_sbe_port` `651bddf`: macOS release 738/738, ASan/UBSan 735/735, `make all`, Linux g++ and clang++ 738/738 (`/tmp/exsim-int4-*.log`). libFuzzer campaign on every target, 302,880,773 executions, 0 crashes (`1891ca6`, docs/FUZZING.md).
- Research determinism across platforms: 4 configs x 3 seeds byte-identical (except status.json and the manifest compiler field) on macOS arm64 Apple clang 21, Linux aarch64 GCC 14.2 and Clang 19.1, Linux x86_64 GCC 14.2 and Clang 19.1 (emulated); CI job `research-determinism` keeps it (`6ede718`).
- QA review fix F6: `exsim_md_client` accepts retransmissions only from the configured request server `5ee5097` (loopback reproducer: forged answer delivered before, dropped after; the server E2E suite should keep a case for it).
- QA review fix F3: config `max_payload` uses the publisher bounds 72..65507 `eeb5b21`: release 739/739, ASan/UBSan 736/736, Linux g++ and clang++ 739/739, 0 warnings (`/tmp/exsim-f3-*.log`).
- Deterministic research runner merged `4783f45` (from `2619a8e`): macOS release 789/789, ASan/UBSan 786/786, `make all`, Linux g++ and clang++ 789/789, 0 warnings (`/tmp/exsim-int5-*.log`).
- SBE fuzz target `sbe_order_entry` `33b6f39`: 81,714,054 ASan/UBSan executions, 0 crashes; 1062-file corpus (36 seeds, 5 of them boundary seeds from a mutation check, plus 1026 campaign inputs) fails the replay for all 8 planted codec mutants: release 790/790, ASan/UBSan 787/787, Linux g++ and clang++ fuzz replay 16/16 (campaign logs and mutants in the session scratchpad `sbe-fuzz/`, `sbe-mutation/`).
- Not integrated yet: server binary and end-to-end tests; C++ HTTP/OpenAPI API; UI on real artifacts; QA fixes (OUCH/core/MD, FIX); final independent review.

## Milestones

- [x] Recover original requirements, interrupted work, completed codec result, and independent review.
- [x] Confirm trader focus and frontend conventions; record product/UI/backend boundary.
- [x] Repair reviewed public contracts and foundation defects (`9a2eaa5`).
- [x] Integrate codecs, networking, admin parser, ledger, bots.
- [x] Integrate oracle/fuzzer/scenarios/journal; validate and integrate engine B.
- [x] Finish engines A and C, validate with the oracle, benchmark on one machine, keep the fastest correct one (C; `b874ce5`).
- [x] Protocol sessions: OUCH 4.2/SoupBinTCP (`1023f60`) and FIX 4.4 (`4a636ca`), server and client side.
- [x] Third order-entry protocol: SBE over FIXP (FIX Trading Community open standards, exsim-owned schema), codec, session, gateway per SERVER.md section 2, client (`340a949`, config `651bddf`).
- [x] Market data: ITCH/MoldUDP64 publisher, retransmission server, client receiver and book builder, feed-correctness test under packet faults (`4a8927a`).
- [ ] Server binary (reactor, gateways, MD, admin, faults, bots, journal) and end-to-end loopback tests; client tool; wire-to-wire latency benchmark.
- [x] Deterministic event runner with latency model and compiled baseline strategies (fixed-spread MM, inventory-aware MM, TWAP), accounting, manifests/artifacts, CLI (`4783f45`).
- [ ] C++ HTTP/OpenAPI research interface and generated TypeScript types.
- [ ] Next.js experiment/compare/inspect vertical slice on real artifacts (UI and contract on a mock service merged, `f2c0b57`).
- [ ] Independent review (correctness, causality, accounting, protocol recovery, UI); resolve findings; README/docs with measured numbers.

## Existing worktrees and continuation ownership

| Task | Checkout | State |
| --- | --- | --- |
| FIX 4.4 gateway | `/Users/jedi/.t3/worktrees/lob/t3-49268324`, branch `t3/fix44-order-entry-gateway` | Done (`190ceb1`, `d136ff6`), merged `4a636ca`; release 689/689. Server delegate now wires it in. |
| QA review: FIX gateway | harness worktree (task `...delegate-task%3Acf7a69b1-c24f-40c2-80fe-cdea4b6f93e5`) | Done. Report `/tmp/exsim-review-fix/REPORT.md`, 13 reproducers on `review/fix44-gateway-qa` `13b5a64`. R1 to the OUCH/core/MD fix delegate, the rest to the FIX fix delegate. |
| QA fixes: FIX gateway | harness worktree (task `...delegate-task%3A8d7857c2-edba-48b5-9969-83237eaa437e`) | Running from `eeb5b21`: R2 to R13, P1, P3, P4, and the FIX-internal sink part of R1. |
| QA fixes: OUCH, core, MD | harness worktree (task `...delegate-task%3Ace8d6fb6-4238-4a7b-af8b-012f79492edb`) | Running: F1 (drop_out as a transport cut), F2 (admission check before J), F4 (reserved deferred slot per user), F5 (publisher status), R1 (core `unbind(SessionId, ConnectionId)` and all gateway call sites). |
| Final review: research runner | harness worktree (task `...delegate-task%3Af427d42a-2817-4b65-8bff-27669983ccc9`) | Report-only at `813f074`: causality, accounting, determinism; report `/tmp/exsim-review-runner/REPORT.md`. |
| Final review: SBE over FIXP | harness worktree (task `...delegate-task%3A00a1bebd-409d-4a92-babe-cc3034f3bd7c`) | Report-only at `813f074`: FIXP session, SBE framing, recovery; report `/tmp/exsim-review-sbe/REPORT.md`. |
| Fuzz corpus mutation check | harness worktree (task `...delegate-task%3A9bdb16a6-a126-42be-92ef-f006402dacb4`) | From `922cb39`: plants codec mutants for the protocol fuzz targets, adds boundary seeds until the committed corpora catch them; seeds, corpora and FUZZING.md only. |
| Research runner | `/Users/jedi/.t3/worktrees/lob/t3-e1e148f1`, branch `t3/deterministic-research-runner` | Done (`2619a8e`), merged `4783f45`. API differences for the HTTP adapter: docs/RESEARCH_RUNNER.md. |
| SBE over FIXP | `/Users/jedi/.t3/worktrees/lob/t3-dd94b274`, branch `t3/add-fixp-sbe-gateway` | Done (`02b3430`), merged `340a949`. |
| Server binary and E2E | harness worktree (task `...delegate-task%3A08e286a4-cae1-4949-b6f0-83a32243c0a3`) | Running on Claude since 2026-11-05, from `e2cdd11`. |
| HTTP/OpenAPI API and UI | harness worktree (task `...delegate-task%3Ae28022a3-cc29-4d4a-86c4-4cfec8e4fb7b`) | Running on Claude since 2026-11-05. Told the runner is final at `2619a8e`; must merge it and reconcile the adapter before reporting. |
| QA review: OUCH, MD, core | harness worktree (task `...delegate-task%3A337bf277-8344-4bed-9afb-78ff48b41a1a`) | Done. Report `/tmp/exsim-review-proto/REPORT.md`, reproducers on `t3/exsim-merged-code-qa-review` `26dd524`. F3 fixed `eeb5b21`, F6 fixed `5ee5097`; F1, F2, F4, F5 with the fix delegate; F7, F8 informational. |
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

2026-11-05: GPT hit its usage limit after merging engine C, OUCH, MD, API+UI and fuzzing; the user said "continue as claude". All delegates now run on Claude Opus 5.5 high. FIX (`4a636ca`), SBE (`340a949`) and the research runner (`4783f45`) are merged; both QA reviews are done and their confirmed findings are with two fix delegates.

Next, on completion notifications (do not poll):
1. QA fix delegates (OUCH/core/MD `ce8d6fb6`, FIX `8d7857c2`): review, bring in the review reproducers, merge, full gate (macOS release, ASan/UBSan, `make all`, Linux g++/clang++). `ce8d6fb6` changes the core unbind API, so merge it before the FIX fixes and resolve the single `ctx.unbind` line.
2. Server binary (`08e286a4`): review, merge after the core API change, run the E2E and wire-to-wire benchmark gates; wire SBE into the server if the delegate did not.
3. API and UI (`e28022a3`): check the adapter against the runner differences table, review, merge, run the C++ and bun gates.
4. Fuzz corpus mutation check (`9bdb16a6`): review the seed additions, check that existing seed files are unchanged, merge, replay gates.
5. Final independent review: runner (causality, accounting) and SBE/FIXP reviews are running; after the server and API/UI merges, review the server end to end (all three protocols plus market data) and the UI on real runs; fix findings; README with wire-to-wire numbers (README status refreshed `813f074`).
