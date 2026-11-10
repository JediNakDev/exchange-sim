# LOB Benchmark Suite

Restored from `0.1.2` and built against the current `lib/src/order_book.cpp`.
The historical sharding harness lives in `support/` and is only linked into this suite.

Install Google Benchmark (`brew install google-benchmark` on macOS), then run:

```sh
make lob-test
make lob-test MODE=debug
make benchmark
make benchmark-filter FILTER=GetBestBid
make benchmark-run ARGS="--csv=output/lob_results.csv --core=0"
make itch-replay
./build/perf/itch_replay --synth 100000 --validate
```

The default CSV output is `build/bench/lob_results.csv`. Custom output directories must exist.
The CSVs in `results/` retain their historical data under the new release names.
See [results/README.md](../results/README.md) for the version mapping.
Release 0.1.2 did not record a separate results CSV.

The suite covers add, match, cancel, modify, best bid/ask, spread, snapshot,
mixed and cancel-heavy workloads, plus sharded throughput and latency.
It uses Google Benchmark, seeded workloads, warmup, CPU pinning where available,
and latency percentiles. Timings include per-operation clock overhead.

`make lob-test` runs the original order, matching, query and ITCH replay tests
with assertions enabled. ITCH replay checks 100,000 synthetic messages against
a reference book after every message. Existing tests under `test/` remain available
through `make test`.

See `profiling/` for the restored scripts and historical captures.
