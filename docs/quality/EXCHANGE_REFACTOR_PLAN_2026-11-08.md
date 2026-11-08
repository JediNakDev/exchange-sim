# Exchange simulator refactor plan

The user clarified that the scope is the whole exchange simulator and limit order book engine. HTTP is one workstream, not the overall scope. Use Opus for implementation and parent review until the integrated changes are mergeable. Favor readable direct C++ with demonstrable improvements, bounded storage, explicit ownership and predictable memory access. Do not rewrite sound code merely to change its style.

## Scope and sequence

1. **Order books and foundational storage:** production matching engine, arena and ladder, legacy order book, sharded engine, object pools, flat maps and queues. Check FIFO, replacement, cancellation, auctions, output preflight, symbol isolation, capacity exhaustion, overflow, pointer lifetime and post-startup allocation. Revalidate historical foundation findings against current code. Simplify duplicated work and unnecessary scans only with preserved observable semantics.
2. **Exchange protocol and runtime:** OUCH, FIX, FIXP/SBE codecs and gateways, ITCH/MoldUDP64 publication/recovery, networking, core dispatch, fault queues, journal worker and reactor. Check bounded buffering, short I/O, descriptor ownership, backpressure, reconnect/replay, stale handles, concurrency and allocation boundaries. Preserve normative wire behavior.
3. **Simulation and research:** event scheduler, strategies/bots, ledger, runner, replay/artifacts, result queries and native service. Check causal event ordering, determinism, fixed-point overflow, data locality, bounded queue exhaustion, I/O separation and hot-path allocation. Preserve canonical hashes and artifact compatibility unless a confirmed correctness defect requires an explicit contract change.
4. **HTTP serving:** remove repeated parse/copy work and strengthen socket/thread ownership using the existing blocking architecture. This workstream was already delegated before the clarification.
5. **Integration review:** parent reviews every implementation diff and resolves defects and complexity. Cross-boundary API changes are coordinated before dependent edits. Each component gets an explicit reviewed/no-change or changed result; a partial workstream is never whole-system clearance.

Frontend visual redesign is not part of this C++ refactor. Existing concurrent UI edits, untracked outputs, interrupted worktrees, services and data remain intact. Cache friendliness here means data locality, avoiding redundant work and respecting OS I/O scheduling, not caching mutable simulation truth or adding speculative cache infrastructure.

## Implementation boundaries

Use harness-created isolated worktrees from the local parent baseline. Core owns `include/lob`, `src/{order_book,sharded_engine}.cpp`, `src/exsim/{matching_engine.cpp,engine}`, `include/exsim/matching_engine.hpp` and associated tests. Runtime owns protocol/gateway/net/md/server/client trees and related tools/tests. Research owns research/sim and non-HTTP API implementation and tests after a compiler slot becomes free. HTTP owns only its recorded files. Shared domain types, normative specs and build infrastructure require parent coordination; do not change another workstream's files. Delegates do their own work without spawning children. Limit simultaneous builds and retain logs outside the plan.

## Merge gates

- Build warning-free with the repository flags and run focused behavioral regressions for the changed components.
- Release allocation guards on each affected declared hot path, including exhaustion/reuse where relevant. Sanitizers do not replace allocation checks.
- ASan/UBSan for changed ownership/arithmetic paths. Report leak-check platform limitations explicitly.
- Engine changes require reference/differential and canonical replay checks, plus relevant FIFO, replace, auction, phase and preflight invariants.
- Protocol/runtime changes require the relevant wire/session/loopback checks; research changes require deterministic replay and independent fill/ledger reconciliation where touched.
- Parent review covers bounds, arithmetic, invalidation, cleanup, concurrency, regressions and unnecessary abstractions. New findings are fixed and the affected checks repeated.
- Performance claims require evidence. Structural elimination of an allocation/copy/scan can be reported directly. Do not invent latency gains or run a broad benchmark merely for a cleanup.

Record each task, checkout, commit, review, exact check command/log, unresolved finding and next action in WORK_PLAN. Final readiness applies to the integrated source and exercised scope, with untested platforms or load conditions stated plainly. No deployment or external publication is requested.
