# Profiling

Build the benchmark from the repository root:

```sh
make bench
```

On Linux, use perf:

```sh
test/benchmark/profiling/run_perf.sh
```

On macOS, use sample and Instruments:

```sh
test/benchmark/profiling/run_macos.sh
```

Both scripts accept an output directory. `PERF_FILTER` selects the benchmark;
Linux also supports `PERF_CORE` for CPU pinning.
The captures already in this directory are historical, not measurements of the current build.
