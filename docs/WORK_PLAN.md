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
- Wire codec benchmark `exsim_codec_bench` `2d3b27f` (CI perf smoke, zero allocations): perf build, binary protocols 1.4 to 6.0 ns per message; FIX 228 ns to decode a NewOrderSingle and 155 ns to encode an ExecutionReport, 38 to 74 times OUCH and SBE. Linux g++ agrees within 10 ns.
- QA fixes for OUCH, the core and market data merged `6227a51` (from `c71acb3`: F1, F2, F4, F5, F6, F8, R1 with the connection-aware `ExchangeCore::unbind(SessionId, ConnectionId)`): release 806/806, ASan/UBSan 803/803.
- FIX 4.4 gateway QA fixes merged `3df3527` (from `3c82c87`: R1 to R13, P1, P3, P4): release 825/825, ASan/UBSan 822/822.
- Server runtime merged `e9495a0` (from `1d6c40c`): `exsim_server` (OUCH, FIX, ITCH/MoldUDP64 with faults and retransmission, admin, bots, journal on one poller thread), `exsim_client`, `exsim_w2w_bench`, 12 loopback E2E tests, wire-to-wire numbers in docs/SERVER_RUNTIME.md section 5 and the README. The merge added the SBE answer to `SubmitResult::ExchangeShutDown` (MarketClosed) with a mutant-checked test. macOS release 840/840, ASan/UBSan 837/837, TSan 837/837, `make all`, perf tools, Linux g++ and clang++ 840/840, all `-Werror` (session scratchpad `merge3-*.log`); README quick start (server, client, admin shutdown) and a w2w smoke run checked by hand.
- Not integrated yet: SBE in the server runtime; SBE and research runner QA fixes; FIX codec speed-up; C++ HTTP/OpenAPI API; UI on real artifacts; final independent review.

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
- [x] Server binary (reactor, OUCH and FIX gateways, MD, admin, faults, bots, journal) and end-to-end loopback tests; client tool; wire-to-wire latency benchmark (`e9495a0`). SBE in the runtime: delegate `8764c2d4`.
- [x] Deterministic event runner with latency model and compiled baseline strategies (fixed-spread MM, inventory-aware MM, TWAP), accounting, manifests/artifacts, CLI (`4783f45`).
- [ ] C++ HTTP/OpenAPI research interface and generated TypeScript types.
- [ ] Next.js experiment/compare/inspect vertical slice on real artifacts (UI and contract on a mock service merged, `f2c0b57`).
- [ ] Independent review (correctness, causality, accounting, protocol recovery, UI); resolve findings; README/docs with measured numbers.

## Existing worktrees and continuation ownership

| Task | Checkout | State |
| --- | --- | --- |
| FIX 4.4 gateway | `/Users/jedi/.t3/worktrees/lob/t3-49268324`, branch `t3/fix44-order-entry-gateway` | Done (`190ceb1`, `d136ff6`), merged `4a636ca`; release 689/689. Server delegate now wires it in. |
| QA review: FIX gateway | harness worktree (task `...delegate-task%3Acf7a69b1-c24f-40c2-80fe-cdea4b6f93e5`) | Done. Report `/tmp/exsim-review-fix/REPORT.md`, 13 reproducers on `review/fix44-gateway-qa` `13b5a64`. R1 to the OUCH/core/MD fix delegate, the rest to the FIX fix delegate. |
| QA fixes: FIX gateway | harness worktree (task `...delegate-task%3A8d7857c2-edba-48b5-9969-83237eaa437e`) | Done (`3c82c87`), merged `3df3527`. |
| QA fixes: OUCH, core, MD | harness worktree (task `...delegate-task%3Ace8d6fb6-4238-4a7b-af8b-012f79492edb`) | Done (`c71acb3`), merged `6227a51`. |
| QA review: research runner | harness worktree (task `...delegate-task%3Af427d42a-2817-4b65-8bff-27669983ccc9`) | Done. Report `/tmp/exsim-review-runner/REPORT.md`; R1 to R7, P1 to P3 with fix delegate `45a51fdd`. |
| QA review: SBE over FIXP | harness worktree (task `...delegate-task%3A00a1bebd-409d-4a92-babe-cc3034f3bd7c`) | Done. Report `/tmp/exsim-review-sbe/REPORT.md`, reproducers `review/fixp-sbe-gateway-qa` `7ab2ab8`; findings with fix delegate `a8748a28`. |
| Fuzz corpus mutation check | harness worktree (task `...delegate-task%3A9bdb16a6-a126-42be-92ef-f006402dacb4`) | From `922cb39`; run failed on a Claude API rate limit, T3 automatic resume pending. Preserve the worktree; resume, do not relaunch. |
| QA fixes: research runner | harness worktree (task `...delegate-task%3A45a51fdd-c193-4e8b-b5b9-c6e7a81285c0`) | Run failed on a Claude API rate limit, T3 automatic resume pending. Preserve the worktree; resume, do not relaunch. |
| QA fixes: SBE over FIXP | harness worktree (task `...delegate-task%3Aa8748a28-76ae-477b-a78c-34faf7eed31c`) | Running from `e9495a0`: SBE-01 to 03, evaluate 04 to 06; owns `include/exsim/gateway/fixp_sbe/*`. |
| SBE in the server runtime | harness worktree (task `...delegate-task%3A8764c2d4-02f0-40c6-ba0f-7682e846f89a`) | Running from `e9495a0`: `FixpSbeGateway`, SBE E2E tests, `exsim_w2w_bench --protocol`, per-protocol wire-to-wire table; owns `include/exsim/server/runtime/*`, E2E tests, tools, SERVER_RUNTIME.md. |
| Final review: server runtime | harness worktree (task `...delegate-task%3A41fe1043-80cb-4c29-ad21-db85d863e8ac`) | Report-only from `12a349d`: OUCH, FIX, MD, admin, faults, shutdown, journal, allocations, w2w method, flakiness; report `/tmp/exsim-review-server/REPORT.md`. |
| FIX decode performance | harness worktree (task `...delegate-task%3A0c6c66ed-9f41-4389-93f4-3edff5c709c7`) | Running from `3df3527`: decode side of `fix44.hpp`, frozen reference decoder differential test. |
| FIX encode performance | harness worktree (task `...delegate-task%3Ac96ec3e0-a18a-4ff3-a654-a1e12f47f1a4`) | Running from `3df3527`: encode side of `fix44.hpp`, byte-identical versus a frozen reference encoder. |
| Research runner | `/Users/jedi/.t3/worktrees/lob/t3-e1e148f1`, branch `t3/deterministic-research-runner` | Done (`2619a8e`), merged `4783f45`. API differences for the HTTP adapter: docs/RESEARCH_RUNNER.md. |
| SBE over FIXP | `/Users/jedi/.t3/worktrees/lob/t3-dd94b274`, branch `t3/add-fixp-sbe-gateway` | Done (`02b3430`), merged `340a949`. |
| Server binary and E2E | harness worktree (task `...delegate-task%3A08e286a4-cae1-4949-b6f0-83a32243c0a3`) | Done (`1d6c40c`), merged `e9495a0`. |
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

2026-11-05 evening: the QA fixes for OUCH/core/MD (`6227a51`) and FIX (`3df3527`) and the server runtime (`e9495a0`) are merged and gated. A Claude API rate limit failed the runner fix and mutation check runs; T3 resumes them automatically.

Next, on completion notifications (do not poll):
1. SBE QA fixes (`a8748a28`) and SBE runtime wiring (`8764c2d4`): review each, merge the gateway fixes first, then the wiring; full gate (release, ASan/UBSan, TSan, `make all`, perf tools, Linux g++/clang++); README per-protocol wire-to-wire table.
2. FIX decode (`0c6c66ed`) and encode (`c96ec3e0`): check the differential tests and the bench method, merge both (same file, disjoint functions), re-run `exsim_codec_bench`, update the README codec table.
3. API and UI (`e28022a3`): check the adapter against the runner differences table, review, merge, run the C++ and bun gates.
4. Runner fixes (`45a51fdd`) and mutation check (`9bdb16a6`) after their automatic resumes: review, merge, gate.
5. Final independent review: the server runtime review (`41fe1043`) is running; after the SBE wiring and API/UI merges, review SBE in the runtime and the UI on real runs; fix findings; final README pass.
