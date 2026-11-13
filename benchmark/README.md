# Benchmarks

Install Google Benchmark, then run from the repository root:

```sh
# macOS
brew install google-benchmark

# Ubuntu
sudo apt-get install libbenchmark-dev

make -j4 bench
```

This builds the optimized C++23 binary and runs the suite. It uses native CPU tuning, LTO, and bounded order-book pools. Compilation is parallel; the benchmark runner starts once after linking.

## Selected runs

```sh
make -j4 bench FILTER=GetBestBid
make -j4 bench FILTER=MixedWorkload ARGS="--csv=output/exsim_results.csv --core=0"
make -j4 bench ARGS="--benchmark_list_tests=true"
make -j4 bench-build
```

`FILTER` is a Google Benchmark regular expression. `ARGS` passes runner options. `bench-build` only builds, for profiling or direct runs.

The binary is `build/bench/exsim_benchmark`. CSV output defaults to `build/bench/exsim_results.csv` and is replaced on each run. Create custom output directories first. Use a separate `BENCH_BUILD` directory when building for another platform.

## Coverage

The suite measures add, match, cancel, modify, best bid/ask, spread, snapshot, mixed, and cancel-heavy operations. Sharded cases use a benchmark-only harness in `utils/sharded_engine.*`. They do not measure production exchange threading or API performance.

Workloads use fixed seeds and warmup. Latency samples include clock overhead. The custom CSV keeps the first sample batch for each benchmark name; it is not a combined summary of every Google Benchmark iteration.

CPU affinity is attempted. Linux can pin a thread to a CPU. macOS uses an affinity hint, which does not guarantee a specific core.

## Replay

```sh
make -j4 itch-replay
./build/perf/itch_replay --synth 100000 --validate
make -j4 exchange-test
```

Replay drives the standalone order book. Validation compares it with a reference book. It does not test TCP gateways or the management API.

See [profiling](profiling/README.md) for capture scripts and [historical results](../results/README.md) for older measurements.
