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
| `O` Enter Order | `NewOrder` | token → `cl_ord_id`; `B` → BUY, `S`/`T`/`E` → SELL; stock → instrument by symbol (unknown symbol → gateway `J` reason `S`, no command); price as-is (4 dp); price `0x7FFFFFFF` → `Market`; TIF 0 → IOC, 99998/99999 → DAY; display `P` → `post_only`; owner/stp from user config |
| `U` Replace | `ReplaceOrder` | existing token → `orig_cl_ord_id`; replacement token → new `cl_ord_id`; OUCH shares = new **total** quantity → `qty = shares`, `force_priority_loss = true` |
| `X` Cancel, shares = 0 | `CancelOrder` | full cancel |
| `X` Cancel, cum < shares < cum + leaves | `CancelOrder` | `reduce_to_total_qty = shares`, token unchanged; priority preserved in every phase; C reports removed shares |
| `X` Cancel, shares ≥ cum + leaves | — | no effect, no response |
| `M` Modify | — | not supported: `J` reason `O` |

### Outbound (per command batch, in event order)

| Event | OUCH |
|---|---|
| `Accepted` | `A`; order reference number = `OrderId`; order state `L`, before any execution/cancel reports (including fully filled IOC); this simulator does not use atomic-dead acknowledgment compression |
| `Rejected` (NewOrder / Replace) | `J` with the request token; reason map: UnknownInstrument→`S`, InvalidPrice→`X`, InvalidQuantity→`Z`, MarketClosed→`C`, InstrumentHalted→`H`, NotAllowedInPhase→`R`, PostOnlyWouldCross→`O`, DuplicateClOrdId / InvalidClOrdId / InvalidField / CapacityExceeded→`O` |
| `Rejected` (Cancel/Replace UnknownOrder) | ignored (OUCH 4.2 behaviour) |
| `Fill` | `E` to the session of each leg: shares = qty, price, match number = `match_id`, liquidity flag `R` for the aggressor leg, `A` for the resting leg, `O` leaving PreOpen, `C` leaving PreClose, otherwise `H` for both auction legs |
| `Canceled` | `C`: decrement shares = `canceled_qty`; reason UserRequested→`U`, ImmediateOrCancel/FillOrKill→`I`, SelfTradePrevention→`Q`, MassCancel→`U`, CancelOnDisconnect→`U`, EndOfDay→`E` |
| `Replaced` | `U` with replacement + previous token (except partial-cancel replaces, see above) |
| `PhaseChanged` | none (OUCH has no instrument state) |

`S` System Event `S` is sent after login and `E` at end of day.

### Sequenced store limits and faults

SoupBinTCP Sequenced Data carries no sequence number: the client numbers
messages by counting from Login Accepted. The gateway therefore never leaves
a gap in a live stream.

* **Store reserve.** Each user's store keeps `store_reserve_messages` free
  for reports on orders that are already live (`X` cancels, executions,
  cancel-on-disconnect, end of day). Admission is checked before anything is
  stored, ahead of every gateway `J` (token table full, unknown stock,
  unsupported instruction, injected reject, `M`). Once the free space is at
  the reserve, an `O`, `U` or `M` stores nothing: the gateway closes the
  connection (close reason `StoreReserveReached`, no End of Session), counts
  it in `requests_refused_at_store_reserve`, and the client may reconnect and
  replay normally. Only a report that does not fit at all overflows the store
  (End of Session, `StoreFull`, no login until the next trading day).
* **`fault user X drop_out n`** is a transport cut: the next n sequenced
  messages are stored but not written, nothing after them is written, and
  the connection closes after the n-th (`FaultDropOut`). A client that
  reconnects asking for its own count + 1 receives every dropped message by
  replay, correctly numbered. Replay is never affected. The fault applies to
  application (sequenced) messages only. If the fault is cleared mid-cut, the
  next message closes the connection instead of being written, and a server
  End of Session during a cut is not sent (the client must reconnect).

## 3. FIX 4.4

### Session layer
* Logon(A): SenderCompID must match a configured user; TargetCompID must be
  the exchange CompID; HeartBtInt honoured; ResetSeqNumFlag=Y resets both
  sides to 1.
* Inbound MsgSeqNum: lower than expected without PossDupFlag → Logout +
  disconnect; higher application sequence → bounded-buffer or discard and send ResendRequest for the gap; never apply it before predecessors are received or validly gap-filled. Lower PossDup messages already applied are suppressed.
* Heartbeat(0) after HeartBtInt of outbound silence; TestRequest(1) after
  1.5 × HeartBtInt of inbound silence; disconnect after 2 × HeartBtInt.
* Garbled framing/checksum/body length, or 8, 9 and 35 not the first three
  fields: discard without advancing inbound sequence; the next valid high
  sequence triggers recovery. Parseable semantic errors consume their in-order
  sequence and receive session Reject(3) with reason 373.
* MsgSeqNum above 2^64 - 2 (the next expected value would wrap) → Logout +
  disconnect. Expected inbound sequence is never 0.
* PossDupFlag=Y requires OrigSendingTime(122): missing → Reject 373=1 371=122
  and the message is not applied; later than SendingTime(52) → Reject 373=10
  371=52, then Logout. An in-order PossDup GapFill is exempt.
* A valid FIX 4.4 application MsgType the gateway does not support →
  BusinessMessageReject(j) 380=3 (stored for resend); an invalid MsgType →
  Reject 373=11.
* GapFillFlag(123) other than Y or N → Reject 373=5 371=123 without moving the
  sequence. GapFill (123=Y) requires NewSeqNo > its MsgSeqNum. In-order gap
  fills advance expected to NewSeqNo. A PossDup GapFill below expected is a
  duplicate and is ignored, so held application messages are never skipped.
  A high gap fill waits for its gap. A duplicate early ResendRequest that is
  already held is serviced once.
  A nonduplicate attempt to lower expected is rejected. Non-gap SequenceReset is
  disaster recovery only: reject decreasing NewSeqNo; increasing NewSeqNo advances
  expected without application delivery. Logon reset is allowed only at a newly
  authenticated session boundary, never while application recovery is pending.
* Reply to TestRequest with Heartbeat echoing TestReqID. Only a matching
  Heartbeat clears an outstanding probe; unrelated inbound traffic does not.

### Application mapping

| FIX | Command | Notes |
|---|---|---|
| `D` NewOrderSingle | `NewOrder` | 11 → `cl_ord_id`; 55 → instrument; 54 1/2; 40 1=Market 2=Limit (others → ER Rejected); 59 0=Day 1=GTC 3=IOC 4=FOK; 18 contains `6` → post_only; 7928 N/R/A/B overrides STP mode |
| `F` OrderCancelRequest | `CancelOrder` | 41 → orig, 11 → request token, 60 required |
| `G` OrderCancelReplaceRequest | `ReplaceOrder` | 41 → orig, 11 → new token, 38 = new **total** qty, 44, 60 required |
| `q` OrderMassCancelRequest | `MassCancel` | 530=1 with 55 → that instrument; 530=7 → all; 530=2..6 → `r` 531=0 532=0 |

| Event | FIX |
|---|---|
| `Accepted` | ExecutionReport 150=0 39=0 |
| `Fill` | ER 150=F, 39=1 or 2, 32/31, 14/151, 6 AvgPx (gateway tracks notional per order) |
| `Canceled` | ER 150=4 39=4 (11 = request token when user-requested, 41 = order token) |
| `Canceled` EndOfDay | ER 150=C 39=C (DAY order expired at the close) |
| `Canceled` by a replace to qty ≤ CumQty | ER 150=5 39=2, 38 = 14 = CumQty, 151=0 (11 = new token, 41 = previous token) |
| `Replaced` | ER 150=5 39=0 or 1 (11 = new token, 41 = previous token) |
| `Rejected` NewOrder | ER 150=8 39=8, 103 OrdRejReason (UnknownInstrument→1, InvalidQuantity/InvalidPrice/InvalidField→99 with Text, DuplicateClOrdId→6, MarketClosed→2, CapacityExceeded→99) |
| `Rejected` Cancel/Replace | OrderCancelReject(9), 434 = 1 (cancel) / 2 (replace). Unknown or replaced (superseded) 41 → 102=1 39=8 37=NONE; order already Filled, Canceled or Expired → 102=0 with its 39 (2, 4 or C); other reasons → 102=99 with the order's current 39 |
| `MassCancelDone` | OrderMassCancelReport(r), 531 = 530 on success, 533 = count |
| Mass cancel refused | `r` 531=0 with 532: 0 unsupported 530, 1 unknown symbol, 99 other |

Price(44) is omitted from ExecutionReports of market orders.

ExecID(17) is unique per session for the day; OrderID(37) = `OrderId`.

## 4. Market data: ITCH 5.0 over MoldUDP64

Stock locate = `InstrumentId`; order reference number = `OrderId`; match
number = `MatchId`; timestamps = event `ts` minus the configured trading-day origin, in ns.
The result must be in [0, 86,400,000,000,000); never truncate to six bytes.
Reject an unrepresentable day/configuration before trading; stop the session
on a timestamp outside its day. Instrument bands must fit both enabled wires
(OUCH limit prices <= 1,999,999,900; ITCH unsigned Price(4)).

At start: System Event `O`, then Stock Directory `R` and Stock Trading
Action `H` for every instrument, then System Event `S` (start of system
hours).

| Event | ITCH |
|---|---|
| `Rested` | `A` Add Order (shares = leaves) |
| `Fill`, continuous | `E` Order Executed on the **resting** leg only (the aggressor was never displayed) |
| `Fill`, auction | `C` Order Executed With Price on **both** legs (price = uncross price, printable `N`) |
| `AuctionSummary` | `Q` Cross Trade (cross type `O` if leaving PreOpen, `C` if leaving PreClose, `H` otherwise) |
| `Canceled`, `was_resting` | `D` if leaves_qty=0; otherwise `X` Order Cancel for canceled_qty |
| `Replaced`, priority kept, leaves reduced | `X` Order Cancel (cancelled shares = old − new leaves) |
| `Replaced`, priority lost, and the batch's next event for the new id is `Rested` | `U` Order Replace (old ref → new ref, shares = leaves, new price); consume that Rested without emitting A |
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
engine's depth for every instrument after all packets of a command batch.
Per-packet checks instead require state reconstructed from the matching event prefix.

## 5. SBE 1.0 over FIXP 1.0

`protocol = fixp_sbe` users connect to `exchange.fixp_sbe_port`. The
transport is TCP with SOFH framing; the client flow is Idempotent and the
server flow Recoverable (FIXP Appendix A 7.1.3); the exsim SBE schema is id
22597 version 1 (`docs/protocols/exsim_order_entry_sbe.xml`). The business
mapping follows the FIX 4.4 tables in section 3, with numeric uint64 ClOrdIds
in place of FIX text tokens. Wire details, deviations from the FIXP
specification and the session state machine are in
[protocols/FIXP_SBE.md](protocols/FIXP_SBE.md).

One `FixpSbeUserState` per configured user lives for the whole trading day
inside a `FixpSbeUserStateDirectory`, which is a `server::ExchangeCore`
Sessions policy. It holds the token table, per-order metadata and the
sequenced store, so a reconnect resumes the server flow at the next sequence
number. A `FixpSbeConnection` lives for one transport. Engine events reach the
user state through `on_events` whether or not a transport is bound.

| `SubmitResult` | Answer |
| --- | --- |
| Applied, Deferred | nothing now; reports follow |
| OutsideTradingDay | reject with MarketClosed |
| OutputCapacityExceeded, DeferredQueueFull | reject with CapacityExceeded |
| void submit, no message answered the request | reject with RequestHadNoEffect |

Faults: `reject` applies to new orders only. With `drop_out` the business
report is stored and sequenced but not sent; the next live message is
preceded by a Sequence message, so the client sees the gap and requests
retransmission. Store exhaustion terminates the session and refuses normal
reconnects for the rest of the day, because later reports could not be
recovered; size `engine.store_messages_per_session` and
`engine.store_bytes_per_session` for the whole day. FIXP finalization
(FinishedSending, FinishedReceiving) is not implemented; at shutdown the
server sends Terminate on every live connection.

## Supported simulator profile and deterministic output

OUCH X with positive shares <= cum performs a full CancelOrder; zero also
cancels fully. Reductions use CancelOrder.reduce_to_total_qty and the existing token, preserving priority even in Halted/Closed.
Only displayed Y/P instructions, minimum quantity zero and cross type N are
supported; reject other display/cross instructions and all unsupported TIFs
(including one-second TIF=1) with J/O. Replacement instruction fields receive
the same validation. U shares <= cum cancels the old remainder under SPEC.
The day-used-token table covers O and replacement tokens, survives completion
and reconnect, and suppresses reuse before engine submission. Reserve tokens
on first well-framed submission, including rejected submissions. Exhaustion
rejects new tokens without evicting history. Reset only at a new trading day.

For every Fill, output buy leg before sell leg, including same-session fills
and auction ITCH C messages. Assign sequences and FIX ExecIDs in that order.
OUCH U acknowledgments use L before subsequent reports; shares are post-amend
leaves, and unknown-order requests are ignored. These are simulator profile
choices where the engine has a smaller instruction set than Nasdaq.

Simplified NOII (not Nasdaq's full hidden-interest model): current-reference
price is last trade or configured reference; near and far prices both equal
indicative price when crossed, otherwise zero. Paired shares and imbalance
come from AuctionIndicative; noncrossed imbalance is total buy leaves minus
total sell leaves in magnitude and direction, paired=0. Cross type follows
PreOpen O / PreClose C; price variation indicator is space (unavailable).
A priority-losing replacement emits exactly one creation: U with consumed
Rested, or D then eventual A, never both U and A for the new reference.
Q uses AuctionSummary.match_id, never a constituent fill's match ID.

Protocol references checked: [OUCH 4.2](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf),
[ITCH 5.0](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf),
[FIX session protocol](https://www.fixtrading.org/wp-content/uploads/download-manager-files/FIX_Transport_1.1.pdf).
