# Legacy order book

Standalone code from the original limit order book project lives here.

- `examples/`: direct OrderBook usage.
- `include/` and `src/`: the old sharded engine, thread pinning and ITCH representation.
- `test/`: sharding and replay tests, standalone benchmarks and profiling captures.
- `results/`: historical benchmark CSV files.

```sh
make -j4 legacy
make legacy-bench
```

Binaries go in `build/<mode>/legacy/`. Run these commands from the repository root.
`make examples` builds just the book example; `make legacy-tools` builds the ITCH replay tool.

The OrderBook itself stays in `lib/include/exchange/` and `lib/src/order_book.cpp` because
exsim now uses it for resting orders. Its pools, maps, price levels, shared types
and journal SPSC queue also stay in the active tree.
Exsim codec tests still compare bytes against the old ITCH representation.
The exsim production build does not include or link the legacy sharded engine.
