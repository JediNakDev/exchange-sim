# Working state and implementation plan

Updated 2026-11-07. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## Current continuation queue

Read this rollup and [quality/PLAN_2026-11-07.md](quality/PLAN_2026-11-07.md) first. Earlier tables below are historical recovery/delegation records; read their detailed reports only for the relevant task.

- Parent branch: `exchange-simulator`. First frontend recovery, durable API, TWAP/sweep browser workflow, Linux gates and FER-01 repair are integrated. Source details and receipts: [quality/INTEGRATION_2026-11-07.md](quality/INTEGRATION_2026-11-07.md), [quality/FILL_IDENTITY_REPAIR_2026-11-07.md](quality/FILL_IDENTITY_REPAIR_2026-11-07.md).
- Integrated API review `ec50c01c` completed as `05c0f71`, merged `df01ab7`. Repair `8ee86f1` passes API71 release/ASan, native HTTP9/all23 operations and warning-free builds. Fresh independent re-check `2a2bd1b`, integrated `5c97846`, clears IAR-01/SC-01 within scope, with independent fill-based accounting and restart probes. Linux GCC/Clang now each pass API71 and API builds, warning-free. Details: [quality/RECORDER_FAILURE_REPAIR_2026-11-07.md](quality/RECORDER_FAILURE_REPAIR_2026-11-07.md), [quality/LINUX_RECORDER_REPAIR_2026-11-07.md](quality/LINUX_RECORDER_REPAIR_2026-11-07.md). Historical Linux64 is not the repair evidence.
- Current frontend: 82 ordinary invariants, type/lint/build. Independent FER-01 review `4086a20`, integrated `c78e044`, clears the repair with browser9/helpers16. Five new independent edge probes are copied unchanged into ordinary CI and pass 5 on the Node24 release artifact, plus types/scoped lint. CI discovers 21 ordinary cases; no fresh combined 21 run is claimed. The old limited reviewer `2381f721` still supplies no clearance; the fresh result supersedes that missing result.
- Deployed pair: `production-20261107-a0b3c8d`, API source `8ee86f1`, frontend source `a0b3c8d`, build `1S7lB4_DnycLm77V_UTS_`, Node v24.22.1. Both LaunchAgents select this immutable bundle. Final offline backup includes all 13 completed runs and new per-key records; the restored copy matches all 101 responses exactly, including native comparison. Final data inventory matched the backup before promotion. Dynamic-route warmup and the live tour preserve the final package inventory; full private HTTPS readiness passes after use. [Release evidence and limits](quality/RELEASE_2026-11-07.md).
- Old copied `production-20261107-candidate-7ca1014` remains unsuitable. The first 8ee frontend added runtime route-cache files and failed post-use inventory; it is retained as superseded evidence, not an immutable pass. The final cache-only rebuild disables ISR disk flushing and adds a whole-tree browser invariant. [Cache correction and receipts](quality/RELEASE_CACHE_REPAIR_2026-11-07.md). The old installed pair, original plists, full private backup and restored copies are retained. Writable downgrade to the JSONL-only API is unsafe after new per-key records; no automatic data rollback is authorized.
- New 27.4-second execution clip shows real TWAP/sweep outcomes, fill evidence, reviewed variant and same-seed comparison; it is decoded/visually checked. The actual live creation pass had 38 successful API responses, final capture 35 on the corrected release, no failed responses or page errors. Existing 11 completed runs are preserved; two intentionally created demonstration runs use the new store. [Media notes](demo/README.md). Source publication targets `origin/exchange-simulator`, including implementation `a0b3c8d` and the corrected release receipt; no main-branch merge or new PR is planned.
- Preserve `.claude/`, `output/` and interrupted worktrees. No running delegates or validation/capture processes remain; the two supervised production processes stay live.

### Independent checks at `8ee86f1`

Both tasks use GPT `gpt-6.1-sol` / xhigh in separate harness worktrees.
Both completion results are integrated; scope-specific limits remain in reports.

| Scope | Full harness task ID | Checks and next action |
| --- | --- | --- |
| Recorder failure / cache bounds | `node:delegated-task:command%3Amcp%3Ac5871c80-7318-4575-b3f9-3687f8548075%3Adelegate-task%3Aexsim-20261107-iar01-repair-review-8ee86f1` | Complete; `/Users/jedi/.t3/worktrees/lob/t3-8c2c07d0`, `t3/review-exchange-simulator-repair`. Report/tests `2a2bd1b` integrated `5c97846`; child record `fdc64fc`. No new confirmed defect. Log `/tmp/exsim-recorder-independent-8ee86f1-complete.log`. Fresh parent Linux71 GCC/Clang also passes. |
| Selected-fill identity | `node:delegated-task:command%3Amcp%3Ac5871c80-7318-4575-b3f9-3687f8548075%3Adelegate-task%3Aexsim-20261107-fer01-fresh-review-8ee86f1` | Complete; `/Users/jedi/.t3/worktrees/lob/t3-c95c391d`, `t3/selected-fill-identity-review`. Report/tests `4086a20` integrated `c78e044`; child record `e0f62a7`. Browser9/helpers16 pass, no remaining scoped defect. `/tmp/exsim-fer01-c95c391d-browser.log`. Separate review build `l0GERnR0u-txND9Nz1FSh` on Node26 is not the deployed build. |

Both independent checks are complete. Restore/promotion and private readiness
passed. No polling or replacement reviewer is needed for these repaired scopes.

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
- Runner QA merged `eccfabb` (source `2c2ab79`), and C++ HTTP/OpenAPI API plus UI on real runs merged `1f8d01c` (source `33b41d3`). SBE gateway QA already merged `89a4d87`. API comparison now requires native artifact reproduction; native artifacts use the same canonical identity as CLI runs. Parent API checks: 33/33; real-binary contract: 9/9, all 23 operations (`/tmp/exsim-delivery-api.log`, `/tmp/exsim-delivery-contract-final.log`).
- SBE runtime wiring merged `221682a` (source `8520aa1`); server review reproducers merged `54688e7` (source `239298f`). Parent fixes cover complete admin line bounds, retransmission peer filtering and a 64-datagram drain budget. Runtime journaling now uses a bounded persistence worker with explicit failure and bounded teardown; independent re-review and sanitizer gates remain pending. Focused combined server checks: 142/142 (`/tmp/exsim-delivery-runtime-final.log`), including six worker tests and all recovered review reproducers.
- Combined parent checks at `b5ff70f`: macOS release 927/927 (`/tmp/exsim-delivery-release.log`), ASan/UBSan 923/923 (`/tmp/exsim-delivery-sanitizers.log`), focused server TSan 142/142 (`/tmp/exsim-delivery-tsan.log`), and `make all -Werror` (`/tmp/exsim-delivery-all.log`). Linux GCC 14.2 and Clang 19.1 each passed 927/927 and `make all`, with `-Werror` and no compiler warnings/errors. Source snapshot `b5ff70f`; log `/tmp/exsim-delivery-linux.log`; container exited 0.
- Frontend parent checks: 59/59, typecheck, lint and production build (`/tmp/exsim-delivery-web-{tests,types,lint,build}.log`). No browser interaction or new performance measurements claimed.
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
1. Runner QA, real API/UI, SBE runtime wiring and server review reproducers are merged. `cd web && bun run dev` launches the C++ API and Next.js; `bun run dev:mock` is explicit. HTTP comparisons re-execute the expected API configuration and seed, require byte-for-byte native reproduction, and derive comparison metrics, samples and final time from the fresh execution. Corrupted or substituted native runs are excluded; outer view caches cannot supply comparison results. Native run IDs match CLI identities while API IDs remain distinct service identities.
2. REVIEW-SRV-01/02/04 are repaired and their original tests are always on. The benchmark keeps separate actual-send and intended-deadline distributions. A deterministic 65-datagram case verifies the 64-entry request drain bound for SRV-05. No new latency numbers are claimed.
3. REVIEW-SRV-03 implementation: runtime `JournalWorker` hands commands and flush requests through a preallocated SPSC queue. It preserves canonical encoding and uses the existing shutdown/continue policies on overflow/I/O error. The shutdown drain deadline applies to persistence, with at most 100 ms additional worker teardown. Stalled detached workers own their buffers and descriptor. Special destinations are rejected before truncation. Offline journal writers remain synchronous. Six worker tests cover replay, blocked I/O, overflow, I/O failure, bounded destruction and core policies. Independent review `0aa017f` found no further journal ordering/lifetime issue; parent resolution of its three integration findings is documented below.
4. Strengthened fuzz mutation branch `6eb78e2` is integrated. Keep FIX encode/decode performance branches parked; no valid new speedup claim or comparative runtime measurements exist.
5. Original Linux combined validation is complete for `b5ff70f`. New review `0aa017f` is completed and merged `0e7c24e`. IR-01 (comparison caches), IR-02 (native/API config and seed binding), and IR-03 (blocking shutdown diagnostics) are fixed on the parent. Final focused Linux GCC/Clang (53/53 each plus API/server builds) and server TSan (143/143) passed for those repairs; do not reuse the earlier Linux counts as evidence of the changed source. See [INTEGRATION_FIXES_2026-11-06.md](reviews/INTEGRATION_FIXES_2026-11-06.md).

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

## Focused integration review launched (2026-11-06)

- Task `node:delegated-task:command%3Amcp%3A22879d5a-e0f7-481c-b85b-83c997d9b0af%3Adelegate-task%3A64634ba1-67d0-4380-8ee1-3f601a64688e`. Child uses GPT-6.1 Sol, high reasoning, harness isolation; no production edits or heavy validation authorized. Checkout/branch and scoped commit will be recorded on completion.
- Target `69bb968` (source identical to validated `b5ff70f`). Owns new report `docs/reviews/INTEGRATION_REVIEW_2026-11-06.md` and, if needed, a new `tests/exsim/server/review/integrated_review_tests.cpp`. Review covers bounded persistence, failure/shutdown/lifetime correctness, canonical format, and HTTP comparison integrity.
- Tool wait timed out while the review continued; its completion wakes the parent automatically. No status polls or repeat builds.

## Integration finding repairs (2026-11-06)

- Completed review task `...delegate-task%3A64634ba1-67d0-4380-8ee1-3f601a64688e`, GPT-6.1 Sol/high, actual checkout `/Users/jedi/.t3/worktrees/lob/t3-99f6f810`, branch `t3/review-journal-comparison`, scoped report commit `0aa017f`, parent merge `0e7c24e`. It found IR-01/P1, IR-02/P2 and IR-03/P2 from source inspection, with no extra confirmed journal defect.
- Parent fixes use fresh simulation outputs for comparisons, bind native config/seed/identity to the API run, and remove reactor/normal CLI-exit stdio. Startup reporting remains outside the polling loop. Detailed behavior and limits: `docs/reviews/INTEGRATION_FIXES_2026-11-06.md`.
- All five added regressions fail against the reviewed pre-fix source in temporary binaries (`/tmp/exsim-ir-before-fix/{api-cache,api-provenance,runtime-log,compile}.log`); fixed focused API 37/37 and runtime review 7/7 pass (`/tmp/exsim-ir-{api-final,runtime}.log`).
- Current fixed source: macOS release 932/932 (`/tmp/exsim-ir-release.log`), ASan/UBSan 928/928 (`/tmp/exsim-ir-sanitizers.log`), real API binary 9/9/all23operations (`/tmp/exsim-ir-contract.log`). Both SIGINT and SIGTERM with full blocking stderr pipes exit 0 within a 1-second observation window (`/tmp/exsim-ir-cli-shutdown.log`). Server TSan 143/143 passed (`/tmp/exsim-ir-tsan.log`); scoped Linux GCC14.2 and Clang19.1 each passed 53/53 and built `exsim_api`/`exsim_server` (`/tmp/exsim-ir-linux.log`, source `edc13b0`, zero warnings/errors). No frontend source changed, so previous 59/type/lint/build checks remain applicable.

- All three confirmed integration findings are closed by parent fix `edc13b0` and the regressions above. The post-fix code has not received a separate second delegate review. No agents, builds or measurements remain active. Original FIX codec optimization checkpoints remain parked; browser interactions have not been independently exercised.

## Production demo and publication (2026-11-06)

- User requested a production server, a Tailscale frontend link, a recorded product walkthrough, README rewrite and GitHub push. Next.js standalone packaging is enabled; the release API and frontend run as separate user LaunchAgents (`com.jedinak.exsim.api`, `com.jedinak.exsim.frontend`). Built artifacts are copied into Application Support, with research data stored separately. Local-only service ports: API7600, frontend3760. Tailscale Serve exposes only frontend HTTPS10000 to the tailnet; pre-existing handlers were preserved. Deployment and lifecycle commands are in `docs/DEPLOYMENT.md`.
- Native preview snapshots timed out twice, and desktop browser control returned `cgWindowNotFound`. Parent used an isolated Playwright/Chromium session for actual UI interactions and video recording. The final walkthrough has eight scenes: experiment creation/assumptions, paused event stepping and resume, real charts/accounting, fill/event inspection, three-seed comparison, drill-down and manifest. It issued 57 API requests with no failed responses or browser exceptions. These are parent browser checks, not an independent UI review.
- The README is rewritten around the trader workflow and actual available surfaces. Historical performance notes moved to `docs/PERFORMANCE.md`; no new performance claim. Screenshots, MP4 and chapter captions are under `docs/demo/`. Build/capture logs: `/tmp/exsim-production-{api,web}-build.log`, `/tmp/exsim-product-demo.log`, `/tmp/exsim-product-demo-report.json`, `/tmp/exsim-video-encode.log`. Earlier unsuccessful recordings are not published.
- Published code, README and demo assets to `origin/exchange-simulator` in `549fa3a`; GitHub contents API verified the MP4 asset (2,587,213 bytes). This pushes the feature branch, not a merge to `main`. Production build, typecheck and lint passed after enabling standalone output. Final HTTPS frontend/proxy checks passed; both services remain running under LaunchAgents. Private run data, `.claude/` and `output/` are excluded from the change.

## Anthropic frontend-design redesign (2026-11-06)

- User requested installation and application of Anthropic's frontend skill. Installed official `anthropics/skills/skills/frontend-design` at `/Users/jedi/.codex/skills/frontend-design` using the skill-installer helper and read SKILL.md before implementation. It becomes automatically discoverable on subsequent turns; it was applied directly from the installed file here. Design plan, rejected direction and self-critique: `docs/UI_DESIGN.md`.
- Reworked the UI as a harbor-blue research rail with a light analysis canvas, self-hosted IBM Plex Sans/Mono, teal reference/bronze candidate charts, a searchable real-data study library, run-outcome strip, readable scenario labels, wider two-column forms, and a persistent keyboard event toolbar. Existing shadcn primitives, Bun/Next.js/TypeScript/Tailwind, exact decimal handling, run controls, comparison semantics and C++ behavior remain intact. Replaced the starter favicon with the exsim order-book mark.
- Verified 59 frontend tests, typecheck, lint, frozen lockfile installation and standalone production build. Actual browser checks cover seven desktop routes, library filtering and empty results, preview validation, keyboard event stepping and fill selection; library, run, comparison and create views have no document overflow at 390 px. Zero browser exceptions or failed API responses. Logs: `/tmp/exsim-redesign-{tests,types,lint,build,browser,live-browser,lockcheck}.log`.
- Updated the production frontend only, using copied bundle `production-20261106-workstation` and build `82moVAIlMwoCVVQmupwpq`. The API process and research data were not restarted or replaced. Loopback frontend3760 remains exposed through the existing private Tailscale HTTPS10000 endpoint. Source checkout and staging servers are not needed for the live service. Deployment commands and paths are in `docs/DEPLOYMENT.md`.
- Refreshed the public walkthrough and screenshots with the redesign. The final eight-scene recording issued 57 successful API requests, with no browser exceptions or API failures. H.264 MP4 decoded fully: 1,352 frames, 54.08 seconds. Media log `/tmp/exsim-redesign-video.log`; capture report `/tmp/exsim-product-demo-report.json`. GitHub publication uses the existing `exchange-simulator` branch, without a main-branch merge. Untracked `.claude/` and `output/` remain preserved.

- Final count-state repair: unavailable experiment data displays `n/a` rather than zero. An isolated browser 503 reproducer fails the initial redesign and passes on staging and the live endpoint (`/tmp/exsim-redesign-count-{before,after,live}.log`). Final frontend bundle `production-20261106-workstation-final`, build `7xqRXybDSlUBIS_ATWSBL`, is healthy through Tailscale; the API process remained running. The redesign and refreshed media were pushed in `19aed81`; this small count-state follow-up retains the same healthy-data visuals.

## Thorough product-quality continuation, 2026-11-07

The user requested continued implementation, explicitly authorized subagents and asked for the best result across the project. Shared task list and acceptance criteria: [quality/PLAN_2026-11-07.md](quality/PLAN_2026-11-07.md). Parent owns uncommitted frontend workflow/retry/pagination/proxy repairs and pending regression tests. No new release has been deployed or pushed. Prior live services remain untouched; `.claude/` and `output/` are preserved.

New GPT/xhigh delegates use harness-created isolated worktrees from dependent parent `6d5c29f`. Worktree paths and final commits will be supplied in completion reports. Build caps: focused `-j2`, no full suites, Docker, benchmarks or extra delegates. Parent integrates serially; independent review is a separate later task.

| Task | Harness task ID | Ownership and next action |
| --- | --- | --- |
| API durability | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3Ab5fbe611-8cd2-40dc-854e-c62784351e74` | Service persistence/idempotency/reload, focused failure/restart tests, report. Completion notification resumes parent. |
| Execution API | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A49036dd0-77f1-40d8-8239-2b5cc61ea1e7` | TWAP/immediate sweep strategy bridge, discovery, exact parent-order metrics, OpenAPI/types and focused integration checks. Parent implements the browser after contract integration. |
| Delivery hardening | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A106e2b06-0df8-4e28-8dfb-52a2ef0e22c9` | Packaging/readiness/consistent backup/restore/rollback in disposable directories, deployment docs and audit. No actual production operations. |

Parent pending: finish regression files for partial seed launches/lost replies, cursor paging, malformed errors, proxy boundaries, step controls and variant/inventory-aware configuration; rerun typecheck/lint after the latest edits, then production build and focused browser checks. `@playwright/test` 1.65.0 was added through Bun for durable interaction regressions; no browser test files or new test pass claims yet. Resume on child completion notifications rather than polling.


### Parent reliability and delivery checkpoint

- Delivery implementation `eca0f55` integrated as `7fbf6c0`; child evidence checkpoint `52f5d7d` read but not merged wholesale. Source checkout `/Users/jedi/.t3/worktrees/lob/t3-2e875d0f`. Parent disposable tool checks pass 40/40 in 5.596 s (`/tmp/exsim-quality-delivery-tests.log`). No production service or run data touched. DA-01/02 routed to the running durability task through a steer message.
- Parent frontend reliability implementation and evidence: [quality/FRONTEND_RELIABILITY_2026-11-07.md](quality/FRONTEND_RELIABILITY_2026-11-07.md). Verified 73/73 frontend invariants (3.01 s), typecheck, zero-warning lint, production standalone build, 6/6 isolated browser invariants (4.3 s). Logs `/tmp/exsim-quality-{web-tests,types,lint,build,browser}.log`. New API schema drift/frontend/real-contract/browser/delivery CI job is configured but has not run on GitHub. No new C++ full suite or benchmark was run.
- Next: commit this parent checkpoint, independently review its failure/retry/identity boundaries, then integrate durability/execution results and implement the execution UI against the actual returned contract. Keep all new product changes out of the live deployment until integrated gates pass.

- Parent frontend checkpoint committed `4595fa0` on `exchange-simulator`; delivery tools integrated `7fbf6c0`. Working tree otherwise contains only preserved untracked `.claude/` and `output/`. No push or deployment performed.
- Independent report-only frontend review launched from `4595fa0`: task `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3Affcf97cd-9e5f-4ef1-877d-e1c696444f7b`, GPT/xhigh, isolated harness worktree. Report target `docs/reviews/FRONTEND_RECOVERY_REVIEW_2026-11-07.md`. Focus: actual recovery/identity/freshness guarantees and adversarial failures, not styling. No production edits, heavy gates or child delegates. Resume on completion, fix findings on parent; do not poll. API durability and execution implementations remain outstanding.


### Execution, durability and frontend-review integration

- API durability `f6b0304` integrated as `795876d`; source checkout `/Users/jedi/.t3/worktrees/lob/t3-a3d1cea4`, evidence checkpoint `6c52fbc` read. API execution `944ef81` integrated as `bc07370`; checkout `/Users/jedi/.t3/worktrees/lob/t3-598ca8e2`, evidence checkpoint `aa5852f` read. No conflicting implementation hunks; no public durability schema change. New execution schema/types were integrated before the frontend forms.
- Independent frontend report `9769191` integrated as `c7c73cc`; checkout `/Users/jedi/.t3/worktrees/lob/t3-d6afb8a8`, evidence checkpoint `11cd5d2` read. FR-01 to FR-04 fixed on the parent with actual browser regressions. O1 replaced with a cancellable early-stop selected-fill search. Review report/reproducers remain unchanged; a new independent re-check is still required.
- Integrated capability-gated TWAP/sweep forms and execution summary/cost labels; browser verifies actual Next proxy -> isolated C++ execution/variant/comparison plus inventory-aware creation. Installed openapi helper type mapping dropped required null-only evidence; a minimal Bun patch and compile-time regression preserve the exact contract.
- Current checks: C++ API release64/ASan64, frontend76, real-binary contract9 (23 operations/284 assertions), browser13 (two native proxy flows), types/lint/build/frozen patched install and schema drift passed. Empty C++ test selection now fails (previously 0/0 success). Exact source boundaries, logs, runtimes and limitations: [quality/INTEGRATION_2026-11-07.md](quality/INTEGRATION_2026-11-07.md).
- Production remains unchanged. Next: commit this checkpoint; launch new independent backend review and frontend re-check plus serial focused Linux validation; resolve findings before packaging, offline backup and promotion. No full suite or benchmark was rerun. Do not downgrade the new idempotency store to an old writable API binary. Original active simulation state is not reconstructible and must not be destroyed by an automatic upgrade.

### Independent integration gates launched from 7ca1014

The integrated frontend/review-repair checkpoint is committed as `7ca1014`, with a clean tracked tree. Three new GPT/xhigh harness-isolated tasks are active; only the Linux task has the compiler slot. No new production release or push occurred. Completion notifications resume the parent; do not poll.

| Task | Harness task ID | Scope |
| --- | --- | --- |
| Integrated API review | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3Aec50c01c-4545-43cc-bcd4-9788f624b7f5` | Report-only durability, restart, executor errors, execution metrics/identity/native verification and OpenAPI. Report `docs/reviews/INTEGRATED_API_REVIEW_2026-11-07.md`. |
| Frontend independent re-check | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A064d0df2-8d57-4f6c-932c-13df7a3a7c3d` | New reviewer of FR-01..04/O1 repairs, execution forms/disclosures, capability/type boundaries and CI evidence. Report `docs/reviews/FRONTEND_EXECUTION_RECHECK_2026-11-07.md`. |
| Linux focused validation | `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A926e3cf2-2e6d-49c0-b157-640c064cb592` | Serial GCC then Clang, API64, exsim_api only, empty-filter exit and delivery40. Existing image, disposable container, 2 CPUs/-j2. Report `docs/quality/LINUX_EXECUTION_DURABILITY_2026-11-07.md`. |

Worktree paths and evidence commits follow in completion reports. Parent remaining: resolve review/portability findings, obtain independent clearance appropriate to any fixes, package and validate a copied release, check active-run quiescence, perform a consistent offline backup and restored-copy preflight, then promote and publish accurately. Native proxy/browser tests have closed their disposable services; installed API/frontend and private data remain untouched.

### Linux completion and copied-release preflight

- Linux validation task `926e3cf2` completed. Report/script source `a3283f0` integrated as `d1f46bb`; evidence checkpoint `924f56a` read. Checkout `/Users/jedi/.t3/worktrees/lob/t3-f2c04d3b`. Native Linux arm64 GCC14.2 and Clang19.1.7 each passed API64 (durability17/execution11), built exsim_api with -Werror and zero warnings, and rejected empty filters with exit2. Python delivery40 passed once. Container removed; compiler slot released. No portability fix needed. Exact archive SHA/compiler/filesystem/log boundaries are in [quality/LINUX_EXECUTION_DURABILITY_2026-11-07.md](quality/LINUX_EXECUTION_DURABILITY_2026-11-07.md).
- Prepared an unpromoted copied release at `$HOME/Library/Application Support/exsim/releases/production-20261107-candidate-7ca1014`. Declared API source `bc07370`, web source `7ca1014`, target Darwin-arm64, Node24.22.1. Production C++ source under src/include/tools is unchanged between those commits. Package and complete inventory verification passed (`/tmp/exsim-quality-candidate-{package,verify}.log`). There is no public/ directory; package correctly omits that optional input.
- Booted the copied API/frontend from a disposable working/data directory, with independent loopback ports and NODE_PATH empty. Readiness verified direct/proxied schema/capabilities, HTML and a packaged asset's bytes. All four strategy capabilities and a same-origin JSON TWAP preview passed through the copied Next proxy. Receipt `/tmp/exsim-quality-candidate-preflight.log`; transient process logs `/tmp/exsim-candidate-{api,frontend}.log`. Owned processes stopped before disposable data removal. No installed service, real experiment data, launchd configuration, Tailscale setting or GitHub branch was changed.
- The candidate is not live and is not independent release clearance. API review `ec50c01c` and frontend re-check `064d0df2` remain outstanding; continue from their completion notifications, without polling. Review fixes can invalidate this candidate and require a new copied release. Offline backup/restored-real-data preflight and active-run quiescence remain required before promotion.

### Frontend independent re-check and FER-01 repair

- Frontend re-check `064d0df2` completed in `/Users/jedi/.t3/worktrees/lob/t3-a4a5cb93`. Report/tests source `7a5d995` integrated as `abdf8a5`; evidence checkpoint `24fe51a` read. FR-01..04 passed its independent browser checks. It found FER-01: the selected-fill key omitted event identity and could show the prior sibling fill's evidence.
- Parent reproduced FER-01 against the old artifact, then added event position to the key and checked returned order/event identity before precedence. The unchanged independent browser reproducer now passes; three ordinary Chromium cases pass for committed/pending/off-page selection and cancellation. Six presentation/helper probes moved into normal test coverage (original review files preserved). Frontend82, types/lint/build pass. No C++ gate was rerun for this readonly UI fix. Exact logs, scopes and receipt: [quality/FILL_IDENTITY_REPAIR_2026-11-07.md](quality/FILL_IDENTITY_REPAIR_2026-11-07.md).
- Old copied candidate `production-20261107-candidate-7ca1014` is blocked from promotion by this finding; it must be replaced with a new reviewed frontend bundle. No installed service or data changed. Next: new independent narrow FER-01 re-check, await integrated API review `ec50c01c`, fix any findings, then recreate the candidate and proceed with consistent backup/restored-data preflight and promotion.


- FER-01 production repair committed `75d9960`. New independent narrow re-check launched from that source: `node:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A2381f721-7ac4-4b86-b44f-253466ec20db`, GPT/xhigh, harness isolation. Report target `docs/reviews/FILL_IDENTITY_RECHECK_2026-11-07.md`; use the new copied standalone build `slpULgNACN27FJgew-XPE`, not the old candidate. Scope includes current ledger/related events, same-order siblings, cancellation and paging. No production edit, new native/Next build or unrelated broad audit. Parent awaits this and API review `ec50c01c` via notifications.

## Trader UX and research critique planning, 2026-11-08

User requested a strict quant-trader review, including possible full UX replacement, with a plan and task list first. Plan: [TRADER_UX_REVIEW_PLAN_2026-11-08.md](reviews/TRADER_UX_REVIEW_PLAN_2026-11-08.md). T01-T02 complete: current product/release context read; execution, market-making, research-validation and first-use lenses defined. T03-T15 remain open, starting with an observed browser walkthrough of the identified build before source inspection. The proposed 80% unaided-use target has a predeclared capability denominator and critical-task gates; no usability pass is claimed.

Parent checkout `/Volumes/PortableSSD/Documents/programming/lob`, branch `exchange-simulator`. No delegates launched, no commit created, no tests/builds needed for planning, no product code/live services/research data changed. Preserved untracked `.claude/` and `output/`. Next: identify review build and dataset; capture current flow evidence, then evaluate persona tasks and produce prioritized findings, replacement flow and implementation backlog.

### Trader UX implementation authorized, 2026-11-08

User approved implementation. Parent owns library/navigation/experiment overview, integration and browser checks. Existing docs and source inspected; this is an expert/source review, not an unaided human test. Shared preview status reports unavailable; try preview_open before choosing fallback. Retain current visual tokens and exact API contracts. Frontend-design skill applied. Two GPT gpt-6.1-sol/high harness-worktree tasks launched from current committed parent, with no live operations or C++ changes:

- Setup task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A35cc6b57-ae4d-4eaa-843e-dcbbf7b4078b`: owns experiment-create-view and dedicated setup helpers/tests/report. Task-oriented execution/MM entry, progressive disclosure, timing units and preview/launch clarity. Parent library will link intent=execution and intent=market-making. Preserve from= variant semantics.
- Analytics task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A522dbbd4-80da-430d-a3d1-077010006798`: owns compare, run-detail, execution-summary and dedicated analytics helpers/tests/report. Outcome hierarchy, paired seeds/comparability and decision-relevant evidence charts. No changes to global CSS or parent-owned views.

Checkout paths, commits and check receipts follow in completion notifications. No polling. Next: parent implements library task entry and experiment decision overview; integrate child commits serially on completion, then build and focused browser/reliability checks. No product changes or release yet.

### Trader workflow parent integration checkpoint

Analytics source f1cd1d1 integrated as e4d5bb7 from `/Users/jedi/.t3/worktrees/lob/t3-686c35ac`; handoff 54418f8 read via report. Parent library now has task entry, ordered baseline/candidate selection and research-task filtering; experiment overview distinguishes completed seeds, active runs, interrupted runs and missing seeds. New browser cases cover both continuations. Frontend95 pass/5 pre-existing skips, type/lint/build pass; selected browser5 pass (4.7s). Native inventory-aware pass; TWAP/sweep initially stops at changed heading, restored in source pending final rebuild/rerun. Logs and evidence: reviews/TRADER_WORKFLOW_IMPLEMENTATION_2026-11-08.md. Setup delegate still outstanding, resume on completion only. No deployment or production data changes.

### Trader workflow final local implementation, 2026-11-08

Both delegates integrated: analytics f1cd1d1 -> e4d5bb7, setup 22f4091 -> ca9193a. Parent workflow checkpoint 83dc30d adds library/overview and the durable review plan. Setup checkout `/Users/jedi/.t3/worktrees/lob/t3-4af7b2d3`; analytics checkout `/Users/jedi/.t3/worktrees/lob/t3-686c35ac`. Parent final follow-up makes selected tasks readable, defaults durations to human units, and preserves character-by-character decimal drafts. No public API or C++ edits.

Final type/lint/build pass; frontend invariants103 pass/5 pre-existing skips before the small final input fix; final production-browser12 pass in 11.8s cover that fix and two native C++ execution/MM flows, paired comparison, recovery, variants, exact timing and mobile routes. Native TWAP/sweep now passes after stable heading restoration. Exact commands/scopes/logs and broader follow-up backlog: [reviews/TRADER_WORKFLOW_IMPLEMENTATION_2026-11-08.md](reviews/TRADER_WORKFLOW_IMPLEMENTATION_2026-11-08.md). No first-use 80% pass or fresh independent audit claimed. No delegates remain running. Verification used disposable native data/mock fixtures, and temporary owned services/tabs were closed. No deployment, push or changes to installed research data. Preserve interrupted worktrees, .claude/ and output/.

Next: human unaided-use validation and review of the local integrated changes; follow-up engine/API capabilities remain explicit in the report. Local implementation is complete within the documented workflow scope.

## Full persona review continuation, 2026-11-08

User explicitly requested continuing all non-human review/fix/recheck steps and preparing the human first-use step. Prior d61c329 scoped implementation is not full persona clearance. Three NEW independent GPT gpt-6.1-sol/high report/reproducer-only tasks use harness worktrees from d61c329. No polling; completion resumes parent.

| Persona | Task ID | Report / next action |
| --- | --- | --- |
| Execution quant | `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3Ae1e539b2-123d-42cd-9ff5-e3cc180ce296` | `docs/reviews/PERSONA_EXECUTION_2026-11-08.md`; >=3-seed native buy/sell TWAP/sweep, costs/completion/benchmark/fill evidence |
| Market-making quant | `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A5c6f9d9c-6f1e-4f1e-95d3-c4db7c030c3a` | `docs/reviews/PERSONA_MARKET_MAKING_2026-11-08.md`; quote-width/skew/latency paired native tasks and fill/own-order evidence |
| Research validation lead | `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A457dd519-1430-4019-a4ae-b7e9657217bc` | `docs/reviews/PERSONA_VALIDATION_2026-11-08.md`; paired eligibility/confounds/provenance/reproduce/export and capability map |

All review the SAME actual isolated native app: frontend http://127.0.0.1:45772, API http://127.0.0.1:45773, BUILD_ID `ZbCMVWqStD4QzLA8AwzWD`, frontend source d61c329 (build after final production edit; docs/tests commit followed). Native existing build/release/exsim_api is unchanged. Data path is stored at `/tmp/exsim-persona-review-data-path`; it is a fresh mktemp directory, never installed research data. Owned exec sessions API56194/frontend32557 remain live for reviewers, do not stop until audits finish. Initial port3773 was occupied; no process on it was changed, and owned frontend3772 was stopped before using45772/45773. Reviewers use own browser tabs and experiment prefixes EXEC-REVIEW, MM-REVIEW, VALIDATION-REVIEW. No builds/production edits/extra delegates in review tasks.

Parent next: first-use/accessibility inventory and coverage audit, prepare human task/scoring artifacts, integrate reviewer reports/reproducers serially, fix actionable findings, run focused regressions then commission NEW independent repair rechecks. Do not claim the process complete until pre-human steps and honest remaining-capability classification are recorded. Actual human evaluation stays pending.

### Execution persona findings received and repair launched

Independent report32940c8 integrated8b79820, source checkout `/Users/jedi/.t3/worktrees/lob/t3-ddf6da87`. EXEC-01 censored execution eligibility, EXEC-02 missing schedule decision attribution, EXEC-03 neutral metrics falsely counted as ties (all P1); EXEC-04 semantic field targeting and EXEC-05 comparison definitions (P2). Native browser and direct buy/sell probes supplied; no clearance. Other persona audits continue on unchanged review app45772/45773.

New GPT/high harness implementation task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A55f3cb90-5eb2-46d1-be20-5435d525b86e` owns backend/runner/API, OpenAPI/generated types and focused native tests for EXEC01-04. Sole native compiler slot, -j2, no broad suites/live operations. Parent owns frontend presentation/focus, new trace UI after schema integration, first-use/accessibility audit, final human packet and WORK_PLAN. Draft packet docs/reviews/HUMAN_FIRST_USE_TEST_2026-11-08.md has 20 predeclared capabilities, all-critical plus16/20 pass threshold; NOT ready or run.

Parent has uncommitted comparison definition/neutral-count presentation fixes and human-test draft. New schema remains exclusively backend delegate-owned; parent must integrate before dependent UI. Next completion: integrate remaining persona reports, fix their findings while backend repair proceeds; then focused gates and NEW independent rechecks. Do not stop at report or successful implementation launch.

### Research-validation persona received, repairs coordinated

Validation report b0cdbcbe integrated a43cb8b, checkout `/Users/jedi/.t3/worktrees/lob/t3-826df8a9`. VALIDATION01 exact executable provenance, VALIDATION02 complete export/reproduction handoff, VALIDATION03 attempt-level comparison eligibility/cohort pinning are P1; VALIDATION04 named experiment/comparison context in inspection is P2. Same-process rerun digests/idempotency/native seed bounds passed scoped probes; no full clearance.

Steered running backend repair55f3cb90 to cover validation01/02/03 because they share its owned API/run/OpenAPI files. Backend owns all native/schema work; parent must not concurrently edit those. Requirements: persisted actual executable/source identity with old-run unavailable handling, full native artifact package + integrity/verify handoff, per-attempt eligibility ledger and pinned selected-run receipts. No extra compiler task or parallel C++ ownership. New independent recheck still required after integration.

Parent uncommitted frontend work: comparison benchmark/quantity/participation definitions; neutral preference counts N/A; named experiment/config/strategy research context on run summary/inspection and baseline/candidate backlink preserved between them; narrower manifest reproduction promise; field focus on service preview validation issues. New tests cover comparison definitions/neutral semantics and browser context continuity. Type/lint and focused analytics tests pass at this checkpoint; final artifact/browser gates await backend schema and remaining MM review. Do not rebuild root .next while remaining reviewer is using it. Human packet remains DRAFT/blocked.

Fourth independent persona audit launched: first-use/accessibility, GPT/high, harness worktree; report `docs/reviews/PERSONA_FIRST_USE_2026-11-08.md`. Task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A0ea9a721-fe1d-4a60-bcbb-cee13ff88896`. Reviews unchanged served d61c329 native app45772/45773, isolated FIRSTUSE-REVIEW records only, no builds/production edits/extra delegates. Needed to cover original fourth lens before human validation. Initial browser observation precedes other persona findings; exact limits must be stated. Parent frontend repairs remain uncommitted and not served. Do not rebuild/stop original review app until MM and first-use audits finish. Next: audit completion notifications, integrate reports, fix and test findings, then NEW independent repair rechecks. Actual human test remains blocked.

### Market-making persona received, repair scope extended

MM report a19f46f integrated ce8f50a, source checkout `/Users/jedi/.t3/worktrees/lob/t3-73e61748`. Fifteen native runs plus independent baseline fill/accounting/markout reconciliation supplied. P1 MM01 replace requests/rejections/race timing absent in diagnostics, MM02 missing FIFO reset/priority-kept evidence; P2 MM03 selected fill vs final replacement-chain identity, MM04 eligible markout share denominators/conditional comparison, MM05 false neutral ties overlaps EXEC03. No P0 demonstrated; no repair clearance.

Steered backend55f3cb90 to add MM01/02/04 native/API projections with exact request/reason counts, replace transit/causal race evidence, all replacement/rest queue snapshots and eligible shares/per-share metrics; all shared native/schema files remain its exclusive ownership. Parent must integrate schema before dependent UI, and keep original review app unchanged for first-use reviewer.

Parent frontend now labels selected fill order/event/match separately from latest client-order chain totals/as-of event, displays stage quantities, names configured Cancel / replace path, clarifies markout exchange-mid formula and eligible-share weighting (including initial edge), labels new-order rejection counts and discloses supported versus deferred deterministic research stresses. Counts/new fields await backend integration. Fresh independent MM recheck required, including nonzero shares ahead after reset and priority-retaining reduction, exact505 replace reject example, sibling fill identity and markout share denominators. No P1 is marked fixed from UI labels alone.

Parent presentation checkpoint: typecheck and full frontend lint pass; focused trader-analytics + execution-presentation tests15 pass/62 assertions (1.66s), logs `/tmp/persona-repair-parent-{types,lint,tests}.log`. New context browser regression is written but not run yet; no fresh frontend build while fourth persona uses unchanged .next. Commit this as an implementation checkpoint only, not persona clearance. Native/API repair55f3cb90 and first-use0ea9a721 still run; do not poll. Next native completion triggers schema integration and dependent trace/export/eligibility/markout/replace UI; next first-use completion triggers its additional repairs. All P1 closures require rebuilt native evidence and NEW independent rechecks.

### Fourth persona completed after usage-limit recovery

First-use report7d218323 integrated71adeb7, checkout `/Users/jedi/.t3/worktrees/lob/t3-111b79c5`. Original first-use run failed at usage limit; actual child resumed automatically and committed report. FIRSTUSE01 censored/unavailable markout totals falsely counted as measured0/ties P1 is distinct from EXEC01 truncated parent windows. FIRSTUSE02 semantic field errors P2 overlaps EXEC04. FIRSTUSE03 selected explanation keyboard reachability and FIRSTUSE05 chart accessible names/sample alternative P2; FIRSTUSE04 Connections empty guidance P3. All four expert persona audits now complete, none supplies repair or human clearance.

Backend steer returned thread_not_sendable; required read confirmed original implementation55f3cb90 also failed at usage limit, checkout `/Users/jedi/.t3/worktrees/lob/t3-961565e2`. Resumed SAME child with mode auto, retaining unfinished native work and ownership; new continuation task ID in tool receipt. Added FIRSTUSE01 null/eligible-n semantics. Parent must record continuation ID and exact completion contract; no polling or provider substitution. Parent now implements remaining keyboard/chart/empty-state changes. Original disposable review pair can be stopped once no remaining audit depends on it, preserve data receipts.

### First-use presentation repair checkpoint

Resumed original native implementation via task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Athread-send%3Acdfab80a-4adb-4d4f-a600-7cdcab7c2523`, same child55f3cb90/checkout. Completion notification resumes parent; no polling. FIRSTUSE01 null/eligible-n fix explicitly added. Exact remaining findings/owners are tracked in [reviews/PERSONA_REPAIR_LEDGER_2026-11-08.md](reviews/PERSONA_REPAIR_LEDGER_2026-11-08.md).

Parent chart/accessibility/empty-state repairs typecheck/lint/build pass. Seven selected production-browser fixture cases pass4.8s, including new read/return focus, unique chart application names/exact sample tables, protocol guidance, linked field-error focus plus context continuity/paging/mobile. Logs `/tmp/firstuse-repair-{types,lint,build,browser}.log`. Original frontend review session32557 stopped after all reviews completed; no remaining audit uses root .next. Original API session56194 can now stop while preserving its disposable data directory. Installed services unchanged. No fresh independent repair clearance or human readiness claimed. Next: integrate native contract, finish dependent trace/export/cohort/markout/replace UI, run relevant native/browser gates then NEW independent persona repair reviews. Human packet stays DRAFT/blocked until those pass.
