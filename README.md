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

## Status (2026-11-05)

Only rows marked **integrated** are on this branch and tested. Everything else
is in progress and must not be read as working.

| Component | State |
| --- | --- |
| Matching engine: price-time priority, phases and auctions, market/IOC/FOK, post-only, STP, replace and partial cancel, mass cancel, expiry | **integrated** (`src/exsim/matching_engine.cpp`, index arena with hot/cold split; see [engine variants](docs/engine-variants/)) |
| Independent reference oracle, differential fuzzer with shrinker, scenario DSL, canonical journal and replay | **integrated** |
| Order entry: OUCH 4.2 over SoupBinTCP 3.0, FIX 4.4, and SBE 1.0 over FIXP 1.0 (exsim schema, [FIXP_SBE.md](docs/protocols/FIXP_SBE.md)); server and client side of each, with sequencing, recovery and cancel-on-disconnect | **integrated** (`include/exsim/gateway/`); OUCH and FIX are served by `exsim_server`, SBE so far only as a gateway library driven by tests |
| Market data: ITCH 5.0 over MoldUDP64 publisher, retransmission server, client receiver and book builder, checked under packet loss, reordering and duplication | **integrated** |
| Exchange core: submit and dispatch, admin console and fault injection, background bots, journal, trading day; server config for all three protocols | **integrated** as a library (`include/exsim/server/`) |
| Networking: kqueue/epoll poller, TCP server and client, UDP and multicast | **integrated** |
| Server binary `exsim_server`: one poller thread running the core, OUCH and FIX order entry, the ITCH/MoldUDP64 feed with packet fault injection and retransmission, the admin console, bots and the journal; `exsim_client`; 12 loopback end-to-end tests; wire-to-wire benchmark `exsim_w2w_bench` | **integrated** ([SERVER_RUNTIME.md](docs/SERVER_RUNTIME.md)) |
| Deterministic research runner: simulated time, latency model, fixed-spread and inventory-aware market makers, TWAP, accounting, risk limits, manifests, seed sweeps, paired comparison, replay verification (`exsim_research`) | **integrated** ([RESEARCH_RUNNER.md](docs/RESEARCH_RUNNER.md)) |
| Coverage-guided fuzzing: 16 libFuzzer targets for every decoder and text parser, committed corpora replayed by `make test` | **integrated** ([FUZZING.md](docs/FUZZING.md)) |
| Research API contract (OpenAPI 3.1) and Next.js research UI | **integrated on a mock service**; the UI does not read real runs yet |
| Three engine layouts compared on one machine (A tick ladder, B sorted vectors, C index arena), independent review of C, SPEC conformance audit | **done** ([engine variants](docs/engine-variants/), [SPEC_CONFORMANCE.md](docs/SPEC_CONFORMANCE.md)) |
| Independent QA reviews of OUCH, ITCH/MoldUDP64, the core, FIX, SBE over FIXP and the research runner | **done**; the OUCH, core, market data and FIX findings are fixed and merged; the SBE and research runner fixes are in progress |
| SBE in the server runtime, FIX codec speed-up, C++ HTTP/OpenAPI research API, UI on real runs, final independent review | in progress |

Live work, task IDs and verification logs: [docs/WORK_PLAN.md](docs/WORK_PLAN.md).

## Quick start

Requires a C++20 compiler (Apple clang 21, GCC 14 and clang 19 are tested) and
GNU Make.

```sh
make test                                     # release build, all tests
make test MODE=debug                          # ASan + UBSan
make test MODE=tsan                           # ThreadSanitizer
make test FILTER=Journal                      # substring filter on test names
make tools                                    # exsim_server, exsim_client, exsim_md_client, exsim_w2w_bench, exsim_bench, exsim_codec_bench, exsim_fuzz, exsim_research, exsim_replay, exsim_scenario, itch_replay

# the exchange: OUCH on 15000, FIX on 15001, admin on 15002, ITCH/MoldUDP64 on 16000
./build/release/exsim_server --config config/exsim.conf
./build/release/exsim_client --user ALICE --password alice --server 127.0.0.1:15000 \
    "buy A1 SPY 100 400.00" "wait 1500" "cancel A1"

./build/release/exsim_scenario tests/scenarios/fifo.scn
./build/release/exsim_fuzz --profile mixed --seeds 200 --steps 10000
make tools MODE=perf && ./build/perf/exsim_bench --ops 2000000
./build/perf/exsim_codec_bench                # wire codec cost per message

# research: two market-making configs over the same five seeds, then a paired comparison
./build/release/exsim_research run config/research/fixed_spread_narrow.conf --out /tmp/narrow --seeds 1..5
./build/release/exsim_research run config/research/fixed_spread_wide.conf --out /tmp/wide --seeds 1..5
./build/release/exsim_research compare /tmp/narrow /tmp/wide
```

Each research run replays its own journal independently and refuses to report
success unless the replay matches. A five-seed sweep of about 23,000 events per
seed takes 0.64 s of wall time on an M-series Mac (release build), and
repeating it gives byte-identical event files. The artifacts are also
identical across platforms and compilers: every research config over seeds
1..3 produced the same bytes on macOS arm64 (Apple clang 21), Linux aarch64
(GCC 14.2, Clang 19.1) and Linux x86_64 (GCC 14.2, Clang 19.1, under
emulation). Only `status.json` (wall clock) and the compiler name in
`manifest.json` differ. CI keeps this property with the `research-determinism`
jobs.

CI (`.github/workflows/ci.yml`) builds and tests with `-Werror` on Linux g++
and clang++ (release and debug), Linux clang++ TSan, and macOS clang (release
and debug), then runs a zero-allocation benchmark smoke test.

## Design

```
            order entry (TCP)                          market data (UDP)
  client -- OUCH 4.2 / SoupBinTCP --+              +-- ITCH 5.0 / MoldUDP64 --> subscribers
  client -- FIX 4.4 ----------------+              |
  client -- SBE 1.0 / FIXP 1.0 -----+              |
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
| Unit and contract tests (`tests/`) | 840 tests: codecs, networking, engine contracts, gateways and session recovery, market data under packet faults, exchange core, research runner, journal, bots, ledger, legacy book |
| Server end-to-end tests (`tests/exsim/server/e2e/`) | a real `exsim_server` runtime on loopback sockets: OUCH and FIX order entry and trades between them, user faults (reject, delay, drop_out recovery, disconnect), cancel-on-disconnect, the ITCH feed rebuilt into a book equal to the engine's under packet faults, shutdown, journal replay to the published event hash, and zero allocations on the server thread in steady state |
| Reference oracle (`tests/exsim/reference/`) | a deliberately simple second implementation of SPEC.md, written independently of the production engine |
| Differential fuzzer (`tools/exsim_fuzz`) | random command streams through engine and oracle, compared event by event, with a shrinker for minimal reproducers |
| Scenarios (`tests/scenarios/*.scn`) | 39 hand-written exchange situations with expected events (16 derived line by line from the spec), run against both implementations; a failure names the file, line and field |
| Journal and replay | canonical serialization, corruption and truncation detection, bit-identical replay |
| Research determinism (CI) | every research config over fixed seeds on x86_64 Linux g++ and clang++ and arm64 macOS; all artifact digests must be equal |
| Allocation guards | per-thread counting of every `operator new` form inside `run_tests`; the engine, journal, networking and bot hot paths must report zero |
| Coverage-guided fuzzing (`fuzz/`) | 16 libFuzzer targets with ASan/UBSan and property checks; the 2026-11-05 campaigns ran 384,594,827 executions (302,880,773 over 15 targets, 81,714,054 on the new SBE target) with no crash, and the committed corpora (7,252 files) replay in every `make test` |
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
[docs/engine-variants/](docs/engine-variants/).

Wire codecs, `exsim_codec_bench`, same perf build and machine, one message at
a time as the gateways call them, 256 distinct inputs per case, every decoded
field and encoded byte forced to memory, median of 7 rounds of 2,000,000
messages (three runs agreed within 0.2 ns, and within 1 ns for FIX), zero
allocations:

| Path | Message | Bytes | ns/msg |
| --- | --- | ---: | ---: |
| inbound order | OUCH 4.2 EnterOrder in a SoupBinTCP packet, decode | 52 | 6.0 |
| inbound order | SBE NewOrderSingle in a SOFH frame, decode | 51 | 4.0 |
| inbound order | FIX 4.4 NewOrderSingle, framing, checksum and decode | 162 | 228 |
| outbound fill | OUCH 4.2 OrderExecuted in a SoupBinTCP packet, encode | 43 | 3.6 |
| outbound fill | SBE ExecutionReportTrade in a SOFH frame, encode | 89 | 2.1 |
| outbound fill | FIX 4.4 ExecutionReport, encode | 220 | 155 |
| market data | ITCH 5.0 AddOrder, encode | 36 | 2.1 |
| market data | ITCH 5.0 OrderExecuted, encode | 31 | 1.4 |
| market data | ITCH 5.0 AddOrder, decode | 36 | 2.7 |

The binary protocols cost a few nanoseconds per message. FIX costs 38 to 74
times more than OUCH or SBE for the same order or fill, which makes it the
codec worth optimizing.

Wire to wire, `exsim_w2w_bench`: the real server runtime on its own thread
(OUCH, FIX and admin listeners, the ITCH feed to a local UDP socket, no bots,
no journal) and an OUCH client on another thread of the same process, over
loopback. The client enters a buy order at a fixed rate, cancels it when it
is accepted, and times on its monotonic clock the Enter Order write until
the Order Accepted read (ouch accept) and until the matching ITCH Add Order
arrives on the feed (itch add). Same Apple M6 Mac mini, perf build, macOS
27, no CPU pinning (macOS has none), load average 2.5 to 7 from other work,
microseconds:

| Run | Server poll | ouch accept p50 / p99 / p99.9 | itch add p50 / p99 / p99.9 | Orders/s |
| --- | --- | --- | --- | ---: |
| 1,000 orders/s, 20,000 orders | blocking | 66 / 3,252 / 7,490 | 61 / 3,190 / 7,442 | 1,000 |
| same | busy poll | 63 / 783 / 3,841 | 57 / 764 / 3,811 | 1,000 |
| 20,000 orders/s, 100,000 orders | blocking | 54 / 1,212 / 3,363 | 44 / 1,010 / 2,925 | 19,997 |
| same | busy poll | 54 / 1,596 / 3,932 | 45 / 1,368 / 3,393 | 20,000 |
| as fast as possible, 32 in flight | blocking | 387 / 488 / 691 | 323 / 424 / 595 | 76,126 |
| same | busy poll | 384 / 484 / 772 | 319 / 424 / 603 | 76,258 |

No run lost a feed message, needed a retransmission or saw a reject. The
medians sit at the machine's floor: a plain 64-byte loopback TCP round trip
through a blocking echo thread measured 63 us p50 and 79 us p99 in the same
minute, so the macOS network stack and thread wake-ups dominate, not the
exchange (whose codecs and matching cost well under a microsecond per
order). The millisecond tails are scheduling on a shared, unpinned machine.
The windowed rows measure throughput: with 32 orders in flight latency is
queueing, and the single-threaded client, not the server, is the likely
limit. Tuned Linux numbers (pinned cores, isolated CPUs) are still to be
measured; full method and p90 and max columns are in
[SERVER_RUNTIME.md](docs/SERVER_RUNTIME.md) section 5.

## Repository layout

| Path | Contents |
| --- | --- |
| `include/exsim/`, `src/exsim/` | simulator: domain types, engine, protocol codecs, networking, gateways, server components, simulation and research |
| `include/lob/`, `src/` | original limit order book and sharded engine |
| `tests/` | test framework, unit tests, oracle, scenarios |
| `tools/` | exchange server, order-entry client, market data client, wire-to-wire, engine and codec benchmarks, fuzz, replay, scenario runner, research runner, ITCH replay |
| `benchmark/` | Google Benchmark suite for the original book |
| `docs/` | specifications, reviews, plans |
