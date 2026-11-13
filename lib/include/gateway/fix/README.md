# FIX 4.4 gateway integration

Include `gateway/fix/fix_gateway.hpp` for the exchange side and
`gateway/fix/fix_client.hpp` for the client side. All types are in
`exsim::gateway::fix`; implementations are header-only C++23 templates.

- Output goes to a `gateway::ByteSink` (`gateway/byte_sink.hpp`):
  `char* prepare(size_t)` and `void commit(size_t)`, both noexcept.
- `FixTime{monotonic_ns, realtime_ns}`: injected timer and Unix wire clocks.
- `FixSessionSettings`: framing/held-message bounds, heartbeat maximum and timeouts.
- `FixUserLimits`: day token count and resend-store message/byte capacities.
- `FixUserDirectory<Sink>(users, exchange_comp_id, limits)`: construct once per day.
- `directory.find(SessionId)` returns a user state pointer (login path, linear);
  `directory.users()` lists every user state.
- `FixUserState<Sink>` owns day-used tokens, order records, ExecIDs and sequences.
- `user.on_events(events, ctx)` encodes, in order, the events of the range that
  concern that user; the range may be the user's events or a whole batch.
- With `ExchangeCore`, the server's session table (GatewaySessions) calls
  `user.on_events` once per batch with only that user's events.
- `FixConnection<Context, Sink>(directory, settings)` owns one transport session.
- `start(sink, server::ConnectionId, ctx)` begins a freshly accepted connection.
- `on_data(span<const char>, ctx)` returns consumed bytes; retain incomplete tails.
- `on_timer(ctx)` drives injected-clock session timers.
- `wants_disconnect()` tells the server to flush/close, then call `on_disconnected(ctx)`.
- Always call `on_disconnected` before destroying a bound connection; the
  destructor asserts it. `start` on a still-bound connection releases the
  binding first (`on_disconnected`). A stale connection never unbinds the sink
  of a newer connection for the same user.
- `begin_logout(text, ctx)` starts the exchange-initiated Logout handshake.
- Context supplies authenticate, bind/unbind, instrument lookups, submit, now/realtime_ns.
- Connection IDs use `server::ConnectionId`, with packing from net IDs owned by the server.
- `submit` may return void (standalone contexts) or `server::SubmitResult` (ExchangeCore).
- Applied/Deferred mean success; other results receive protocol application rejections.
- Optional Context `consume_injected_reject(session)` and `consume_drop_out(session)`
  connect ExchangeCore admin faults directly to gateway application reports.
- Local `inject_new_order_rejects`/`drop_out_transmissions` support standalone contexts.
- `FixClient<Handler, Sink>(handler, FixClientSettings)` owns client day state.
- `connect(sink, reset, FixTime)` sends Logon; `on_data`, `on_timer`, `logout`,
  `on_disconnected` supply the client transport lifecycle.
- Typed `send_new_order_single`, `send_order_cancel_request`,
  `send_order_cancel_replace_request`, `send_order_mass_cancel_request` return success.
- Handler receives on_logon, on_execution_report, on_order_cancel_reject,
  on_order_mass_cancel_report, on_session_reject and on_logout callbacks, and
  on_business_message_reject when the handler defines it.
- Callback text borrows input storage; copy it if it must survive the callback.

## Policy and limits

Logon timeout is 10 seconds, Logout timeout and resend retry are 2 seconds.
HeartBtInt 0 disables heartbeat/probe timers; maximum is 300 seconds by default.
Probe after 1.5 heartbeat intervals of silence; an unanswered probe times out
half an interval after transmission. Only its matching Heartbeat clears it.
Early ResendRequest, TestRequest and Heartbeat are serviced immediately but
consume their sequence only after predecessors. Early application messages
are held in a bounded queue, discarded on overflow and requested again until
recovery completes. A high Logout closes immediately. A high GapFill waits.

AvgPx rounds to nearest price unit (four decimal places). Replacements use total
quantity, carry fill notional/cumulative quantity, and reserve new day-used tokens.
Authenticated sequence reset clears the resend store and both sequence counters,
but preserves tokens, order records and ExecIDs. Duplicate low non-PossDup messages
log out; a low PossDup message, GapFill included, is ignored and never advances
expected sequence. A duplicate early ResendRequest already held is serviced once.
MsgSeqNum above 2^64 - 2 is refused with Logout so expected never wraps.

Live output that overflows the sink disconnects while stored reports remain
recoverable. A resend that does not fit pauses and resumes from the session
timer as the transport drains; messages sequenced meanwhile are withheld and
follow the requested range as part of the resend. Application reports use
PossDupFlag=Y; up to 16 unsent session messages retain their payloads in
startup-allocated storage (overflow fails closed), so TestReqID and other
session fields are delivered rather than gap-filled. MsgSeqNums stay in order. Resend
store exhaustion instead latches `store_exhausted()`, increments
`messages_not_stored()`, and disconnects without transmitting or gap-filling the
unstored report. Normal reconnect is refused. Only application messages take
store slots; session messages are gap-filled on resend. Provision the store for
the whole day's application messages. The server must treat exhaustion as a fatal
capacity incident: reports from that point cannot be recovered. An explicit
new authenticated reset may restart sequence storage but cannot restore those
reports. No ring eviction or token reclamation occurs during the day.

The server loop owns inbound application delay (`ctx.inbound_delay_ns`) and admin
disconnect requests. Gateways consume reject/drop faults for application messages;
heartbeats and other session messages are not suppressed. Fault delay must preserve
arrival order and allow session timer/recovery work. No socket reactor or standalone
CLI is included in this component.

The existing codec handles application decode/encode, framing and semantic field
reads. A local frame writer is restricted to PossDup rebuilds because the codec's
Builder has no raw-field splice operation; original application body bytes are
copied intact. Raw header scans are only used for malformed messages the codec
rejects, to recover reference sequence/type and the duplicate tag for Reject(3).
