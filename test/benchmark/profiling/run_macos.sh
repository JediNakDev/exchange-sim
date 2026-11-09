#!/usr/bin/env bash
# macOS-equivalent of run_perf.sh, using Apple's profiling tools.
#
# - `xcrun xctrace record --template "CPU Counters"` captures cycles,
#   instructions, branch mispredicts, and cache misses via the Apple
#   Silicon PMU. Open the resulting .trace bundle in Instruments to
#   browse hotspots.
# - `sample` captures a sampled call-stack profile (similar to perf record).
#
# Usage:
#   test/benchmark/profiling/run_macos.sh [output_dir]
set -euo pipefail

BENCH=build/bench/benchmark
OUT=${1:-test/benchmark/profiling/macos_$(date +%Y%m%d_%H%M%S)}
FILTER=${PERF_FILTER:-Mixed}

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not found — run 'make bench' first" >&2
    exit 1
fi
mkdir -p "$OUT"

echo "=== xctrace: CPU counters ==="
xcrun xctrace record \
    --template 'CPU Counters' \
    --launch -- "$BENCH" --benchmark_filter="$FILTER" --benchmark_min_time=2s \
    --output "$OUT/cpu_counters.trace" || \
    echo "(xctrace requires developer tools; skipping if unavailable)"

echo "=== sample: call-stack profile ==="
"$BENCH" --benchmark_filter="$FILTER" --benchmark_min_time=5s &
PID=$!
sleep 0.3
/usr/bin/sample "$PID" 3 -file "$OUT/sample.txt" -mayDie || true
wait "$PID" 2>/dev/null || true
echo "  → $OUT/sample.txt"

echo
echo "Artifacts in $OUT/. Open the .trace in Instruments.app for the GUI."
