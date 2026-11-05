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
`fuzz_regression.<target>` for all 16 targets. Each one replays every file in
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
| `sbe_order_entry` | `protocol/sbe_order_entry.hpp` (SBE 1.0 over SOFH, schema 22597) | `decode_frame` equals a model of the SOFH and message header rules; all 25 message types decode every accepted frame with the status of a template, block length, group and variable-length-field model; canonical version 1 frames re-encode to their input bytes, later-version frames normalize to a canonical frame that round trips, and every re-encoded frame fits `kMaxEncodedFrameSize`; back-to-back frames walk the input |

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

`sbe_order_entry` was added on 2026-11-05 after these runs. Its first
campaign, Docker `exsim-dev` (clang 19, ASan and UBSan), was
`make -C fuzz run TARGET=sbe_order_entry SECONDS=300 JOBS=4 WORKERS=4` (20
CPU-minutes) from the 31 generated seeds: 81,714,054 executions, cov 1398, ft
14533, campaign corpus 2566 to 2678 per job, no crash, no sanitizer report and
no property violation. `make merge` kept 1026 inputs, committed with the seeds.

A mutation check then planted eight bugs in a copy of the codec (a dropped root
block field, the trailing byte check removed, the later-version field count
loosened, ignored repeating groups, and off-by-one errors in the 1024-byte
frame limit, the 128-byte variable length field limit, and the block length
checks against the frame and the root block) and replayed the committed corpus
through each. Three of the off-by-one mutants survived because no input sat
exactly on the limit, so the generator gained five boundary seeds: a 128-byte
and a 129-byte variable length field, 1024-byte and 1025-byte frames, and a
blockLength one byte past the frame. With them the corpus (36 seeds plus 1026
campaign inputs, 1062 files, 4.1 MB) catches all eight.

The committed corpora of the original 15 targets (6,389 files, 1.7 MB) replay
in about 0.2 s in `make test` and 0.5 s in `make test MODE=debug` on an
M-series Mac.

### Mutation check

On 2026-11-05, 103 single-line bugs were planted, one at a time, in a copy of
the code under test for ten of the original targets. Each mutant was replayed
through its target's committed corpus with ASan and UBSan, so a sanitizer
report also counts as a kill. A FIX mutant counts as killed when either FIX
corpus kills it, because `fix44_message` and `fix44_stream` share the codec.
Survivors were then handled in one of three ways:

- Equivalent: no input can tell the mutant apart from the original.
- Corpus gap: no input sits on the boundary, so a seed was added at the end of
  the target's seed list in `fuzz/seed_generator.cpp`. Existing `seed-NNN`
  files keep their bytes.
- Oracle gap: the property could not see the difference, so the target was
  strengthened.

Oracle changes:

- `fixed_layout_codec.hpp` (OUCH and ITCH): an exact-size message must now
  decode `ok` exactly when every enumerated one-byte field (side, event code,
  order state) holds a listed code, and `invalid_field` otherwise. Before, the
  property only required "ok or invalid_field".
- `fix44_message.hpp`:
  - The frame must report `overflow` exactly when its declared extent passes
    1 MiB.
  - BodyLength has at most 20 digits.
  - Tag 0 is never a field.
  - The canonical re-encoding keeps the value of every field the input had,
    up to decimal spelling and the `.000` that a 17-character UTCTimestamp
    gains.

| Target | Planted | Killed, original corpus | Killed, with new seeds | Equivalent | Seeds added |
|---|---:|---:|---:|---:|---:|
| `ouch42_inbound` | 11 | 7 | 10 | 1 | 26 |
| `ouch42_outbound` | 10 | 5 | 10 | 0 | 54 |
| `itch50` | 12 | 6 | 11 | 1 | 82 |
| `soupbintcp` | 11 | 7 | 10 | 1 | 9 |
| `moldudp64` | 9 | 7 | 9 | 0 | 2 |
| `fix44_message` | 12 | 4 | 10 | 1 | 13 |
| `fix44_stream` | 10 | 8 | 10 | 0 | 5 |
| `wire_text_fields` | 8 | 5 | 8 | 0 | 4 |
| `price_text` | 11 | 7 | 10 | 1 | 4 |
| `stream_buffers` | 9 | 5 | 5 | 2 | 0 |
| Total | 103 | 61 | 93 | 7 | 199 |

Three non-equivalent mutants still survive, and the reasons are explained
below:

- F8: an oracle gap. Telling the two statuses apart needs a full reference
  model of the field scan.
- B4 and B8: a harness gap. The `stream_buffers` harness flushes under 64
  bytes at a time to a peer that drains straight away, so `send` is never
  partial and SendBuffer compaction and the `Blocked` status are never
  reached. Covering them would need a peer with a full receive buffer.

The new seeds include one 196,622-byte MoldUDP64 packet that holds 65,534
messages.

Mutants, with line numbers at commit 922cb39. The kinds of kill are:

- "seed": the new seeds kill the mutant even with the old properties.
- "seed + oracle": the mutant needs both the new seeds and the stronger
  property.
- "original": the original corpus already killed it.

OUCH 4.2 inbound (`include/exsim/protocol/ouch42.hpp`):

- I1 `:54` EnterOrder short check `n < 49` to `n < 48`: original.
- I2 `:56` EnterOrder exact-size check `n != 49` to `n > 50`: original.
- I3 `:62` EnterOrder side set `"BSTE"` to `"BST"`: seed + oracle.
- I4 `:68` EnterOrder price offset `p + 28` to `p + 27`: original.
- I5 `:32` EnterOrder encode price bound `> UINT32_MAX` to `>=`: seed.
- I6 `:32` EnterOrder encode `price < 0` to `price < -1`: equivalent. The
  `uint64_t` cast in the next check rejects every negative price.
- I7 `:75` minimum quantity load replaced by `0`: original.
- I8 `:113` ReplaceOrder short check `n < 47` to `n < 46`: original.
- I9 `:153` CancelOrder exact-size check `n != 19` to `n > 20`: original.
- I10 `:65` shares loaded byte-swapped: original.
- I11 `:192` ModifyOrder side check removed: seed + oracle.

OUCH 4.2 outbound (`ouch42.hpp`):

- O1 `:217` SystemEvent short check `n < 10` to `n < 9`: original.
- O2 `:281` OrderAccepted exact-size check `n != 66` to `n > 67`: original.
- O3 `:304` OrderAccepted order state set `"LD"` to `"L"`: seed + oracle.
- O4 `:224` SystemEvent event code check removed: seed + oracle.
- O5 `:387` OrderExecuted match number read as `load<4>(p + 36)`: seed.
- O6 `:551` OrderReplaced encode price bound `>` to `>=`: seed.
- O7 `:607` OrderReplaced previous token read from `p[64 + i]`: original.
- O8 `:475` CancelReject short check `n < 23` to `n < 22`: original.
- O9 `:285` timestamp loaded byte-swapped: original.
- O10 `:519` OrderModified side check removed: seed + oracle.

ITCH 5.0 (`include/exsim/protocol/itch50.hpp`):

- T1 `:187` AddOrder encode timestamp bound `>` to `>=`: seed.
- T2 `:188` AddOrder encode price bound `>` to `>=`: seed.
- T3 `:213` AddOrder side check removed: original.
- T4 `:39` SystemEvent code set `"OSQMEC"` to `"OSQME"`: seed + oracle.
- T5 `:97` StockDirectory exact-size check `n != 39` to `n > 40`: original.
- T6 `:431` OrderDelete short check `n < 19` to `n < 18`: seed.
- T7 `:653` NOII price offset `p + 40` to `p + 41`: original.
- T8 `:615` NOII encode `near_price < 0` to `< -1`: equivalent (same reason
  as I6).
- T9 `:584` Cross Trade shares read as `load<4>(p + 15)`: original.
- T10 `:366` OrderExecutedWithPrice price offset `p + 32` to `p + 31`: original.
- T11 `:532` Trade side check accepts only `'B'`: seed + oracle.
- T12 `:211` AddOrder timestamp read as `load<5>(p + 6)`: original.

SoupBinTCP (`include/exsim/protocol/soupbintcp.hpp`):

- S1 `:23` incomplete check `n < len + 2` to `n < len + 1`: seed.
- S2 `:25` exact-size check `n != len + 2` to `n > len + 3`: original.
- S3 `:30` LoginRequest packet size `47` to `46`: original.
- S4 `:33` one-byte packet type set `"ROHZ"` to `"ROH"`: original.
- S5 `:37` LoginRejected reason check removed: original.
- S6 `:21` zero-length check disabled: original.
- S7 `:149` reader frame limit `n > max_` to `n > max_ + 1`: seed.
- S8 `:153` reader capacity check `>` to `>=`: original.
- S9 `:100` LoginRequest sequence width 20 to 19: original.
- S10 `:117` LoginAccepted sequence field shifted by one byte: seed.
- S11 `:46` encode payload bound `> 65534` to `> 65535`: equivalent. The
  64 KiB payload wraps its 16-bit length to 0, and encode's own decode check
  then refuses it.

MoldUDP64 (`include/exsim/protocol/moldudp64.hpp`):

- M1 `:91` message length prefix check `< 2` to `< 1`: original.
- M2 `:95` zero-length message check disabled: original.
- M3 `:97` message length bound off by one: seed.
- M4 `:88` decoder end-of-session count `65535` to `65534`: original.
- M5 `:57` builder message limit `count_ >= 65534` to `>= 65533`: seed.
- M6 `:61` builder size check `+ 2` to `+ 1`: original.
- M7 `:51` builder end-of-session count `65535` to `65534`: original.
- M8 `:31` message count read at `p + 17`: original.
- M9 `:68` builder writes `count_ + 1` without incrementing: original.

FIX 4.4 (`include/exsim/protocol/fix44.hpp`), checked in `fix44_message`:

- F1 `:176` short frame check `< 12` to `< 11`: original.
- F2 `:182` BodyLength digits limit `>= 20` to `>= 21`: seed + oracle.
- F3 `:191` frame size limit off by one: seed + oracle.
- F4 `:215` SOH before CheckSum not checked: original.
- F5 `:219` CheckSum bound `> 255` to `> 256`: equivalent. `sum % 256` is
  never 256.
- F6 `:219` CheckSum digits read as `{size - 3, 2}`: original (killed by
  `fix44_stream`).
- F7 `:233` tag 0 accepted: seed + oracle.
- F8 `:241` mid-body tag 10 check disabled: survives (oracle gap). The frame
  gives `duplicate_field` where `invalid_length` was expected.
- F9 `:151` UTCTimestamp seconds `> 59` to `> 60`: seed + oracle.
- F10 `:157` leap year rule `y % 400 == 0` to `!= 0`: seed + oracle.
- F11 `:847` NewOrderSingle OrderQty bound `> UINT32_MAX` to `> UINT32_MAX + 1`: seed + oracle.
- F12 `:1269` ExecutionReport ExecType set widened to the alphanumerics:
  original (killed by `fix44_stream`).

FIX 4.4 stream (`fix44.hpp`), checked in `fix44_stream`:

- G1 `:407` reader full check `used_ + 1`: original.
- G2 `:397` reader frame limit `buffer_.size()` to `+ 1`: original.
- G3 `:308` builder SOH-in-value check removed: original.
- G4 `:314` builder capacity check `+ 2` to `+ 1`: seed.
- G5 `:360` builder `finish` total check `+ 1`: original.
- G6 `:246` field table limit `count_ == N` to `N - 1`: seed.
- G7 `:373` CheckSum tens digit `sum / 10 % 10` to `sum / 10`: original.
- G8 `:138` milliseconds `% 1000` to `% 100`: original.
- G9 `:836` NewOrderSingle Side `case '2'` to `case '3'`: original.
- G10 `:1219` ExecutionReport LeavesQty written from CumQty: original.

Wire text fields (`include/exsim/protocol/wire.hpp`):

- W1 `:72` numeric width check `digits > n` to `> n + 1`: original.
- W2 `:92` overflow guard weakened to `r > UINT64_MAX / 10`: original.
- W3 `:57` alpha width check `> n` to `> n + 1`: original.
- W4 `:64` trailing pad trim `== ' '` to `<= ' '`: seed.
- W5 `:50` 48-bit load masked to 40 bits: original.
- W6 `:91` digit range `c > '9'` to `c > ':'`: seed.
- W7 `:70` digit count `t >= 10` to `t > 10`: seed.
- W8 `:86` numeric parse starts at 1: original.

Price text (`include/exsim/sim/price_text.hpp` (SP),
`src/exsim/server/config.cpp` (CF), `include/exsim/protocol/fix44.hpp` (FX)):

- P1 SP`:62` fraction digit limit `> 4` to `> 5`: original.
- P2 SP`:79` negative limit `1 << 63` to `(1 << 63) - 1`: original.
- P3 SP`:67` early guard `<< 59` to `<< 62`: equivalent. The limit check at
  `:80` rejects the same values, and the shift cannot overflow.
- P4 CF`:515` whole part bound `limit / kPriceScale` to `+ 1`: seed.
- P5 CF`:525` combined bound `limit - fraction_units` to `+ 1`: seed.
- P6 FX`:43` decimals limit `> 4` to `> 5`: original.
- P7 FX`:37` negative limit loses its extra unit (INT64_MIN rejected): original.
- P8 FX`:88` trailing zero trim disabled: original.
- P9 SP`:50` `fraction_digits >= 0` to `> 0`: original.
- P10 CF`:500` empty fraction after the dot accepted: original.
- P11 SP`:80` combined bound `(limit - fraction)` to `(limit - fraction + 1)`: seed.

Stream buffers (`include/exsim/net/stream_buffers.hpp` (BH),
`src/exsim/net/stream_buffers.cpp` (BC)):

- B1 BH`:65` ReceiveBuffer compaction test `<` to `<=`: original.
- B2 BH`:65` full-buffer clause removed: equivalent. With a capacity of 2 or
  more, `free < capacity / 2` already holds in that state. With a capacity of
  1, the state cannot be reached.
- B3 BH`:56` reset on drain disabled: original.
- B4 BH`:121` SendBuffer compaction test `<` to `<=`: survives (harness gap).
- B5 BH`:137` overflow latch ignored: original.
- B6 BC`:5` BufferFull check removed: original.
- B7 BC`:23` `sent == 0` branch removed: equivalent. `send` of a nonzero
  length never returns 0.
- B8 BC`:29` flush always reports Complete: survives (harness gap).
- B9 BH`:116` overflow branch disabled: original.

Unmutated, the extended corpora of all ten targets replay with no property
violation and no sanitizer report.

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
