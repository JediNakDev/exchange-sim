# LOB Benchmark Suite

## Iteration 1.0.0

### Methodology

- **Google Benchmark**: `DoNotOptimize` / `ClobberMemory` prevent compiler optimizations
- **Warm-up**: 10,000 ops before measurement to stabilize CPU frequency and prime caches
- **Realistic Workload**: Pre-generated random data defeats branch prediction
- **CPU Pinning**: `--core=N` flag or Linux `taskset` for single-core execution
- **Statistics**: Mean, P50, P99, P99.9, P99.99, Min, Max, StdDev

### Benchmarks

| Benchmark | Description |
|-----------|-------------|
| `BM_AddOrder` | Passive orders (no matching) |
| `BM_MatchOrder` | Aggressive orders crossing the spread |
| `BM_CancelOrder` | Cancel existing orders |
| `BM_ModifyOrder` | Modify order quantities |
| `BM_GetBestBid` | Query best bid price |
| `BM_GetBestAsk` | Query best ask price |
| `BM_GetSpread` | Query bid-ask spread |
| `BM_GetSnapshot` | Get order book snapshot (depth 5/10/20) |
| `BM_MixedWorkload` | 60% query, 25% add, 10% cancel, 5% modify |
