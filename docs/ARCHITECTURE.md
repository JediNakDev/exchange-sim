# exsim — Exchange Simulator Architecture

exsim turns the `lob` limit-order-book core into a full exchange simulator:
trading applications connect to it over real exchange protocols, trade
against each other (or against simulated liquidity), receive execution
reports and a market-data feed, and the whole session can be journaled,
replayed deterministically, and fault-injected for testing.

```
            order entry (TCP)                          market data (UDP)
  client ── OUCH 4.2 / SoupBinTCP ─┐              ┌── ITCH 5.0 / MoldUDP64 ──► subscribers
  client ── FIX 4.4 ───────────────┤              │
                                   ▼              │
                         ┌──────────────────┐     │
                         │  gateway layer   │  decode wire → Command
                         │  (per session)   │  encode Event → wire
                         └────────┬─────────┘     │
                                  │ Command       │ Event
                         ┌────────▼─────────┐     │
     journal  ◄──────────│ MatchingEngine   │─────┘
  (replayable)           │ pure, no I/O     │
                         └──────────────────┘
```

## Layers

| Layer | Path | Notes |
|---|---|---|
| Book core (legacy, market-data side) | `include/lob/`, `lib/src/order_book.cpp` | price-time book used to rebuild books from ITCH |
| Domain types | `lib/include/exsim/{types,command,event,instrument}.hpp` | protocol-neutral vocabulary |
| Matching engine | `lib/include/exsim/matching_engine.hpp`, `lib/src/exsim/` | pure function of the command stream; spec in `docs/SPEC.md` |
| Protocol codecs | `lib/include/exsim/protocol/` | zero-copy, zero-alloc encode/decode of wire formats |
| Gateways | `lib/include/exsim/gateway/` | per-session protocol state machines ↔ commands/events |
| Networking | `lib/include/exsim/net/` | non-blocking TCP/UDP, kqueue/epoll reactor |
| Simulation & test infra | `lib/include/exsim/sim/`, `tests/exsim/` | journal, replay, scenarios, reference model, fault injection |
| Research daemons | `src/exsim_{apid,traderd,exchanged}/` | API orchestration and independent trader/exchange workers; [lifecycle and transport](architecture/WORKERS.md) |
| Tools | `tools/<name>/` | real-time server, clients, replay, bench |

## Hot-path coding standard

"Hot path" = anything executed per message after startup: engine `handle()`,
gateway decode/encode, market-data publishing, the network loop.

1. **No heap allocation after startup.** Size pools, maps and buffers from
   config at construction (`lob::ObjectPool`, `lob::FlatHashMap`, fixed
   arrays, pre-reserved vectors that are only `clear()`ed). When capacity runs
   out, reject deterministically instead of growing. `exsim_bench` and the
   allocation-guard test enforce this.
2. **No exceptions, no RTTI, no virtual dispatch, no `std::function`,
   no `std::string`, no iostreams** in per-message code. Use templates,
   `switch` on tags, and `std::string_view`/`std::span` over caller buffers.
3. **No locks.** Cross-thread hand-off only via SPSC queues
   (`include/lob/engine/spsc_queue.hpp`).
4. **Integer prices** (4 implied decimals) — never floating point.
5. **Wire codecs use explicit byte loads/stores** (big-endian helpers), never
   `reinterpret_cast` of packed structs: no alignment or aliasing UB.
6. **Branch hints** (`LOB_LIKELY`/`LOB_UNLIKELY`) on error/rare paths only.
7. **Data layout**: hot fields first, `static_assert` on sizes of hot structs.
8. **Determinism**: the engine reads no clocks and no randomness; timestamps
   come in on commands.

## Conventions

* C++20, `-Wall -Wextra -Wpedantic -Wshadow` clean (zero warnings).
* `snake_case` functions/variables, `PascalCase` types, `trailing_` members,
  `kConstants`, header guards `EXSIM_<PATH>_HPP`.
* Tests: `TEST_CASE(suite, name)` in `tests/exsim/**.cpp` (framework in
  `tests/framework/test.hpp`); `make test`, `make test MODE=debug` (ASan/UBSan).
* Every new source file is picked up by the Makefile automatically — do not
  list files in the Makefile.

## Build modes

`release` (default, -O2 -g, asserts on), `debug` (ASan+UBSan), `tsan`,
`perf` (-O3 -march=native -flto -DNDEBUG), `bench` (perf + Google Benchmark).

## Foundation integration gates

The legacy OrderBook and ShardedEngine are not production simulator
infrastructure. Review findings 16, 36 and 37 remain open: symbol isolation,
bounded fill storage, and cleanup of completed client mappings require a
separate focused implementation and sanitizer/allocation regressions.
The canonical engine contract is SPEC.md, including bounded EventBuffer
preflight and field-based replay. Headers alone do not implement an engine.
