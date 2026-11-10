# Profiling

Build the optimized benchmark binary first:

```sh
make -j4 bench-build
```

Run the script for your platform:

```sh
benchmark/profiling/run_macos.sh
benchmark/profiling/run_perf.sh
```

Both scripts default to the mixed workload and write to a timestamped directory under `benchmark/profiling/`. Set `PERF_FILTER` to select another benchmark.

The macOS script uses `xcrun xctrace` and `/usr/bin/sample`. CPU counter capture needs the appropriate Apple developer tools and permissions. Linux uses `perf stat`, `perf record`, and `perf report`; it also needs `taskset`. Set `PERF_CORE` to choose an available Linux CPU.

## Replay

```sh
make -j4 itch-replay
./build/perf/itch_replay --synth 1000000
./build/perf/itch_replay --synth 100000 --validate
```

Synthetic stream generation happens before timed replay. Validation adds reference-book and snapshot work, so compare validated runs separately.

## Historical captures

Files under `macos_capture/` were retained from 0.1.4. They are not measurements of the current code. Recorded symbols can refer to older data structures.

Keep the binary revision, build flags, workload, hardware, and profiler settings with new captures. A sampled stack alone does not establish why throughput changed.
