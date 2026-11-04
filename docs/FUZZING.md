# Fuzzing

Coverage-guided fuzzing for every place where exsim reads bytes or text it did
not produce itself: the wire protocols, the TCP stream buffers, the admin
console, the configuration file, the price text parsers, the command journal,
the scenario DSL and the bot options.

Every target is libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer, and
every target also checks properties (round trips, models, documented rejection
contracts), so a fuzzer finds logic bugs as well as memory errors.

## Layout

| Path | What |
|---|---|
| `fuzz/targets/<name>.hpp` | Target body: `int fuzz_<name>(const std::uint8_t*, std::size_t)`. Plain header, no libFuzzer dependency. |
| `fuzz/targets/fuzz_support.hpp` | `EXSIM_FUZZ_REQUIRE`, `FuzzByteReader` (structured values from fuzz bytes). |
| `fuzz/libfuzzer_entry.cpp` | The one `LLVMFuzzerTestOneInput`, compiled once per target. |
| `fuzz/seed_generator.cpp` | Deterministic seeds from the real encoders, `config/exsim.conf` and `tests/scenarios/*.scn`. |
| `fuzz/corpus/<name>/` | Committed corpus: the seeds merged and minimized with every campaign's findings. |
| `fuzz/corpus/known_bugs/<name>/` | Historical minimized bug reproducers, always replayed by regression tests. |
| `fuzz/Makefile` | Build, campaign, merge, replay. Separate from the top-level Makefile. |
| `tests/exsim/fuzz_regression/` | Replays every committed corpus file in the normal test suite. |
| `docker/Dockerfile` | Debian trixie image with clang, libFuzzer and the sanitizer runtimes. |

A violated property throws `FuzzPropertyViolation`, which deliberately does not
derive from `std::exception`, so a target that catches `std::exception` from a
throwing parser (journal, scenario DSL) can never swallow it. Under libFuzzer
the entry point prints the property and aborts, which saves the input as a
crash; in the test suite the replay fails with the input's path.

## Regression replay on every `make test`

`make test` (and `make test MODE=debug`, which adds ASan and UBSan) runs
`fuzz_regression.<target>` for all 15 targets. Each one replays every file in
`fuzz/corpus/<target>/` through the same target body libFuzzer runs, on macOS
and Linux, without libFuzzer, and requires the directory to be non-empty.

```sh
make test FILTER=fuzz_regression             # just the replays
make test MODE=debug FILTER=fuzz_regression  # with sanitizers
```

To turn a crash into a permanent regression test, copy the minimized input
into `fuzz/corpus/<target>/` (any file name) once the bug is fixed.

## Running a campaign

Apple clang has no libFuzzer, so campaigns run in Docker:

```sh
docker build -t exsim-dev docker

# Build every target (build/fuzz/<target>):
docker run --rm -v "$PWD":/src:ro -v "$PWD/build/fuzz-work":/work exsim-dev sh -c \
  'rsync -a --exclude build --exclude .git /src/ /work/ && make -C /work/fuzz -j4'

# One campaign: 2 jobs, 2 at a time, 300 s each (10 CPU-minutes):
docker run --rm -v "$PWD":/src:ro -v "$PWD/build/fuzz-work":/work exsim-dev sh -c \
  'rsync -a --exclude build --exclude .git /src/ /work/ && make -C /work/fuzz run TARGET=fix44_message SECONDS=300 JOBS=2 WORKERS=2'
```

The campaign reads `fuzz/corpus/<target>/`, writes new inputs to
`build/fuzz/campaign/<target>/`, crashes to `build/fuzz/artifacts/<target>/`
and the libFuzzer logs (`fuzz-0.log`, `fuzz-1.log`, with the final
`cov:`/`ft:`/`corp:` lines and `Done N runs`) to `build/fuzz/logs/<target>/`,
all under the writable `build/fuzz-work` mount. Fold the new inputs back into
the committed corpus with:

```sh
docker run --rm -v "$PWD":/src:ro -v "$PWD/build/fuzz-work":/work exsim-dev sh -c \
  'make -C /work/fuzz merge TARGET=fix44_message'
cp -R build/fuzz-work/fuzz/corpus/fix44_message/. fuzz/corpus/fix44_message/   # after review
```

The machine is shared: keep `JOBS`/`WORKERS` at 2 and always bound the run
with `SECONDS` (`-max_total_time`).

### Replaying one input

```sh
# Under libFuzzer, ASan and UBSan, in Docker:
docker run --rm -v "$PWD":/src:ro -v "$PWD/build/fuzz-work":/work exsim-dev sh -c \
  'make -C /work/fuzz replay TARGET=price_text INPUT=/src/fuzz/corpus/known_bugs/price_text/server-parse-price-overflow'

# Natively, through the test runner: put the file in fuzz/corpus/<target>/ and
make test MODE=debug FILTER=fuzz_regression.<target>
```

`make -C fuzz seeds` rebuilds the deterministic seeds into `build/fuzz/seeds/`
(it needs only a C++20 compiler, so it also works on macOS).

## Targets

| Target | Code under test | Properties beyond "no crash, no UB" |
|---|---|---|
| `ouch42_inbound` | `protocol/ouch42.hpp` client messages | decode status order (incomplete, invalid_length, invalid_type); ok decodes re-encode byte-identical into 0x00- and 0xFF-prefilled buffers (no unwritten bytes); short buffers refused; encode of structured messages with 64-bit prices and timestamps rejects exactly what does not fit the wire field |
| `ouch42_outbound` | `protocol/ouch42.hpp` server messages | same, for the 9 outbound types |
| `itch50` | `protocol/itch50.hpp` | same, for 13 types, with 48-bit timestamps |
| `soupbintcp` | `protocol/soupbintcp.hpp` | packet decode equals a model of the length rules; typed login decoders round trip; `FrameReader` fed in fuzz-chosen chunks yields exactly the frames and terminal error of a contiguous-stream model (`full`, `overflow`, zero length) |
| `moldudp64` | `protocol/moldudp64.hpp` | `PacketReader` equals a block-layout model; accepted packets rebuild byte-identical with `PacketBuilder`; request round trip; builder direction: payload limit, `full`, sequence numbers advance by the message count |
| `fix44_message` | `protocol/fix44.hpp` `Parser` and the 14 typed decoders | parse ok implies BeginString, exact BodyLength and correct CheckSum; framing contract: any changed body byte is `checksum`, an extra byte `invalid_length`, a truncated frame `incomplete`, never a semantic status; typed decoders only report semantic statuses; decode, re-encode, decode again is a fixed point |
| `fix44_stream` | `fix44::FrameReader`, `Builder`, `NewOrderSingle`/`ExecutionReport` encode | chunked reader equals a model; builder rejects exactly duplicates, SOH in values and overflow; structured messages round trip (ms timestamps, `missing_field`, `invalid_field`) |
| `wire_text_fields` | `protocol/wire.hpp` | `numeric` parse equals a model; format right-aligns, refuses what does not fit, round trips; `alpha` drops only trailing spaces; 16/32/48/64-bit load/store round trip |
| `stream_buffers` | `net/stream_buffers.hpp` | `ReceiveBuffer` and `SendBuffer` match an offset model op by op (compaction rule, sticky overflow, reserve placement), driven over a real non-blocking socket pair (partial reads, would-block, peer close, EPIPE without SIGPIPE) |
| `admin_command` | `server::parse_admin` | rejected line leaves the command unchanged and sets an error; unknown verbs are reported as such; accepted commands keep their ranges; canonical rendering parses back identically; structured commands rendered with random keyword case, whitespace and rate key order parse back exactly |
| `server_config` | `server::parse_config` | rejected file leaves the config unchanged, error is `<line>: <msg>` inside the file; accepted config satisfies every validation and is accepted by the engine; canonical rendering is a parse fixed point |
| `price_text` | `sim::parse_price`, `server::parse_price`, `fix44::parse_price` and their formatters | each parser equals a model of its own documented grammar and range; formatters are minimal and round trip through all three parsers |
| `journal_reader` | `sim::JournalReader` / `JournalWriter` | arbitrary file bytes are rejected with an exception or accepted; an accepted journal is canonical (rewriting it reproduces every byte); a second mode rebuilds the checksums so mutations reach the semantic checks |
| `scenario_dsl` | `sim::run_scenario<MatchingEngine>` | every failure is a `std::exception` naming its line; runs are deterministic |
| `bot_options` | `sim::bots::apply_bot_option` | transactional on rejection; accepted options keep the documented ranges and are idempotent; rendered options rebuild the configuration |

### Not fuzzed, and why

- **OUCH and FIX gateway session layers**: not in the tree yet (owned by the
  concurrent gateway work). Their wire input is covered at the codec and framing
  level above; add a session target when they land.
- **`gateway/sequenced_store.hpp`, `gateway/token_table.hpp`,
  `research/position_ledger.hpp`, `sim/packet_faults.hpp`**: take typed values
  from already decoded messages, not bytes or text; covered by unit tests.
- **`net/tcp_server`, `tcp_client`, `udp`, `poller`**: socket plumbing; the
  bytes they move go through `stream_buffers` and the codecs, which are fuzzed.
- **Scenario DSL engine size**: `config max_orders=`/`max_instruments=` size
  the engine's preallocated storage and the DSL (test code) has no upper
  bound, so the harness skips files asking for more than 16384 orders or 64
  instruments. A design limit of test tooling, not a bug.

## Campaign results

Campaign of 2026-11-03, Docker `exsim-dev` (Debian trixie, clang 19), each
target `make -C fuzz run TARGET=<name> SECONDS=300 JOBS=2 WORKERS=2`
(`-use_value_profile=1`, two parallel 300 s jobs, 10 CPU-minutes per target),
starting from the generated seeds. Executions are summed over both jobs; `cov`
(edges) and `ft` (features, including value profile) are the highest reported
by either job; "Campaign corpus" is what the jobs kept, "Committed corpus" is
`fuzz/corpus/<name>/` after `make merge` (minimized without value profile)
plus the seeds.

| Target | Executions | cov | ft | Campaign corpus | Committed corpus | Crashes |
|---|---:|---:|---:|---:|---:|---:|
| `ouch42_inbound` | 83,511,732 | 163 | 2398 | 1875 | 67 | 0 |
| `ouch42_outbound` | 74,429,457 | 328 | 3653 | 2783 | 128 | 0 |
| `itch50` | 73,487,377 | 488 | 7483 | 7923 | 195 | 0 |
| `soupbintcp` | 45,997,774 | 348 | 4299 | 2463 | 199 | 0 |
| `moldudp64` | 40,152,622 | 197 | 3276 | 1645 | 176 | 0 |
| `fix44_message` | 159,203,010 | 1149 | 6736 | 2742 | 434 | 0 |
| `fix44_stream` | 25,245,550 | 862 | 8578 | 8519 | 507 | 0 |
| `wire_text_fields` | 130,591,569 | 100 | 1694 | 1189 | 93 | 0 |
| `stream_buffers` | 32,561,864 | 212 | 3461 | 3483 | 384 | 0 |
| `admin_command` | 29,053,652 | 648 | 7124 | 4698 | 399 | 0 |
| `server_config` | 26,934,093 | 1270 | 12231 | 24615 | 789 | 0 |
| `price_text` | 45,505,379 | 431 | 5033 | 2947 | 139 | 0 |
| `journal_reader` | 9,963,648 | 478 | 7991 | 12667 | 96 | 0 |
| `scenario_dsl` | 5,328,929 | 1724 | 11897 | 10089 | 929 | 0 |
| `bot_options` | 4,479,447 | 791 | 10900 | 13647 | 1655 | 0 |

786,446,103 executions in total, no crash, no sanitizer report and no property
violation with the then-open findings below steered around. The 20-second smoke
run before the campaign found findings 1 and 2.

Re-run of 2026-11-05 on the integrated tree after the fixes, without any
steering: each target `SECONDS=120 JOBS=2 WORKERS=2` (4 CPU-minutes per
target), 302,880,773 executions over the 15 targets, no crash, no sanitizer
report and no property violation (`/tmp/exsim-fuzz-campaign.log`; per-target
libFuzzer logs in the session scratch work directory).

The committed corpora (6,190 files, 1.5 MB) replay in about 0.2 s in
`make test` and 0.5 s in `make test MODE=debug` on an M-series Mac.

## Fixed findings (historical)

The fuzzers found the three defects below; all are fixed on
`exchange-simulator` (`98c3e09`), as are the two observations at the end. Their
minimized reproducers live in `fuzz/corpus/known_bugs/<target>/` and are
replayed on every `make test` by `fuzz_known_bugs.*`, which now must pass. The
targets no longer steer around any input.

### 1. FIX parser accepts a frame whose CheckSum field is not SOH-separated

- **Where**: `include/exsim/protocol/fix44.hpp:215`, `Parser::parse`.
- **Input**: `fuzz/corpus/known_bugs/fix44_message/checksum-field-without-separator`,
  `8=FIX.4.4|9=5|35=0X10=250|` (`|` is SOH): the body `35=0X` lacks the SOH
  before `10=`, but its BodyLength and checksum are correct.
- **Expected**: `Status::invalid_length` (garbled framing). docs/GATEWAYS.md
  section 3: garbled framing is discarded without advancing the inbound
  sequence.
- **Actual**: `Status::ok`. The trailer check tests that the last 7 bytes are
  `10=NNN<SOH>` but not that the byte before them is SOH, so the last body
  field swallows the CheckSum field (`get(35) == "0X10=250"`, `has(10)` is
  false) and a garbled frame reaches the semantic layer, which would consume a
  sequence number and send a session Reject.
- **Fix**: also require `p[size - 8] == soh` in that check.

### 2. `server::parse_price` signed overflow (undefined behaviour)

- **Where**: `src/exsim/server/config.cpp:341`
  (`value += (c - '0') * scale;`).
- **Input**: `fuzz/corpus/known_bugs/price_text/server-parse-price-overflow`
  (`922337203685477.5808`), also `-922337203685477.587.` (overflows before
  the trailing `.` would reject it) and, through the config file,
  `fuzz/corpus/known_bugs/server_config/server-parse-price-overflow`
  (`max_price = 922337203685477.5808`).
- **Expected**: `false`, as documented ("rejects anything else, including
  overflow").
- **Actual**: signed 64-bit overflow, reported by UBSan as
  `config.cpp:341:15: runtime error: signed integer overflow:
  9223372036854775800 + 8`. Without sanitizers it wraps and returns `true`
  with a negative price. The whole part is bounded (`value > kMax / kPriceScale`)
  but the fraction digits are added unchecked; a whole part of exactly
  922337203685477 with a fraction above .5807 overflows. As a consequence
  `server::parse_price` also cannot read back `server::format_price(INT64_MIN)`.
- **Fix**: check `value > kMax - (c - '0') * scale` before adding.

### 3. `parse_config` accepts engine sizes the engine rejects

- **Where**: `src/exsim/server/config.cpp:183-184`, `[engine]`
  `max_instruments` and `max_orders` (only `> 0` is checked).
- **Input**: `fuzz/corpus/known_bugs/server_config/engine-max-orders-above-uint32`
  (`max_orders = 4294967296`) and `engine-max-instruments-above-uint16`
  (`max_instruments = 65536`), each with one valid instrument.
- **Expected**: rejected by `parse_config` with a line-numbered error, like
  every other out-of-range value.
- **Actual**: accepted; the server then fails later when the engine
  constructor calls `required_event_capacity`, which throws
  `std::invalid_argument("EngineConfig bounds")` for `max_instruments > 65535`
  or `max_orders > 4294967295`.

### Observations (no reproducer, not bugs in the fuzzed contracts)

- `include/exsim/sim/scenario.hpp:151`, `expect book`: when the quantity token
  fails to parse (`expect book AAPL bid 100.00 x`), `count` is never assigned
  and is read uninitialized if the price level exists. Test tooling only;
  ASan and UBSan cannot see it (MemorySanitizer would). Initialize `qty` and
  `count` to 0 and reject a failed extraction.
- `include/exsim/protocol/fix44.hpp:314`, `Builder::field` accepts tag 0,
  which `Parser::parse` rejects as `invalid_field`. No encoder uses tag 0, so
  `fix44_stream` keeps tags at 1 and above.
