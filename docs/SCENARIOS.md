# Exchange scenarios and differential checks

The oracle implements SPEC.md using maps, FIFO lists and straightforward
loops. It allocates intentionally. Production tests are opt-in until a real
MatchingEngine implementation is linked. No reference shim is a library source.

Run independent checks from the repository root:

```sh
make test
make test MODE=debug
make build/release/exsim_fuzz build/release/exsim_replay build/release/exsim_scenario EXTRA_CXXFLAGS=-DEXSIM_REFERENCE_ONLY
build/release/exsim_fuzz --seed 1 --seeds 200 --steps 10000 --profile mixed
build/release/exsim_fuzz --seed 1 --seeds 200 --steps 10000 --profile tiny
build/release/exsim_fuzz --inject --seed 1 --seeds 1 --steps 10000 --artifact /tmp/fifo-bug
build/release/exsim_scenario /tmp/fifo-bug.scn
build/release/exsim_replay /tmp/fifo-bug.journal
build/release/exsim_fuzz --replay /tmp/fifo-bug.journal
```

The injection command deliberately exits 1. It enables a test-only LIFO
mutation on one reference instance; normal runs have all mutations disabled.
A failure prints seed, step, command, both complete event streams and state
differences, then minimizes the prefix and writes a numeric, pasteable scenario
and canonical journal. Minimization removes chunks, then individual commands
until no individual deletion retains a divergence. This is 1-minimal, not a
claim of globally shortest reproduction. Give concurrent fuzz processes unique
`--artifact` prefixes. Generator weights are configurable through
CommandWeights in command_gen.hpp. CLI profiles select mixed/default capacity
or eight live order slots. All RNG draws use mt19937_64 modulo arithmetic,
without implementation-dependent standard distributions.

For EACH engine variant, run these in its integrated checkout after applying
this testing infrastructure. Do not copy the local disabled shim.

```sh
make test          # the Makefile defines EXSIM_TEST_PRODUCTION
make test MODE=debug
make tools
build/release/exsim_fuzz --seed 1 --seeds 200 --steps 10000 --profile mixed
build/release/exsim_fuzz --seed 1 --seeds 200 --steps 10000 --profile tiny
build/release/exsim_fuzz --replay /tmp/fifo-bug.journal
```

These checks compare every event field, ordering, both complete depth ladders,
FIFO IDs at each level, order enumeration and both order lookups, phases,
best prices, last price and auction indicative state after each command.
The production allocation test measures 200,000 mixed commands; generation
and queries run outside the measured handle call. Global replacements include
aligned and sized forms. ASan/TSan builds exclude allocation replacement tests.
Direct malloc/mmap calls are outside this C++ allocation guard.
The default make test does not exercise the production engine.

## Language

Blank lines and trailing `# comments` are ignored. Errors include line numbers.
Declarations precede commands. Prices use decimal dollars with up to four
fractional digits; no binary floating point is used. Integers use decimal
(or 0x prefixes). Boolean fields accept true/false or 0/1. Enums use the exact
uppercase names in types.hpp. Unspecified command fields have API defaults.

```text
config max_orders=128 max_instruments=3
instrument AAPL tick=0.01 lot=1 max_qty=100 min=99 max=101 ref=100 phase=CONTINUOUS
session alice firmA
session bob firmB
NEW_ORDER instrument=AAPL session=alice owner=firmA cl_ord_id=alice:s1 side=SELL qty=40 price=100.25
expect count 2
expect accepted order_id=1 price=100.25 qty=40
expect rested leaves_qty=40
NEW_ORDER instrument=AAPL session=bob owner=firmB cl_ord_id=bob:b1 side=BUY qty=10 price=100.25
expect count 2
expect accepted
expect fill price=100.25 qty=10 buy.cl_ord_id=bob:b1 sell.cl_ord_id=alice:s1
expect book AAPL ask 100.25 30 1
expect depth AAPL bid
expect depth AAPL ask 100.25:30:1
expect order session=alice cl_ord_id=alice:s1 cum_qty=10 leaves_qty=30
CANCEL session=alice orig_cl_ord_id=alice:s1 reduce_to_total_qty=20
expect canceled canceled_qty=20 cum_qty=10 leaves_qty=10
REPLACE session=alice orig_cl_ord_id=alice:s1 qty=20 price=100.25 force_priority_loss=true
expect replaced priority_kept=false
expect rested
SET_PHASE instrument=AAPL phase=HALTED
expect phase AAPL HALTED
MASS_CANCEL session=alice cancel_on_disconnect=true
expect canceled reason=CANCEL_ON_DISCONNECT
expect mass_cancel_done canceled_count=1
```

Instrument fields: tick, lot, max_qty, min, max, ref, phase.
All command types and their exact public field names are supported:
NEW_ORDER, CANCEL, REPLACE, MASS_CANCEL, SET_PHASE. Symbol and session names
must be declared; owner and token names are assigned numeric IDs on first use.
A session's owner declaration reserves a name; commands explicitly specify
owner (the default zero disables STP). Tokens are scoped by command session in
the engine; using session-qualified text names is recommended for clarity.
Numeric IDs are also accepted, including intentionally invalid IDs/enums.

Each `expect <event_type> key=value ...` consumes the next event from the
previous command. Field names are those in fields.hpp; fill legs use
`buy.session`, `sell.cl_ord_id`, etc. Assertions may specify every field or any
subset. Add `expect count N` to assert the exact number of events; unmentioned
events are otherwise permitted. `expect no events` asserts zero output.
`expect book SYMBOL bid|ask PRICE QTY COUNT` asserts one level.
`expect depth SYMBOL bid|ask [PRICE:QTY:COUNT ...]` asserts the complete side in
priority order, including an empty side. `expect order session=... cl_ord_id=...
[field=value ...]` checks live order fields. `expect phase SYMBOL PHASE`
checks phase. Timestamps are explicit command fields (default zero).

## Journal schema 1

All values are explicit big-endian slots, independent of C++ layouts:

* Header: eight-byte magic 0x455853494d4a4e4c, u64 schema=1,
  u64 max_instruments, u64 max_orders, u64 instrument count.
* Each instrument: exactly eight symbol bytes, then u64 tick, lot, max quantity,
  min, max, reference, initial phase in that order.
* Header checksum: u64 FNV-1a over all preceding header bytes, including schema
  and configuration. This identifies configuration changes; it is not a
  cryptographic authentication mechanism.
* Record: u64 zero-based sequence, 264-byte command encoding, u64 checksum
  of the preceding 272 bytes.
* Command encoding: u64 command tag, then u64 slots for every active command
  field in fields.hpp visitor order. Unused bytes through 264 are zero.
  Signed prices use two's complement. All new cancel/replacement fields are
  encoded. Noncanonical widths, bools, tags and reserved bytes are rejected.
* Footer: u64 magic 0x455853494d454e44, u64 record count, u64 checksum of the
  first 16 footer bytes. Detects missing final records and missing footer.

The writer reserves its fixed buffer at construction. append and flush return
bool and never allocate or throw. A failed writer stays failed. Explicitly
check flush before considering a journal complete; the destructor only closes
the file and cannot report write failures. flush checkpoints a readable prefix;
a later append removes its footer before extending the stream. This is a
single-writer regular-file format. flush drains to write(2), not fsync; power
loss durability is outside this API. Readers validate the complete header,
configuration, footer, record counts, sequence, checksums and canonical command
encodings before returning a command. Empty, truncated and corrupt journals
fail with an exception in the reader/setup path.

Event hash: FNV-1a over successive 264-byte canonical event encodings, with the
event tag first, instrument and timestamp next, then active fields in
fields.hpp order, zero unused bytes. Includes partial cancellation leaves and
auction cross match IDs. Schema changes must bump kSchema. Never hash raw
Command/Event bytes. Replay prints commands, events and this hash.

## Remaining contract interpretations

* An unknown top-level CommandType has no specified rejection payload. The
  oracle applies a no-op (after output preflight); journals reject unknown tags.
  Random generation uses valid top-level tags and invalid member enums.
* phase(unknown instrument) returns Closed; the contract does not define it.
* Market FOK runs the FOK precheck, following section 4.2 literally. Section 4.6
  says market TIF is ignored for resting disposition. This could be read more
  broadly; the precheck is isolated in apply(NewOrder).
* Canonical repairs resolve the old initial-phase/config, rejected field,
  Halted crossing and summary-ID ambiguities. Those are no longer alternatives.

The oracle's mutation switches are disabled by default and exist solely to
exercise the differential detector. Reference-vs-reference fuzzing proves
harness consistency, not independent evidence that matching semantics are
correct. Hand-authored scenarios, config/preflight gates, wide aggregates and
random state invariants provide that independent validation; integration with
each real engine remains required.
