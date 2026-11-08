# Native simulator refactor integration, 2026-11-08

Parent judgment: the implemented refactor is mergeable within the validation scope below. No outstanding blocking finding remains in the changed code. This is not a guarantee that every untouched component or future workload is defect-free. Nothing was pushed or deployed, and running services and participant data remain unchanged.

## Integrated source

Baseline `52d5724`; parent branch `ui-green-design-system` already carried concurrent uncommitted UI work. That UI work and its WORK_PLAN entry were preserved, excluded from these native implementation commits.

| Workstream | Source and integration |
| --- | --- |
| HTTP | Opus `/Users/jedi/.t3/worktrees/lob/t3-e5925115`, `bac7ec9`, integrated `9254238`; parent review repair `67bb05b` |
| Core, legacy LOB, storage | Opus `/Users/jedi/.t3/worktrees/lob/t3-b3a9b033`, uncommitted work recovered after API rate limit; parent review/completion `2f09739` |
| Protocol/runtime | Opus `/Users/jedi/.t3/worktrees/lob/t3-6cc707be`, uncommitted work recovered after API rate limit; parent review/completion `cb52780` |
| Simulation, research, non-HTTP API | Opus `/Users/jedi/.t3/worktrees/lob/t3-f5cfef64`, uncommitted work recovered after API rate limit; parent review/completion `1d079ed` |

Opus 5.5/high performed the requested implementation. Parent reviewed the diffs and completed unfinished work after the provider rate limit. No replacement delegate model was selected. Original task IDs and continuation failures are recorded in WORK_PLAN; worktrees are retained.

## Outcome

- Production matching semantics, event ordering, wire encodings and research artifact schemas are preserved. A read-only FIFO level visitor removes repeated engine-wide lookups from research queue evidence, plus its maximum-order-sized ID scratch buffer.
- Legacy books support explicitly bounded operation in ordinary builds, and sharded books always use it. Stack aggressors can trade at full resting capacity. Ladder growth commits only after replacement allocations succeed; pool bookkeeping owns blocks before any potentially throwing vector growth. Orders occupy one 64-byte aligned slot. Sparse snapshots walk occupancy words.
- Shard shutdown no longer writes stop messages into producer-owned SPSC queues. Workers apply commands directly and update the shared in-flight count per batch. Cached opposite queue indices avoid unnecessary cross-thread cache reads. Partial worker-start failures join already-started workers.
- Legacy midpoint and level-volume arithmetic are checked without signed overflow or silent unsigned wrap. A failed pooled-object constructor restores its free slot.
- OUCH and FIX recovery pause at transport capacity, then resume. Live overload still fails closed. Pending FIX session messages retain payloads such as TestReqID in startup-allocated storage, bounded at 16 per replay; exhaustion disconnects without allocation. Admin callback exceptions are contained at the operator boundary.
- HTTP parses each complete request head once while receiving its body, consumes pipelines by offset, and uses fixed connection slots with explicit cleanup. Duplicate/failed starts, thread creation exceptions and throwing handlers/responders are contained.
- API fill pages reuse a dense markout-row index instead of rebuilding a full hash table each request. Artifact writing avoids intermediate JSON copies and reuses a decision writer. Parent rejected unconditional fsync on progress updates; existing atomic replacement and explicit idempotency durability remain unchanged.

No speculative allocator, framework, reactor rewrite, timing claim, or frontend redesign was added.

## Parent review findings resolved

In addition to the delegate fixes, parent required or implemented: midpoint overflow and partial thread startup cleanup; the coordinated FIFO visitor; catch-all HTTP thread-construction failure cleanup; compatible HTTP aggregate member order; rejection of draft FIX gap-filling for never-transmitted session payloads; bounded pending session storage and its allocation/exhaustion test; client-side nonsticky replay reservation; aggregate legacy volume bounds; pool constructor failure recovery. Two new research test fixtures were corrected: missing seed membership and excessive arithmetic risk bounds. Production validation was not relaxed to make fixtures pass.

## Final checks

| Gate | Result | Evidence |
| --- | --- | --- |
| Integrated release | 1010/1010 unique tests, 16.54 s | `/tmp/exsim-parent-integrated-release-tests.log` |
| Integrated ASan/UBSan | 1006/1006 unique tests, 31.47 s | `/tmp/exsim-parent-integrated-debug-tests.log` |
| Final FIX pending-payload allocation assertion | release 9/9 and ASan/UBSan 9/9 after strengthening the existing capacity test | `/tmp/exsim-parent-fix-allocation-{release,debug}-{build,tests}.log` |
| Linux GCC ASan/UBSan/LeakSanitizer ownership paths | 66/66; leak detection enabled, warning-free with `-Werror` | `/tmp/exsim-parent-linux-ownership.log` |
| Dependent binaries | server, research CLI, native API, client, ITCH replay all built warning-free | `/tmp/exsim-parent-dependent-tools.log` |
| Baseline/new deterministic research outputs | 12 runs across four strategy configurations and three seeds; 120 artifacts byte-identical | `/tmp/exsim-parent-artifact-equivalence.log`, new data `/tmp/exsim-parent-research-runs` |
| Shard stop race | delegate TSan baseline reports a data race; fixed probe exits 0 over 20 rounds | `/tmp/exsim-core-lob-20261108/tsan.log` |
| Partial worker startup | delegate injected every constructor allocation: after 70 clean exits; baseline 11 abnormal exits; ASan after also clean | `/tmp/exsim-core-lob-20261108/startup-failure-probes.log` |

The integrated selection includes all non-`server_e2e` tests plus the new OUCH/FIX recovery and existing allocation E2E cases. Five FIXP E2E tests were additionally selected because the runner uses substring filters. Every listed expected test was executed, all five extra tests passed, and executed names/counts were reconciled. The temporary selection script initially returned a count-mismatch error after all tests passed; that was a receipt error, not a failing test. Exact final totals above are unique test names, not duplicated counts. The full E2E suite was not run.

The release selection includes all declared hot-path allocation guards. Four allocation tests are compiled out under sanitizers (production engine, journal and two FIXP cases); other allocation-counter scopes skip counting there by design. Release provides the allocation evidence, separate from sanitizer evidence. The final pending-session guard checks both successful bounded enqueues and capacity rejection allocate zero.

Final source builds use repository warning flags, with no warnings. Relevant logs are `/tmp/exsim-parent-integrated-release-final4-build.log`, `/tmp/exsim-parent-integrated-debug-final3-build.log`, and the final FIX assertion build logs. Test commands select suites with `./build/{release,debug}/run_tests <suite>.`; the executed commands and every test result are retained in the test logs. No empty selection was accepted.

The Linux ownership check compiled the pool, legacy book, shard/SPSC and HTTP tests with `g++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined` and ran with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` inside the existing `exsim-dev:latest` image. It does not claim whole-system Linux coverage.

Artifact equivalence compared commands.journal, commands.jsonl, events.jsonl, execution-decisions.jsonl, ledger.jsonl, lifecycle.jsonl, markouts.jsonl, series.csv, summary.json and experiment.conf. Status/manifest files were excluded because they include wall-clock/build provenance. Canonical command/event, lifecycle, accounting, markout and series bytes are preserved across all 12 pairs.

## Limits and retained decisions

- No performance benchmark was run. Benefits reported here are eliminated work and bounded storage, not measured latency gains.
- macOS ASan cannot provide LeakSanitizer coverage. The separate Linux ownership run covers selected allocation/cleanup paths, not all runtime processes.
- TSan is targeted at the shard stop race, not the whole simulator. No new third-party FIX conformance run or full-system Linux rebuild was performed.
- Graceful TCP close remains best effort if a peer keeps sending while close occurs. The bounded discard removes the reproduced unread-input failure but does not guarantee FIN under arbitrary concurrent input. Existing graceful-close slots have no deadline if a peer never drains; configured connection capacity bounds that exposure.
- Legacy growth mode intentionally allocates and can request large ladders for outlier prices. Select bounded mode for allocation-free use; production simulation uses its bounded matching engine. Legacy best-price recovery still scans occupancy words; no unsupported latency guarantee is added.
- Existing lab-feed requester policy and unused moved-from store/token behavior were not redesigned. Untouched codecs, MD builders and tools received focused tests and structural review rather than a new exhaustive independent audit.
