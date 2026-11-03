# lob iteration history (1.0.0 to 1.4.0)

This is the original single-book `lob` project history, moved here from the top-level README when the repository became the exsim exchange simulator. The numbers below are from those iterations and were not re-measured.


A high-performance limit order book implementation in modern C++17.

## Building

```bash
make             # Build the demo
make test        # Run tests (includes ITCH replay validation)
make benchmark   # Build benchmark suite
make itch-replay # Build the ITCH 5.0 replay tool
make clean       # Clean build artifacts
```

## ITCH 5.0 Replay & Validation

The engine is driven end-to-end from a NASDAQ ITCH 5.0 message stream and
validated against an independent reference book (`std::map` + `std::list`)
on every message. See [tools/itch_replay/](../tools/itch_replay/) for the
driver, reference oracle, and synthetic stream generator.

```bash
make itch-replay
./build/itch_replay --synth 1000000              # 1M synthetic msgs
./build/itch_replay --synth 1000000 --validate   # diff vs reference book on every msg
./build/itch_replay file.itch                    # real NASDAQ ITCH file
./build/itch_replay file.itch50 --framed         # 2-byte BE length-prefixed dump
```

`make test` runs a 100,000-message synthetic ITCH stream through the
engine and the reference book, diffing the top 10 levels of both books
**after every single message**. Zero divergence is required to pass.

| Run | Messages | Throughput | Validation |
|-----|----------|------------|------------|
| Replay, fresh book        | 1,000,000  | 3.48 M msgs/s | n/a |
| Replay, growing to 14M live orders | 50,000,000 | 1.06 M msgs/s | n/a |
| Replay with per-msg diff vs reference | 1,000,000  | 12.4 K msgs/s | 0 mismatches |

See [benchmark/profiling/README.md](../benchmark/profiling/README.md) for
profiling artifacts (perf scripts for Linux, sample/xctrace for macOS)
and the captured hot-path breakdown.



## Iteration 1.4.0 (Latest)

### Key Changes

- **ITCH 5.0 replay driver**: Drives the production `OrderBook` end-to-end from a NASDAQ ITCH 5.0 message stream (A/F/E/X/D/U). Exchange-ref → internal-id mapping; supports both raw and length-prefixed (`.itch50`) framing.
- **Reference book validator**: An independent `std::map<Price, std::list<Order>>` oracle. `make test` diffs the top 10 levels of both books **after every message** on a 100,000-msg synthetic stream, zero divergence required.
- **Deterministic synthetic ITCH generator**: Wire-format A/F/E/X/D/U emitter with realistic mix; non-marketable by construction; preserves side across `U` replaces.
- **Profiling infrastructure**: `benchmark/profiling/` ships Linux `perf` (stat + record + report) and macOS (`sample` + `xctrace`) scripts plus checked-in captures showing the real hot path during replay (`do_delete → cancel_order` dominant, `do_add → ensure_price_range` + `ObjectPool::allocate_block` secondary).

### Replay Throughput (Apple M-series, macOS 26.2)

| Workload | Throughput | Notes |
|----------|------------|-------|
| 1M synthetic ITCH msgs, fresh book      | 3.48 M msgs/s | 285K live orders end-state |
| 50M synthetic ITCH msgs, growing book   | 1.06 M msgs/s | 14.3M live orders end-state, flat-hashmap rehash + cache pressure |
| 1M msgs **with per-msg diff** vs reference | 12.4 K msgs/s | 0 mismatches across full run |

See [benchmark/profiling/README.md](../benchmark/profiling/README.md) for captured artifacts and methodology.

---

## Iteration 1.3.0

### Key Changes

- **Zero heap allocations on the hot path**: All `Order` and `PriceLevel` objects served from pre-allocated object pools. Fill buffer reused across `match_order` calls, no `std::vector<Fill>` allocation per match. Deterministic mode (`-DLOB_DETERMINISTIC_POOL`) enforces zero-allocation at compile time.
- **Templatized matching on Side**: `match_order`, `add_order_to_book`, and `remove_order_from_book` are `template<Side S>`, the compiler generates separate BUY/SELL code paths, eliminating runtime branch in the inner matching loop.
- **Branch hints**: `LOB_LIKELY`/`LOB_UNLIKELY` annotations on all hot-path branches (pool allocation, price range checks, matching loop predicates).
- **Order struct layout**: `Side` narrowed to `uint8_t`, fields reordered for zero internal padding. Hot fields (id, price, quantity, pointers) packed into first 48 bytes (single cache line).
- **ITCH 5.0 parser**: Zero-copy binary protocol parser for Add Order (A/F), Order Executed (E), Order Cancel (X), Order Delete (D), and Order Replace (U) messages.
- **Realistic workload benchmark**: 93% cancel, 5% add, 2% modify, matching real exchange traffic patterns where the vast majority of orders are cancelled before execution.
- **Cycle counter**: Cross-platform `rdtsc` (x86) / `CNTVCT_EL0` (ARM64) cycle timer for sub-nanosecond measurement resolution.

### Architecture

- Each shard runs on a **dedicated pinned CPU core** (`pthread_setaffinity_np` on Linux, `thread_policy_set` on macOS), eliminating context-switch jitter and core migration.
- Single-threaded per shard, no locks, no atomics on the hot path.
- SPSC queues between producer and consumer threads.

### Performance Summary

**AddOrder 26 ns | MatchOrder 47 ns | CancelHeavy 27 Mops/s**

| Benchmark | Mean | Throughput | vs 1.2.0 |
|-----------|------|------------|----------|
| AddOrder | 26 ns | 38.9 Mops/s | **1.1x faster** (was 29 ns) |
| MatchOrder | 47 ns | 21.4 Mops/s | comparable |
| CancelOrder | 16 ns | 63.6 Mops/s | comparable |
| MixedWorkload | 27 ns | 21.6 Mops/s | comparable |
| CancelHeavyWorkload | 19 ns | 27.0 Mops/s | **NEW** |

### Tail Latency

| Operation | P99 | P99.9 | P99.99 | vs 1.2.0 |
|-----------|-----|-------|--------|----------|
| AddOrder | 42 ns | 250 ns | 1.8 us | P99.9 **4x better** (was 1 us) |
| MatchOrder | 84 ns | 167 ns | 250 ns | P99.99 **1.3x better** (was 333 ns) |
| CancelHeavyWorkload | 83 ns | 666 ns | 833 ns | **NEW** |
| MixedWorkload | 167 ns | 250 ns | 500 ns | P99.99 **7x better** (was 3.5 us) |

See `results/benchmark_result_1.3.0.csv` for full data.

---

## Iteration 1.2.0

### Key Changes

- **Tick-indexed ladders**: Contiguous arrays replace `std::map` + `std::unordered_map` for price levels. Active levels tracked via bitset with `__builtin_ctzll`/`__builtin_clzll` scanning.
- **Object pool with growth control**: Pre-allocated pools for `Order` and `PriceLevel`. Deterministic mode (`-DLOB_DETERMINISTIC_POOL`) prevents runtime heap allocation.
- **Sharded engine**: SPSC queue per shard, strict single-producer ownership, batched consumer processing, thread pinning.

### Performance Summary

**AddOrder 28 ns | MatchOrder 46 ns | MixedWorkload 22.8 Mops/s**

| Benchmark | Mean | Throughput | vs 1.1.0 |
|-----------|------|------------|----------|
| AddOrder | 28 ns | 35.0 Mops/s | **1.9x faster** (was 54 ns) |
| MatchOrder | 46 ns | 22.0 Mops/s | **2.6x faster** (was 118 ns) |
| MixedWorkload | 28 ns | 21.2 Mops/s | **1.6x faster** (was 46 ns) |
| CancelOrder | 15 ns | 65.3 Mops/s | comparable |
| ModifyOrder | 16 ns | 61.3 Mops/s | comparable |

### Tail Latency vs 1.1.0

| Operation | P99 | P99.99 | vs 1.1.0 |
|-----------|-----|--------|----------|
| AddOrder | 42 ns | 1.6 us | P99.99 **7x better** (was 11 us) |
| MatchOrder | 84 ns | 333 ns | P99.99 **1.9x better** (was 625 ns) |
| MixedWorkload | 125 ns | 3.5 us | P99 **2.7x better** (was 333 ns) |

### Sharded Throughput (Synthetic Add Flow)

Each row shows `shards / consumer batch size`. Producer submits 100k orders; workers process in parallel.

| Shards / Batch | Throughput |
|----------------|------------|
| 1 / 64 | 9.69 Mops/s |
| 2 / 64 | 9.40 Mops/s |
| 4 / 64 | 10.55 Mops/s |
| 8 / 64 | 8.69 Mops/s |
| 1 / 256 | 9.61 Mops/s |
| 2 / 256 | **13.40 Mops/s** |
| 4 / 256 | 10.60 Mops/s |
| 8 / 256 | 8.74 Mops/s |

### Sharded End-to-End Latency

Submit-to-completion latency per order, including queue transit and matching.

| Shards / Batch | Mean | P95 | P99 | P99.9 |
|----------------|------|-----|-----|-------|
| 1 / 256 | 348 ns | 417 ns | 875 ns | 14.1 us |
| 2 / 256 | 529 ns | 958 ns | 3.7 us | 9.1 us |
| 4 / 256 | 1.6 us | 2.3 us | 13.8 us | 35.3 us |

See `results/benchmark_result_1.2.0.csv` for full data.

## Iteration 1.1.0

Implemented optimizations based on ["How to Build a Fast Limit Order Book"](https://web.archive.org/web/20110219163448/http://howtohft.wordpress.com/2011/02/15/how-to-build-a-fast-limit-order-book/) article.

### Key Changes

- **Integer Prices**: Changed from `double` to `int64_t` (ticks) for faster comparisons and better hashing
- **Intrusive Doubly-Linked Lists**: Orders contain `prev/next` pointers for O(1) removal
- **Hash Maps**: `std::unordered_map` for O(1) price level and order lookup
- **Binary Search Tree**: Price levels organized in BST for O(log M) insertion of new levels
- **Cached Best Bid/Ask**: Direct pointer access (`highest_buy_`/`lowest_sell_`) for O(1) best price queries

### Performance Summary

**AddOrder 47 ns | MatchOrder 117 ns | MixedWorkload 16.6 Mops/s**

#### Significant Improvements vs 1.0.0

| Operation | Mean | Improvement |
|-----------|------|-------------|
| AddOrder | 47 ns | **2.3x faster** (was 109 ns) |
| CancelOrder | 16 ns | **1.4x faster** (was 23 ns) |
| ModifyOrder | 15 ns | **1.6x faster** (was 24 ns) |
| MatchOrder | 117 ns | **1.4x faster** (was 160 ns) |
| MixedWorkload | 42 ns | **1.6x faster** (was 69 ns) |

#### Tail Latency Improvements

| Operation | P99 | P99.99 | vs 1.0.0 |
|-----------|-----|--------|----------|
| AddOrder | 84 ns | 1.8 µs | P99 **9x better** (was 792 ns), P99.99 **19x better** (was 34 µs) |
| MatchOrder | 209 ns | 334 ns | P99.99 **69x better** (was 23 µs) |
| MixedWorkload | 292 ns | 708 ns | P99.99 **28x better** (was 20 µs) |

### Time Complexity

| Operation       | Complexity | Notes |
|-----------------|------------|-------|
| `add_order` (passive) | O(1) / O(log M) | O(1) at existing limit, O(log M) for new price level |
| `add_order` (aggressive) | O(F + L log M) | Includes matching; see below |
| `cancel_order`  | O(1) | Hash lookup + intrusive list removal |
| `modify_order`  | O(1) | Hash lookup + quantity update |
| `get_best_bid`  | O(1) | Cached pointer |
| `get_best_ask`  | O(1) | Cached pointer |
| `get_snapshot`  | O(D) | D = depth requested |

**Matching complexity: O(F + L log M)**
- **F** = number of fills (resting orders matched)
- **L** = number of price levels fully exhausted (L ≤ F)
- **M** = total price levels on the opposing side

| Scenario | Complexity | Example |
|----------|------------|---------|
| Passive (no cross) | O(1) | Bid below best ask |
| Single partial fill | O(1) | Match one order, level remains |
| Single level exhausted | O(log M) | Clear one price level |
| Multi-level sweep | O(F + L log M) | Large aggressive order |

---

## Iteration 1.0.0

- **Price-Time Priority**: Orders matched by best price first, then arrival time (FIFO)
- **Efficient Data Structures**: `std::map` for price levels, `std::list` for order queues, `std::unordered_map` for O(1) order lookup
- **Clean API**: Structured results with I/O separated from business logic
- **Modern C++17**: `std::optional`, `[[nodiscard]]`, namespaces, const-correctness

### Performance Summary

**AddOrder 109 ns | MatchOrder 160 ns | MixedWorkload 11 Mops/s**

| Benchmark | Mean | P99 | P99.99 |
|-----------|------|-----|--------|
| GetBestBid | 19 ns | 42 ns | 1.4 µs |
| GetBestAsk | 17 ns | 42 ns | 166 ns |
| GetSpread | 18 ns | 42 ns | 167 ns |
| CancelOrder | 23 ns | 125 ns | 459 ns |
| ModifyOrder | 24 ns | 42 ns | 167 ns |
| AddOrder | 109 ns | 792 ns | 34 µs |
| MatchOrder | 160 ns | 292 ns | 23 µs |
| MixedWorkload | 69 ns | 459 ns | 20 µs |

See [benchmark/README.md](../benchmark/README.md) for methodology.

### Potential Improvements

1. Memory pool allocators
2. Market orders, IOC, FOK order types
3. Thread safety / lock-free structures
4. SIMD optimizations for batch operations
