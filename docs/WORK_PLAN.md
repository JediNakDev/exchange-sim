# Working state and implementation plan

Updated 2026-11-09. This is a recovery checkpoint, not live delegate status. Keep it concise; put detailed evidence in the relevant report or log.

## Current continuation queue

**Publication and main merge, 2026-11-09:** user authorized pushing and merging.
Source317ff66 is pushed on `exchange-simulator`; PR
https://github.com/JediNakDev/exsim/pull/3 targets main and remains watched.
GitHub reports the repository moved from `lob` to `exsim`; existing origin
redirects successfully. Personal/local files and separate demo media are
preserved outside the commit. No deployment or migration.

CI GCC release/debug jobs129014144973/129014144762 failed on an ignored pipe
write result under `-Werror=unused-result`. Parent now checks the complete
write in that durability probe and two equivalent runtime readiness probes;
child failure gets a distinct exit status. No assertions or production code
changed. Linux GCC14 fortified release/debug compilation of both files passes;
focused durability17/17 and runtime-review7/7 pass. Logs:
`output/process-architecture-2026-11-09/logs/exsim-pr3-gcc-*`.
Next: push this repair, follow existing CI notifications, and merge PR3 once
checks pass. No delegate or replacement watch is needed.

**PR3 PostgreSQL CI fixture repair, 2026-11-09:** job129014131122 on15434f5
passed26/27 tests; its API-suspension probe required600 simulated seconds to
complete within15 wall seconds. Confirmed actual worker independence: original
probe passes normally, and the workload completes with the API suspended under
0.25CPU when the diagnostic wait permits it (whole test46.956s). Production
code is unchanged. The committed fixture now uses60 simulated seconds with the
same15-second deadline, plus exact end-time/nonzero-event assertions and proof
that PostgreSQL stays running-state until the suspended API resumes.

Repaired focused test passes under0.25CPU (whole test5.894s). Full real
PostgreSQL18 suite passes27/27 in69.973s in a disposable init-enabled container;
all test containers/volumes removed. Initial combined test harness without an
init process mishandled adopted orphan processes; the init-enabled rerun is
the valid full-suite evidence. Logs retained under
`output/process-architecture-2026-11-09/logs/exsim-pr3-postgres-*`.
Next: push this fixture repair, follow the existing PR3 watch and merge once CI
passes. Earlier GCC repair144b34c is already pushed. No new watch or delegate.

**PR3 web CI repair, 2026-11-09:** PostgreSQL failure on144b34c is the same
old600s fixture already repaired by a151e24. Web job129015250568 passed29/32
browser cases. Parent repairs the three failures: CI now builds the independent
`exsim_research` verifier as well as the API; browser preparation fails early if
either binary is missing. The audit test retains actual unpack/reproduce and
zero-exit checks and adds an explicit verifier-launch assertion. Route-reset
coverage follows the loaded study name after summary navigation settles, then
opens Runs and recovery, preserving same-document and late-response assertions.
Connections coverage follows the documented unavailable-telemetry state and
built-in setup continuation. Remaining custom-strategy Coming soon copy was
removed from Connections, setup help and sidebar; no backend semantics changed.

Fresh production build and three scoped browser cases pass3/3 in9.3s; ordinary
frontend123/123 in4.19s, typecheck and scoped lint pass. UI_DESIGN records the
navigation/copy decisions. Test-tier guidance used scoped cases; full browser
coverage remains for CI. Logs:
`output/process-architecture-2026-11-09/logs/exsim-pr3-web-*`.
Next: push this web repair, follow the existing PR3 watch, fix any remaining
checks and merge once green. Do not create a replacement watch or delegate.

**PR3 journal-readiness CI repair, 2026-11-09:** GCC release job129015250526
on144b34c passed1033/1034 cases; only journal backpressure failed. Its child
signaled readiness before server construction, charging cold startup against
the300ms reactor deadline. Parent moves readiness after startup/pipe filling
(or after the supported nonregular-journal rejection), validates an actual R
byte, and contains unexpected child exceptions. The existing5s startup wait,
300ms reactor wait and exit-status assertions remain. A deliberate350ms startup
pause proves startup is excluded; an isolated old-readiness negative control
fails with that pause, while the repaired case passes. Production is unchanged.

Focused runtime-review7/7 passes in release and ASan/UBSan. Linux GCC14 fortified
release/debug compilation passes. Logs and negative-control source/binaries:
`output/process-architecture-2026-11-09/`, with
`logs/exsim-pr3-journal-ready-*` and `logs/exsim-pr3-gcc-release-144b34c.log`.
Next: push this repair, follow the existing PR3 watch and merge after CI passes.
Earlier fixes144b34c/a151e24/317ff66 are already pushed. No new watch/delegate.

**Trader/exchange architecture, completed 2026-11-09:**
User requested `exsim_apid`, `exsim_traderd`, `exsim_exchanged`, reusable
libraries, custom-strategy removal and Docker correction. Implemented by parent
in this shared checkout, without delegates. Shared sources/headers moved to
`lib/src` and `lib/include`, preserving pre-existing Pg/worker/UI work and
untracked data. Trader/exchange observations and actions now cross real UDS
IPC while retaining the four strategy classes and existing run timing. Daemon
entry points and worker runtimes live under their respective `src/` folders.
Research IPC uses explicit research framing, not OUCH/ITCH; standard codecs
remain reusable libraries. Custom setup and Coming soon entries removed; no
backend custom loader existed. API/exchange/trader executable provenance,
release packaging, Docker images, runbooks and generated API types updated.

Final: warning-free release all-target build; process7/7 (all four strategies,
two seeds, byte-identical artifacts), wire3/3, API99/99, research68/68,
allocation26/26; focused ASan/UBSan10/10 (allocation counting disabled there).
Real HTTP integration proves three PIDs, pause/step/resume, cold inspection
and trader-crash failure with API availability. HTTP contract9/9/all23 ops,
Bun129pass/5existing skips, types/scoped lint, delivery42/42, credentials4/4.
Both Docker images build; real disposable Compose/PostgreSQL check passes all
three daemon names, controls, artifacts and exact persistence after recreation.
A60s TWAP seed42 independently reproduces. Fixed a missing review-test mock
export without altering its assertions. Receipt and precise limits:
[PROCESS_ARCHITECTURE_2026-11-09.md](quality/PROCESS_ARCHITECTURE_2026-11-09.md).
Logs/artifacts/image IDs: `output/process-architecture-2026-11-09/`.
No commit, push, deployment promotion or data migration. Disposable test stacks
cleaned; existing installed/dev services and data preserved. No required work
remains for this scoped process split. Next: review/commit or explicitly roll
out the updated build when requested.

**Docker Compose, completed 2026-11-09:** user requested Docker deployment.
Added production API/web Dockerfiles (existing development Dockerfile retained),
`compose.yaml` with PostgreSQL18, private API/DB, non-root apps, a non-superuser
DB role, named database/artifact volumes and local worker processes inside the
API container. Atomic credential helper and build-context exclusions added.
Both images build; real disposable Compose check passes frontend HTML/assets,
proxy, native worker/controls/results and exact persistence/replay after full
container recreation. Credential helper4/4 and config validation pass. GPT/high
read-only QA `/root/compose_review` complete, no blockers; parent fixed its
helper-generation robustness note. Receipt/logs/image IDs:
[DOCKER_COMPOSE_2026-11-09.md](quality/DOCKER_COMPOSE_2026-11-09.md).
Test projects/volumes removed; images and ignored local credentials retained.
Host PostgreSQL/installed services/data and concurrent UI work preserved. No
commit, image push or deployment promotion. Startup:
`sh docker/create-secrets.sh` then `docker compose up -d --build --wait`, frontend
http://localhost:3761. Runbook [DOCKER.md](architecture/DOCKER.md). Next: user can
start the stack; explicit migration/rollout remains separate from these checks.

**Worker separation, 2026-11-08:** user requested worker isolation with no IPC on
the hot path. Implemented on top of the uncommitted PostgreSQL milestone in this
parent checkout: default CLI process-per-run execution, local control/snapshot
RPC only, sole API metadata writer, cold query workers, durable launch
reservation, inherited root ownership fence, crash/timeout recovery and partial
cache preservation. No matching/gateway event-path IPC was added. The API can be
suspended while a worker completes. Receipt:
[WORKER_SEPARATION_2026-11-08.md](quality/WORKER_SEPARATION_2026-11-08.md).
Read-only GPT/high QA task `/root/worker_review` completed; parent fixed its
file-mode persistence, child provenance and late-reply findings and added
regressions. Final macOS real-Pg27/27 release +27/27 ASan/UBSan, nativeAPI99/99,
allocation25/25, HTTP contract9/9/all23 operations, delivery41/41. Linux GCC
`-Werror` API99/99/allocation25/25 and actual worker/query process smoke pass.
Exact logs in receipt. No deployment, migration, commit or push. Task-owned
validation APIs/containers stopped, temporary DBs cleaned, concurrent UI work
preserved. API death still fails active workers; independent supervisors,
distributed leases, retries and multi-user operations remain future scope.
Next: review/commit local changes and plan an explicit verified rollout when
requested. The older PostgreSQL-only handoff below is historical.

**PostgreSQL architecture milestone, 2026-11-08:** user accepted the production
direction and selected local artifacts for now. First milestone implemented,
uncommitted in this parent checkout: PostgreSQL experiment/run/idempotency
metadata, atomic creation/replay response, queued restart recovery, explicit
non-destructive file import, authoritative audit export, exclusive DB/artifact
ownership, and development/CI wiring. `exsim_local` initialized against
`output/postgres-local`; bootstrap stopped. Existing installed services and data
remain untouched. Receipt: [POSTGRES_METADATA_2026-11-08.md](quality/POSTGRES_METADATA_2026-11-08.md).
Read-only GPT/high QA `/root/postgres_review`, shared checkout, completed; parent
fixed its provenance/audit/marker findings. Release and ASan/UBSan integration
15/15 each, existing release API96/96, delivery41/41, frontend types/scoped lint,
PostgreSQL release/debug and file-only builds pass. Exact logs in receipt.
No commits, push or deployment in this milestone. Next: isolate C++ run workers
with durable control acknowledgments, leases/attempt ownership and crash
recovery while preserving pause/step/inspection. Authentication, workspaces,
quotas and operations follow before remote multi-user use. No claim of complete
production architecture or worker integration.

**Current user handoff, 2026-11-08:** trader workflow overhaul and all reported persona findings are implemented and independently closed within the recorded scopes. Final product pair: frontend75cf28f/native360ad69, Next`efg6fmC6FKFd5gxu0E1Hb`, Node24.22.1. Durable exact release `output/releases/human-review-75cf28f`. Fresh empty participant app http://127.0.0.1:46072/experiments, data/logs `output/human-participant-01-75cf28f`, supervisor30473, owning tool session74383. Native API46073. Only the actual human evaluation remains; results NOT RUN. See [human packet](reviews/HUMAN_FIRST_USE_TEST_2026-11-08.md), separate [facilitator key](reviews/HUMAN_FACILITATOR_KEY_2026-11-08.md), and [repair ledger](reviews/PERSONA_REPAIR_LEDGER_2026-11-08.md). No running delegates. Expert/reference processes stopped with all data retained; installed production remains unchanged. Historical deployed pair below is not this local human-test release.

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

### Native repairs integrated, frontend contract work launched

Native implementation db43a42 integrated5394b84; contract/check report9fee4cc integrated50d5ed4. Backend checkout `/Users/jedi/.t3/worktrees/lob/t3-961565e2` released ownership/compiler slot. Focused implementation receipts API77 release/ASan, runner11, HTTP9/all23,344 schema-valid responses, complete7715-event package integrity/native verify; NOT independent clearance. Exact schema/limits in reviews/EXECUTION_PERSONA_REPAIR_2026-11-08.md. Parent TypeScript labels/directions/error mappings/nullable markout test consumer updates committed; typecheck passes.

Two GPT/high harness UI implementation tasks now cover execution-trace-panel (new component/helpers/tests only) and MM diagnostics (inspector, trader-run-outcomes, new request diagnostics). Exact task IDs/checkouts follow tool receipts/completion reports. Parent owns run-detail mounting, provenance/export/rerun, comparison ledger/pinned selection/conditional markout evidence, fixtures and final native/browser checks. No parallel native code changes. Trace lifecycle deep links use ?order=exact exchange ID, MM task adds support. No task has fresh independent clearance. After UI commits integrate, build isolated new pair, native/browser regression then NEW independent persona rechecks. Human remains DRAFT.

UI task IDs: execution trace `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A2343afd9-9558-4099-bf8a-cea5bd8ef075`; MM diagnostics `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A4a96b413-a863-46ca-9e0b-52cef315c836`. Parent consumer commit b707c87. Resume parent comparison/export/provenance UI on child completion, do not poll or claim native implementation is integrated feature before these paths and independent rechecks exist.

### Parent native evidence UI integration checkpoint

Execution trace UI bef0408 integratede2ff8c0 from `/Users/jedi/.t3/worktrees/lob/t3-daf09b37`,14 focused tests/type/lint implementation receipt. Parent mounts trace for execution runs. Parent added comparison per-attempt eligibility ledger, selected digests and censoring reasons, pinned runSelection URL and downloadable receipt; conditional markout coverage for selected attempts with eligible shares/fills and USD/share; exact executable provenance in manifest; repeat-same-seed action; complete browser audit download bounded32MiB, all SHA256/size/offset identities verified before saving, local streaming fallback and unpack/verify/reproduce commands. `scripts/research_audit.py unpack` validates browser packages before creating a new native artifact directory. Tests cover corrupt/missing/unsafe/oversized downloads and >1000-row unpack.

Native binaries rebuilt at parent with -j2/-Werror, log `/tmp/persona-integrated-native-build.log`. Type/lint/build pass; frontend117 pass/5 pre-existing skips/0 fail at this checkpoint (`/tmp/persona-integrated-{types,lint,tests,web-build}.log`). Initial mock all-operation guard exposed25 versus old23 operations; added explicit409 evidence_unavailable handling/tests for native-only trace/export endpoints, preserving every-operation coverage instead of fabricating mock native artifacts.

Selected browser batch5 pass plus new native handoff probe initially failed because test-generated idempotency key included a period. Corrected fixture key and retained response diagnostics. New native handoff probe passes1 in6.1s, including censored-parent exclusion; pinned old attempt after retry; downloaded comparison receipt; native trace; source provenance; browser complete audit with >1000 native events -> Python unpack/integrity -> exsim_research verify; completely unobserved5s markout n0/null/no ties and UI n/a. Logs `/tmp/persona-integrated-browser.log` (initial5pass/1fixturefail) and `/tmp/persona-integrated-new-browser.log` (fixed native1pass). Python unpack2pass `/tmp/persona-audit-unpack-tests.log`. Additional eligible-share summary column added after build, awaits next build with MM integration.

MM UI task4a96b413 remains outstanding; no polling. Parent UI/native semantics are not independently cleared. Next: integrate MM UI, mount its diagnostics; final checks/new copied native review pair; NEW independent rechecks covering all persona findings and complete export/native reproduction before human packet readiness. Original review data preserved; no installed service/data changes.

NEW independent native persona repair recheck launched from8e5cb45: GPT/high harness review, report `docs/reviews/NATIVE_PERSONA_RECHECK_2026-11-08.md`. Native API/CLI/provenance/export/cohort/trace/MM evidence scope only; no frontend/human clearance. Task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A5e5ff212-346b-40e0-ad5c-0a0655441f13`. Independent isolated processes/data and parent binaries permitted, sole native compiler slot only if cheap focused/crossbuild checks needed, no parent binary/Next rebuild. Parent can continue frontend integration; do not poll reviewer. Final frontend repair recheck remains separate after MM UI and final artifact build.

### Final MM integration and parent-detected race repair

MM UI1d7427d integratedd057581 from `/Users/jedi/.t3/worktrees/lob/t3-91b14a38`; diagnostics mounted in summary and selected-attempt comparison details. Original MM renderer26test receipt is implementation only. Parent added absolute per-seed execution filled/residual/completion/cost evidence, eligible-share summary column and same-route order-link reset/regression. Final frontend source type/lint/build passed; frontend124pass/5pre-existing skips/0fail,13320assertions4.24s (`/tmp/persona-final-{types,lint,web-build,tests}.log`).

Selected browser batch16pass/1fail (23s) exposed REAL native MM01 projection defect, not a label/test failure: submittedorder7 filled#69 between replace#48 and arrival#104, but arrival.order_id0 made write_replace_transits miss causal fill. Parent fixc2a412d matches against NONZERO submitted exchange ID. Strengthened regression uses ID0 arrivals and trade-leg fill identity;1/1 passes, affected execution_api26/26 passes. Separate clone-on-write build/persona-race was built -j2/-Werror, leaving root build/release API/CLI unchanged for original independent reviewer5e5ff212. Native compiler slot released to reviewer. Logs `/tmp/persona-race-{build,test,api-tests}.log`.

Fixed native browser2/2 pass7.4s (`/tmp/persona-fixed-native-browser.log`): complete audit->unpack->nativeverify, pinned retries/censor/unobservedmarkouts; MM505replace/3new/0cancel, actualcausalrace, selectedorder7event45 versus latestchain12, post-replaceFIFO#51,135665eligible shares and comparison diagnostics. Browser fixed binary is build/persona-race/exsim_api with explicit sourcec2a412d. Original browser log `/tmp/persona-final-browser.log` remains failure evidence; no claim of combined17pass rerun. New order-query reset test passed in that16pass subset. API original reviewer was notified of defect and fix without asking it to change/recheck its original review scope; fresh corrected-build independent check remains required.

Next: freeze copied final native/frontend pair, launch NEW independent browser/persona repair rechecks on corrected pair; integrate original native review findings and fix/recheck anything else. Human environment and facilitator packet stay unready until all gates cleared. No production change.

### Independent native recheck objections and R2/R3 repairs

Native reviewe1adc829 integrated3a4d232 from `/Users/jedi/.t3/worktrees/lob/t3-ff6e6207`. Request changes: NATIVE-R1 actual fill-before-replace omitted on original5394b84 (parent c2a412d fixes but unreviewed); NATIVE-R2 CLI still admits censored parent windows; NATIVE-R3 corrupt partial-trace acceptance depends on requested page. Independent audit export/native reproduction, actual distinct-build provenance and restart pinning passed scoped probes. No full clearance.

Parent repaired R2 in native load/admission: derive execution observation status from canonical manifest start/duration and recorded final_time_ns, checked deadline arithmetic; exclude censored or unavailable-window evidence, preserve fully observed partial parents. R3 now parses/validates every scanned row before page selection, enforces native scalar/quantity/enum types and retains only bounded requested rows. Regression exercises limit1 and1000 corruption rejection, restoration success and CLI censor rejection on both sides with full-window partial self-pair retained.

First mixed-object run crashed with signal138: copy-on-write build's .d targets still pointed at build/release, so changed LoadedRun ABI did not rebuild run_verification.o. Corrected ONLY build/persona-race .d target prefixes; rebuilt affected objects. Focused execution26/26 and runner11/11 then pass. Logs `/tmp/persona-native-r23-{rebuild,api,runner}.log`; initial failed log superseded by this correctly rebuilt run, no crash clearance from merely relaunching. No root release binary/production process changed.

Old copied review release `/tmp/exsim-persona-review-bc089ea` remains superseded, not human ready. Attempts to close old exec handles returned unknown-session; targeted lsof confirms no45872/45873 listeners. Old disposable data path preserved. Next: commit these native fixes, package a NEW immutable pair, fresh native/browser rechecks (new tasks) on that exact pair; prepare human answer key/environment only after independent closure. Implementation-thread usage-limit message after its committed handoff is not evidence of lost source or a new repair pass.

### Corrected native pair for final independent rechecks

Committed360ad69 native R2/R3 fixes plus docs; c2a412d race fix included. New copied artifact `/tmp/exsim-persona-review-360ad69`, API source360ad69529aa57b5e8856391812cc3d1d93c6896, frontend sourcebc089ea6eba59497dcf0b5034ec89ef2ffe354e4, NextQ-wBK50aSIFE_xNPhvB0e/Node24.22.1. Package/readiness pass `/tmp/persona-r23-{package,readiness}.log`. Native browser2/2 pass7.6s `/tmp/persona-r23-native-browser.log`; focused API26/runner11 pass from coherent alternate build. Old copiedbc089ea retained as superseded; its listeners absent.

NEW shared corrected review service frontend45872/API45873, owned sessions16243/9709, fresh data pointer `/tmp/exsim-r23-persona-data-path`, explicit source360ad69/releasepersona-review-360ad69. Preserve this pair unchanged during fresh expert reviews; no installed services/data touched. Draft facilitator answer key `reviews/HUMAN_FACILITATOR_KEY_2026-11-08.md` states exact semantics,20-capability scoring and environment separation, remains BLOCKED/NOT RUN. Final human environment/reference receipts follow independent closure.

### Fresh corrected-build independent gates launched

Three NEW GPT/high harness recheck tasks launched from360ad69: (1) native R1/R2/R3 and actual-scenario gaps, report NATIVE_PERSONA_FINAL_RECHECK_2026-11-08.md; (2) execution/MM native browser repair recheck, report TRADING_BROWSER_FINAL_RECHECK_2026-11-08.md; (3) validation/first-use/accessibility browser repair recheck, report VALIDATION_FIRSTUSE_FINAL_RECHECK_2026-11-08.md. Tool receipts supply exact IDs below. No polling. Shared copied review pair45872/45873 stays fixed at release360ad69; browser prefixesTRADING-RECHECK/VALIDATION-RECHECK, native reviewer owns separate ephemeral processes. Only native reviewer has -j2 compiler slot if required, no parent outputs. All owned processes/tabs close on handoff; parent services remain for reviewers.

Prior original review gave request-changes, not clearance; these NEW tasks evaluate actual repaired artifact and remaining actual-zero/nonzeroFIFO scenarios. Human packet/key remainDRAFT, production untouched. Parent next: consolidate completion findings, fix small confirmed issues, commission NEW scoped rechecks as needed, then freeze separate fresh human environment/reference receipts and stop before human evaluation.

Exact fresh task IDs: native `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3Ad9013f48-05f1-4a70-9acf-79592e315a37`; trading browser `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3Aadf0e599-c49d-41b1-a01b-d6c9fcc399dc`; validation/first-use browser `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A8d5c7369-9f0d-4c73-9ca5-8858ae085635`. Checkout/commits/checks follow completions. Native actual-scenario nonzero FIFO/kept reduction and measured-zero markout gaps are explicitly in scope, not silently closed by original helper tests.

### Trading browser closure integrated, human session tooling prepared

Trading recheckfa9a210f integratedddb3e20 from `/Users/jedi/.t3/worktrees/lob/t3-fb1fa7af`. Independent176 exact reconciliations; EXEC01-05 browser/API and MM01/03/04/05 closed within exercised scope, corrected fill69 race verified. MM02 actual nonzero reset/kept reduction/truncation explicitly remains untested here and awaits native reviewer d9013f48; do not call all-MM cleared. Native and validation/accessibility reviews remain running, no polling. Repair ledger updated with scoped closures, no product changes.

Prepared `scripts/start_human_review.py` to supervise a verified copied release on default loopback46072/46073 with a NEW participant directory, exact Node/version/inventory readiness, retained logs/identities/data, owned-only shutdown and human_result NOT RUN. Actual smoke passed using separate45982/45983 ports, fresh empty data, direct/proxy readiness, graceful stop and data preservation. Receipt `/var/folders/99/cjkytqh130n5xq3vzsr3f17c0000gn/T/exsim-human-launcher-smoke-3y91c9xq/smoke-receipt.json`. Does not touch shared expert45872/45873 or production. Participant packet contains draft command and remainsblocked; final reference runs/freeze and actualhuman step await native + validation/first-use independent closure.

### Corrected native pass and three remaining presentation repairs

Native final review1bc51987 integrated3f09ab0 from `/Users/jedi/.t3/worktrees/lob/t3-6baeba53`: R1/R2/R3 closed,27 corruption rejections, actual engine replacement behind30 shares/kept reduction, actual measured-zero versus unobserved comparison, complete export/native verify. No future frontend/human clearance. Validation/first-use1e3ad078 integratedb749809 from `/Users/jedi/.t3/worktrees/lob/t3-fa5a42aa`: original core repairs pass within scope but three new objections remain: unavailable deadline label P2,390px Summary width967 P2, stale row-return/status after paging P3.

Parent fixes: label unavailable deadlines independently from explicit not_applicable; constrain Summary to a single minmax column below xl; clear row origin when paging/changing artifact tab and describe actual list fallback. Focused native browser2/2 pass4.9s, including390px Summary native diagnostics width390 and real MM fill paging/status/focus. Typecheck/scoped lint/production build pass, logs `/tmp/firstuse-final-repair-{types,lint,build,browser}.log`. Need NEW immutable frontend package with unchanged independently reviewed API360ad69 and a NEW affected-scope browser review. Human packet remainsDRAFT/NOT RUN. No production changes or native compiler work.

### Final affected-scope presentation recheck launched

New copied release `/tmp/exsim-persona-review-75cf28f`: native360ad69 unchanged, frontend75cf28fcdbee8e82bd71ba20264604ff9dfdac15, Nextefg6fmC6FKFd5gxu0E1Hb, Node24.22.1. Package/readiness pass logs `/tmp/firstuse-final-repair-{package,readiness}.log`. Fresh frontend45882 owned session40913 proxies unchanged expertAPI45873. Original45872 retained. Keep all three unchanged while review runs. NEW GPT/high harness review task `node:delegated-task:command%3Amcp%3A961726a6-a279-4e3d-8735-7951cbaa0f6b%3Adelegate-task%3A80e78434-1e66-41e4-8b9a-e2cddaf7526f` owns PRESENTATION_FINAL_RECHECK_2026-11-08.md, three small findings plus adjacent paging/tab/desktop/MM behavior only. Initial rejected optioneffort caused no child; capabilityrefresh confirmed reasoningEfforthigh and successful newtask. No polling. Next completion: integrate/fix/recheck if required, then final facilitator reference scenarios and clean participant session. Human remainsblocked/NOT RUN.

### Final presentation pass and last human step prepared

Independent presentation review141e73ab integrated0ca8bfe from `/Users/jedi/.t3/worktrees/lob/t3-da213789`, task80e78434, closes R1/R2/R3 on frontend75cf28f/native360ad69.53 assertions,1490 exact package files/23 served scripts, zero page errors/HTTP failures. Native1bc51987/tradingfa9a210f/validation1e3ad078 evidence remains scoped, with later presentation objections now closed. All original reported findings closed; coverage limits remain explicit, not blanket certification.

Froze exact delivery SHA2562c1dee66607947a282313f451b5b5827ad61e35a3132846f75fd6023f00f6d1c, API10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5, native360ad69/web75cf28f. Verified temporary artifact then exact durable copy `output/releases/human-review-75cf28f` (44MiB, untracked output preserved). No product source changed after independently reviewed frontend75cf28f. Final readiness direct/proxy/HTML/static and empty participant library pass `/tmp/human-final-readiness.log`; durable inventory `/tmp/human-durable-release-verify.log`.

Final facilitator references regenerated AFTER independent closure on separate45992/45993 data:20 completed study runs covering TWAP/sweep/capped/censored and MM/width/inventory/cancel-latency. Complete manifests/summaries/comparisons/trace/lifecycle receipts and scripts in `reviews/HUMAN_REFERENCE_2026-11-08/`. Parent start unit roundtrip and interrupted-after-acceptance Step retry passed with same key and exactly100 events, then actual Resume completed, no page errors. These are preparation checks, not human results. One preparer draft incorrectly read provenance at manifest top level; fixed to documented nested provenance and regenerated from a NEW empty reference session. Old failed/abandoned reference data retained, not counted as passing. Reference control fixture helper exercised: paused run, deliberate bound failure and seed602 unlaunched, stable operation keys. It has NOT populated participant data.

Facilitator launcher initially raced API startup because only HTML readiness was awaited. Fixed to await API and frontend before full identity/readiness; then actual isolated reference and participant sessions pass. Added supervisor PID receipt for owned stop instructions. Python compile/Node syntax/diff checks pass, no product rebuild needed. Expert45872/45873/45882 and reference45992/45993 services stopped; preserve their data. Old temporary empty participant session also stopped without human activity. Final durable NEW participant session `output/human-participant-01-75cf28f` running frontend46072/API46073, supervisor30473/tool74383, verified empty and independently isolated. Both launcher and release inventory checks pass; current source only docs/facilitator tools differ from reviewed product.

Packet/key frozen READY FOR HUMAN EVALUATION.20-capability denominator fixed, >=16/20 plus every critical unaided/correct, no coaching; timing separately measured. Controlled network-outage and failed/paused fixture provisioning explicit, fixture preparation is never participant success. Supplied paused state is not successful human Pause. Screen reader/real device/huge export/actual legacy migration/full independent rebuild/new500-transition truncation remain declared coverage limits. Actual human results NOT RUN. Stop here per user request, give participant URL and task packet; after real human observations fix/recheck any newly demonstrated defect. No production deployment, external publication or installed-data changes.

## Backend simplicity and efficiency refactor, 2026-11-08

User requests plan first, Opus implementation, then parent code review through merge readiness. Explicit current Opus selection supersedes historical GPT-only preference for this task. Parent baseline `52d5724`, current branch `ui-green-design-system` (historical work-plan branch is stale). Existing uncommitted web styling and untracked files are unrelated and must be preserved. No deployment or running-service changes.

Plan: refactor the HTTP serving backend (`http_message`, `http_server`, focused tests) to avoid repeated header decoding while receiving fragmented bodies and repeated front erases of pipelined buffers; use direct bounded storage and safe socket/thread lifetime handling. Preserve HTTP validation, errors, limits, keep-alive and research semantics. Retain blocking per-connection architecture; no framework, generic allocator, reactor rewrite or speculative cache. HTTP remains control plane, not an allocation-free matching path. Review relevant server/network hot-path allocation checks without changing matching semantics.

Acceptance: focused HTTP release and ASan/UBSan tests, fragmented body/pipeline correctness, early malformed/oversize rejection, connection capacity/reuse, repeated start/stop and failed-start cleanup. Demonstrate eliminated repeated parse/allocation work with a focused regression or bounded measurement; do not infer speedups from style. Parent reviews ownership, concurrency, arithmetic, invalidation, limits and maintainability before integration. Task ID, worktree, commits, logs and final findings follow below.

Opus 5.5/high implementation launched in a harness-isolated worktree, task `node:delegated-task:command%3Amcp%3A41881c90-3064-4d46-92b2-66ceec46e41c%3Adelegate-task%3A56d5e115-1140-46c3-b59e-fc056ad12359`. Scope ownership is HTTP message/server sources and headers, focused HTTP tests, and `docs/quality/BACKEND_HTTP_REFACTOR_2026-11-08.md`. Checkout path and implementation commits pending completion. Next: completion notification, parent diff review, repair findings, focused validation and integration. No polling.

### Scope correction: whole exchange simulator and limit order book

User clarified full native simulator/LOB scope, not only HTTP. The HTTP task continues as one bounded workstream. Expanded plan: [EXCHANGE_REFACTOR_PLAN_2026-11-08.md](quality/EXCHANGE_REFACTOR_PLAN_2026-11-08.md). Queue: core/LOB/storage; protocol/runtime; simulation/research/non-HTTP API; HTTP; parent review and integrated validation. Every workstream requires explicit closure before claiming whole-scope readiness. Preserve concurrent frontend changes on `ui-green-design-system`. Additional Opus workstreams and their receipts follow.

Additional Opus 5.5/high harness tasks launched from local plan commit `c49bf79`:

- Core/LOB/storage: `node:delegated-task:command%3Amcp%3A41881c90-3064-4d46-92b2-66ceec46e41c%3Adelegate-task%3A7d6be364-445b-439a-b0cd-975d33ba974e`; owns core/legacy storage, engines and associated tests, report `quality/CORE_LOB_REFACTOR_2026-11-08.md`.
- Protocol/runtime: `node:delegated-task:command%3Amcp%3A41881c90-3064-4d46-92b2-66ceec46e41c%3Adelegate-task%3Ac323d94a-1ba1-4b33-bbcd-01bcd7b0dfc5`; owns protocol/gateway/net/md/server/client trees and related tests/tools, report `quality/RUNTIME_PROTOCOL_REFACTOR_2026-11-08.md`.

HTTP worker informed this is now one part of broader scope, with unchanged ownership. All workers limited to `-j1` future builds. Research/simulation/non-HTTP API queued for a free slot, not silently omitted. Checkout paths and implementation SHAs pending receipts. Follow completion notifications without polling; parent reviews returned diffs, schedules remaining workstream, repairs and integrates.
### HTTP refactor implementation (Opus delegate), 2026-11-08

Owner: Opus implementation delegate, isolated worktree `/Users/jedi/.t3/worktrees/lob/t3-e5925115`, branch `t3/e5925115`, from `4200598`. Owned files: `include/exsim/api/http_{message,server}.hpp`, `src/exsim/api/http_{message,server}.cpp`, `tests/exsim/api/http_tests.cpp`, `docs/quality/BACKEND_HTTP_REFACTOR_2026-11-08.md`. Work: head parsed once per request, offset-based pipeline consumption, lock-free bounded connection slots, and start/stop/thread/exception lifecycle repairs. Checks (`-j1`): release HTTP 17/17 (20 repeats clean), API consumers, existing allocation guards, and ASan/UBSan HTTP and API. Baseline comparison: the allocation regression measures 173 vs 13, and the baseline aborts on duplicate start and on a non-standard handler exception. Report: [BACKEND_HTTP_REFACTOR_2026-11-08.md](quality/BACKEND_HTTP_REFACTOR_2026-11-08.md). This is HTTP evidence only, not whole-system clearance. Next: parent code review and integration.


### Parent HTTP review complete, broader refactor continues

HTTP `bac7ec9` from `/Users/jedi/.t3/worktrees/lob/t3-e5925115` integrated as `9254238`, resolving only append-only WORK_PLAN conflict and preserving concurrent UI work. Parent fixed missed bad_alloc handling in connection-thread construction and kept existing HttpParseResult aggregate field order. Parent release and ASan/UBSan focused builds warning-free, HTTP17/17 each; exact logs `/tmp/exsim-parent-http-{release,debug}-{build,tests}.log`. Failure injection/TSan not run. See report parent-review section. Core/runtime still pending completion; start simulation/research workstream next, then integrated validation before whole-scope readiness.

Research/simulation/non-HTTP API Opus5.5/high task launched from parent `67bb05b`: `node:delegated-task:command%3Amcp%3A41881c90-3064-4d46-92b2-66ceec46e41c%3Adelegate-task%3A4587f4c2-941b-421e-bbbd-2915c324dfd4`. Harness-isolated checkout path pending receipt. Owns research/sim/non-HTTP API trees and corresponding tools/tests; report `quality/RESEARCH_SIM_REFACTOR_2026-11-08.md`. No HTTP/core/runtime ownership overlap. Builds -j1. Next: completion-driven parent review/integration for all three remaining workstreams; HTTP reviewed and integrated through `67bb05b`, broader clearance pending.

### Runtime partial handoff, implementation continues

Runtime first completion was incomplete: no commit and sanitizer build pending. Read actual child thread record once in response to that notification: ordinal2 already running in `/Users/jedi/.t3/worktrees/lob/t3-6cc707be`, branch `t3/refactor-runtime-protocol`. Continuation task `node:delegated-task:provider-continuation%3Amessage%3Athread%3Athread%253Adelegated-task%253Acommand%25253Amcp%25253A41881c90-3064-4d46-92b2-66ceec46e41c%25253Adelegate-task%25253Ac323d94a-1ba1-4b33-bbcd-01bcd7b0dfc5%3Aordinal%3A1%3Aba1e3c48-0a1e-4de7-934e-c6a92768f804`. Parent inspected static draft diff/report without taking ownership or polling build. Draft fixes OUCH replay reservation overflow and bounded unread-input discard on close; not integrated or cleared.

Parent steered existing implementation to reproduce and address reported FIX resend beyond send-buffer capacity, contain confirmed admin callback exceptions at operator boundary, and describe graceful close as best effort under arriving/flooding input. No new reviewer/task spawned, no worker files edited. Finish tests and commit before final handoff. Parent independent final diff review and integrated validation remain required; other workstreams continue by completion notification.

### Core partial handoff and coordinated query boundary

Core first completion was a build-wait checkpoint, not finished source. Actual child read after notification shows ordinal2 already running in `/Users/jedi/.t3/worktrees/lob/t3-b3a9b033`, branch `t3/refactor-core-lob`. Parent inspected draft, leaving worker ownership intact. Draft legacy changes include bounded mode, growth exception safety, pool ownership, sharded stop race and SPSC locality; no commit/parent clearance yet. Parent requested completion of midpoint extreme-band arithmetic and partial constructor thread-start failure cleanup before handoff.

Approved shared boundary: core worker implements additive allocation-free `MatchingEngine::for_each_level_order(InstrumentId, Side, Price, F&&) const`, FIFO `const OrderInfo&` visitor using existing callback-erasure pattern, invalid/missing level visits none. Preserve existing queries and canonical matching behavior. Research worker notified to replace `measure_level_queue` per-ID global scans and remove unused ID scratch buffers, without editing core-owned files. Core API commit must integrate first; then research coherent rebuild and deterministic/FIFO/allocation checks. Both workers notified, no duplicate ownership. Other implementation checks remain pending; parent will review final commits.

### Web UI green design system revamp, 2026-11-08

Uncommitted in the main checkout (`web/` and `docs/UI_DESIGN.md` only); no backend files touched. `docs/UI_DESIGN.md` was rewritten as the design-system source of truth: green primary, shadcn preset `b6snPS1nE` (applied as `radix-lyra`, olive base, Lucide), Inter and Geist Mono, validated chart palette (baseline chart-2, candidate chart-1), signed-figure convention. Shell moved to shadcn Sidebar plus breadcrumb header with a theme menu. Shared components: `page-heading`, `research-section`, `key-figure-list`, `definition-list`, `signed-figure`, `run-state-badge`. All bespoke CSS removed from `globals.css`. Four views were revamped by GPT-6.1 Sol/high delegates with file ownership; parent integrated and fixed the region naming, duplicate link and overview action regressions they introduced. One test change: `browser-tests/research-workflow.browser.ts` now waits for the inspect URL before asserting context, which removes a race with Next keeping the previous route hidden in an Activity boundary. `execution-api.browser.ts` "native MM diagnostics" failed identically on untouched HEAD earlier in the session (native data, not UI) and passes in the final run. Independent GPT-6.1 Sol/high code review (7 findings) and visual QA (6 findings plus 1 UX note) were all addressed. Final: typecheck, ESLint, build, Bun 124 pass / 5 skip, browser 32/32, review fill-identity 9/9, review frontend-execution 21/21. Not committed.

### Parent takeover after Opus API limits

Research continuation failed with Claude rate limit in `/Users/jedi/.t3/worktrees/lob/t3-f5cfef64` (`t3/audit-sim-research-api`); runtime and core continuations then failed with the same limit. Parent read each failed thread once and cancelled stale task continuations, preserving all worktrees. No alternate delegate provider/model was selected. Parent now owns completion and review of their unfinished source. Integrated source files copied into parent, excluding delegate WORK_PLAN copies and preserving concurrent UI files/entry. No research stand-in build is accepted as integration evidence.

Parent follow-up fixes: bounded pending FIX session payloads during replay (retain TestReqID instead of gap-filling an unsent Heartbeat); nonsticky client transport reservation for replay; legacy aggregate volume overflow rejection; pool constructor-failure slot restoration. Retained existing atomic artifact writer and explicitly durable idempotency writer rather than introducing fsync on every progress status update. Real core FIFO visitor and research consumer now build together. Parent combined release/debug builds and final review/checks in progress. Logs `/tmp/exsim-parent-integrated-{release,debug}-build.log`. No whole-simulator clearance yet.

### Whole native simulator refactor integrated and mergeable

Completed parent review/completion after Opus rate limits. Core/LOB/storage `2f09739`, protocol/runtime `cb52780`, research/simulation/API `1d079ed`; prior HTTP integrated `9254238` plus parent repair `67bb05b`. All original delegate checkouts retained. Final receipt [EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md](quality/EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md), with per-workstream reports alongside it.

Final parent release1010/1010 (16.54s), ASan/UBSan1006/1006 (31.47s), Linux GCC ASan/UBSan/LSan ownership66/66, strengthened pending-FIX allocation guard9/9 release and9/9 sanitizer. Server/research/API/client/ITCH dependent builds warning-free.12 baseline/new research runs,120 deterministic artifacts byte-identical. Exact logs `/tmp/exsim-parent-integrated-{release,debug}-tests.log`, `/tmp/exsim-parent-fix-allocation-{release,debug}-{build,tests}.log`, `/tmp/exsim-parent-linux-ownership.log`, `/tmp/exsim-parent-dependent-tools.log`, `/tmp/exsim-parent-artifact-equivalence.log`. Release guards provide zero-allocation evidence separately from sanitizer builds; four allocation tests compile out under ASan. Temporary selection count mismatch reconciled to five additional passing FIXP E2E cases, no test failures hidden. Full E2E/benchmarks not run.

Parent finds no blocking issue remaining in changed code within this recorded scope. Existing best-effort TCP close/no drain deadline, lab UDP policy, legacy explicit growth mode, non-global TSan/LSan/platform coverage remain documented limits. No timing claims, deployment, push, data migration or frontend edits. Concurrent UI work/WORK_PLAN entry preserved outside native commits. Next action: review/merge these local native commits when desired; no further implementation task remains for this refactor.

### Combined main-target PR preparation, 2026-11-08

User explicitly requested opening a PR to main and identified frontend thread `415fd59d-fb71-42ec-ae9a-eddc3814c03e`. Read its final handoff and coordinated a source/index freeze. It had used this same checkout/branch; no merge or cherry-pick of UI was needed. Committed completed frontend snapshot as `320cf4c` (web tree, design document and its existing WORK_PLAN entry only). Native refactor remains integrated through `2a1486f`. Preserved untracked `.agents/`, `.claude/`, `output/`, and `skills-lock.json` outside the PR additions. No services, Tailscale mappings, deployed bundles or user data changed by this PR task.

Fresh parent frontend checks on Node24.22.1/Bun1.4.4: `bun run typecheck`, `bun run lint`, `bun test` (124 pass,5 skip,0 fail), `bun run build` all pass. Exact logs `/tmp/exsim-pr-frontend-{types,lint,unit,build}.log`. UI owner reports final browser32/32, fill-identity9/9, frontend-execution21/21 and addressed independent visual/code findings; these are owner evidence, not new parent browser runs. Native parent receipts remain in quality/EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md. No source changes after these checks.

Target `origin/main` is the historical `404feb5` LOB release, so this is a cumulative simulator/research-platform PR including the native refactor and UI revamp, not just a cosmetic patch. Publish branch `ui-green-design-system`, create PR base main, link it to this thread immediately. Opening a PR is authorized; merging/deployment is not requested.

### PR source branch correction

User clarified: merge `ui-green-design-system` into `exchange-simulator` locally and open `exchange-simulator` -> `main`. Local exchange-simulator was an ancestor of the complete UI/native branch (0 divergent,15 incoming commits), so `git merge --ff-only ui-green-design-system` integrated it at `ea0d7c3` with no conflicts or source edits. Earlier local exchange branch also carried29 unpublished commits, now included as requested. Existing validated source is unchanged by this branch integration; only this receipt is added. Preserved all untracked files.

The earlier UI-head PR was created just before the correction arrived: https://github.com/JediNakDev/lob/pull/1. Closed and unlinked it as a duplicate; no merge/deployment happened. Publish `exchange-simulator`, open the replacement main-target PR, and link it to this thread. Frontend/native validation remains the receipts above; no redundant test rerun for a fast-forward plus documentation.

### Trader workspace structural revamp implemented for live review

User clarified that the earlier work improved the old hierarchy and requested implementation of the new trader proposal, a running development server and iterative UX checks. Read frontend-design, project vercel-react-best-practices, current UI_DESIGN and installed Next docs. Parent owns frontend/design additions only; concurrent uncommitted PostgreSQL/worker files, .gitignore, development runner, other source and production data remain untouched. No delegates or new checkouts in this implementation. No commit/push/deployment performed.

Implemented Research/Strategies/Scenarios/Runs/Compare primary navigation; Connections utility; lineage-grouped study landing/family/results; capability-backed libraries and preset editor entry; cross-study run queue; execution completion/cost/progress and MM equity-change/inventory/drawdown/markout hierarchy; separate diagnostics/export; comparison trade-off/pair/evidence sections; pinned finding export/reopen; URL analysis context and optimistic run search. UI_DESIGN explicitly amended; green tokens/shared primitives preserved. Native contracts unchanged. Current-contract limitations are explicit in reviews/WORKSPACE_REVAMP_IMPLEMENTATION_2026-11-08.md, not represented by fake controls. Earlier three-menu human-test readiness does not certify this new structure.

Three parent browser loops15/23/7 checks plus final4-check smoke closed observed duplicate actions, chart displacement, null-root Compare selection, URL/seed/tab/pin state and lost search characters. Native data proves actual studies/controls, receipt identity, 390px light/dark and keyboard paths; no human80% or independent redesign clearance claimed. Type/lint/build pass; ordinary127pass/5pre-existing skips,13330 assertions3.92s; nativebrowser4/4 in10.8s; targeted recovery/accessibility9/9 in6.0s. Exact logs `/tmp/revamp-{types,lint,tests,build,native-browser,workflow-browser}.log`, durable review receipts/screens/probes in reviews/WORKSPACE_REVAMP_2026-11-08/.

LIVE review UI http://127.0.0.1:46172/research, nativeAPI46173. Supervisor PID14570 via scripts/run_workspace_dev.py, receipt output/revamp-review/dev-session.json. Detached owned supervisor retains data output/revamp-dev-data and logs output/revamp-review/{supervisor,api-dev,next-dev}.log. Uses reviewed native360ad69 binary from output/releases/human-review-75cf28f, avoiding concurrent native rebuild/store changes. Five real configurations form three example studies including one intentional failure; existing installed services unchanged. Stop verified supervisor with SIGTERM, or restart after stopping via python3 scripts/run_workspace_dev.py. User next: inspect the running revamp, report task friction; retain source/data for iteration. Human first-use, durable editable finding metadata, independent scenario CRUD, multi-candidate and parameter sweeps are not silently claimed implemented.

### Subtitled trader workspace demo, 2026-11-09

User requested ONE demo video covering the new design, flow and trader use cases, with subtitles. Captured in actual T3 shared preview (now functional),1440x900, real dev UI46172/nativeAPI46173. Created through rendered controls new execution baseline exp_c7571b67808fcb6d and sweep variant exp_3ae9d94a5c1e979c, three completed seeds101/102/103 each. TWAP500/483/483 shares versus sweep500/500/500; signed costs/reproduction-backed paired comparison retained in docs/demo/workspace-revamp-evidence/native-evidence.json. Existing MM quote-width study explicitly described as existing. No mock interception, production changes or engine rebuilds. Actual finding-export completion observed; transient browser download was cleaned when its tab closed, so no separate byte-integrity claim is made for that captured download.

Final ONE video docs/demo/workspace-revamp-demo.mp4,138.205s (2m18s),1440x1000 H.26430fps,4,613,284bytes. Application900px + separate100px subtitle band.21 English burned-in captions, editable SRT/VTT and embedded chapters. Initial subtitle scale was too large; fixed via explicit ASS PlayRes1440x1000, then inspected final frames. Trimmed one repeated static hold in fill inspection. Full final decode PASS, cue/clip continuity PASS, final sampled frames1/7/11/15/18/21 visually inspected. SHA256b0ae79a6b04ed6f88c8d21d8bf31f8fe97aff1de7b229fa2f38945f4c3e6793d. Poster and updated docs/demo/README.md included. Capture/render metadata, native/source-hash evidence and verification stored in docs/demo/workspace-revamp-evidence/. Raw clips/intermediates remain ignored under output/workspace-demo-2026-11-09; ffmpeg-static installed only in /tmp/exsim-demo-tools, no repository dependency changes. Shared preview tab closed, dev supervisor/server retained. No new human usability or live-market performance claim. No commit/push/deployment for this media task.


## Configuration UX continuation, 2026-11-09

User confirmed both built-in models and their own external strategy clients.
Parent implemented clickable breadcrumb ancestors with pinned run context,
first-section built-in model selection and parameter focus link, visible
external-client entry, protocol/session/ITCH/custom-parameter editor and local
handoff export. Added explicit native server/client steps and corrected absent
telemetry interpretation. Existing backend sources and interrupted work retained.
No delegation, commit, push or deployment. All work is in the shared parent tree.

Checks: types/scoped lint pass; Bun16/16,100 assertions; browser23/23 on native
API-backed dev46172/46173, including actual export bytes, focus and390px
containment. Shared preview snapshots timed out twice; isolated fallback browsers
closed. Evidence/logs/reproducer: output/setup-flow-review. Receipt:
[CONFIGURATION_SETUP_2026-11-09.md](reviews/CONFIGURATION_SETUP_2026-11-09.md).
Flow: [CONFIGURATION_FLOW_2026-11-09.html](reviews/CONFIGURATION_FLOW_2026-11-09.html).
Dev supervisor14570 remains available. The JSON is an adapter handoff, not an
importable native config. Browser gateway launch/control/live telemetry and
external-session research-result ingestion remain unimplemented. Next runtime
work requires those native integrations; no external end-to-end or human
first-use success is claimed for this amendment.


## Custom strategy UI deferred, 2026-11-09

Latest user decision supersedes the external-client editor above: revert custom
trading strategy setup and mark it to implement. Removed the protocol/session/
ITCH/strategy JSON form, handoff export helper and their three tests. Research,
New study and Connections retain a non-actionable Custom trading strategy
button with a Coming soon tooltip on hover and keyboard focus. Connections is
again a read-only utility. Built-in strategy selectors, parameter links and
clickable breadcrumb ancestors are preserved. PRODUCT and UI_DESIGN record the
planned status; the prior setup report is marked historical for custom setup.
The flow artifact likewise labels that branch Coming soon. Native protocol tools
and concurrent backend/deployment files are untouched. No commit or deployment.
Parent checks:13 focused tests pass,79 assertions; typecheck/scoped lint checked.
Dev review stays at http://127.0.0.1:46172/research. Next custom-strategy work must
implement the actual native client/runtime integration before enabling the UI.
