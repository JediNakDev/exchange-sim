# Vector ladder engine B

Checkout: `/Users/jedi/.t3/worktrees/lob/t3-3500e31b`
Branch: `t3/vector-price-level-matching-engine`
Canonical sync: `25a69ee` (exact named files from `9a2eaa5`). No other engine imported.

## Layout and output capacity

Orders occupy 64 aligned bytes, including 32-bit FIFO and ascending-ID links.
PriceLevel occupies 16 bytes (uint64 quantity, two 32-bit FIFO endpoints).
Each level has a separate 8-byte sorted price key. Best is at the back;
lookup scans backward at most 64 keys before binary search. Insert/erase
moves the better tail; consuming a best level only decrements size.
Each side reserves min(band ticks, max live orders) levels at setup.
Two fixed FlatHashMaps index client keys and order IDs. No hot-path growth.

Every handle overload preflights before mutation and checks every event push.
For n live orders, New/Replace require n+2 free events: acknowledgment, at
most one event per removed resting order, and disposition or aggressor STP
cancellation. A final partial fill terminates the aggressor and replaces the
disposition; CancelBoth uses the resting removal and terminal event slots.
Cancel requires one; MassCancel requires n+1. SetPhase requires
2*n+2*registered_instruments+1, allowing fills, expiry, summaries, phase
changes and rejection. Auction steps remove at least one live order, and
expiry removes each surviving DAY order once. These conservative live-state
bounds avoid allocating config-sized buffers for small semantic tests.
InsufficientOutput preserves existing output, state, and IDs; tests compare
identical retries to untouched twins using explicit active-field encoding.

## Verification

- `make test`: 85/85, warning-free, `/tmp/vector-release.log`.
- `make test MODE=debug`: 85/85, ASan/UBSan, `/tmp/vector-debug.log`.
- 20,000 seeded random commands: depth/FIFO/lookup/count, phase crossing,
  per-order and amendment-chain conservation, deterministic active fields.
- `make tools MODE=perf`: `/tmp/vector-tools.log` and
  `/tmp/vector-restore-build.log`.
- Standalone canonical benchmark self-tests pass normally and under
  ASan/UBSan: `/tmp/vector-bench-self.log`, `/tmp/vector-bench-self-san.log`.
  The canonical source exposes these through EXSIM_BENCH_SELF_TEST, not
  runtime --self-test (which prints usage). No benchmark modifications made.
- Extra public-API allocation probe: `/tmp/vector-allocation-probe.cpp`,
  `/tmp/vector-allocation.log`: 1800-fill sweep, 1800-order mass cancel and
  900-fill auction all have zero C++ new/array/aligned allocations.
- Named canonical paths compare exactly to 9a2eaa5.

## Benchmarks

Apple ARM64 macOS, perf (-O3 -march=native -flto -DNDEBUG), seed 24301,
max_orders 2097152, max_instruments 4, baseline workloads unchanged.
No affinity pin. Counter 1 tick/ns; timer/quantization limitations are printed
by benchmark. Means include clear/event count, latency includes timer boundary.
C++ allocation guard does not instrument direct malloc/mmap.

Initial 1M run (`/tmp/vector-bench-1m.log`):

| workload | Mops/s | mean ns | allocations |
| --- | ---: | ---: | ---: |
| passive_add | 8.39 | 119.2 | 0 |
| cancel | 5.08 | 196.7 | 0 |
| aggressive | 13.94 | 71.7 | 0 |
| mixed | 10.80 | 92.6 | 0 |

Final 2M run (`/tmp/vector-bench-final.log`):

| workload | ops | Mops/s | mean ns | p50 | p90 | p99 | p99.9 | p99.99 | max ns | allocs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| passive_add | 2000000 | 6.02 | 166.2 | 209 | 250 | 375 | 500 | 834 | 34083 | 0 |
| cancel | 2000000 | 4.58 | 218.2 | 292 | 416 | 541 | 625 | 1125 | 14625 | 0 |
| aggressive | 2000000 | 7.61 | 131.4 | 167 | 250 | 375 | 417 | 542 | 11416 | 0 |
| mixed | 2000000 | 6.31 | 158.6 | 209 | 333 | 500 | 1417 | 5417 | 242834 | 0 |

Timing is noisy on this shared machine. Earlier 2M results are retained in
`/tmp/vector-bench-2m.log`, `/tmp/vector-baseline-repeat.log`.
No stable cross-variant speed ranking is claimed.

## Profile and optimization experiment

`/usr/bin/sample`, 1 second, 1ms sampling: `/tmp/vector-profile.txt`.
Sample includes generation, setup and replay, not exclusively timed matching;
append_to_level and resting/map work are visible in the engine stacks.
Peak footprint observed during that sampled interval was about 585 MB.

Tried changing linear scan threshold from 64 to 16. Two 16-key runs had
mean ns [passive,cancel,aggressive,mixed] of [121.5,157.5,79.5,102.0]
and [117.5,138.6,85.6,142.7]. Restored 64-key run measured
[131.6,213.8,72.8,101.4]. Cancel improved, aggressive regressed, mixed was
inconsistent; retained original 64. Logs: `/tmp/vector-opt16.log`,
`/tmp/vector-opt16-repeat.log`, `/tmp/vector-baseline-repeat.log`.

## Integration boundaries

No spec changes or unresolved semantic ambiguities. FOK market orders use
section 4.3 before disposition; market TIF cannot allow a remainder to rest.
One-sided/noncrossed indicative imbalance uses total side leaves, per repairs.
Differential validation against the independently owned oracle remains an
integration gate, as do gateway/publisher/server tests. No competing oracle
was added: the interrupted untracked vector_reference_model.hpp is preserved
but excluded from the commit and build. No child tasks were launched/resumed.
`docs/WORK_PLAN.md` is absent in this old base and is outside the authorized
source/test plus exact canonical-import scope; this record supplies handoff
facts for the parent coordinator.
