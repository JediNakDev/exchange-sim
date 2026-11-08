# Exchange core

`exsim::server::ExchangeCore<Sessions, MarketData, Clock>`
(`include/exsim/server/exchange_core.hpp`) is the protocol-independent heart
of the exsim server. It owns the matching engine, the event buffer, the
configured users and their connection bindings, the canonical journal, the
simulated-liquidity bots and all admin and fault-injection state. Gateways
(OUCH, FIX) use it as the Context of `docs/SERVER.md` section 2.1; the market
data publisher and the gateways are plugged in as compile-time policies.

Everything runs on the single server thread. Construction and admin
commands may allocate and throw; `submit`, dispatch, `timer`, `bind`,
`unbind` and the fault queries never allocate, throw or use virtual calls.

## 1. Construction

```cpp
ServerConfig config;  // load_config(...) has validated it
MySessions sessions;
MyMarketData market_data;
MyClock clock;
ExchangeCore<MySessions, MyMarketData, MyClock> core(config, sessions, market_data, clock);
```

The core copies the configuration (pointers returned by `authenticate` point
into that copy), adds the instruments (ids 1..n in file order), indexes
users by `SessionId`, registers one bot per `[bot NAME]` section and opens
the journal when `[journal] path` is set. `CoreOptions::event_buffer_capacity`
exists for tests only (section 4).

## 2. Policies

Contracts live in `include/exsim/server/core/policies.hpp`.

```cpp
using ConnectionId = std::uint64_t;  // opaque; e.g. (slot << 32) | generation

concept CoreClock:      c.day_ns()      -> Timestamp      ns since local midnight
                        c.realtime_ns() -> uint64         ns since the Unix epoch
concept CoreMarketData: md.on_events(std::span<const Event>)  one call per batch
                        md.flush()                            emit the open packet
concept CoreSessions:   s.on_events(SessionId, std::span<const Event>, Core&)
                        once per distinct non-bot session of a batch, in
                        first-appearance order, with the whole batch

optional, detected with requires-expressions:
  md.on_timer(Timestamp, const Core&)  or  md.on_timer(Timestamp)
                                       heartbeat, NOII; called from core.timer()
  md.next_sequence() -> uint64         shown by `status`
  md.packet_faults() -> sim::PacketFaultInjector&
                                       target of `fault md ...`
  md.reset_packet_faults()             used by `fault md off` when present
  md.on_shutdown(Timestamp)            end of system hours / end of session
  s.on_shutdown(Timestamp, Core&)      OUCH / FIX end-of-day messages
  md.on_events / on_timer / on_shutdown returning md::PublishStatus
                                       observed by the core (below)
  md.state() -> md::PublisherState, md.stop_reason() -> md::PublishStatus
                                       shown by `status`
```

**Market-data status.** The real `md::MarketDataPublisher` reports failures
as `md::PublishStatus` and stops publishing after an invalid batch (state
`Stopped`). When a MarketData hook returns `md::PublishStatus` instead of
void, the core observes it: every `on_events` result other than `Ok` counts
in `CoreCounters::market_data_failed_batches` (a batch that did not reach the
feed), and the last non-Ok result of `on_events`, `on_timer` or
`on_shutdown` is kept in `CoreCounters::market_data_last_failure`. Matching
and journaling go on; the feed outage is visible in `status`
(`md state=stopped stop_reason=... failed_batches=N last_failure=...`) and the
operator decides. Policies stay compile-time: the core detects the return
type and the optional `state()`/`stop_reason()` with requires-expressions.

`Sessions::on_events` receives the core so a gateway can look up instruments,
read `now()`, or call `unbind`/`submit` (those are deferred, section 5).

## 3. Context API (SERVER.md 2.1)

| Call | Behaviour |
| --- | --- |
| `authenticate(user, password, protocol)` | `const UserConfig*` or `nullptr` for an unknown user, wrong protocol, wrong password or a user driven by a bot. An empty configured password accepts any password. Names are case sensitive. |
| `bind(session, connection)` | `false` for an unknown session, a bot session, a session that is already bound (second login), or a call from inside a dispatch callback (section 5). At most one live binding per session. |
| `unbind(session, connection)` | No-op unless `session` is bound to `connection`, so a late release from an older connection never unbinds a newer login or cancels its orders. Clears a pending disconnect request. When the user has `cancel_on_disconnect`, submits `MassCancel{session, instrument 0, cancel_on_disconnect = true}` through the normal path (journaled, dispatched). After shutdown that MassCancel either is refused (`OutsideTradingDay`, past the end of the trading day) or finds nothing to cancel: an unbind after shutdown relies on the end-of-day close, which has already cancelled every order. |
| `find_instrument(symbol)`, `instrument(id)` | Engine lookups. |
| `submit(command)` | Section 4. |
| `now()` | ns since local midnight: the clock, never earlier than a value already returned or a command already stamped. |
| `realtime_ns()` | The clock's wall time, for FIX SendingTime. |

Queries for tests and status: `user`, `is_bound`, `connection`,
`bound_connection_count`, `is_bot_session`, `engine`, `config`, `counters`,
`event_buffer_capacity`, `last_command_timestamp`, `bots`, `journal_state`.

## 4. submit

`SubmitResult submit(const Command&)` follows SERVER.md section 1:

1. **Stamp.** `ts = clock.day_ns()`, or `last + 1` when the clock did not
   move forward (repeating or stepping back). Command timestamps are strictly
   increasing for the whole run.
2. **Trading day.** A stamp outside `[trading_day_start, trading_day_end)` is
   refused: `OutsideTradingDay`, nothing applied, journaled or dispatched.
3. **Capacity and handle.** The buffer is cleared (the previous batch has
   been fully dispatched), then `engine.handle`. The buffer holds
   `required_event_capacity(engine config)` events, the engine's proven bound
   for any single command, so `InsufficientOutput` cannot happen in
   production. If it does (only with a test-sized buffer), the engine and
   buffer are unchanged; a retry on the same empty buffer would fail the same
   way, so the core returns `OutputCapacityExceeded` without journaling or
   dispatching anything and counts it. The command is not silently lost: the
   caller gets the result. `server_core_dispatch.output_capacity_failure_*`
   proves engine state is unchanged and the core keeps working.
4. **Journal.** Every applied command (semantic rejections included) is
   appended with its stamp. Failures follow section 9.
5. **Dispatch.** Sessions first (each distinct session once, first
   appearance, buy leg before sell leg in a Fill, session 0 and bot
   sessions skipped), then `MarketData::on_events(batch)` and `flush()`,
   then every bot's `on_events`. Empty batches are not dispatched.

Results: `Applied`, `Deferred`, `OutsideTradingDay`, `OutputCapacityExceeded`,
`DeferredQueueFull`, `ExchangeShutDown` (section 13). When `submit` returns `Applied`, the command and every
command deferred meanwhile have been fully dispatched.

The dispatch set is a 65536-bit seen set plus a fixed list of every possible
session id, so deduplication never allocates or overflows. Sessions that are
not configured (for example a SetPhase rejection to session 0, or a test
session) are dispatched too when non-zero; the gateway policy decides.

## 5. Re-entrancy

A gateway may call `submit` or `unbind` from inside `Sessions::on_events`
(for example closing a connection while it handles a batch). The core never
re-enters the engine: such commands go to a fixed ring
(`DeferredCommandQueue`, capacity users + 64) and are applied, each with its
own stamp and batch, right after the current batch reaches market data and
bots. `submit` returns `Deferred`, or `DeferredQueueFull` (counted) when the
ring is full. A pending shutdown runs after the deferred commands.

Cancel-on-disconnect is never dropped. The ring holds `2 * users + 64`
commands: ordinary deferred submits may fill only `users + 64` of them, and
the remaining `users` slots are reserved for the MassCancel of `unbind`.
`bind` is refused during dispatch (logins come from inbound data, never from
a dispatch callback), so between two drains each user unbinds at most once
and every unbind MassCancel fits. `server_core_review.unbind_during_full_deferred_queue_keeps_cancel_on_disconnect`
fills every ordinary slot and then unbinds.

## 6. Timer

`core.timer(loop_now)`, once per server loop iteration:

1. The time handed on is made non-decreasing (never behind a stamped
   command).
2. At or after `trading_day_end`, shutdown runs (section 8).
3. Bots: `on_timer(now, core)` when not shut down and `now >= trading_day_start`.
4. Market data: `on_timer(now, core)` or `on_timer(now)` when present.
5. Journal: flushed every `flush_interval_ms` (0 disables the periodic
   flush), so the file on disk is a valid journal ending at the last flush.

## 7. Bots

`[bot NAME]` sections build a `sim::bots::BotSet<32>` registered in config
order. Each bot drives one configured user exclusively:

* the bot's `session_id` and `owner` come from that user, `stp` too unless
  the section sets it; `instrument` is a symbol;
* that user cannot authenticate or bind, and `fault user` rejects it;
* bot sessions are never handed to `Sessions` (there is no wire for them),
  but every batch reaches every bot after market data;
* bot commands go through `submit` from the timer: stamped, range-checked,
  journaled and dispatched like client commands. Bots never submit from
  `on_events`.

`bots on|off` (or `set_bots_enabled`) toggles every bot; a disabled maker
pulls its quotes on its next timer.

## 8. Admin

`execute_line(line, reply)` parses with `parse_admin` and runs `execute`.
Every reply is `\n`-terminated lines ending with `OK` or a single
`ERR <message>`.

| Command | Reply lines before `OK` | Errors |
| --- | --- | --- |
| `help` | usage text | |
| `status` | exchange, trading day, connections, one line per user (bound, connection, faults), live orders, one line per instrument (phase, bid, ask, last), md sequence, md faults, md state (publisher state and stop reason, or `unavailable`; failed batches; last failure), bots, one line per bot, journal, command counters | |
| `phase SYM\|* PHASE` | `SYM OLD -> NEW` per instrument; applied through `submit` | unknown instrument, `phase not applied: <reason>`, `exchange is shut down` |
| `book SYM [n]` | `book SYM phase=P`, asks from deepest to best, bids from best down (`ask 150.01 qty=150 orders=2`), or `(empty)`; at most 1000 levels | unknown instrument |
| `fault md drop\|dup N`, `fault md reorder N [D]`, `fault md rate ...`, `fault md off` | `md faults ...` (config, held, stats) | `market data exposes no packet fault injector` |
| `fault user U reject N\|delay US\|drop_out N\|disconnect\|off` | `user U session=S reject=.. delay_us=.. drop_out=.. disconnect=..` | unknown user, bot user, disconnect when not connected |
| `bots on\|off` | `bots on (N configured)` | |
| `shutdown` | `all instruments closed`, `SetPhase Closed not applied: <reason>` or `already shut down` | |

**Market-data faults** are owned by the MarketData policy: it exposes its
`sim::PacketFaultInjector` through `packet_faults()` and the core only
configures it. `reorder N D` sets the reorder delay through `configure`
(which reseeds the injector's random generator with the configured seed)
before adding N one-shot reorders. `off` calls `reset_packet_faults()` when
the policy has it; otherwise it clears the random rates and reports a note
when one-shot faults or held packets remain, because the injector has no way
to clear them.

**User faults** live in the core, one `UserFaultState` per user, and are
consumed by gateways and the loop:

| Query | Who | Meaning |
| --- | --- | --- |
| `consume_injected_reject(session)` | gateway, per inbound new order | `true`: reject it yourself (OUCH J, FIX 39=8 Text "injected"), do not submit. Consumes one. |
| `inbound_delay_ns(session)` | loop or gateway | hold each inbound application message this long (a setting, not consumed) |
| `consume_drop_out(session)` | gateway, per sequenced outbound application (business) message | `true`: store it but do not transmit. Consumes one. Session-level messages (logon, heartbeats, logout, resend replies) are never dropped. |
| `has_pending_drop_out(session)` | OUCH gateway, after a drop | `true` while drop counts remain (not consumed); OUCH closes the connection after the last dropped message |
| `has_disconnect_requests()`, `take_disconnect_requests(span)` | loop | sessions to close now, config order; the loop closes each one's connection and tells its gateway (`on_disconnected`), whose disconnect handler calls `unbind(session, connection)` (cancel-on-disconnect applies); the loop does not unbind itself |
| `user_faults(session)` | status, tests | the whole state |

A new `reject`/`drop_out`/`delay` replaces the previous value (counts start at
1; `off` clears all). Counts survive a reconnect; a pending disconnect
request is dropped by `unbind`.

**Shutdown** (admin, `shutdown()`, end of trading day, or a journal failure
under the `shutdown` policy): `SetPhase{instrument 0, Closed}` through the
normal path, stamped inside the trading day (clamped to its last
nanosecond, so the closing batch is representable on every wire), then
`Sessions::on_shutdown` and `MarketData::on_shutdown` when present, then a
journal flush. `shutdown_requested()` turns true; the server loop exits when
it sees it. Idempotent. If the day's last nanosecond is already used, the
close is reported as not applied but the exchange still shuts down.

## 9. Journal

`[journal]` keys: `path` (empty disables), `on_failure = shutdown | continue`
(default `shutdown`), `flush_interval_ms` (default 1000, 0 = only explicit
and shutdown flushes).

* Records are the stamped applied commands in apply order (bots, admin
  phases and cancel-on-disconnect included). Replaying them with
  `sim::JournalReader` into a fresh `MatchingEngine` reproduces every
  dispatched event (`server_core_journal.replay_*`).
* `JournalWriter` buffers 128 records; the 129th append writes the buffer.
  The file is readable (has a footer) only after a flush; it ends at the last
  successful flush.
* On the first failed append or flush the journal latches `failed`
  (`journal_state()`, `status`), stops writing, and counts the failure. With
  `shutdown` the exchange shuts down once the current batch has been fully
  dispatched (the command whose append failed was applied and is not
  undone); with `continue` trading goes on without a journal. Buffered
  records not yet written at the failure are lost, so `journal_records`
  counts accepted appends, not records on disk.
* The default library writer is synchronous: `flush_journal()` writes buffered
  records and the footer; its destructor flushes but cannot report errors.
  The real-time runtime supplies `runtime::JournalWorker` as the core's journal
  policy. Reactor appends and flush requests enter a startup-allocated SPSC
  queue with 16,384 usable entries. The persistence thread owns subsequent
  writes, seeks and close. Queue overflow and worker I/O failure use the same
  configured failure policy; timers observe worker failure even when periodic
  flushing is disabled. The record counter means accepted commands, not durable writes.
* Runtime `flush_journal()` acknowledges request acceptance. `journal_drained()`
  acknowledges that all submitted entries reached a successful footer write.
  Repeated flush requests with no intervening command share that request.
  Shutdown waits within the existing session drain deadline. On expiry the
  journal is marked failed, and the process returns failure. Worker teardown
  waits at most another 100 ms, then detaches a stalled writer with ownership
  of its buffers and file descriptor. It never accesses the destroyed runtime.
  An incomplete journal cannot be treated as a complete capture. A detached
  kernel write may outlive the runtime until it returns or the process exits.
* Journal destinations must resolve to regular files. Nonblocking open and
  descriptor validation reject pipes/devices before truncation. Setup may do
  blocking regular-file I/O before the reactor starts. Successful writes do
  not imply `fsync` durability against power loss.

## 10. Configuration keys added

| Section | Key | Default |
| --- | --- | --- |
| `[exchange]` | `trading_day_start`, `trading_day_end` (`HH:MM[:SS[.fraction]]`, end may be `24:00`) | `00:00`, `24:00` |
| `[journal]` | `on_failure` (`shutdown`, `continue`) | `shutdown` |
| `[journal]` | `flush_interval_ms` | `1000` |
| `[bot NAME]` | `type` (`market_maker`, `noise_taker`), `user`, `instrument` (symbol), then `apply_bot_option` keys except `name`, `session_id`, `owner` | required |

Validation: start before end; at most 32 bots; unique names; the user exists
and drives at most one bot; the symbol exists; a maker's quote size rounds to
at least one lot within `max_order_qty` and `order_capacity >= 2 * levels`; a
taker's size range contains a whole lot. Errors carry the line number.

## 11. Memory

Sized once at construction, never grown:

| Part | Size |
| --- | --- |
| EventBuffer | `(2 * max_orders + 2 * max_instruments + 4) * sizeof(Event)`; `sizeof(Event)` is 104 B, so the defaults (1,048,576 orders, 64 instruments) reserve 2,097,284 events, about 208 MiB of virtual memory. Pages are touched only as far as the largest batch reaches; ordinary batches touch a few KiB. |
| Dispatch set | 8 KiB seen bits inline + 128 KiB session list |
| Session index | 128 KiB (`uint16` per possible `SessionId`) |
| Deferred queue | `(2 * users + 64) * 56 B` (`users` slots reserved for unbind MassCancels) |
| Bots | `BotSet<32>` 23 KiB inline, plus each bot's own-order table (`order_capacity` entries) |
| Journal writer | 35 KiB (128-record buffer) |
| Core object | 68 KiB inline (mostly bots and journal); the engine sizes itself from `[engine]` |

## 12. Tests

`tests/exsim/server/core/`, suites `server_core_*`: dispatch order (two-leg
fills, sweeps, STP, auctions, mass cancel, end-of-day expiry, capacity
failure), time (repeating and backward clocks, trading-day window, end-of-day
shutdown), sessions (authenticate, bind, unbind with and without
cancel-on-disconnect, deferred commands, deferred overflow), admin (every
command and error, each MarketData shape), faults, bots (deterministic
bot-to-bot and bot-to-client trading), journal (replay equality, periodic
flush, both failure policies via `RLIMIT_FSIZE`) and allocation (zero heap
allocations in steady state with bots, journal, churn and fault queries, via
`testfw::AllocationScope`). `server_core_review` covers the QA review fixes:
unbind MassCancel under a full deferred queue, bind refused during dispatch,
stale `unbind` from an older connection, and publisher failures in counters
and `status` (with the real `md::MarketDataPublisher`).

## 13. Decisions (formerly open questions)

Resolved with the server runtime (docs/SERVER_RUNTIME.md):

* **Commands after shutdown.** `submit` refuses every command once
  shutdown has run and returns `SubmitResult::ExchangeShutDown`
  (`CoreCounters::commands_after_shutdown`, shown by `status`). Nothing is
  journaled or sent to the engine. The OUCH gateway answers an Enter Order
  with `J` reason `C` (exchange closed), the FIXP/SBE gateway rejects with
  `MarketClosed`, and FIX rejects with the text `exchange shut down`. Test:
  `submit_after_admin_shutdown_is_refused_and_not_journaled`.
* **Cancel-on-disconnect events of an unbound session.** They are
  dispatched to the session's gateway like any other event. OUCH and FIX
  store them in the session's outbound store while no connection is bound
  and replay them when the user logs in again (OUCH: Login Request
  sequence number; FIX: ResendRequest). Tests:
  `server_e2e_faults.admin_disconnect_closes_the_session_and_cancels_on_disconnect`,
  `server_e2e_sessions.cancel_on_disconnect_follows_the_user_setting`.
* **`fault md off`.** `PacketFaultInjector::clear_faults()` zeroes the
  random rates and one-shot counters (seed and delay settings are kept);
  the MD runtime's `reset_packet_faults()` calls it and then flushes
  packets already held for reordering, so nothing is left behind. The
  runtime also does this at end of day so End of Session always reaches
  receivers. Tests: `clear_faults_stops_new_faults_and_flush_releases_held`,
  `server_e2e_md.feed_rebuilds_the_engine_book_through_packet_faults_and_retransmission`.
* **Trading days crossing midnight.** Rejected when the configuration is
  parsed: `exchange.trading_day_start` must be before
  `exchange.trading_day_end`, and the error says that days crossing midnight
  are not supported. Times stay local time since midnight.
