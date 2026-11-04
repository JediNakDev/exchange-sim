# exsim binary order entry

This is an **exsim-owned application schema**, not a venue protocol. Schema
22597 (`0x5845`), version 1, uses SBE 1.0 little-endian with SOFH framing.
The supported FIXP 1.0 profile is point-to-point TCP, client Idempotent and
server Recoverable (FIXP Appendix A section 7.1.3). It does not implement the
entire FIXP standard. No proprietary venue formats are used.

## Standards and wire representation

The authoritative FIX Trading Community sources consulted are:

- [FIXP 1.0 standard](https://github.com/FIXTradingCommunity/fixp-specification/tree/master/v1-0-STANDARD),
  sections 3.2 (types), 3.4 (sessions/flows), 3.5 (implicit sequencing),
  4.2 (negotiation), 4.3 (establishment), 4.4 (transport termination),
  4.5 (heartbeat), 4.6 (retransmission), 4.8 (idempotent operations),
  6.2 (NotApplied as an application response), and 7.1.3 (chosen flows).
- [SBE 1.0, November 2020 errata](https://github.com/FIXTradingCommunity/fix-simple-binary-encoding/tree/master/v1-0-STANDARD),
  sections 4.4.3 (byte order), 4.5 (decimal), 4.7.2 (fixed strings),
  4.8.2 (variable data), 4.10 (epoch/time units), 5.1.1 (framing),
  5.2.1 (header), 5.3.3.1 (no implicit padding), 7.3.1 and 7.4 (extension).
- [SOFH 1.0](https://github.com/FIXTradingCommunity/fix-simple-open-framing-header/blob/master/v1-0-DRAFT/sofh.md),
  sections “Encoding of the Simple Open Framing Header”, “Message_Length”
  and “Encoding_Type”.

The XML validates against the `sbe.xsd` published with SBE 1.0.

Each frame is SOFH (6 bytes) + SBE header (12 bytes) + root block + variable
data. SOFH contains a big-endian uint32 total length, including itself, and
big-endian uint16 encoding `0xEB50`. Every other integer is little-endian.
The SBE header is six uint16 fields: blockLength, templateId, schemaId,
version, numGroups, numVarDataFields. There are no repeating groups.
Session credentials/reasons use a uint16 length followed by at most 128 bytes.
Application messages have no variable data. Inbound frames are capped at
1024 bytes; outbound frames need at most 184 bytes.

Prices are signed int64 mantissas with constant exponent -4 (the exponent
occupies no bytes). The minimum int64 means null price for market orders.
Timestamps are uint64 nanoseconds since the Unix epoch. Context `now()` is
used for timers and engine commands; `realtime_ns()` is used for wire time.
Symbols are eight NUL-padded ASCII bytes. Client request IDs are nonzero
uint64 values interned into the unchanged day-long TokenTable. Used request
IDs remain used after rejection or order completion.

The codec preserves raw enum values; application validation happens in the
gateway. Schema ID must match and version must be at least 1. Known messages
must have a sufficiently long root block. A larger blockLength is skipped,
including before variable data. Future versions may append root fields and
variable fields. Repeating-group extensions are outside this profile.
Unknown application templates consume a sequence and receive
BusinessMessageReject; unknown session templates terminate the transport.

## Session and recovery

```text
TCP connected -> AwaitingNegotiate
  Negotiate(UUIDv4, timestamp, Idempotent, credentials)
    -> NegotiationResponse(Recoverable) -> Negotiated
  Establish(UUID, timestamp, keepalive, null NextSeqNo)
    -> EstablishmentAck(next server sequence) -> Established
  reconnect: Establish with credentials for the existing UUID skips Negotiate
Established
  Sequence(N) -> subsequent application messages implicitly numbered N,N+1,...
  outbound silence -> Sequence(next server sequence)
  incoming client gap -> sequenced NotApplied(from,count)
  client duplicate below high-water -> silently discarded
  RetransmitRequest -> Retransmission + stored frames + Sequence(live resumes)
  Terminate -> Terminate response -> Closed
  timeout/malformed frame/output failure -> Closed
Closed -> caller closes TCP and calls on_disconnect(context)
```

A new Negotiate supersedes the previous logical session and restarts both
flow sequence spaces. Order tokens and engine orders remain day-scoped.
Only the current logical session can be recovered. At most 64 distinct
logical UUIDs can be negotiated per user per day; further negotiations are
rejected rather than forgetting old UUIDs and permitting their reuse.
A transport reconnect must use Establish, not Negotiate, to recover reports.

All outbound application responses, including NotApplied, are stored before
transmission. Completed orders need no live mapping for replay. Disconnected
users continue to receive stored engine event reports. Drop-out fault hooks
skip live output only, and force a Sequence before the next live report.
Retransmission sends at most the configured batch count and then explicitly
restates the real-time sequence. The client requests further batches as
needed and delivers reports in sequence, discarding duplicates. It drops
live messages beyond a gap and recovers them from the store, avoiding a
per-message allocation or an unbounded reorder buffer.

The idempotent client flow uses a high-water mark. Numbers below that mark
are never reapplied, including gaps explicitly declared by a later Sequence.
A NotApplied retry must use a fresh client sequence and the original unused
ClOrdId. The client exposes typed callbacks and `resend` but leaves application
acknowledgment timers, durable pending-request storage and retry policy to the
caller. It does not promise automatic order resubmission.

A connection has a 5 second establishment deadline by default. Keepalive is
1000 ms by default; accepted client intervals are 100..60000 ms. Incoming
silence exceeding twice the peer interval terminates the transport. Caller
supplied monotonic/day time drives server timers; the client uses supplied
Unix nanoseconds. Clocks must not move backward during a transport lifetime.
No socket, clock read, allocation, virtual dispatch or exception occurs on
the declared gateway/client/codec message paths after setup.

## Application mappings

| Wire message/field | Engine command or event mapping |
| --- | --- |
| NewOrderSingle | NewOrder with configured session/owner, interned ClOrdId, symbol lookup |
| Price, OrderQty, Side, OrdType, TimeInForce | Integer price, quantity, side, type, Day/GTC/IOC/FOK |
| ExecInst bit 0 | post_only; other bits rejected |
| SelfTradePrevention | user default or None/CancelAggressor/CancelResting/CancelBoth |
| OrderCancelRequest OrigClOrdId | CancelOrder.orig_cl_ord_id; request ID is separately interned |
| ReduceToTotalQty | CancelOrder.reduce_to_total_qty; zero means full cancel |
| OrderCancelReplaceRequest OrderQty | ReplaceOrder.qty, TOTAL quantity including cumulative fills |
| ForcePriorityLoss | ReplaceOrder.force_priority_loss |
| OrderMassCancelRequest | MassCancel for session, optionally symbol; own request ID retained |
| Accepted | ExecutionReportNew with initial leaves, zero cumulative fills |
| Fill | ExecutionReportTrade per own leg, match ID, last price/qty, cum/leaves and maker/taker/auction liquidity |
| Canceled | ExecutionReportCancel with canceled qty, remaining leaves, cumulative fills and engine reason |
| Replaced | ExecutionReportReplace with old/new order ID, request/original token, total/cum/leaves and priority-retained flag |
| Rejected new | ExecutionReportReject with request token and normalized plus exact exsim reason |
| Rejected cancel/replace | OrderCancelReject with request token, original token, response-to and reason |
| MassCancelDone | OrderMassCancelReport, affected count and request correlation |
| Unsupported application template | BusinessMessageReject with implicit sequence and template ID |

Partial reductions keep the original order token and set OrdStatus to New or
PartiallyFilled when leaves remain. A reduction that makes no change gets an
explicit RequestHadNoEffect reject. Replace acknowledgment precedes its fills.
Fill legs are emitted buy then sell if both concern this user. Engine event
handling never submits commands or binds/unbinds a session.

Differences from the FIX 4.4 gateway: numeric uint64 request tokens, fixed binary
fields, explicit exsim priority flag and cancel-reduction quantity, no FIX text
parser or FIX session resend/gap-fill messages. Business semantics follow the
FIX mappings in GATEWAYS.md. FIXP sequencing replaces FIX MsgSeqNum handling.

## Profile restrictions and deviations

- Session template IDs and field layouts follow the open FIXP reference SBE
  schema, but are declared under exsim schema ID 22597 with a 12-byte header.
  A peer using the standalone FIXP schema/header must adapt to this profile.
- Only Idempotent client and Recoverable server flows over one TCP connection
  are supported. No UDP, multicast, multiplexed Context, Topic, dynamic schema
  delivery, Applied, or other flow combinations are implemented.
- FinishedSending/FinishedReceiving finalization (FIXP 4.7) is not implemented.
  Terminate unbinds the transport; the logical session remains recoverable.
  End-of-day shutdown uses Terminate and does not claim the finalization handshake.
- Credentials are exsim ASCII `username:password`, split at the first colon.
  This is a local simulator profile, not an authentication or encryption standard.
- Timestamp plausibility requires Unix nanoseconds at or after 2000-01-01.
  Request timestamp echoes and supported server flow are checked by the client.
- Store and token capacity are bounded at construction. New/replace traffic
  is rejected near store exhaustion. Actual store exhaustion increments a loss
  counter and closes the connection; a report that could not be stored cannot
  be recovered. Size the store for the trading day, including cancellations.
- Client recovery is in-memory within the client object's lifetime. Crash
  persistence and application retry scheduling are caller responsibilities.

## Integration with the server

`FixpSbeUserStateDirectory<ByteSink>` holds one `FixpSbeUserState` per
configured FIXP/SBE user for the whole trading day and is a
`server::ExchangeCore` `Sessions` policy: the core calls
`on_events(session, batch, core)` for every session in an event batch, bound
or not. `server::ExchangeCore` satisfies `FixpSbeServerContext`, so a
`FixpSbeConnection` built with the directory runs directly on the core. The
driving contract (on_data, on_timer, close_requested, on_disconnect,
end_session) is in the header comment of
`include/exsim/gateway/fixp_sbe/fixp_sbe_server.hpp`.

Submit outcomes map as follows:

| SubmitResult | Gateway answer |
|---|---|
| Applied, Deferred | nothing extra; reports follow from engine events |
| OutsideTradingDay | reject with MarketClosed |
| OutputCapacityExceeded, DeferredQueueFull | reject with CapacityExceeded |
| void submit, no message answered the request | reject with RequestHadNoEffect |

`tests/exsim/gateway/fixp_sbe/exchange_core.cpp` runs the gateway on the real
core, including admin faults, cancel on disconnect and a zero allocation
check of the steady state order path.

The shared changes still needed (a `Protocol::FixpSbe` enumerator, config
parsing, a listener port and a Sessions router next to OUCH and FIX) are
proposed in `docs/proposals/SBE_FIXP.md`. Until they land the gateway uses the
placeholder `kProposedFixpSbeProtocol` (value 2).
