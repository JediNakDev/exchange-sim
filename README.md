# exsim: exchange simulator

exsim is a C++20 exchange simulator for developing, testing and researching
trading applications. Clients are meant to connect over real exchange
protocols, trade against each other or against simulated liquidity, and
receive execution reports and a market-data feed. Every session can be
journaled, replayed deterministically and fault-injected. A deterministic
research mode runs strategies against the same matching engine on simulated
time for execution and market-making research.

It grew out of `lob`, a single-instrument limit order book. That history and
its benchmarks are in [docs/LOB_HISTORY.md](docs/LOB_HISTORY.md).

## Status (2026-11-03)

Only rows marked **integrated** are on this branch and tested. Everything else
is in progress or planned and must not be read as working.

| Component | State |
| --- | --- |
| Matching engine: price-time priority, phases and auctions, market/IOC/FOK, post-only, STP, replace and partial cancel, mass cancel, expiry | **integrated** (`src/exsim/matching_engine.cpp`, index arena with hot/cold split; see [engine variants](docs/engine-variants/)) |
| Independent reference oracle, differential fuzzer with shrinker, scenario DSL, canonical journal and replay | **integrated** |
| Wire codecs: OUCH 4.2, SoupBinTCP 3.0, FIX 4.4, ITCH 5.0, MoldUDP64 | **integrated** (codec level, no sessions yet) |
| Networking: kqueue/epoll poller, TCP server and client, UDP and multicast | **integrated** |
| Admin command parser, server config, background bots (market maker, noise taker), position ledger | **integrated** as components, not yet wired into a server |
| Legacy `lob` fixes: per-symbol books per shard, bounded fill output, client mapping reclamation | **integrated** |
| OUCH and FIX gateways, ITCH publisher and feed handler, exchange core, research runner | in progress |
| Three engine layouts compared on one machine (A tick ladder, B sorted vectors, C index arena), independent review of C, SPEC conformance audit | **done** ([engine variants](docs/engine-variants/), [SPEC_CONFORMANCE.md](docs/SPEC_CONFORMANCE.md)) |
| Coverage-guided fuzzing, SBE over FIXP order entry, research API contract and UI | in progress or queued |
| Server binary, end-to-end loopback tests, wire-to-wire latency benchmark, C++ HTTP/OpenAPI research API, final independent review | planned |

Live work, task IDs and verification logs: [docs/WORK_PLAN.md](docs/WORK_PLAN.md).

## Quick start

Requires a C++20 compiler (Apple clang 21, GCC 14 and clang 19 are tested) and
GNU Make.

```sh
make test                                     # release build, all tests
make test MODE=debug                          # ASan + UBSan
make test MODE=tsan                           # ThreadSanitizer
make test FILTER=Journal                      # substring filter on test names
make tools                                    # exsim_bench, exsim_fuzz, exsim_replay, exsim_scenario, itch_replay

./build/release/exsim_scenario tests/scenarios/fifo.scn
./build/release/exsim_fuzz --profile mixed --seeds 200 --steps 10000
make tools MODE=perf && ./build/perf/exsim_bench --ops 2000000
```

CI (`.github/workflows/ci.yml`) builds and tests with `-Werror` on Linux g++
and clang++ (release and debug), Linux clang++ TSan, and macOS clang (release
and debug), then runs a zero-allocation benchmark smoke test.

## Design

```
            order entry (TCP)                          market data (UDP)
  client -- OUCH 4.2 / SoupBinTCP --+              +-- ITCH 5.0 / MoldUDP64 --> subscribers
  client -- FIX 4.4 ----------------+              |
                                    v              |
                          +------------------+     |
                          |  gateway layer   |  decode wire -> Command
                          |  (per session)   |  encode Event -> wire
                          +--------+---------+     |
                                   | Command       | Event
                          +--------v---------+     |
     journal  <-----------| MatchingEngine   |-----+
  (replayable)            | pure, no I/O     |
                          +------------------+
```

- The engine is a pure function from `Command` to `Event`s with no clock, I/O
  or threads. Same inputs give the same outputs, which is what makes journal
  replay, differential fuzzing and the research mode possible.
- Prices are 4-decimal fixed-point integers. Wire formats use explicit
  big-endian codecs.
- Hot paths allocate nothing after setup and avoid exceptions, virtual
  dispatch, `std::function`, `std::string` and iostreams. Output goes to a
  bounded `EventBuffer`; every `handle` call preflights the output space it
  needs and returns `InsufficientOutput` without changing state if it would
  not fit.
- The server is single-threaded: submit a command, dispatch the resulting
  events to every session, then flush market data. Gateway state is split
  into per-user state for the trading day and per-connection state.

Normative documents: [SPEC.md](docs/SPEC.md) (matching semantics),
[GATEWAYS.md](docs/GATEWAYS.md) (protocol mapping),
[SERVER.md](docs/SERVER.md) (runtime, gateway contract, admin, faults),
[ARCHITECTURE.md](docs/ARCHITECTURE.md), and
[PRODUCT.md](docs/PRODUCT.md) for the research product and UI direction.

## Testing

| Layer | What it checks |
| --- | --- |
| Unit and contract tests (`tests/`) | about 350 test cases: codecs, networking, engine contracts, journal, bots, ledger, legacy book |
| Reference oracle (`tests/exsim/reference/`) | a deliberately simple second implementation of SPEC.md, written independently of the production engine |
| Differential fuzzer (`tools/exsim_fuzz`) | random command streams through engine and oracle, compared event by event, with a shrinker for minimal reproducers |
| Scenarios (`tests/scenarios/*.scn`) | 39 hand-written exchange situations with expected events (16 derived line by line from the spec), run against both implementations; a failure names the file, line and field |
| Journal and replay | canonical serialization, corruption and truncation detection, bit-identical replay |
| Allocation guards | per-thread counting of every `operator new` form inside `run_tests`; the engine, journal, networking and bot hot paths must report zero |
| Sanitizers | the full suite under ASan/UBSan and TSan |

Engine validation so far: 1000 seeds x 20000 steps for each fuzz profile
(40M commands) with no divergence between the production engine and the
oracle, run separately for the current engine and the one it replaced.
Differential agreement does not prove both match the spec, so a separate
hand-derived SPEC conformance audit checked 82 rules against both engines and
the oracle and added 16 scenarios ([SPEC_CONFORMANCE.md](docs/SPEC_CONFORMANCE.md));
it found no conformance failure, and its ambiguities are now settled in the
spec text.

## Performance

Matching engine, `exsim_bench --ops 2000000`, perf build (`-O3 -march=native
-flto`), Apple M6 macOS, zero allocations in every workload. The production
engine (C, index arena) and the previous one (B, sorted level vectors) were
alternated three times in one session under the same background load, so the
comparison is fair but the absolute numbers are pessimistic. Median of three:

| Workload | C: Mops/s | C: mean ns | C: p99 ns | B: Mops/s | C / B |
| --- | ---: | ---: | ---: | ---: | ---: |
| passive_add | 11.91 | 84 | 291 | 6.61 | 1.80x |
| cancel | 7.59 | 132 | 458 | 4.28 | 1.77x |
| aggressive | 12.99 | 77 | 291 | 10.38 | 1.25x |
| mixed | 13.11 | 76 | 333 | 7.23 | 1.81x |

On a quieter machine the variant author measured 15.9, 9.0, 16.6 and 17.5
Mops/s for C. A third variant (A, tick ladder) was also benchmarked against C
over seven alternating rounds: C was 1.10x to 1.19x faster on passive,
aggressive and mixed flow and A about 5% faster on pure cancels. Layouts,
measurements and rejected optimizations are recorded in
[docs/engine-variants/](docs/engine-variants/). Wire-to-wire latency
through the gateways will be measured once the server binary exists.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/exsim/`, `src/exsim/` | simulator: domain types, engine, protocol codecs, networking, gateways, server components, simulation and research |
| `include/lob/`, `src/` | original limit order book and sharded engine |
| `tests/` | test framework, unit tests, oracle, scenarios |
| `tools/` | bench, fuzz, replay, scenario runner, ITCH replay |
| `benchmark/` | Google Benchmark suite for the original book |
| `docs/` | specifications, reviews, plans |
