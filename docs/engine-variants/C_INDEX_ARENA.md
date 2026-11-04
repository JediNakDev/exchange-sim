# Index arena engine C (production engine)

Source branch: `t3/implement-arena-matching-engine`, commit `cd8376d`
(worktree `/Users/jedi/.t3/worktrees/lob/t3-2d9b3f09`). Integrated into
`exchange-simulator` as the production `MatchingEngine`, replacing variant B
(sorted level vectors, see `B_SORTED_LEVEL_VECTORS.md`). The public headers
did not change, so callers are unaffected.

## Layout

- One order arena sized `EngineConfig::max_orders`, addressed by `uint32`
  indices. Slot 0 means none. Free slots form a stack threaded through the
  hot record's `next` field.
- Each order is split into a 32 byte hot record (next, leaves, cum, owner,
  order id, session token), read while matching, and a 32 byte cold record
  (prev, tick, instrument, side, tif, stp, post_only, id-list and
  session-list links), touched only on add and remove. Both sizes are
  `static_assert`ed in `src/exsim/engine/order_arena.hpp`.
- Price levels are 24 byte records indexed by tick, per instrument and side,
  with a three-level occupancy bitmap for best-price search and a cached best
  tick. A side may span at most 2^24 ticks; `add_instrument` rejects wider
  bands at setup.
- One fixed `lob::FlatHashMap` keyed by `(session << 48) | cl_ord_id`, which
  is exact because SPEC limits `ClOrdId` to 48 bits and `SessionId` is 16 bits.
  Sized at construction, at most half full, never grown.
- Engine-wide, per-session and per-instrument intrusive lists in ascending
  `OrderId` order, so no query or command sorts: `for_each_order` walks the
  engine-wide list, MassCancel the session list, and EndOfDay expiry and the
  non-crossed indicative imbalance the instrument list. The per-instrument
  links live in their own 8 byte array (`InstrumentListLinks`) because the
  cold record is full.
- Memory: 72 bytes per order slot (hot, cold, instrument links) plus the
  hash map at 16 bytes per slot with at least twice `max_orders` slots
  rounded up to a power of two, so roughly 104 to 136 bytes per order.
  Each instrument needs 48 bytes per tick (one level per side) plus the
  bitmaps, so a maximum 2^24-tick band costs about 770 MiB. Everything is
  allocated and zero-filled at setup; an allocation that does not fit
  throws from the constructor or `add_instrument`.
- The only prefetch kept is the next resting order in the level FIFO while
  filling. Cold-record prefetch on cancel and a reworked list removal were
  measured within noise and reverted.

## Output preflight

Every `handle` overload runs a read-only preflight with a proven event bound
before any mutation. The bound is tighter than `required_event_capacity`, as
the header allows, but not always exact: a market IOC order sweeping 7
resting orders needs 10 free slots and emits 9 events. The contract only promises
`Applied` at `required_event_capacity(config)`, which is what the tests check.

## Verification at integration (2026-11-03)

- macOS Apple clang, `-Werror`: release 351/351, ASan+UBSan 349/349,
  TSan 349/349. The production differential, scenario, preflight and
  allocation tests now run by default (`-DEXSIM_TEST_PRODUCTION` in the
  Makefile). B's public-API tests (`tests/exsim/engine/vector_*`) were kept as
  a second independent suite; one asserted B's exact preflight bound and now
  asserts the contract bound instead.
- Linux container (Debian trixie), `-Werror`: g++ 14 and clang++ 19, release
  351/351 and ASan+UBSan 349/349 each; perf build clean, zero allocations.
- Differential fuzz against the oracle: 1000 seeds x 20000 steps on each of
  the `mixed` and `tiny` profiles (40M commands), no divergence. The variant's
  own session had already run 20 seeds x 200k steps on both (8M commands).

## Independent review (2026-11-04)

A separate reviewer (task `38045dc3`, report `/tmp/exsim-c-review/REPORT.md`)
found no correctness, memory-safety, undefined-behaviour or preflight defect.
It reran the preflight bounds under a 26M-command fuzz with exact output
sizing, checked integer edges, compared the occupancy bitmap against
`std::set`, and confirmed zero allocations. Its findings and what was done:

| # | Severity | Finding | Resolution |
| --- | --- | --- | --- |
| 1 | medium | EndOfDay expiry walked the engine-wide order list once per instrument, so `SetPhase{0, Closed}` cost instruments x live orders inside `handle()` | Fixed: per-instrument order lists. With 1M GTC orders on one instrument, `SetPhase{0, Closed}` went from 905 ms to 1.4 ms at 1000 instruments and from 7984 ms to 1.1 ms at 8000 (`/tmp/exsim-c-eod-fix.log`). Test `arena_phase.end_of_day_and_imbalance_see_only_their_instrument`. |
| 2 | low | `max_orders` is accepted up to `UINT32_MAX`, which cannot be allocated | Kept the shared `required_event_capacity` bound; construction throws when the arrays do not fit. Documented in the header and the memory note above. |
| 3 | low | `add_instrument` throws `std::bad_alloc` instead of returning 0 | Documented: 0 means a rejected config, an allocation failure throws. |
| 4 | note | `find_order(OrderId)` is O(live orders); the non-crossed indicative imbalance was O(live orders) | `find_order(OrderId)` stays a list walk (no OrderId map on the hot path). The imbalance now walks only its instrument's list. |

The extra list costs one 8 byte link record per add and remove. Five
interleaved rounds of `exsim_bench --ops 2000000` before and after the change
moved the medians by -3% to +26% per workload, inside this machine's
round-to-round spread of 10 to 17 Mops/s (`/tmp/exsim-c-eod-bench.log`).

## Foundation review gates owned by the engine

`docs/FOUNDATION_REPAIRS.md` leaves these engine gates to whichever engine is
production. All of them run against C in `run_tests`:

| Finding | Gate | Tests |
| --- | --- | --- |
| 6 | insufficient output with a nonempty buffer leaves state, counters and output unchanged; identical retry; allocation-free worst cases | `vector_output.nonempty_insufficient_output_retry_all_commands`, `vector_output.bounds_cover_worst_case_sweep_mass_cancel_and_phase`, `arena_buffer.insufficient_output_changes_neither_engine_nor_output`, `arena_buffer.startup_sized_buffer_covers_worst_case_commands`, harness preflight and allocation tests |
| 10 | crossed PreOpen to Halted to Continuous or Closed | `arena_phase.halted_keeps_a_crossed_book_and_leaving_it_uncrosses`, `arena_auction.halted_cross_summary_and_expiry`, `vector_auction.halted_cross_summary_ids_and_expiration` |
| 14 | invalid instrument and engine configs | `arena_config.invalid_configs_and_wide_aggregates`, `arena_config.engine_config_bounds_throw`, `vector_instrument.*config*`, `vector_capacity.invalid_engine_configs_throw` |
| 15 | complete active event fields | the oracle differential and scenarios compare every field; arena and vector field tests |
| 30 | aggregate widths (two maximum quantities at one level) | `arena_config.invalid_configs_and_wide_aggregates` |
| 31 | conservation across amendment chains | `vector_invariants.randomized_commands_and_deterministic_active_fields`, `arena_randomized.invariants_hold_after_every_command_*` |

## Benchmarks

`exsim_bench --ops 2000000`, perf build, Apple M6 macOS, B and C alternated
three times in one session under the same background load (load average
13 to 15, other agents compiling), so both saw the same noise. Every run
had zero allocations. Median Mops/s:

| Workload | B | C | C / B |
| --- | ---: | ---: | ---: |
| passive_add | 6.61 | 11.91 | 1.80x |
| cancel | 4.28 | 7.59 | 1.77x |
| aggressive | 10.38 | 12.99 | 1.25x |
| mixed | 7.23 | 13.11 | 1.81x |

C was faster than B on every workload in every round. The variant author
measured 15.9, 9.0, 16.6 and 17.5 Mops/s for C on a quieter machine.

## SPEC readings flagged by the variant author

- A market FOK order runs the fill pre-check (SPEC 4.2 read literally).
- `phase()` of an unknown instrument returns Closed.
- An unknown `CommandType` is a no-op that returns `Applied`.
- A canceled market order echoes the price from its command.
- When the book is not crossed, the indicative imbalance uses each side's
  total leaves (SPEC 9).

All of these agree with the independent oracle; the SPEC conformance audit
is the separate check that both agree with the text.
