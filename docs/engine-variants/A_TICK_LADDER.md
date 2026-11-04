# Tick ladder engine A (evaluated, not selected)

Source branch: `t3/implement-ladder-matching-engine`, commit `b77d206`
(worktree `/Users/jedi/.t3/worktrees/lob/t3-eaaf2612`). It implements the
same public `MatchingEngine` header as the production engine C
(`C_INDEX_ARENA.md`) and was kept on its branch after the comparison below.

## Layout

- Per instrument and side, a rank-indexed ladder of 16 byte levels (head,
  tail, 64-bit leaves total). Asks rank by price index and bids by reversed
  index, so the best level is the lowest occupied rank on both sides.
- Two-level occupancy bitmap (leaf and summary words) finds the next occupied
  level in O(1) for gaps up to 4096 ticks.
- 64 byte order slots in one pre-faulted pool, with intrusive level FIFOs and
  per-session lists in ascending OrderId for MassCancel and EndOfDay.
- `(session, cl_ord_id)` to slot in a fixed `lob::FlatHashMap`, and an
  OrderId to slot index that keeps four consecutive ids per cache line and
  erases an entry only when its slot is reused (a cheaper cancel path).

## Verification against the production test suite (2026-11-03)

A's engine files were swapped into a scratch copy of `integrate-engine-c`, so
the oracle differential, all scenarios, the preflight and allocation gates,
B's public-API tests and C's arena tests ran against A: 448 of 449 passed.
The one failure, `arena_instruments.add_instrument_rejects_each_invalid_rule`,
asserts C's own limit of 2^24 price levels per band, which SPEC 2 now lists
as engine-specific; A accepts wider bands. Log: `/tmp/exsim-a-release.log`.

## Benchmark against C

`exsim_bench --ops 2000000`, perf build, Apple M6 macOS, A and C alternated
for seven rounds in one session under the same background load. Every run had
zero allocations. Medians (`/tmp/exsim-bench-a-vs-c.log`):

| Workload | A Mops/s | C Mops/s | C / A | p99 ns A | p99 ns C |
| --- | ---: | ---: | ---: | ---: | ---: |
| passive_add | 11.07 | 12.18 | 1.10x | 291 | 250 |
| cancel | 8.56 | 8.09 | 0.95x | 416 | 417 |
| aggressive | 11.68 | 13.92 | 1.19x | 292 | 250 |
| mixed | 10.93 | 12.45 | 1.14x | 333 | 292 |

## Decision

C stays the production engine: it is faster on three of four workloads and
on the mixed workload that is closest to a real session, with equal or lower
p99. A is about 5% faster on pure cancels, which is small next to this
machine's round-to-round spread. A plausible cause, not measured separately,
is that a cancel touches one 64 byte slot in A but a hot and a cold record in
C; that is the first thing to test if cancel latency becomes the bottleneck.
