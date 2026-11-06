# Working state and implementation plan

Updated 2026-11-06. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## User decisions and constraints

- Goal remains a low-latency C++ exchange simulator with realistic protocols, strong testing, and no allocations on declared hot paths.
- First trader workflow: **execution and market-making research**.
- Primary research UI: **Bun + Next.js + TypeScript + Tailwind CSS + shadcn/ui**, following `/Users/jedi/AGENTS.md`. Prefer existing components and meaningful names. C++ owns simulation/backend behavior.
- API recommendation: C++ HTTP/OpenAPI with generated TS types; omit a tRPC wrapper initially. The user asked us to assess fit after reopening tRPC, so this is a reasoned recommendation rather than an explicit final veto.
- Resumed/new delegates use GPT/ChatGPT with high reasoning (latest user decision 2026-11-05, "continue and turn all subagent to chatgpt", replacing the earlier Claude-only rule). No Ollama while the user works here. Do not add co-author attribution to commits.
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
- Runner QA merged `eccfabb` (source `2c2ab79`), and C++ HTTP/OpenAPI API plus UI on real runs merged `1f8d01c` (source `33b41d3`). SBE gateway QA already merged `89a4d87`. API comparison now requires native artifact reproduction; native artifacts use the same canonical identity as CLI runs. Parent API checks: 33/33; real-binary contract: 9/9, all 23 operations, 284 assertions (`/tmp/exsim-delivery-api.log`, `/tmp/exsim-delivery-contract.log`).
- SBE runtime wiring merged `221682a` (source `8520aa1`); server review reproducers merged `54688e7` (source `239298f`). Parent fixes cover complete admin line bounds, retransmission peer filtering and a 64-datagram drain budget. Runtime journaling now uses a bounded persistence worker with explicit failure and bounded teardown; independent re-review and sanitizer gates remain pending. Focused combined server checks: 142/142 (`/tmp/exsim-delivery-runtime-final.log`), including six worker tests and all recovered review reproducers.
- Frontend parent checks: 59/59, typecheck and lint (`/tmp/exsim-delivery-web-{tests,types,lint}.log`). No browser interaction or new performance measurements claimed.
- Strengthened fuzz corpus and properties are integrated from `6eb78e2` (199 added seeds, 94 mutants killed; B4/B8 remain documented harness gaps). FIX codec optimization remains unintegrated. Codec optimizations and measurements remain parked.

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
- [x] C++ HTTP/OpenAPI research interface and generated TypeScript types (`1f8d01c`).
- [x] Next.js experiment/compare/inspect on real artifacts (`1f8d01c`); real-binary HTTP contract verified, browser interaction not yet independently reviewed.
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
| Fuzz corpus mutation check | harness worktree (task `...delegate-task%3A9bdb16a6-a126-42be-92ef-f006402dacb4`) | Completed source `6eb78e2`, clean original checkout; all 19 focused fuzz tests passed on macOS release/ASan and Linux GCC/Clang. 94/103 mutants killed, 7 equivalent, B4/B8 harness gaps remain. Integrated on the parent after `db8c710`. |
| QA fixes: research runner | harness worktree (task `...delegate-task%3A45a51fdd-c193-4e8b-b5b9-c6e7a81285c0`) | Done `2c2ab79`, merged `eccfabb`. Mac release856/ASan853 and Linux GCC/Clang856 passed; 112 cross-platform digests agree. Report `/tmp/exsim-runnerfix-logs/REPORT.md`. |
| QA fixes: SBE over FIXP | harness worktree (task `...delegate-task%3Aa8748a28-76ae-477b-a78c-34faf7eed31c`) | Done production `096f9fd`, assertions `4134656`, merged `89a4d87`. Parent source includes corrected cases; child historical checks below. |
| SBE in the server runtime | harness worktree (task `...delegate-task%3A8764c2d4-02f0-40c6-ba0f-7682e846f89a`) | Done `8520aa1`, merged `221682a`. Parent combined focused server checks 142/142 after runtime repairs. Comparative measurements remain parked. |
| Final review: server runtime | harness worktree (task `...delegate-task%3A41fe1043-80cb-4c29-ad21-db85d863e8ac`) | Report-only review done `239298f`, merged `54688e7`; report `tests/exsim/server/review/REPORT.md`. Parent repairs and re-review status are above. |
| FIX decode performance | harness worktree (task `...delegate-task%3A0c6c66ed-9f41-4389-93f4-3edff5c709c7`) | Checkpoint `8b3f676` in original checkout; 3/3 differential release/ASan. Not ready for merge or speedup claim; parked. |
| FIX encode performance | harness worktree (task `...delegate-task%3Ac96ec3e0-a18a-4ff3-a654-a1e12f47f1a4`) | Checkpoint `25fd168` in original checkout; release846/ASan843/Linux GCC+Clang846 and differential6 passed. Approach comparison and measurements pending; parked. |
| Research runner | `/Users/jedi/.t3/worktrees/lob/t3-e1e148f1`, branch `t3/deterministic-research-runner` | Done (`2619a8e`), merged `4783f45`. API differences for the HTTP adapter: docs/RESEARCH_RUNNER.md. |
| SBE over FIXP | `/Users/jedi/.t3/worktrees/lob/t3-dd94b274`, branch `t3/add-fixp-sbe-gateway` | Done (`02b3430`), merged `340a949`. |
| Server binary and E2E | harness worktree (task `...delegate-task%3A08e286a4-cae1-4949-b6f0-83a32243c0a3`) | Done (`1d6c40c`), merged `e9495a0`. |
| HTTP/OpenAPI API and UI | harness worktree (task `...delegate-task%3Ae28022a3-cc29-4d4a-86c4-4cfec8e4fb7b`) | Done `33b41d3`, merged `1f8d01c`; reproduction integration `96e75c3`. Parent 33 API checks and 9 real-binary contract cases passed. TWAP through the UI remains unsupported. |
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
| Provider/model configuration | Resume existing interrupted children with the user's current provider choice (GPT/high as of the latest 2026-11-05 instruction), preserving their actual checkouts and partial files. |
| App-owned delegate tasks and isolated worktrees | Independent scopes and concise completion reports; new worktrees only via the harness. |
| Completion notifications | Resume dependent work on completion; no polling loops or scheduled status checks. |
| Native plan/checklist plus repository checkpoint | Short live milestones in the plan; durable decisions/dependencies/task pointers in these files survive context and provider changes. No separate T3 todo tool was exposed in the inventory. |
| Saved project scripts | The project had no scripts. Added named C++ release and ASan/UBSan test actions; neither runs on worktree creation. |
| Context fork/merge transfers | Available, but not needed for ordinary continuation. Do not create top-level threads without a user request. |
| Scheduled tasks and PR watches | Useful for unattended schedules/PR monitoring when required; not a context-management substitute for current development. |
| Inline HTML preview/render | Available for design review when a visual clarifies a decision; do not start a dev server just to show a diagram. |

For each completion record: task ID, actual branch/commit, changed scope, tests and log path, unresolved risks, and the next dependent action. Ask children for concise reports, not copies of source files or full logs. When public interfaces change, synchronize those changes before building dependent branches.

## Immediate next action

2026-11-06 parent integration checkpoint:
1. Runner QA, real API/UI, SBE runtime wiring and server review reproducers are merged. `cd web && bun run dev` launches the C++ API and Next.js; `bun run dev:mock` is explicit. HTTP comparisons require byte-for-byte native reproduction, and corrupted runs are excluded from seed pairing. Native run IDs match CLI identities while API IDs remain distinct service identities.
2. REVIEW-SRV-01/02/04 are repaired and their original tests are always on. The benchmark keeps separate actual-send and intended-deadline distributions. A deterministic 65-datagram case verifies the 64-entry request drain bound for SRV-05. No new latency numbers are claimed.
3. REVIEW-SRV-03 implementation: runtime `JournalWorker` hands commands and flush requests through a preallocated SPSC queue. It preserves canonical encoding and uses the existing shutdown/continue policies on overflow/I/O error. The shutdown drain deadline applies to persistence, with at most 100 ms additional worker teardown. Stalled detached workers own their buffers and descriptor. Special destinations are rejected before truncation. Offline journal writers remain synchronous. Six worker tests cover replay, blocked I/O, overflow, I/O failure, bounded destruction and core policies. Independent re-review remains required.
4. Strengthened fuzz mutation branch `6eb78e2` is integrated. Keep FIX encode/decode performance branches parked; no valid new speedup claim or comparative runtime measurements exist.
5. Validate the combined tree and commission a focused independent review of the new journal boundary and API comparison integration. Do not confuse earlier child gates with verification of later parent changes.

All prior eight continuation processes have ended. The earlier shared-host scheduling hold caused deferred child checks; no new benchmark slot has been released. Historical child evidence remains below and in the registry.

## SBE gateway QA completion (2026-11-05, child a8748a28)

- Checkout `/Users/jedi/.t3/worktrees/lob/t3-73fbba65`, branch `fixp-sbe-qa-fixes`. Preserved reproducer commit `6b28f17`, existing partial implementation and untracked `output/`. Fix commit `096f9fd`; parent integrated through `89db302` (merge `31820a1`).
- SBE-01: sticky day-long store-loss latch, stop all appends after loss, reject Establish and Negotiate with an explicit reason. Regressions use passive partial fills so admission cannot mask overflow.
- SBE-02: live order terminal-report reservation in both count and byte budgets, distinct new/replace admission, refuse requests and forward gaps unapplied when only terminal reserve remains. Tests cover disconnect, repeated requests/gaps and end-of-day byte budget.
- SBE-03: Terminate response and end_session send Terminate in Negotiated as well as Established. SBE-04: correct client resend comment. SBE-05: commit UUID only after NegotiationResponse enters the sink, preserving the previous flow on failure. SBE-06: documented and tested the existing AlreadyEstablished continuation policy; no behavior change required.
- Public connection method signatures unchanged, including end_session. Added diagnostic accessors `store_overflowed`, `live_order_count`, statistic `requests_refused_at_store_reserve`, and close reason `StoreReserveReached`. No wire schema change or codec change.
- Behavioral mutation checks remove each SBE-01/02/03/05 fix and confirm regression failures: `output/logs/mutations.log`, individual `output/logs/mutant-sbe-{01,02,03,05}.log`. Restored production fixes before final gates. Reused baseline failures from `output/logs/baseline_fixp.log`; 39/39 focused checks before final assertions in `output/logs/resume-focused.log`.
- Final macOS release gate: 857/858, with only the newly added command-count assertion failing because close triggers cancel-on-disconnect (`output/logs/final-release.log`). Corrected that assertion; final focused SBE recheck 40/40 (`output/logs/final-focused.log`). Every other release test passed, including `fuzz_regression.sbe_order_entry` and both SBE allocation checks. No production change after the full gate.
- Shared-host scheduling instruction arrived while Docker was already compiling. It uses the pre-correction test snapshot and will fail the same assertion; the chained clang gate will not start. Local debug/all were gated behind the failed release command and did not start. Remaining: focused ASan/UBSan and corrected Linux g++/clang++ plus make all, only after parent releases a build slot. Logs: `output/logs/final-linux.log`; preserve failed gate evidence. Runtime owner in `t3-59ca89aa` will integrate after parent merges these QA commits; its files were not edited here.

- Runtime follow-up: initial ASan/UBSan 842/842 and Linux g++/clang++ 845/845 passed (`output/runtime-sbe/{debug,linux}.log`). Benchmark REVIEW-SRV-02 fixed in `d6eb71c`: retain intended deadlines, both latency distributions, late and missed deadline counters; direct recorder regressions in `b585294` await serial validation slot. Parent SBE QA `89a4d87` merged before final tests.
## Server runtime review completion (2026-11-05)

- Original child task `41fe1043-80cb-4c29-ad21-db85d863e8ac`, original target `12a349d` / runtime `e9495a0`, resumed in `/Users/jedi/.t3/worktrees/lob/t3-6663db04`, branch `t3/review-exsim-server-runtime`. Parent documentation-only updates fast-forwarded through `4440d8f` before final handoff; runtime and original scope unchanged. SBE remains excluded.
- Review commit `d94edfe`: only `tests/exsim/server/review/runtime_review_tests.cpp` and its `REPORT.md`. Four opt-in failures (`EXSIM_RUN_SERVER_REVIEW_FAILURES=1`): oversized complete admin lines, benchmark missed-send-deadline omission, blocking journal flush, untrusted MD retransmission sender. All remain open by report-only ownership. Unbounded retransmission draining is plausible and deferred, not experimentally confirmed. No production fixes or public API changes.
- Report: `/tmp/exsim-review-server/REPORT.md`, also tracked next to the tests. Release normal review entries 7/7; explicit opt-in run produces the four expected failures (`checks-release.log`, `reproducers-release-final.log`). Three always-on checks cover generation boundaries, duplicate login/binding/slot reuse, busy-poll day end. Normal runs skip the four known-defect bodies.
- Recovered and reused all 30 release E2E passes; all 10 new ASan/UBSan repetitions pass 12/12, logs `/tmp/exsim-review-server/flaky/{release,debug}_run_N.log`. Focused ASan/UBSan and TSan gates each pass existing E2E 12/12 (`focused-debug.log`, `focused-tsan.log`); normal review entries 7/7 in each (`checks-debug.log`, `checks-tsan.log`). SIGINT and SIGTERM idle process shutdown exit 0 (`signals-final.log`). All log basenames are under `/tmp/exsim-review-server/`.
- Linux g++ `-Werror` validation setup failed because the temporary copy omitted a fuzz header (`linux-gcc.log`); new-test Linux compilation remains incomplete. No replacement Docker/full-suite build or performance measurement started after the parent's shared-host resource instruction. Tests use a bounded fork for the pipe-backed journal reproducer; Linux portability still needs verification. No new latency/throughput claims.
- Next: parent may cherry-pick scoped review commit, route confirmed defects to owners, decide persistence/benchmark contracts, and run a focused Linux gate when resources allow. No pushes or parent merges performed.

## Fuzz corpus mutation check completion (2026-11-05)

- Task `9bdb16a6-a126-42be-92ef-f006402dacb4`, existing checkout
  `/Users/jedi/.t3/worktrees/lob/t3-d19563e5`, branch
  `t3/fuzz-corpus-mutation-check`. Resumed on GPT per latest user instruction.
- Scoped implementation `3cfea1a`; parent b2e6b02 integrated as `d1a7527`.
  Added 199 boundary seeds across ten targets, with small OUCH/ITCH/FIX
  property strengthening. Existing corpus files are unchanged. Production
  codecs and public APIs are untouched; FIX performance owner informed via parent.
- 103 mutants: 61 killed originally, 94 after seeds/properties, 7 equivalent,
  2 deferred send-buffer backpressure harness gaps (B4/B8). F8 is now killed
  by a correctly reframed premature-CheckSum property. No new production bug.
- Original full gates retained: macOS release 790/790, ASan/UBSan 787/787,
  Linux GCC and Clang 790/790 each. Recovered campaign: 68,358,584 executions,
  no crash artifacts. Integrated focused release and sanitizer replay 19/19
  each; final FIX release replay 447 inputs without violations. Generator
  strict warning check and byte comparison of 62 original plus 199 appended
  seeds pass; historical cancel frame bytes preserved after parent QA changes.
- Exact logs and mutation evidence index: `/tmp/exsim-mutation-resume/EVIDENCE.md`.
  Per-target results and deferred limitations: `docs/FUZZING.md`.
- Integrated Linux GCC and Clang focused fuzz replay passed 19/19 each
  with `-Werror` (`/tmp/exsim-mutation-resume/linux.log`). No warnings.
  Active validation is complete; no further builds or measurements started.
- Next action: parent review/integration; retain B4/B8 for a future bounded
  backpressure harness change. No push or parent merge performed by this child.
