# Core and LOB refactor, 2026-11-08

Workstream 1 of [EXCHANGE_REFACTOR_PLAN_2026-11-08.md](EXCHANGE_REFACTOR_PLAN_2026-11-08.md): order books and foundational storage. Implementation by Opus 5.5 in worktree `/Users/jedi/.t3/worktrees/lob/t3-b3a9b033`, branch `t3/b3a9b033`, parent baseline `c49bf79`. Logs live under `/tmp/exsim-core-lob-20261108/`.

## Result by component

| Component | Result | Summary |
|---|---|---|
| `exsim::MatchingEngine` (`src/exsim/matching_engine.cpp`, `include/exsim/matching_engine.hpp`) | read-only FIFO query added | Preflight bounds, FIFO, replace, auctions, FOK, per-instrument iteration and arithmetic checked against SPEC and current tests. |
| `src/exsim/engine/{fixed_array,order_arena,price_ladder}.hpp` | reviewed, no change | Index arena, hot/cold split and three-level occupancy bitmap are sound. |
| `lob::FlatHashMap` | reviewed, no change | Historical findings 1, 18, 19, 20 are fixed in the current code; backward-shift delete is correct. |
| `lob::OrderBook` (`order_book.{hpp,cpp}`) | changed | Overflow, growth exception safety, level volume clamp, full-book matching, bounded mode switch, sparse snapshot walk. |
| `lob::ObjectPool` | changed | Leak on failed block bookkeeping fixed; exact reservation; free-node guarantee. |
| `lob::Order`, `lob::PriceLevel` | changed | Dead timestamp removed (exactly one cache line); clamping volume update removed. |
| `lob::engine::ShardedEngine` | changed | Shard books always bounded; out-of-band prices refused at submit; stop/submit race closed; per-batch counter update with no batch vector. |
| `lob::engine::SPSCQueue` | changed | Cached opposite index per side. |

## Defects found and fixed

Each was reproduced on the baseline before the change (`baseline-probes.log`, probe `probes/legacy_baseline_probe.cpp` built against copies of the baseline sources in `baseline-src/`) and re-run after it (`after-probes.log`, probe `probes/legacy_after_probe.cpp`).

1. **Price band arithmetic overflow.** `max_price - min_price + 1` and `price - min_price` were signed `Price` subtractions. A band of `[INT64_MIN, INT64_MAX]` overflowed into a zero-sized ladder. Baseline: release SIGSEGV (exit 139); UBSan reports signed overflow at `order_book.cpp:51` and `:107`. Now the span and index use exact unsigned arithmetic, and a band whose ladder cannot be addressed throws `std::length_error` at construction. A narrow band at either extreme works (`order_book_growth.narrow_band_at_extreme_prices_works`).
2. **Ladder growth was not exception safe.** Growth replaced the ladders first and then assigned the occupancy words. If that assignment threw `bad_alloc`, the ladders were wider than the bitmaps, and the next add wrote past them. Baseline ASan: heap-buffer-overflow in `set_active`. Now `ensure_price_range` builds all four replacements, then commits with `noexcept` swaps. After the change, the probe fails each growth allocation in turn (4 of them). After every failure the book is unchanged and a later growth succeeds, in release and under ASan/UBSan.
3. **Pool block leak.** `ObjectPool::allocate_block` held a raw `new Node[]` while `blocks_.push_back` could throw. Baseline: 1 allocation leaked. Now the block is owned by `unique_ptr` before the push. After the change: 0 leaked.
4. **Level volume clamped at `INT64_MAX`.** `PriceLevel::update_quantity(int64_t)` converted the unsigned volume to signed and clamped. With a resting quantity above `2^63`, a 5-lot fill reported 0 at the top of book. The function is removed; callers use exact unsigned arithmetic (`order_book.level_volume_is_exact_above_int64_max`).
5. **Full bounded book refused a marketable order.** The aggressor took a pool slot before matching, so a deterministic book at capacity returned `order_id 0` and no fills, even for an order that would trade and free a slot. The aggressor now matches from a stack `Order` and takes a pool slot only if a remainder rests (`order_book_bounds.full_book_rests_exactly_its_capacity_and_still_trades`, `partially_filled_aggressor_rests_in_full_book_after_freeing_a_slot`).
6. **Sharded engine could allocate terabytes in a worker.** Shard books grew their ladder for any price, so `submit_add(price = 1 << 40)` made the worker try an 8 TB ladder. Baseline: release killed (exit 137); ASan reports allocation-size-too-big in thread T1. Shard books are now always bounded (`book_config.allow_growth` is overridden), and `submit_add` rejects prices outside the configured band before claiming the producer (`sharded_engine.prices_outside_the_book_band_are_refused_at_submit`).
7. **Stop raced concurrent submissions.** `stop()` pushed a `Stop` command into every shard queue from the stopping thread while a producer could still be pushing. That made two writers on an SPSC queue, and work accepted after the `Stop` was never applied. Now `push_command` counts the command (`seq_cst`) and then re-checks `stopped_` (`seq_cst`). `stop()` sets `stopped_`, waits for the in-flight count to reach 0, and only then signals the workers. A submission either sees the stop and backs out, or is counted and applied before the workers exit. Only producers push. Test: `sharded_engine.stop_racing_the_producer_applies_every_accepted_submission` (a producer thread submits while the main thread stops; the book holds exactly the accepted orders). The baseline cannot pass this test. See the TSan section for the race evidence.
Not a defect, new coverage only: the baseline already cancelled a remainder that found no price-level slot. `order_book_bounds.level_exhaustion_cancels_remainder_and_freed_level_is_reused` now pins that behavior, including reuse of the freed level slot.

## Simplifications and efficiency

- **Growth versus deterministic mode is an explicit config field.** `OrderBookConfig::allow_growth` defaults to `true`, or to `false` under `LOB_DETERMINISTIC_POOL`.
  - Bounded (`false`): the limits are hard; `add_order` never allocates, rejects out-of-band prices and cancels a remainder that has no slot.
  - Growth (`true`): the limits are initial sizes. All growth (ladder, one free pool node of each kind, one map slot) happens before matching, so a failed allocation leaves the book unchanged and resting a remainder cannot allocate.
  - The ITCH replay tool keeps growth. The explicit setting permits bounded books in ordinary builds too; shard books always select bounded mode. Benchmark prices (9900 to 10100) are inside the default band.
- **Order is one cache line.** `entry_time` was written on every add, never read, and only meaningful under an unused macro. `Order` is now `alignas(64)` with `static_assert(sizeof(Order) == 64)`.
- **Removed dead state.** `ladder_initialized_`, `initialize_ladders`, `refresh_best_levels`, `clear()` and the custom destructor are gone; pool blocks own their storage. The pool's `block_count` and `growth_failures_` had no readers and are also gone.
- **ObjectPool reserve is exact.** It allocates `n - existing` nodes in one block, instead of rounding up to `BlockSize` blocks, so a 5-order bounded book holds exactly 5 (`object_pool.bounded_pool_holds_exactly_its_reservation_and_reuses_slots`). `T` must be trivially destructible, which is now asserted.
- **Snapshot walks occupied levels.** `get_snapshot(depth)` used to step tick by tick across the band. It now follows `find_prev_active` / `find_next_active` over the occupancy words (`order_book_query.snapshot_walks_sparse_levels_across_bitmap_words`).
- **Sharded worker.**
  - It no longer copies each batch into a `std::vector<Command>`; it applies commands as it pops them.
  - It updates the shared `inflight_` counter once per batch instead of once per command.
  - `apply_add` probes `has_order` only for the last fill, because matching consumes resting orders front to back.
- **SPSC queue.** Each side keeps a private copy of the other side's index, on its own cache line, and reloads the shared atomic only when the ring looks full or empty. `empty()` still reads both atomics for the journal worker. A two-thread test moves 2,000,000 values through a 64-slot ring in order.

No latency figure is claimed; benchmarks were not run, per the plan. The structural changes above are an eliminated per-batch vector copy, one atomic RMW per batch instead of per command, eliminated per-fill hash probes, eliminated per-tick snapshot scanning, and one cache line per order.

## Production engine review (matching semantics unchanged)

- **Preflight.** Worst-case event counts per operation are NewOrder/Replace `live + 3`, Cancel `1`, MassCancel `live + 1`, SetPhase `2 * live + 2 * books + 1`. All are checked against the `EventBuffer` before mutation, and `HandleResult::InsufficientOutput` leaves state unchanged.
- **Capacity.** SPEC 4.1 requires a slot even for an incoming order that fully fills. The engine's `CapacityExceeded` behavior is normative, so the legacy full-book change above deliberately does not apply here.
- **Per-instrument iteration.** EndOfDay expiry and the indicative imbalance already iterate the per-instrument order list, not the engine-wide list. `books` is reserved at construction, so book pointers stay stable.
- **Arithmetic.** FOK availability, auction uncrossing and tick distance math use widened or checked arithmetic.

**Cross-boundary finding (resolved by coordinated FIFO visitor).** `src/exsim/research/simulation_core.cpp:655` `measure_level_queue` calls `engine_.find_order(id)` for each order on a level. `find_order` walks the engine-wide ascending list, so measuring one level costs O(L * N). The recommended coordinated fix is for the research workstream to read order leaves from the level iteration the engine already exposes, or for the parent to approve a narrow engine accessor. Parent approved `for_each_level_order`, implemented on the level list and consumed by research without an ID scratch array. Matching behavior is unchanged.

## Validation

All builds used `-j1`. Commands were run from the worktree root.

| Check | Command | Log | Result |
|---|---|---|---|
| Baseline release build | `make -j1 MODE=release build/release/run_tests` | `baseline-release-build.log` | exit 0, 0 warnings |
| Baseline focused tests | `run_focused.sh ./build/release/run_tests ...` | `baseline-release-focused.log` | OVERALL 0 |
| Baseline defect probes | see the log header | `baseline-probes.log` | defects 1 to 6 reproduced |
| Release build | `make -j1 MODE=release build/release/run_tests` | `release-build-1.log` | exit 0, 0 warnings |
| Release focused tests | `run_focused.sh ./build/release/run_tests release-focused-1.log` | `release-focused-1.log` | OVERALL 0; order_book 33/33 (24 before), sharded_engine 9/9 (7 before), object_pool 2/2, spsc_queue 2/2 |
| Post-change probes | release, ASan/UBSan and `-DLOB_DETERMINISTIC_POOL` builds of `legacy_after_probe.cpp` | `after-probes.log` | every case exit 0 in release and ASan/UBSan (deterministic `growth_exception` exits 1 by design: bounded books refuse rather than grow) |
| Delegate ASan/UBSan focused | `run_focused.sh ...` | `debug-focused-3.log` | OVERALL 0 |
| Delegate TSan stop race | probe, baseline and after | `tsan.log` | baseline reports race; after exits 0, 20 rounds |
| Dependent targets | integrated parent build | `/tmp/exsim-parent-dependent-tools.log` | pass, warning-free |

`run_focused.sh` runs these filters: `order_book object_pool spsc_queue sharded_engine flat_hash_map itch_replay foundation. arena_ vector_ ProductionEngine Oracle fuzz_regression fuzz_known_bugs Journal. server_core_journal server_journal_worker md_book_builder md_feed_scenario md_allocation research_determinism server_core_allocation sim_bots_allocations gateway_ouch_core gateway_fix_core test_framework`. That covers reference/oracle, differential (`vector_`, `arena_`), canonical replay (`itch_replay`, `fuzz_regression`, `research_determinism`) and the allocation guards.

**Allocation evidence.** `order_book_bounds.full_book_rests_exactly_its_capacity_and_still_trades` asserts 0 allocations across adds, matching and rests on a bounded book under `testfw::AllocationScope`. The existing `order_book` allocation guards still pass. `AllocationScope` counts the calling thread only, so ShardedEngine worker allocations are covered structurally: books are bounded and the worker has no batch vector. They are not measured directly.

## Remaining limitations

- A legacy book in growth mode can still attempt a very large allocation for an outlier price (for example a corrupted ITCH price far from the band). It now throws and leaves the book unchanged instead of corrupting it, but it still allocates. Callers that need hard bounds set `allow_growth = false`.
- Legacy best-price recovery after a level empties is still a scan of up to `band / 64` occupancy words. The production engine's three-level bitmap does not have this cost; the legacy book was left single-level to avoid a rewrite.
- Midpoint overflow was fixed after parent review with quotient/remainder arithmetic preserving truncation toward zero. Narrow extreme-price regressions pass in delegate checks; integrated results follow in the final receipt.
- macOS ASan does not run LeakSanitizer by default. The leak evidence is the operator-new counting probe for the pool and growth paths, plus ASan's clean exit for the focused set. That is not a global no-leak claim.
- TSan coverage is limited to the sharded engine and SPSC tests listed above. No full-system TSan run was made.
- Not run, per the plan: full E2E, benchmarks, Linux.

## Parent takeover and review

Opus hit its API limit before committing. Parent preserved the worktree and copied its implementation into the combined checkout. Parent reviewed all core changes and additionally fixed aggregate legacy level-volume overflow, restored a pool slot after a throwing constructor, and removed the reservation count stored only for construction. The constructor-startup cleanup and midpoint fixes requested earlier were present in the preserved source. Startup failure probe log shows all 70 injected allocation points exit cleanly after the fix, versus 11 abnormal baseline exits; ASan probe also exits cleanly. See `/tmp/exsim-core-lob-20261108/startup-failure-probes.log`. Parent integrated tests and limitations are recorded in `EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md`.
