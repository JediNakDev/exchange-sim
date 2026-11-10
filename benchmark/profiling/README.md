These captures are historical artifacts from 0.1.2, not current measurements.

# Profiling

Captured profiling artifacts for the limit order book engine, plus the
scripts that produce them. Linux uses `perf` (the standard) and macOS
uses `/usr/bin/sample` + `xcrun xctrace`.

## Layout

```
profiling/
  run_perf.sh             # Linux: perf stat + perf record + perf report
  run_macos.sh            # macOS: sample + xctrace
  macos_capture/          # checked-in artifacts captured locally
    bench_stdout.txt        # Google Benchmark output for Mixed workload
    sample.txt              # /usr/bin/sample profile of `make benchmark` Mixed
    itch_sample_replay.txt  # /usr/bin/sample profile during ITCH replay hot path
    itch_replay_1M.txt          # 1M-msg synthetic replay throughput
    itch_replay_1M_validated.txt# same, with per-message diff vs reference book
    itch_replay_50M.txt         # 50M-msg replay (book grows to 14M live orders)
```

## How the captures were taken (macOS arm64, Apple M-series, macOS 26.2)

```bash
# Build the optimized binaries.
make benchmark itch-replay

# Sample the engine's hot path during ITCH replay. Generation runs first,
# so we sample several seconds into the run to catch the replay phase.
./build/perf/itch_replay --synth 50000000 > out.txt &
sleep 20
/usr/bin/sample $(pgrep -n itch_replay) 5 -file itch_sample_replay.txt -mayDie

# Throughput numbers (no profiler attached).
./build/perf/itch_replay --synth 1000000
./build/perf/itch_replay --synth 1000000 --validate   # per-msg diff vs reference book
./build/perf/itch_replay --synth 50000000
```

## Headline numbers

| Workload                                  | Throughput        | Notes                          |
|-------------------------------------------|-------------------|--------------------------------|
| ITCH replay, 1M msgs, fresh book          | 3.48 M msgs / sec | book end-state: 285K live      |
| ITCH replay, 50M msgs, growing book       | 1.06 M msgs / sec | book end-state: 14.3M live     |
| ITCH replay, 1M msgs, **per-msg validate**| 12.4 K msgs / sec | dominated by `std::map` snapshot diff |
| Google-Benchmark MixedWorkload            | 20.0 M ops / sec  | 60% query, 25% add, 10% cancel, 5% modify |

The 1M→50M throughput drop is real and measured: as `live_orders` grows
from ~285K to ~14M, the `std::unordered_map<OrderId, Order*>` hits its
rehash boundary repeatedly and L1/L2 hit rate drops on lookups. That
flat-hashmap pressure is the next target.

## Engine hot path (from `itch_sample_replay.txt`)

Top stack frames during steady-state replay, by sample count:

```
ItchReplayer::do_delete           → OrderBook::cancel_order            (dominant)
ItchReplayer::do_add              → OrderBook::add_order
                                  ├─ ensure_price_range                (ladder growth on first hit)
                                  └─ ObjectPool<Order>::allocate_block
```

`cancel_order` dominates because the synthetic mix is delete-heavy (~25%
deletes + ~14% cancels + ~7% executes), matching real exchange tapes
where the vast majority of orders are never executed.

## Reproducing on Linux

`run_perf.sh` uses Linux `perf` to capture the same shape of data, plus
hardware counters that macOS sandbox restricts:

```bash
# requires linux-tools-common (perf)
make benchmark
benchmark/profiling/run_perf.sh                  # writes perf_<timestamp>/
PERF_FILTER=AddOrder benchmark/profiling/run_perf.sh
```

Counters captured: `cycles`, `instructions`, `cache-references`,
`cache-misses`, `branch-instructions`, `branch-misses`,
`L1-dcache-loads`, `L1-dcache-load-misses`.

`perf record` uses `-F 999 --call-graph dwarf`, suitable for piping
through `stackcollapse-perf.pl | flamegraph.pl`.
