# Canonical foundation repair checkpoint

Task checkout: `/Users/jedi/.t3/worktrees/lob/t3-12335207`.
Branch: `t3/repair-simulator-foundation-contracts`. No engine, gateway,
publisher, networking, bots, journal, research API or frontend is implemented
by this repair. WORK_PLAN and delegation registry are intentionally untouched
under this task's ownership boundary. Integrator must record the resulting
commit in those coordination records and distribute it before resuming engines.

## Shared interface migration

- [x] Append ReplaceOrder.force_priority_loss=false; OUCH U sets it true.
- [x] Append CancelOrder.reduce_to_total_qty=0. Positive values reduce total
  exposure without increasing leaves; zero cancels fully. This works in all
  phases without routing a cancel through phase-restricted ReplaceOrder.
- [x] Append CanceledEvent.leaves_qty=0. Partial reductions retain the order,
  token and FIFO position. Consumers remove orders only when leaves reaches
  zero. Publishers emit X for a partial reduction, D for full removal.
- [x] Append AuctionSummaryEvent.match_id=0 for source compatibility. Actual
  engine summaries MUST set a fresh nonzero global ID after constituent fills.
- [x] EventBuffer never grows during push. push is nodiscard bool; caller must
  handle failure. remaining/can_append expose available output space.
- [x] handle overloads return HandleResult. Applied includes semantic rejection;
  InsufficientOutput changes neither state nor output. Engine preflight is
  mandatory before IDs, orders, phases or any other state changes. Existing
  call syntax remains valid, but nodiscard requires integrations to inspect status.
- [x] required_event_capacity validates limits and returns a checked conservative
  bound. Startup must allocate it, or the engine must prove a tighter bound.
  The conservative buffer can be large; startup memory planning is required.
- [x] valid_instrument_config checks layout-independent bounds; engine setup
  must additionally validate allocation byte sizes for its chosen data layout.
- [x] for_each_order erases a local callable object, supporting free-function
  references, pointers and const callables without allocation or const removal.

## Verified review disposition

Numbers refer to FOUNDATION_REVIEW.md. Every relevant original implementation
or contract was inspected; protocol semantics were checked against the primary
OUCH 4.2, ITCH 5.0 and FIX session documents linked in GATEWAYS.md.
“Contract repaired” does not close the downstream implementation/test gate.

| Findings | Disposition and remaining gate |
| --- | --- |
| 1,18,19,20 | Fixed in FlatHashMap: sentinel lookup, reset moved-from state, checked reserve rounding/byte bounds, const slot iteration. Allocation failure during rehash also preserves the original table. Regression tests cover lookup lifecycle, moves, reuse, overflow and constness. |
| 4 | Fixed benchmark tracker: priority loss removes old token and waits for Rested; retention only changes token mapping when necessary. Duplicate insertion fails loudly. Standalone tests cover changed and unchanged tokens and retention. |
| 17 | Fixed comparison macro: operands live through a lambda invocation's full expression; noncopyable operands remain supported. Sanitizer regression uses a temporary optional subobject. |
| 21 | Fixed aligned scalar/array new and matching deletes. Standalone guard deliberately exercises ordinary and aligned allocations. Direct C allocation/mmap, custom allocators and allocations in other processes are outside this guard. |
| 32 | Fixed per-mode compile/link/source stamps, compatible with host GNU Make 3.81. Scratch checks prove unchanged builds stable, flag/compiler changes rebuild and removed test TU forces relink. |
| 33 | Fixed local callback wrapper, compilation exercises free-function references, function pointers and const lambdas. No production engine needed. |
| 34,35,38 | Fixed benchmark operation/workload validation, bounded supported runs below capacity, fatal unexpected generation/replay rejects, replay event-count check, explicit empty-workload guard, timer baseline, quantization and affinity reporting with optional Linux --cpu. Engine replay checks remain unexecuted until integration. |
| 2,7,8,9,24,25,26,29 | Contract repaired: OUCH total quantities, cancel reductions, priority loss, acknowledgment ordering, day-token history, supported instructions, reasons, phase-specific liquidity and deterministic buy-before-sell output. Gateway owner must test partial fill then U/X, halted reductions, IOC A/E/C ordering, same-session legs, unsupported instructions and day-token retransmission after completion/reconnect. |
| 3,27,28 | Contract repaired: ordered FIX application delivery, garbled framing without sequence advance, duplicate/gap-fill/reset/probe handling. FIX session owner must test gaps with trading messages, duplicate and overlapping gap fills, decreasing resets, garbled frames and matching TestReqID. Bound overflow must disconnect/recover, never process a high application sequence early. |
| 5 | Raw-journal guarantee removed. Journal/oracle owner must supply a versioned explicit serializer and field-based equality/hash tests, including alternate union payloads and padding perturbation; this task does not implement serialization. |
| 6 | Bounded buffer implementation and conservative sizing tested; engine return/preflight contract repaired. Each engine/oracle must test insufficient output with a nonempty buffer and prove all state/counters/output unchanged, retry identical output, and allocation-free worst-case mass cancel/phase/sweep. No engine implementation is present here. |
| 10,14,15,30,31 | Contract repaired for inherited crossed Halted books, config/aggregate bounds, complete active event defaults, unsigned volume/distance arithmetic and amendment-chain conservation. Layout-independent config helper is tested. Engine/oracle owners must test crossed PreOpen -> Halted -> Continuous/Closed, invalid configs, UINT32_MAX volume sums, event fields and cancellation/amendment chains. |
| 11,12,13,22,23 | Contract repaired for fresh cross match ID, exactly-one replacement creation, wire price/time bounds, batch validation boundaries and explicitly simplified NOII. Publisher/server owners must test two-leg auction output, cross IDs, replace/rest suppression, day/price range rejection, one-sided NOII and multi-packet mass cancel with feed reconstruction at the batch boundary. No codec commit dependency introduced. |
| 16,36,37 | Explicitly deferred legacy implementation: cross-symbol matching, fill-vector allocation boundary, completed-order mapping leaks. A separate legacy owner must add same-shard instrument isolation, >1024-fill allocation guard, and long-running mapping-reclamation tests. Legacy ShardedEngine is prohibited as production simulator infrastructure until these gates pass. |

## Validation evidence

Logs are outside tracked sources in `/tmp/exsim-foundation-logs/`:

- `release.log`: 50/50 linkable tests pass with current C++20 warning flags.
- `sanitizers.log`: 50/50 ASan/UBSan tests pass.
- `bench-compile.log`, `bench-compile-debug.log`: engine-dependent benchmark
  objects compile without linking an engine, release and sanitizer flags.
- `bench-self-test.log`, `bench-self-test-san.log`: standalone tracker,
  workload-bound checks, buffer no-growth allocation check and C++ allocation
  guard tests, including aligned array allocations, pass without an engine.
- `incremental.log`: exact scratch directory, build commands and assertions
  for no-op, compiler/flag change and TU removal; no parent make clean used.
- `callback-compile.log`: explicit no-link compile of callback wrapper probes.

No engine benchmark throughput/latency result is claimed. The three engines
and oracle must adopt this interface together before resumption. Header/API
compilation and foundation tests cannot prove matching or protocol integration.

Reproduce the standalone tool test without an engine:

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -Iinclude -I. \
  -DEXSIM_BENCH_SELF_TEST tools/exsim_bench/main.cpp -o /tmp/exsim-bench-self-test
/tmp/exsim-bench-self-test
```

Add `-fsanitize=address,undefined` for the sanitizer variant. Normal engine-
dependent tool checks use `make build/release/obj/tools/exsim_bench/main.o`
and its `MODE=debug build/debug/obj/...` counterpart, never claim a linked
benchmark run from an object compilation.
