# exsim server: runtime, gateway contract, admin and fault injection (normative)

How the pieces (engine, codecs, gateways, network, market data, journal,
bots) are wired into the `exsim` server binary. Read with `docs/SPEC.md`
(engine), `docs/GATEWAYS.md` (protocol mapping) and `docs/ARCHITECTURE.md`
(coding standard).
The implementation, how to run it and measured latency are in
`docs/SERVER_RUNTIME.md`.

## 1. Process model

One thread owns everything that touches order state, in this loop:

```
loop:
  now = clock()
  poller.wait(busy_poll ? 0 : timeout)           # net layer
  for each readable connection:
      recv into the connection's receive buffer
      connection.on_data(bytes, now, exchange)   # gateway: frames -> commands
          └─ exchange.submit(cmd)                # synchronous, see §2
  timers(now): session heartbeats/timeouts, MoldUDP64 heartbeat,
               NOII, bots, delayed (fault-injected) inbound messages
  flush every connection with pending output
```

* Commands are processed strictly one at a time in arrival order. Before
  the next inbound message is decoded, every event of the previous command
  has been encoded to every affected session and to market data. This is
  what makes the server deterministic and replayable from the journal.
* `exchange.submit(cmd)`:
  1. stamps `cmd.ts` from a monotonic day-relative clock, checks the configured trading-day range,
  2. ensures sufficient EventBuffer remaining capacity before submission,
  3. `engine.handle(cmd, events)`; InsufficientOutput leaves state/output unchanged and must be retried before further commands; append applied commands to the canonical journal (if enabled),
  4. dispatches the batch (§2.3),
  5. flushes the current MoldUDP64 packet (never held across commands).
* `--busy-poll` spins with a zero timeout; `--cpu N` pins the thread
  (Linux; ignored with a warning on macOS).
* Startup allocates everything (engine pools, token tables, stores,
  connection slots, MD ring). After the first `poll` the hot path must not
  allocate. The allocation-guard end-to-end test enforces this.

## 2. Gateway contract

Gateways are templates over the server's context type (no virtual
dispatch). State is split by lifetime:

| Object | Lifetime | Holds |
|---|---|---|
| `XxxUserState` | whole trading day, one per configured user | `TokenTable`, per-order metadata indexed by `ClOrdId`, `SequencedStore` of outbound application messages, protocol counters (ExecID, FIX outbound MsgSeqNum) |
| `XxxConnection` | one TCP connection | framer, login state, heartbeat timers, inbound sequence (FIX), pointer to the bound `XxxUserState` once logged in |

### 2.1 Context (implemented by the server, used by gateways)

```cpp
struct Context {                                // exposition only
    const server::UserConfig* authenticate(std::string_view user,
                                           std::string_view password,
                                           server::Protocol) noexcept;
    bool bind(SessionId, ConnectionId) noexcept; // false: already logged in
    void unbind(SessionId, ConnectionId) noexcept; // logout/disconnect of that
                                                 // connection (no-op if another one
                                                 // is bound); triggers
                                                 // cancel-on-disconnect if configured
    InstrumentId find_instrument(const Symbol&) const noexcept;  // 0 if unknown
    const InstrumentConfig* instrument(InstrumentId) const noexcept;
    void submit(const Command&) noexcept;        // §1; events dispatched before return
    Timestamp now() const noexcept;              // ns since local midnight
    std::uint64_t realtime_ns() const noexcept;  // ns since Unix epoch (FIX SendingTime)
};
```

### 2.2 Inbound

`std::size_t XxxConnection::on_data(std::span<const char> bytes, Context&)`
consumes complete frames and returns the number of bytes consumed (the
remainder stays in the receive buffer). Session-level messages are handled
inside the gateway; application messages become `Command`s passed to
`ctx.submit`. A message the gateway itself rejects (unknown symbol,
token table full, injected reject, ...) never reaches the engine; the
gateway encodes the reject directly.

### 2.3 Outbound dispatch

After `engine.handle`, the server calls, for every distinct session that
appears in the batch (fill events reference two sessions), exactly once:

```cpp
void XxxUserState::on_events(std::span<const Event> batch, Context&);
```

The gateway encodes the events that concern its session, in batch order,
looking ahead within the batch where `GATEWAYS.md` requires it (OUCH
order state `L`/`D`). Each application message is appended to the user's
`SequencedStore` (assigning its sequence number) and, if a connection is
bound and not suppressed by fault injection (§4), written to that
connection's send buffer. Sessions with no live connection still get every
message stored, so a later login can recover it.

Then `MarketDataPublisher::on_events(batch)` and `flush()` (ITCH + MoldUDP64,
§3).

### 2.4 Output buffers

Gateways write into a byte sink with two operations:

```cpp
char* prepare(std::size_t n) noexcept;  // nullptr: no room
void  commit(std::size_t n) noexcept;
```

The server adapts the net layer's send buffer to it. If `prepare` fails,
the connection is overloaded: the server disconnects it (the messages are
still in the store, so the client recovers on reconnect). Gateways never
allocate to buffer output.

### 2.5 Client side

Every gateway also provides a client implementation (login, sequencing,
heartbeats, recovery, typed callbacks for outbound-from-exchange messages)
used by tests, `exsim_client` and the latency benchmark. The client side may
use simpler code but must not allocate per message either, so it can be used
for latency measurement.

## 3. Market data

`MarketDataPublisher` (ITCH 5.0 encoder + MoldUDP64 packetizer):

* holds the next MoldUDP64 sequence number and a pre-allocated ring of the
  last `market_data.retransmit_buffer` messages (bytes + sequence number);
* `on_events(batch)` appends ITCH messages to the current packet, emitting a
  full packet early when the next message would exceed `max_payload`;
* `flush()` emits the current packet if non-empty;
* every emitted packet goes through `sim::PacketFaultInjector` and then the
  UDP sender;
* heartbeat (message count 0) after 1 s of silence; end of session (count
  0xFFFF) at shutdown;
* NOII (`I`) for every instrument in a call phase every
  `noii_interval_ms`, from `engine.auction_indicative()`;
* the retransmission server answers MoldUDP64 request packets
  (session, sequence number, count) from the ring with unicast packets to
  the requester; requests outside the ring are answered with nothing (the
  client must then rebuild from a snapshot or give up; documented).

Client side: `MoldUdp64Receiver` (gap detection, duplicate suppression,
out-of-order buffering, retransmission requests with timeout) feeding
`ItchBookBuilder`, a client-side order book rebuilt purely from ITCH
(`A/F/E/C/X/D/U/Q/H/I`). It must reproduce `engine.depth()` exactly after
all packets for each command batch; this is the feed-correctness test, run with and without
injected packet faults.

## 4. Admin interface and fault injection

Line protocol over TCP on `exchange.admin_port` (loopback only by default).
Each request is one line; each reply is one or more lines ending with
`OK` or `ERR <message>`. Commands:

| Command | Effect |
|---|---|
| `help` | list commands |
| `status` | connections, logged-in users, orders, MD sequence, fault state |
| `phase <SYMBOL\|*> <closed\|pre_open\|continuous\|halted\|pre_close>` | `SetPhase` (auctions uncross per SPEC) |
| `book <SYMBOL> [levels]` | depth of both sides |
| `fault md drop <n>` / `dup <n>` / `reorder <n> [delay]` | one-shot MD packet faults |
| `fault md rate drop=<ppm> dup=<ppm> reorder=<ppm> delay=<n> seed=<s>` | random MD faults |
| `fault md off` | clear MD faults |
| `fault user <USER> reject <n>` | the gateway rejects the user's next n new orders itself (OUCH `J` reason `O`, FIX ER 39=8 Text "injected"); the engine never sees them |
| `fault user <USER> delay <us>` | hold each inbound application message of the user for `us` microseconds before decoding (orders stay in arrival order) |
| `fault user <USER> drop_out <n>` | do not transmit the user's next n sequenced application messages (still stored; session-level messages are never dropped): exercises client recovery. FIX and FIXP/SBE leave a gap the client detects from explicit sequence numbers (FIX sends ResendRequest); OUCH (implicit SoupBinTCP numbering) closes the connection after the n-th, and the client recovers them by replay on reconnect (GATEWAYS.md section 2) |
| `fault user <USER> disconnect` | close the user's connection now; the gateway is told as for any disconnect (cancel-on-disconnect applies) |
| `fault user <USER> off` | clear the user's faults |
| `bots <on\|off>` | start/stop simulated liquidity |
| `shutdown` | end of day: `SetPhase Closed` for all instruments, OUCH `S`/`E`, ITCH `S`/`E` and End of Session, flush, exit |

Admin commands that change engine state go through `exchange.submit` like
any other command, so they are journaled and replay identically.

## 5. Simulated liquidity

Bots are in-process sessions. Each bot is bound to a configured user
(`[user ...]` with its own `session_id`/`owner`) and submits commands
through `exchange.submit` on timer ticks (and in reaction to its own fills),
so bot activity is journaled and replayable. Bots are deterministic for a
given seed and must not allocate after startup.

* `market_maker`: quotes `levels` price levels per side around a fair
  value that follows a seeded random walk, `size` per level, `spread_ticks`
  wide, re-quoting via replace (keeping priority where possible) every
  `requote_ms` and after fills; obeys phases (no orders while Closed or
  Halted; quotes during call phases form the auction).
* `noise_taker`: at a seeded Poisson rate submits IOC orders of random size
  crossing the spread (and occasional limit orders), creating trades for
  clients to observe.

## 6. Journal and replay

The journal records every command handed to the engine (after
timestamping) plus a header with the configuration hash. `exsim_replay`
re-runs a journal through a fresh engine with the same configuration and
verifies the event-stream hash, which makes any production incident
reproducible offline. Market-data and order-entry output can be
regenerated from the replayed events.
