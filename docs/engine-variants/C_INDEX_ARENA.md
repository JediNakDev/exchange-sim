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
- Engine-wide and per-session intrusive lists in ascending `OrderId` order
  serve MassCancel, EndOfDay expiry and `for_each_order` without sorting.
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
- Differential fuzz against the oracle: see `docs/WORK_PLAN.md` for the
  integration run and log paths. The variant's own session ran 20 seeds x
  200k steps on both profiles (8M commands) with no divergence.

## Benchmarks

See `docs/WORK_PLAN.md` and the README performance table for the side by side
B versus C run taken at integration.

## SPEC readings flagged by the variant author

- A market FOK order runs the fill pre-check (SPEC 4.2 read literally).
- `phase()` of an unknown instrument returns Closed.
- An unknown `CommandType` is a no-op that returns `Applied`.
- A canceled market order echoes the price from its command.
- When the book is not crossed, the indicative imbalance uses each side's
  total leaves (SPEC 9).

All of these agree with the independent oracle; the SPEC conformance audit
is the separate check that both agree with the text.
