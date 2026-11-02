# Gateway & Market-Data Mapping (normative)

How each external protocol maps onto the protocol-neutral engine API
(`docs/SPEC.md`). Gateways are **pure state machines over byte buffers**
(bytes in → commands + bytes out); the network layer only moves bytes. This
keeps every gateway unit-testable without sockets and lets the server stay
single-threaded and deterministic.

```
TCP bytes ─► Framer ─► Session (login, seq, heartbeats) ─► App mapping ─► Command
                                                                           │
Event stream (per command batch) ◄─────────────────────────────────────────┘
   ├─► OrderEntryGateway::on_events  → per-session outbound messages
   └─► MarketDataPublisher::on_events → ITCH 5.0 messages → MoldUDP64 packets
```

## 1. Sessions and users

Users are configured statically (`exsim.conf`):

| Field | Meaning |
|---|---|
| `username`, `password` | login credentials (SoupBinTCP username/password; FIX SenderCompID + optional Password(554)) |
| `protocol` | `ouch42` or `fix44` |
| `session_id` | `SessionId` used for every command from this user |
| `owner` | `OwnerId` (firm) for self-trade prevention |
| `stp` | default `StpMode` (OUCH has no STP field; FIX may override per order via tag 7928) |
| `cancel_on_disconnect` | on TCP loss or logout, send `MassCancel{session, cancel_on_disconnect=true}` |

A user may be logged in on at most one connection; a second login is
rejected (SoupBinTCP `J`/`S`; FIX Logout with Text).

Every outbound application message is **sequenced and stored** per user for
the trading day (pre-allocated append-only store) so a client can reconnect
and recover:
* SoupBinTCP: Login Request "requested sequence number" N ≥ 1 replays stored
  messages from N; 0 or blank = start after the last stored message.
* FIX: inbound ResendRequest(2) for an application range is answered by
  resending stored messages with PossDupFlag(43)=Y and the original
  MsgSeqNum; session-level messages in the range are replaced by
  SequenceReset-GapFill(4, 123=Y).

Client tokens (OUCH 14-byte token, FIX ClOrdID ≤ 20 chars) are interned per
session into `ClOrdId` values (1, 2, ...). The gateway keeps both directions
(token → ClOrdId for inbound, ClOrdId → token for outbound) in fixed-capacity
tables sized from config; when full, new orders are rejected by the gateway.

## 2. OUCH 4.2 over SoupBinTCP 3.0

### Inbound

| OUCH | Command | Mapping |
|---|---|---|
| `O` Enter Order | `NewOrder` | token → `cl_ord_id`; `B` → BUY, `S`/`T`/`E` → SELL; stock → instrument by symbol (unknown symbol → gateway `J` reason `S`, no command); price as-is (4 dp); price `0x7FFFFFFF` → `Market`; TIF 0 → IOC, any other → DAY; display `P` → `post_only`; owner/stp from user config |
| `U` Replace | `ReplaceOrder` | existing token → `orig_cl_ord_id`; replacement token → new `cl_ord_id`; OUCH shares = new **open** quantity → `qty = cum + shares` |
| `X` Cancel, shares = 0 | `CancelOrder` | full cancel |
| `X` Cancel, 0 < shares < leaves | `ReplaceOrder` | same price, `qty = cum + shares`, token unchanged (keeps priority); answered with `C` (decrement = old leaves − shares, reason `U`), not `U` |
| `X` Cancel, shares ≥ leaves | — | no effect, no response |
| `M` Modify | — | not supported: `J` reason `O` |

### Outbound (per command batch, in event order)

| Event | OUCH |
|---|---|
| `Accepted` | `A`; order reference number = `OrderId`; order state `L` if the same batch contains `Rested` for this order, else `D` |
| `Rejected` (NewOrder / Replace) | `J` with the request token; reason map: UnknownInstrument→`S`, InvalidPrice→`X`, InvalidQuantity→`Z`, MarketClosed→`C`, InstrumentHalted→`H`, NotAllowedInPhase→`R`, PostOnlyWouldCross→`W`, DuplicateClOrdId / InvalidClOrdId / InvalidField / CapacityExceeded→`O` |
| `Rejected` (Cancel/Replace UnknownOrder) | ignored (OUCH 4.2 behaviour) |
| `Fill` | `E` to the session of each leg: shares = qty, price, match number = `match_id`, liquidity flag `R` for the aggressor leg, `A` for the resting leg, `C` for both legs of an auction fill |
| `Canceled` | `C`: decrement shares = `canceled_qty`; reason UserRequested→`U`, ImmediateOrCancel/FillOrKill→`I`, SelfTradePrevention→`Q`, MassCancel→`U`, CancelOnDisconnect→`U`, EndOfDay→`E` |
| `Replaced` | `U` with replacement + previous token (except partial-cancel replaces, see above) |
| `PhaseChanged` | none (OUCH has no instrument state) |

`S` System Event `S` is sent after login and `E` at end of day.

## 3. FIX 4.4

### Session layer
* Logon(A): SenderCompID must match a configured user; TargetCompID must be
  the exchange CompID; HeartBtInt honoured; ResetSeqNumFlag=Y resets both
  sides to 1.
* Inbound MsgSeqNum: lower than expected without PossDupFlag → Logout +
  disconnect; higher → process it and send ResendRequest for the gap.
* Heartbeat(0) after HeartBtInt of outbound silence; TestRequest(1) after
  1.5 × HeartBtInt of inbound silence; disconnect after 2 × HeartBtInt.
* Malformed message (checksum, body length, missing required tag) →
  session Reject(3) with SessionRejectReason(373), message not processed.

### Application mapping

| FIX | Command | Notes |
|---|---|---|
| `D` NewOrderSingle | `NewOrder` | 11 → `cl_ord_id`; 55 → instrument; 54 1/2; 40 1=Market 2=Limit (others → ER Rejected); 59 0=Day 1=GTC 3=IOC 4=FOK; 18 contains `6` → post_only; 7928 N/R/A/B overrides STP mode |
| `F` OrderCancelRequest | `CancelOrder` | 41 → orig, 11 → request token |
| `G` OrderCancelReplaceRequest | `ReplaceOrder` | 41 → orig, 11 → new token, 38 = new **total** qty, 44 |
| `q` OrderMassCancelRequest | `MassCancel` | 530=1 with 55 → that instrument; 530=7 → all |

| Event | FIX |
|---|---|
| `Accepted` | ExecutionReport 150=0 39=0 |
| `Fill` | ER 150=F, 39=1 or 2, 32/31, 14/151, 6 AvgPx (gateway tracks notional per order) |
| `Canceled` | ER 150=4 39=4 (11 = request token when user-requested, 41 = order token) |
| `Replaced` | ER 150=5 39=0 or 1 (11 = new token, 41 = previous token) |
| `Rejected` NewOrder | ER 150=8 39=8, 103 OrdRejReason (UnknownInstrument→1, InvalidQuantity/InvalidPrice/InvalidField→99 with Text, DuplicateClOrdId→6, MarketClosed→2, CapacityExceeded→99) |
| `Rejected` Cancel/Replace | OrderCancelReject(9), 434 = 1 (cancel) / 2 (replace), 102 = 1 UnknownOrder, else 99 |
| `MassCancelDone` | OrderMassCancelReport(r), 531 = 530 on success, 533 = count |

ExecID(17) is unique per session for the day; OrderID(37) = `OrderId`.

## 4. Market data: ITCH 5.0 over MoldUDP64

Stock locate = `InstrumentId`; order reference number = `OrderId`; match
number = `MatchId`; timestamps = event `ts` (ns since midnight).

At start: System Event `O`, then Stock Directory `R` and Stock Trading
Action `H` for every instrument, then System Event `S` (start of system
hours).

| Event | ITCH |
|---|---|
| `Rested` | `A` Add Order (shares = leaves) |
| `Fill`, continuous | `E` Order Executed on the **resting** leg only (the aggressor was never displayed) |
| `Fill`, auction | `C` Order Executed With Price on **both** legs (price = uncross price, printable `N`) |
| `AuctionSummary` | `Q` Cross Trade (cross type `O` if leaving PreOpen, `C` if leaving PreClose, `H` otherwise) |
| `Canceled`, `was_resting` | `D` Order Delete |
| `Replaced`, priority kept, leaves reduced | `X` Order Cancel (cancelled shares = old − new leaves) |
| `Replaced`, priority lost, and the batch's next event for the new id is `Rested` | `U` Order Replace (old ref → new ref, shares = leaves, new price) |
| `Replaced`, priority lost, otherwise | `D` for the old ref; later `Rested` (if any) → `A` |
| `PhaseChanged` | `H`: Continuous `T`, Halted `H`, PreOpen/PreClose `Q`, Closed `H` |

During call phases the publisher emits Net Order Imbalance Indicator `I` for
each instrument at a configurable interval from `auction_indicative()`.

MoldUDP64: messages are packed into packets up to the configured payload size;
a packet is flushed at the end of every command batch (never held across
commands). Heartbeat packets (count 0) every second of silence; end of session
(count 0xFFFF) at shutdown. A retransmission server answers MoldUDP64 request
packets from a ring buffer of recent messages.

**Feed correctness test**: a client-side book builder consuming the feed (no
matching; tolerant of crossed books during auctions) must reproduce the
engine's depth for every instrument after every packet.
