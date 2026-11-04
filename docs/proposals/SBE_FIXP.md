# Proposal: FIXP/SBE gateway integration

Branch `t3/add-fixp-sbe-gateway` adds a third order entry protocol (SBE 1.0
application messages over a FIXP 1.0 session, SOFH framed). The wire profile
is in `docs/protocols/FIXP_SBE.md` and the schema in
`docs/protocols/exsim_order_entry_sbe.xml`. This file lists the shared
changes the branch needs but does not make. The orchestrator decides what to
merge into `docs/GATEWAYS.md`, `docs/SERVER.md` and shared headers.

## What already works without shared changes

`fixp::FixpSbeUserStateDirectory<ByteSink>` is a valid `server::ExchangeCore`
`Sessions` policy, and `server::ExchangeCore` satisfies
`fixp::FixpSbeServerContext`. `tests/exsim/gateway/fixp_sbe/exchange_core.cpp`
runs the gateway on the real core with `DiscardingMarketData` and a settable
clock, and covers:

- two users trading
- admin `reject` and `drop_out` faults
- cancel on disconnect followed by recovery through retransmission
- outside trading day answered with MarketClosed
- zero heap allocations on the steady state order path (new, partial fill,
  cancel, timer), measured with `testfw::AllocationScope`

The only shared value the branch invents is the placeholder
`fixp::kProposedFixpSbeProtocol = static_cast<server::Protocol>(2)`.

Status (orchestrator, 2026-11-05): section 1 is applied except the optional
`exchange.fixp_sbe_*` timeout keys (the protocol value, its parser aliases
`fixp_sbe` and `sbe`, and `exchange.fixp_sbe_port`); section 2 is merged into
`docs/GATEWAYS.md` section 5. Section 3 goes to the server binary. Answers to
section 4: store exhaustion stays a terminate (documented); 64 session UUIDs
per user per day is enough for the planned scenarios; FIXP finalization is not
needed for end of day, where Terminate is sent.

## 1. config.hpp and the config parser

```cpp
enum class Protocol : std::uint8_t { Ouch42 = 0, Fix44 = 1, FixpSbe = 2 };

constexpr std::string_view to_string(Protocol protocol) noexcept {
    switch (protocol) {
        case Protocol::Ouch42: return "ouch42";
        case Protocol::Fix44: return "fix44";
        case Protocol::FixpSbe: return "fixp_sbe";
    }
    return "unknown";
}
```

- Parse `protocol = fixp_sbe` in `[[user]]` sections.
- Add `std::uint16_t fixp_sbe_port = 0;` to `ExchangeConfig`, with the same
  semantics as `ouch_port` and `fix_port` (0 means OS assigned, logged at
  startup).
- Optional keys, defaulting to the values in `FixpSbeSessionSettings`:
  `exchange.fixp_sbe_establishment_timeout_ms`,
  `exchange.fixp_sbe_min_keepalive_ms`, `exchange.fixp_sbe_max_keepalive_ms`
  and `exchange.fixp_sbe_retransmission_batch`. Without them
  `FixpSbeSessionSettings::from(config.exchange)` stays as it is.
- Existing keys are reused: `exchange.heartbeat_interval_ms` becomes the
  server keepalive interval in `FixpSbeSessionSettings::from(config.exchange)`,
  and `engine.max_tokens_per_session`, `engine.store_messages_per_session` and
  `engine.store_bytes_per_session` size each user in
  `FixpSbeUserStateSettings::from(config.engine)`.

Once `Protocol::FixpSbe` exists, delete `kProposedFixpSbeProtocol` and use
the enumerator. Until then `to_string` returns "fix44" for value 2, so logs
for FIXP users are mislabelled.

## 2. GATEWAYS.md section

> **fixp_sbe**: SBE 1.0 over FIXP 1.0, TCP, client Idempotent and server
> Recoverable (FIXP Appendix A 7.1.3), exsim schema 22597 version 1. The
> business mapping follows the FIX 4.4 tables in this file, with numeric
> uint64 ClOrdIds in place of FIX text tokens. Wire details, deviations and
> the session state machine are in `protocols/FIXP_SBE.md`.
>
> One `FixpSbeUserState` per configured user lives for the whole trading day
> inside a `FixpSbeUserStateDirectory`. It holds the token table, per order
> metadata and the sequenced store, so a reconnect resumes the server flow
> at the next sequence number. A `FixpSbeConnection` lives for one transport.
> Engine events reach the user state through `on_events` whether or not a
> transport is bound.
>
> SubmitResult mapping: Applied and Deferred send nothing (reports follow).
> OutsideTradingDay is answered with a reject carrying MarketClosed.
> OutputCapacityExceeded and DeferredQueueFull are answered with
> CapacityExceeded. A void submit that produced no message for the request is
> answered with RequestHadNoEffect.
>
> Faults: `reject` applies to new orders only. With `drop_out` the report is
> stored and sequenced but not sent; the next live message is preceded by a
> Sequence, so the client sees the gap and can request retransmission.

## 3. SERVER.md and the server reactor

### Sessions policy

`ExchangeCore` takes one `Sessions` type. With OUCH, FIX and FIXP side by
side, the server needs a small router:

```cpp
struct OrderEntrySessionRouter {
    OuchUserStateDirectory* ouch_user_states;
    FixUserStateDirectory* fix_user_states;
    fixp::FixpSbeUserStateDirectory<SendBufferByteSink>* fixp_sbe_user_states;

    template <class Core>
    void on_events(SessionId session_id, std::span<const Event> batch, Core& core) {
        // Exactly one directory owns each session id (directories filter by
        // protocol), so forward to the one whose find(session_id) is non-null.
    }
};
```

A session id lookup table built at startup avoids three lookups per batch.

### Connection lifecycle, per accepted TCP connection on `fixp_sbe_port`

1. Construct `FixpSbeConnection<SendBufferByteSink>(sink, fixp_sbe_user_states,
   connection_id, FixpSbeSessionSettings::from(config.exchange), core.now())`.
   `SendBufferByteSink` wraps the existing prepare/commit send buffer and
   satisfies `fixp::FixpSbeByteSink`.
2. On readable bytes call `on_data(bytes, core)`. It returns how many bytes it
   consumed. Keep the rest at the front of the receive buffer.
3. Call `on_timer(core)` from the reactor timer tick, at least as often as the
   smallest accepted keepalive interval.
4. When `close_requested()` is true: flush first if `flush_before_close()`,
   close the socket, then call `on_disconnect(core)`. That unbinds the session
   and the core applies cancel on disconnect.
5. On peer close or socket error, call `on_disconnect(core)` directly.

### Shutdown

FIXP has no end of day application message. At shutdown the server calls
`end_session(core)` on every live connection, which sends Terminate. The
connection does not wait for the peer's Terminate answer. An optional
`on_shutdown` on the router may forward to the directory if a later version
needs per user end of day work.

## 4. Open questions for the orchestrator

- Should store exhaustion stay a terminate (the current behaviour, counted
  and documented) or become a hard reject of all new orders earlier in the
  day?
- Each user may negotiate at most 64 session UUIDs per trading day (fixed
  table, no allocation). Is that enough for the scenarios planned?
- FIXP finalization (FinishedSending and FinishedReceiving, FIXP 4.7) is not
  implemented. Is it needed for end of day?
