# Runtime and protocol refactor, 2026-11-08

Workstream 2 of [EXCHANGE_REFACTOR_PLAN_2026-11-08.md](EXCHANGE_REFACTOR_PLAN_2026-11-08.md): OUCH, FIX, FIXP/SBE gateways, ITCH/MoldUDP64 publication and recovery, networking, core dispatch, journal worker and reactor. Opus 5.5 (high), no subagents. Checkout `/Users/jedi/.t3/worktrees/lob/t3-6cc707be`, branch `t3/refactor-runtime-protocol`, base `c49bf79`. Not pushed or deployed.

Result: four confirmed defects fixed. Each has a reproducer that failed before its fix and passes after. The remaining components were audited and left unchanged, with the evidence below. No wire format changed and no new abstraction was introduced. Parent updated the shared normative SERVER.md 2.4 contract after coordinating the final behavior.

## Fixes

### 1. OUCH replay larger than the connection send buffer could never complete

**Defect.**
- `TcpSendSink::prepare` used `TcpServer::reserve_send`. A reservation that does not fit sets the sticky `SendBuffer` overflow flag, and the next flush closes the connection with `SendBufferOverflow`.
- The OUCH gateway treats a `nullptr` from `prepare` during replay as "pause and resume". The sticky flag closed the connection before anything was flushed, and every reconnect repeated the failure.
- The defaults make this reachable by any client: the send buffer is 4 MB, while the store holds up to 64 MB or 1M messages per session.

**Fix.**
- New `TcpServer::reserve_send_if_room` (include/exsim/net/tcp_server.hpp). It refuses a reservation that does not fit and leaves the buffer and the connection untouched.
- `TcpSendSink::prepare` now uses it.
- Every live gateway write still closes on refusal: OUCH live output (`SendBufferFull`), the FIX live transmit (`output_overflowed`), FIX `refuse_logon` (disconnects regardless), and FIXP `write_frame`/`retransmit` (`SendBufferFull`).
- `reserve_send` and `send` keep their sticky contract for the remaining callers.

**Reproducer.** `server_e2e_ouch_replay.replay_larger_than_the_connection_send_buffer_completes`, in tests/exsim/server/e2e/recovery_beyond_send_buffer_e2e_tests.cpp:
- Setup: a 4096-byte connection send buffer, 300 orders by KEEP, then a reconnect from sequence 1.
- Expected: all 301 sequenced messages, in order, on an open connection; a live order follows at 302.
- Before the fix: 0 logins accepted, 0 sequences, disconnected (`/tmp/exsim-runtime-refactor/repro-before-fix.log`). After: passes.

### 2. FIX ResendRequest larger than the connection send buffer could never complete

**Defect.**
- `FixSessionDayState::resend` wrote the whole range in one pass. A range beyond the send buffer (about 16k reports at the 4 MB default) set `output_overflowed_`, and the session closed without flushing.
- The client asked again on reconnect and failed the same way.

**Reproducer.** `server_e2e_fix_resend.resend_larger_than_the_connection_send_buffer_completes_in_order`:
- Setup: 4096-byte send buffer. CAROL enters 300 orders and disconnects, so 300 cancel-on-disconnect reports are also stored. The client then forgets what it received (expected inbound 1) and reconnects.
- The Logon response reveals the gap, and the client sends ResendRequest(1, 0). Once logged on, the client sends one live order.
- Before the fix: the connection was closed and 0 of 601 reports arrived (`/tmp/exsim-runtime-refactor/fix-resend-before-fix.log`).
- After the fix: 601 reports, each order's accept and cancel exactly once, MsgSeqNum strictly increasing, the live order last, connection open (`/tmp/exsim-runtime-refactor/fix-resend-after-fix.log`).

**Fix.** A bounded, resumable resend inside `FixSessionDayState` (include/exsim/gateway/fix/fix_session_day_state.hpp), the same model as the OUCH replay:
- **State.** Three sequence numbers: next to resend, last of the range, and the first one withheld. Parent review added startup-allocated storage for up to 16 unsent session payloads, with deterministic fail-closed overflow and no runtime allocation.
- **Pause and resume.** `continue_resend` writes frames while the sink has room, using a new non-overflowing `write_if_room`. When the sink is full it stops. `FixSessionLayer::run_timers` resumes it on every timer pass while the day state is bound to this connection.
- **Withheld live output.** While a resend is in progress, newly sequenced messages are stored and given MsgSeqNums but not transmitted. After the requested range, the resend extends over them: stored application messages as PossDupFlag=Y with OrigSendingTime, and unsent session messages with their original payloads. Already-transmitted historical session messages are gap-filled. The peer therefore sees every MsgSeqNum once and in order on one connection.
- **Timer heartbeats.** These are skipped while a resend is in progress. Otherwise a stalled socket would consume a MsgSeqNum on every loop pass.
- **Overlapping requests.** A ResendRequest arriving during a resend restarts at its BeginSeqNo; withheld messages still follow. The peer ignores PossDup duplicates below its expected number.
- **Connection changes.** `bind_sink`, `unbind_sink` and `reset_sequence_numbers` end any resend in progress.
- **Fault injection.** `drop_out` applies to the next live transmissions after the resend.
- **Encoding failure.** A frame that cannot be encoded still fails closed. This now includes GapFill, which can never be too large; previously a failed GapFill encode was skipped silently.
- **Visibility.** `transmit` became private (`transmit_live`): there were no external callers.

**Deterministic unit test.** `gateway_fix_resend.resend_beyond_sink_capacity_pauses_and_keeps_live_output_in_order` uses a 2048-byte sink and 40 orders. During the pause it injects B0's fill (ExecutionReport 42) and a TestRequest answer (Heartbeat 43). It asserts:
- MsgSeqNum 1..44 arrive exactly once and in order.
- The fill arrives as a PossDup resend at 42, and Heartbeat 43 retains TestReqID PING.
- The following order (44) is live again, with no disconnect.

**Parent review correction.** The draft gap-filled session messages created during the paused replay. That lost TestReqID and Logout payloads that had never reached the peer. Parent rejected this behavior and added bounded pending-session storage, cleared at binding/reset/replay completion. A new capacity regression checks that a seventeenth pending session message fails closed without growth. A peer that never drains remains subject to the ordinary session timeouts; buffered messages may be lost on a forced disconnect, as on other transport failures.

### 3. Admin endpoint exceptions terminated the server

**Defect.**
- `AdminEndpoint::on_connection_data` may throw: it appends to a `std::string`, and `ExchangeCore::execute_line` builds replies.
- It runs inside the `noexcept` `TcpServer::handle_event`, so an exception called `std::terminate` and stopped the whole exchange.
- Confirmed with a stand-in core that throws `std::bad_alloc`. Against the old header the test binary aborts with `libc++abi: terminating due to uncaught exception of type std::bad_alloc`; against the new header it passes. Log: `/tmp/exsim-runtime-refactor/admin-endpoint-before-after.log`.

**Fix.**
- `on_connection_data` is now `noexcept` and wraps the existing line handling (`execute_received_lines`).
- On an exception it clears that connection's partial input, replies `ERR internal error` and closes that connection after flushing. The reactor and other operator connections continue.
- A command that threw part-way may already have changed core state; the operator gets ERR and checks `status`. This is the operator path only.

**Test.** `server_admin_endpoint.command_exception_closes_only_that_connection` (tests/exsim/server/admin_endpoint_tests.cpp) checks that a healthy connection keeps working before and after another connection's command throws.

### 4. Graceful close dropped queued output when the peer had sent unread bytes

**Defect.**
- `close_after_flush` flushed the queued output, then called `close()`.
- With unread peer input pending, `close()` sends RST instead of FIN. The kernel then discards the queued output, and the peer reads `ECONNRESET` instead of its final messages (reject, logout).

**Fix.** `TcpServer::finish_graceful_close` (src/exsim/net/tcp_server.cpp) discards input that has already arrived before closing. The discard is bounded at `kMaxDiscardReadsBeforeClose = 16` reads, so a flooding peer cannot hold the reactor.

**Contract.** The `close_after_flush` comment now says this is best effort, not guaranteed FIN. Input that arrives after the last discard read, from a flooding or concurrently sending peer, still turns the close into RST and can drop unread output. A guaranteed close would need `shutdown(SHUT_WR)` plus a lingering drain state with a deadline, which is not implemented.

**Reproducer.** `net_server.graceful_close_with_unread_input_delivers_queued_output`:
- Setup: 1 MB queued behind a 4096-byte socket send buffer. The peer sends "heartbeat", which the server never reads.
- Expected: the peer reads the full payload to EOF with no error.
- Before the fix: failed on Linux g++ and clang++ (17/18; `/tmp/exsim-runtime-refactor/linux-net-before-graceful-fix.log`) and on macOS.
- After the fix: passes (Linux evidence below).

`net_server.reserve_if_room_refuses_without_overflowing` pins the non-sticky reservation contract.

## Spec wording (applied by parent with session-payload correction)

docs/SERVER.md 2.4 currently says that a failed `prepare` means the connection is overloaded and is disconnected. That still holds for live output. Recovery output now pauses instead. The proposed replacement for its second paragraph:

> The server adapts the net layer's send buffer to it; a failed `prepare` leaves the connection and the queued bytes intact. Live output that does not fit means the connection is overloaded: the gateway closes it (the messages are still in the store, so the client recovers on reconnect). Recovery output (an OUCH replay, a FIX resend) pauses instead and resumes from the gateway timer as the socket drains; FIX withholds newly sequenced messages until the resend has sent them in order. Gateways never allocate to buffer output.

## Remaining findings (not fixed)

1. **A closing slot whose peer never reads can be held indefinitely. Low.**
   - There is no closing deadline. The exposure is bounded by `max_connections` per gateway, and the runtime destructor closes such slots.
   - A deadline belongs with the `shutdown`/linger change described in fix 4.
2. **Graceful close is best effort (fix 4).** A concurrent or flooding peer can still receive RST.
3. **The MoldUDP64 retransmission server answers any UDP requester. Design note.**
   - Each reply is up to `max_payload` bytes, with no requester allowlist.
   - The work is bounded at 64 requests per readiness (market_data_runtime.hpp:160).
   - Fine for a loopback or lab feed; review before exposing it on a shared network.
4. **Moved-from `SequencedStore` and `TokenTable` keep stale counts with null storage. Low.**
   - The defaulted moves cause this, similar to foundation finding 18.
   - No caller uses a moved-from object.
5. The draft unsent-session gap-fill defect was fixed by parent, as described above; it is no longer an open finding.

## Audited without change

Each item was read against the plan's checklist: lifecycle, bounds, partial I/O, ownership, backpressure, stale handles and allocation. The evidence for leaving it unchanged follows each item.

- **net, sockets.** `socket`, `poller`, `stream_buffers`, `tcp_connection`, `tcp_client`, `udp`, `clocks`.
  - Sockets are move-only RAII, `CLOEXEC`, non-blocking and SIGPIPE-safe.
  - A partial kqueue add rolls back.
  - Short reads and writes keep their offsets.
  - UDP truncation reports `EMSGSIZE`.
  - The cycles do not allocate (`Network.connect_echo_close_and_udp_cycles_allocate_nothing_after_setup`).
- **net, `TcpServer` (apart from fixes 1 and 4).**
  - Generation-tagged `ConnectionId` rejects stale handles.
  - The flush queue is a fixed ring.
  - Accepts are bounded per event.
  - `close_now` and `close_after_flush` refuse stale or closing ids.
- **Server runtime.** `server_runtime`, `order_entry_endpoint`, `gateway_sessions`, `server_clock`, `market_data_runtime`, `order_entry_gateways`, `admin_endpoint` (apart from fix 3).
  - Single-threaded reactor with level-triggered dispatch by poll tag.
  - Endpoint delay marks are bounded at 256, and stale `net_id` values are checked.
  - `run_timers` visits every slot (64 per gateway), and each slot is a separate heap allocation. The slots stay hot under busy polling; no change was made without a measurement.
- **ExchangeCore, DeferredCommandQueue, SessionDispatchSet.**
  - Fixed capacity and allocation-free (`server_core_allocation.steady_state_allocates_nothing`).
  - Apply and dispatch ordering preserves the journal failure policy.
- **JournalWorker.**
  - SPSC handoff. The persistence thread polls idly with a 1 ms sleep, which avoids reactor-side wake syscalls.
  - `drained()` and flush semantics were checked.
  - Teardown is bounded at 100 ms, then abort and detach. This is safe because the shared state outlives the detached thread.
  - `Journal.append_and_flush_allocate_nothing` passes.
- **md.**
  - The retransmission ring and server bounds are checked: message size at most 50 bytes, and the payload limits are validated.
  - The receiver window is bounded, and the publisher configuration is validated.
  - `md_allocation.steady_state_feed_allocates_nothing` passes.
- **gateway.**
  - `SequencedStore` and `TokenTable` are bounded.
  - Also checked: OUCH user state and connection; OUCH and FIXP byte sinks; FIX inbound sequencing, held queue, `refuse_logon` and `wants_disconnect`; FIXP `retransmit` (bounded by `maximum_retransmission_batch = 256`), `write_frame` and `flush_before_close`.
- **client.**
  - `TcpClientSendSink` now checks free capacity before reserve so the same FIX replay behavior works in both directions. Live FIX writes still fail closed at the session layer; OUCH writes report refusal without sending a partial frame.
  - `pump_tcp_client` was checked.

## Validation

All builds used the repository Makefile at `-j1`. Logs are in `/tmp/exsim-runtime-refactor/`.

| Check | Command | Result | Log |
| --- | --- | --- | --- |
| Baseline release focused | `run_focused.sh build/release/run_tests` | 588/588 | `baseline-release-focused.log` |
| Final release build | `make -j1 MODE=release build/release/run_tests` | exit 0, 0 warnings | `final-release-build.log` |
| Final release focused | `run_focused.sh build/release/run_tests` | 594/594 (588 + 6 new) | `final-release-focused.log` |
| Allocation guards | `build/release/run_tests alloc` | 21/21 | `final-release-alloc.log` |
| Allocation guards | `build/release/run_tests allocate` | 13/13 | `final-release-allocate.log` |
| New tests repeated | each of the 5 new socket/gateway tests, 10 runs | 50/50 | `final-repeat-new-tests.log` |
| Delegate final ASan/UBSan build | `make -j1 MODE=debug build/debug/run_tests` | compilation completed; no delegate test handoff | `final-debug-build.log` |
| Delegate final ASan/UBSan focused | not completed by delegate | parent integrated results supersede | final integration receipt |
| Delegate Linux net tests | earlier focused GCC/Clang probes | see recorded before/after evidence, final placeholder not claimed | `linux-net-after-graceful-fix.log` |
| Admin before/after | scratch build of the admin test against the old and the new header | old aborts, new 1/1 | `admin-endpoint-before-after.log` |

**Notes on the counts and runs.**
- `run_focused.sh` (scratchpad) runs, one at a time, every listed test that matches `^(net_|server_|gateway_|md_|proto_|sbe_codec|fixp_sbe|alloc_guard|network)`, and parses each run's `N/M tests passed` line.
- Under sanitizers, 2 FIXP allocation tests are compiled out (`#if !defined(__SANITIZE_ADDRESS__)`). The debug focused list is therefore 2 shorter than release.
- The Linux run builds `tests/main.cpp`, `tests/exsim/net/network_tests.cpp` and `src/exsim/net/*.cpp` with `-Wall -Wextra -Wpedantic -Wshadow -Werror`, using g++ 14.2 and clang++ 19.1 (aarch64).

**Allocation on the changed paths.**
- `server_e2e_allocation.steady_state_order_entry_market_data_and_bots_do_not_allocate` drives the real `TcpSendSink` through OUCH, FIX and FIXP.
- `gateway_ouch_allocation.steady_state_conversation_and_replay_do_not_allocate` covers OUCH replay.
- `gateway_fix_allocation.steady_state_trading_and_recovery_do_not_allocate` covers the FIX resend path.
- The paused FIX resend uses the existing `resend_buffer_` and startup-allocated pending-session storage. The discard loop reuses the receive buffer.

## Coverage gaps

- **Not run.** The whole-repository suite, TSan and benchmarks. The focused set does include every `server_e2e` test. No latency claim is made.
- **Leak checking.** macOS ASan has no LeakSanitizer, so the sanitizer runs cover address and UB errors, not leaks. On Linux, only the net tests ran, without sanitizers.
- **The FIX paused-resend timing is not proven in the e2e test.** The e2e test does not prove that its live order arrived while the resend was paused. The unit test covers that interleaving deterministically.
- **Not tested.** No third-party FIX engine (for example QuickFIX) was run against the paused resend. The behaviour follows FIX 4.4 PossDup and GapFill rules.
- **Audited for structure and bounds only, not line by line.**
  - Protocol codecs: `protocol/*` and the SBE codec. They are covered by their own focused tests, which pass.
  - `book_builder` and `itch_dispatch`.
  - `config.cpp` and `admin.cpp`.
  - The `exsim_server`, `exsim_client` and `exsim_md_client` tools.
- **Scope.** No claim of whole-system clearance. Core/LOB, simulation/research and HTTP belong to other workstreams.

## Parent takeover

Opus rate-limited before committing; parent preserved the worktree and completed review and integration. Draft placeholder results above are not passing evidence. The final integrated receipt supersedes unfinished tables: `EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md`. Parent inspected every changed protocol/runtime path and corrected the unsent-session payload regression described above. Graceful close is best effort under newly arriving/flooding input; bounded discard does not promise FIN for arbitrary concurrent peer traffic.
