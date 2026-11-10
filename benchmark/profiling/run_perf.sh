#!/usr/bin/env bash
# Linux perf-based profiling for the LOB benchmark binary.
#
# Captures:
#   - perf stat counters (cycles, instructions, cache-misses, branch-misses,
#     L1-dcache-load-misses) over the full benchmark run
#   - perf record sampled call stacks → flamegraph-ready perf.data
#   - perf annotate top hotspots
#
# Usage:
#   benchmark/profiling/run_perf.sh [output_dir]
#
# Defaults: writes artifacts under benchmark/profiling/perf_$(date +%s)/
# Requires: linux-tools-common (perf), repo built via `make benchmark`.
set -euo pipefail

BENCH=build/bench/lob_benchmark
OUT=${1:-benchmark/profiling/perf_$(date +%Y%m%d_%H%M%S)}
CORE=${PERF_CORE:-0}
FILTER=${PERF_FILTER:-Mixed}

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not found — run 'make benchmark' first" >&2
    exit 1
fi
mkdir -p "$OUT"

echo "=== perf stat (cache + branch counters) ==="
perf stat -e cycles,instructions,cache-references,cache-misses,branch-instructions,branch-misses,L1-dcache-loads,L1-dcache-load-misses \
    -o "$OUT/perf_stat.txt" \
    taskset -c "$CORE" \
    "$BENCH" --benchmark_filter="$FILTER" --benchmark_min_time=3s
echo "  → $OUT/perf_stat.txt"

echo "=== perf record (sampled call stacks) ==="
perf record -F 999 --call-graph dwarf -o "$OUT/perf.data" -- \
    taskset -c "$CORE" \
    "$BENCH" --benchmark_filter="$FILTER" --benchmark_min_time=2s
echo "  → $OUT/perf.data"

echo "=== perf report (top symbols) ==="
perf report -i "$OUT/perf.data" --stdio --no-children -n --percent-limit=0.5 \
    > "$OUT/perf_report.txt"
echo "  → $OUT/perf_report.txt"

echo
echo "Artifacts in $OUT/. For a flamegraph:"
echo "  perf script -i $OUT/perf.data | stackcollapse-perf.pl | flamegraph.pl > $OUT/flame.svg"
