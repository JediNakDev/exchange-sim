# exsim server runtime

The running exchange: `exsim_server`, the single-threaded reactor behind
it, the client tools, and measured wire-to-wire latency. The normative
contract is `docs/SERVER.md`; the core (submit, dispatch, bots, journal,
admin) is `docs/EXCHANGE_CORE.md`.

## 1. Running the server

```
make tools MODE=release
./build/release/exsim_server --config config/exsim.conf
```

| Flag | Meaning |
| --- | --- |
| `--config PATH` | configuration file (default `config/exsim.conf`) |
| `--busy-poll` | the loop polls with a zero timeout: lowest latency, one core at 100% |
| `--cpu N` | pin the server thread to CPU N (Linux; on macOS a warning is printed and the flag is ignored) |
| `--journal PATH` | override `journal.path` (an empty string disables the journal) |
| `--day-time HH:MM[:SS]` | start the trading-day clock at this local time instead of the wall clock (tests, demos) |
| `--md-heartbeat-ms N` | MoldUDP64 heartbeat interval while idle (default 1000) |
| `--no-bots` | start with the configured bots switched off (`bots on` starts them) |
| `--admin-bind ADDR` | admin console address (default `127.0.0.1`) |

The server runs one trading day. It ends on the admin `shutdown` command,
SIGINT or SIGTERM, the configured `exchange.trading_day_end`, or a journal
failure under `journal.on_failure = shutdown`. At the end it prints the
last market-data sequence number and the event-stream hash, which
`exsim_replay` reproduces from the journal. Exit status: 0 after a clean
end of day, 1 for bad arguments, configuration or startup errors (or a
journal failure).

Startup allocates everything: every user's token table and outbound store
(`engine.max_tokens_per_session`, `engine.store_*_per_session`), the
engine pools, connection slots and buffers, and the market-data ring. With
the example configuration this took 0.1 s on the machine in section 5
(longer when the machine is busy: wait for the "running" log line before
connecting). After that the loop
does not allocate on order entry, market data, bot or timer paths; only
operator paths (admin commands, log lines) do.

### Ports (example configuration)

| Port | Transport | What |
| --- | --- | --- |
| `exchange.ouch_port` 15000 | TCP | OUCH 4.2 over SoupBinTCP 3.0 |
| `exchange.fix_port` 15001 | TCP | FIX 4.4 |
| `exchange.admin_port` 15002 | TCP, loopback by default | admin and fault-injection console |
| `market_data.port` 16000 | UDP to `market_data.address` | ITCH 5.0 over MoldUDP64 (multicast or unicast) |
| `market_data.retransmit_port` 16001 | UDP | MoldUDP64 retransmission requests |

Port 0 means "pick an ephemeral port" for the listeners (the server logs
the chosen ports); for `market_data.port` it means "do not send". The
configuration format is documented in `include/exsim/server/config.hpp`
and every key appears in `config/exsim.conf`.

### Admin console

One command per line, replies end with `OK` or `ERR <message>`:

```
$ nc 127.0.0.1 15002
status
phase <SYMBOL|*> <closed|pre_open|continuous|halted|pre_close>
book <SYMBOL> [levels]
fault md drop <n> | dup <n> | reorder <n> [delay]
fault md rate [drop=<ppm>] [dup=<ppm>] [reorder=<ppm>] [delay=<n>] [seed=<s>]
fault md off
fault user <USER> reject <n> | delay <us> | drop_out <n> | disconnect | off
bots <on|off>
shutdown
help
```

Semantics are in `docs/SERVER.md` section 4 and `docs/EXCHANGE_CORE.md`
section 8. Runtime details:

* `fault user U delay us` is applied by the loop: received bytes stay in
  the connection's receive buffer and are released to the gateway in
  arrival order once their time has come (up to 256 delayed reads per
  connection are tracked).
* `fault user U drop_out n` affects application (business) messages only;
  FIX recovers in-session through ResendRequest, OUCH by reconnecting with
  its next expected sequence number.
* `fault user U disconnect` closes the connection and tells its gateway,
  exactly as when the client goes away (cancel-on-disconnect applies).
* `fault md off` also releases packets held for reordering.
* `shutdown`: every instrument goes to Closed through the engine, OUCH
  users get System Event `E` and End of Session, FIX users get Logout
  ("end of day"), the feed sends ITCH `S` `E` and `C` then MoldUDP64 End of
  Session. The server waits up to 3 seconds for clients to answer and for
  output to drain, then closes what is left and exits.

## 2. Client tools

`exsim_client`: OUCH 4.2 order entry from the command line.

```
./build/release/exsim_client --user ALICE --password alice --server 127.0.0.1:15000 \
    "buy A1 SPY 100 400.00" "wait 1500" "cancel A1"
./build/release/exsim_client --user BOB --password bob --script orders.txt
```

Commands (one argument each, or one per script line, `#` comments):
`buy|sell TOKEN SYMBOL SHARES PRICE [ioc]`, `replace TOKEN NEW_TOKEN SHARES
PRICE`, `cancel TOKEN [REMAINING_SHARES]`, `wait MILLISECONDS`. After each
order command the client waits for the exchange's answer about that token
(`--reply-timeout-ms`, default 2000), prints every message it receives,
lingers `--linger-ms` (default 200) and logs out. `--sequence N` logs in
asking for replay from N. Exit status 0 when every command was answered, 1
for bad arguments or a failed login, 2 when a command went unanswered or
the connection dropped. Remember cancel-on-disconnect: orders of a user
with `cancel_on_disconnect = true` (the default) are canceled when the
client logs out.

`exsim_md_client` prints the ITCH feed or the rebuilt book (see its
header). The admin console is a plain line protocol (`nc`).

The client-side socket plumbing used by the tools and the end-to-end tests
is `include/exsim/client/exchange_client_sessions.hpp`: `OuchTcpSession`,
`FixTcpSession`, `MarketDataFeed` (with gap requests) and
`AdminLineClient`.

## 3. Inside the runtime

`include/exsim/server/server_runtime.hpp`:
`using ServerRuntime = BasicServerRuntime<runtime::OuchGateway, runtime::FixGateway>`.

One thread runs `poll_once()` until the day is finished:

1. `poller.wait(busy_poll ? 0 : 1 ms)`, then route each event: order-entry
   connections (accept, read, `Connection::on_data` -> `core.submit` ->
   dispatch to sessions, market data and bots, all before the next message
   is decoded), admin lines, MoldUDP64 retransmission requests.
2. `core.timer(day_now)`: end of trading day, bots, NOII and the MoldUDP64
   heartbeat, journal flush.
3. Per endpoint: release delayed inbound bytes, gateway timers
   (heartbeats, logon timeouts), closes the gateway asked for.
4. Admin `disconnect` requests.
5. Shutdown progress (end-of-day messages once, then drain or timeout).
6. Flush every connection with pending output.

Pieces (all under `include/exsim/server/runtime/`):

| File | Role |
| --- | --- |
| `order_entry_gateways.hpp` | `OuchGateway`, `FixGateway`: the compile-time gateway shape |
| `order_entry_endpoint.hpp` | one TCP listener per gateway: preallocated connection slots, inbound delay, close handling; packs the core `ConnectionId` as `(endpoint tag << 56) \| (generation << 32) \| slot` |
| `gateway_sessions.hpp` | the core's `Sessions` policy: routes each event batch to the gateway owning the session |
| `market_data_runtime.hpp` | the core's `MarketData` policy: ITCH publisher -> `PacketFaultInjector` -> UDP, retransmission server, heartbeat, event-stream hash |
| `admin_endpoint.hpp` | admin TCP console (line buffers, 4096-byte line limit) |
| `server_clock.hpp` | trading-day clock (local time since midnight, monotonic) |
| `tcp_send_sink.hpp` | gateway byte sink over a connection's send buffer |
| `thread_affinity.hpp` | `--cpu` |

### Adding a protocol (gateway extension point)

A gateway is a type with the static shape documented at the top of
`order_entry_gateways.hpp` (name, user directory, port, `dispatch`,
`on_shutdown`, and a per-connection `Connection<Core>` with `open`,
`on_data`, `on_timer`, `wants_close`, `flush_before_close`,
`close_for_end_of_day`, `on_closed`, `bound_session`). There are no
virtual calls: the server lists gateways as template arguments, and the
loop, the delay fault, admin disconnects, end-of-day handling and the
allocation guarantees are shared.

SBE over FIXP (`docs/proposals/SBE_FIXP.md` section 3 on the
`exchange-simulator` branch) is not wired into this runtime yet. Wiring it
takes:

1. a `FixpSbeGateway` type in `order_entry_gateways.hpp`:
   `configured_port` returns `exchange.fixp_sbe_port`, the user directory
   holds the users with `protocol = fixp_sbe`, `dispatch` finds the user's
   session and hands it the batch, and `close_for_end_of_day` calls the
   session's `end_session` (FIXP Terminate after the end-of-day messages);
2. adding it to the list:
   `BasicServerRuntime<OuchGateway, FixGateway, FixpSbeGateway>`;
3. an end-to-end test next to `tests/exsim/server/e2e/fix_and_lifecycle_e2e_tests.cpp`
   and FIXP traffic in the allocation-guard test.

## 4. Tests

`tests/exsim/server/e2e/` (part of `make test`; filter with
`./build/<mode>/run_tests server_e2e`). Each test starts a server on
ephemeral loopback ports in a background thread and drives it with real
sockets; waits are bounded (5 s per condition).

| Test | Covers |
| --- | --- |
| `server_e2e_ouch.two_clients_trade_replace_cancel_and_hit_the_bots` | login, enter, replace, cancel, trades between two OUCH clients and against the bots |
| `server_e2e_md.feed_rebuilds_the_engine_book_through_packet_faults_and_retransmission` | `ItchBookBuilder` on the feed equals `engine.depth()` exactly, with and without MD drop/dup/reorder faults, gaps filled by the retransmission server |
| `server_e2e_admin.status_phase_and_book_commands` | admin status, phase, book |
| `server_e2e_faults.user_reject_and_delay_keep_arrival_order` | `reject`, `delay` |
| `server_e2e_faults.drop_out_is_recovered_by_reconnecting_with_the_sequence_number` | OUCH `drop_out` recovered through the SoupBinTCP login sequence number |
| `server_e2e_faults.admin_disconnect_closes_the_session_and_cancels_on_disconnect` | `disconnect` |
| `server_e2e_sessions.cancel_on_disconnect_follows_the_user_setting` | cancel-on-disconnect on and off, replay of stored cancels |
| `server_e2e_fix.enter_replace_cancel_and_trade_against_an_ouch_client` | FIX order entry, FIX vs OUCH trade |
| `server_e2e_fix.drop_out_is_recovered_through_resend_request` | FIX `drop_out` recovered in-session (PossDup resend) |
| `server_e2e_lifecycle.shutdown_ends_every_session_and_the_feed` | `shutdown`: OUCH `S`/`E` + End of Session, FIX Logout, ITCH `S` `E`/`C` + End of Session |
| `server_e2e_lifecycle.journal_replays_to_the_published_event_stream` | the journal replays to the same event count and hash as the live feed |
| `server_e2e_allocation.steady_state_order_entry_market_data_and_bots_do_not_allocate` | after warm-up, 10000+ OUCH and FIX round trips with trades, bots and the ITCH feed: zero heap allocations on the server thread (`testfw::AllocationScope`; counting is unavailable under sanitizers) |

## 5. Wire-to-wire latency

`exsim_w2w_bench` runs the same `ServerRuntime` in-process on its own
thread (OUCH, FIX and admin listeners, ITCH feed to a local UDP socket, no
bots, no journal) and an OUCH client on the main thread. The client sends
Enter Orders (buy 100 @ 99.00, a fresh token each) at a fixed rate and
cancels each one when it is accepted. On the client's monotonic clock it
measures:

* **ouch accept**: Enter Order written to the socket -> Order Accepted read
  back;
* **itch add**: Enter Order written -> the matching ITCH Add Order read from
  the UDP feed.

```
make tools MODE=perf
./build/perf/exsim_w2w_bench --rate 20000 --orders 100000 --warmup 10000 [--busy-poll]
./build/perf/exsim_w2w_bench --rate 0 --window 32 --orders 200000 --warmup 10000 [--busy-poll]
```

Measured 2026-11-05 on an Apple M6 Mac mini (12 cores, 24 GB, macOS
27.0.1), `MODE=perf`, Apple clang, no CPU pinning (macOS has none), load
average about 2.5 to 7 from other work on the machine. Nanoseconds:

| Run | Server poll | ouch accept p50 / p90 / p99 / p99.9 / max | itch add p50 / p90 / p99 / p99.9 / max | Orders/s |
| --- | --- | --- | --- | --- |
| `--rate 1000 --orders 20000 --warmup 2000` | blocking | 66000 / 243000 / 3252000 / 7490000 / 17421000 | 61000 / 225000 / 3190000 / 7442000 / 17440000 | 1000 |
| same | busy | 63000 / 80000 / 783000 / 3841000 / 6552000 | 57000 / 73000 / 764000 / 3811000 / 6481000 | 1000 |
| `--rate 20000 --orders 100000 --warmup 10000` | blocking | 54000 / 127000 / 1212000 / 3363000 / 4940000 | 44000 / 107000 / 1010000 / 2925000 / 4121000 | 19997 |
| same | busy | 54000 / 144000 / 1596000 / 3932000 / 5387000 | 45000 / 123000 / 1368000 / 3393000 / 5000000 | 20000 |
| `--rate 0 --window 32 --orders 200000 --warmup 10000` | blocking | 387000 / 429000 / 488000 / 691000 / 5705000 | 323000 / 374000 / 424000 / 595000 / 5611000 | 76126 |
| same | busy | 384000 / 427000 / 484000 / 772000 / 2245000 | 319000 / 373000 / 424000 / 603000 / 2219000 | 76258 |

Every run: zero retransmission requests, zero lost messages, zero rejects.
Plus one cancel per order, so the server handled about 152000 inbound
messages per second in the windowed runs.

Reading the numbers:

* A raw loopback TCP round trip (64 bytes, blocking echo thread, 1000/s)
  on the same machine and minute measured p50 62875, p90 68042, p99 79375,
  p99.9 123208 ns. The server's median is at that floor: the medians above
  are dominated by the macOS loopback path and thread wake-ups, not by
  exsim.
* ITCH add arrives before OUCH accept because the core sends the MoldUDP64
  packet right after dispatching each command's events, while order-entry
  output is flushed once at the end of the loop iteration.
* The tails (p99 in the millisecond range) are macOS scheduling: the
  client and the server are unpinned and share the machine with other
  work. Busy polling cuts the low-rate p90 and p99 (no wake-up from a
  blocking `kevent`), but at 20000/s the blocking loop rarely sleeps and
  the two modes are the same within noise.
* The windowed runs measure throughput, not latency: with 32 orders in
  flight, latency is queueing (32 / 76000 per second is about 420 us). The
  single-threaded client (one `write` per order, then reads) is likely the
  limit there; the server was not saturated.
* These are not Linux numbers. A smoke run in a Linux container (OrbStack
  VM on the same Mac, g++, `--rate 5000 --orders 5000 --warmup 500
  --server-cpu 0 --client-cpu 1`) confirmed that pinning works
  (`Cpus_allowed_list: 0` for `exsim_server --cpu 0`) and gave ouch accept
  p50 12041, p90 15250, p99 616626 ns; a VM sharing a busy host is not a
  latency reference, so tuned Linux numbers are still to be measured.
