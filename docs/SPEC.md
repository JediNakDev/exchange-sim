# Matching Engine Specification (normative)

This document defines the observable behaviour of `exsim::MatchingEngine`.
It is precise enough that two independent implementations — the production
engine (`lib/src/exsim/matching_engine.cpp`) and the reference model used for
differential testing (`tests/exsim/reference/reference_engine.hpp`) — produce
**identical event streams** for any command stream. Anything not specified
here is a bug in this document.

Types referenced below live in `lib/include/exsim/{types,command,event}.hpp`.

---

## 1. Units and identifiers

| Thing | Representation |
|---|---|
| Price | `int64`, 4 implied decimals (`$150.25 = 1502500`) |
| Quantity | `uint32` shares |
| Timestamp | `uint64` ns, opaque; every event copies `ts` from the command that produced it |
| `OrderId` | assigned by the engine, starts at 1, +1 per assignment, engine-global |
| `MatchId` | starts at 1, +1 per `Fill` and `AuctionSummary` event, engine-global |
| `InstrumentId` | 1, 2, ... in `add_instrument` order (also the ITCH stock locate) |
| `SessionId` | 1..65535; 0 is invalid for order commands |
| `ClOrdId` | client token, 1..2^48-1; unique per session **among live orders** |
| `OwnerId` | STP group; 0 means "no owner" (never self-trades) |

**OrderId assignment.** An `OrderId` is consumed exactly when (a) a new order
passes validation (assigned *before* the `Accepted` event), or (b) a replace
loses priority (assigned before the `Replaced` event). Rejected commands never
consume an `OrderId`.

**Live order.** An order is live from acceptance until it is fully filled or
cancelled. Only resting orders are live after a command completes (IOC/FOK/
market orders never survive their own command).

## 2. Instruments

`InstrumentConfig` fields: `symbol`, `tick_size`, `lot_size`, `max_order_qty`,
`min_price`, `max_price`, `reference_price`, `initial_phase`.

`add_instrument` returns 0 (rejecting the config) unless:
`tick_size > 0`, `lot_size > 0`, `max_order_qty >= lot_size`,
`0 < min_price <= max_price`, `min_price % tick_size == 0`,
`max_price % tick_size == 0`, `min_price <= reference_price <= max_price`,
`reference_price % tick_size == 0`, `initial_phase` is a defined `Phase`, the
symbol is not already registered, and fewer than `max_instruments` exist.
An engine may also return 0 for a band of more price levels,
`(max_price - min_price) / tick_size + 1`, than its layout supports; the
production engine allows at most 2^24 levels and the reference model has no
such limit. This is a setup-time difference only and never changes events.

A limit price `p` is **valid** iff `min_price <= p <= max_price` and
`p % tick_size == 0`.

A quantity `q` is **valid** iff `q > 0`, `q % lot_size == 0`, `q <= max_order_qty`.

Each instrument has a `last_trade_price` (initially unset) updated by every
`Fill`, and a phase (initially `initial_phase`).

**Auction reference price** = `last_trade_price` if set, else `reference_price`.

## 3. Phases

| Phase | New order | Replace | Cancel / MassCancel | Matching |
|---|---|---|---|---|
| `Closed` | reject `MarketClosed` | reject `MarketClosed` | allowed | none |
| `PreOpen` | limit DAY/GTC only | allowed | allowed | none (book may cross) |
| `Continuous` | all | allowed | allowed | continuous |
| `Halted` | reject `InstrumentHalted` | reject `InstrumentHalted` | allowed | none |
| `PreClose` | limit DAY/GTC only | allowed | allowed | none (book may cross) |

`PreOpen` and `PreClose` are the **call phases**. In a call phase, market
orders and IOC/FOK orders are rejected with `NotAllowedInPhase`.

## 4. NewOrder

### 4.1 Validation

Checks run in this order; the first failure emits one `Rejected` event
(`cmd = NewOrder`, `cl_ord_id = cmd.cl_ord_id`, `orig_cl_ord_id = 0`,
`order_id = 0`, `instrument = cmd.instrument`) and nothing else.

1. `instrument` not registered → `UnknownInstrument`
2. `session == 0`, `cl_ord_id == 0` or `cl_ord_id > kMaxClOrdId` → `InvalidClOrdId`
3. a live order with the same `(session, cl_ord_id)` exists → `DuplicateClOrdId`
4. enum field out of range (`side`, `type`, `tif`, `stp`), or `post_only` combined
   with `type == Market` or `tif ∈ {IOC, FOK}` → `InvalidField`
5. quantity invalid → `InvalidQuantity`
6. `type == Limit` and price invalid → `InvalidPrice` (market order prices are ignored)
7. phase: `Closed` → `MarketClosed`; `Halted` → `InstrumentHalted`;
   call phase and (`type == Market` or `tif ∈ {IOC, FOK}`) → `NotAllowedInPhase`
8. `post_only`, phase `Continuous`, and the order would cross the opposite best
   (buy: `price >= best_ask`; sell: `price <= best_bid`) → `PostOnlyWouldCross`
9. no capacity for another live order → `CapacityExceeded`

Capacity: an incoming order needs a slot even if it will fully fill. The
engine rejects with `CapacityExceeded` iff the number of live orders equals
`EngineConfig::max_orders` at the moment of the check.

### 4.2 Acceptance

On passing validation: assign `OrderId`, emit `Accepted` (all order fields;
`price` is copied verbatim from the command, including for market orders).

Then:

* **Call phase**: the order rests (see 4.4). No matching, no cross check.
* **Continuous**: FOK pre-check (4.3) if `tif == FOK`, then matching (4.5),
  then disposition (4.6).

### 4.3 FOK pre-check

Walk the opposite side in priority order (best price first, FIFO within a
level) over resting orders whose price *crosses* the incoming order (4.5).
Maintain `avail = 0`. For each resting order `R`:

* if `R` is a **self-match** (4.5) and `stp == CancelResting`: skip `R`;
* else if `R` is a self-match (any other STP mode): stop walking;
* else `avail += R.leaves`; stop walking once `avail >= qty`.

If `avail < qty`: emit `Canceled{reason = FillOrKill, canceled_qty = qty,
cum_qty = 0, was_resting = false}` and stop. Otherwise proceed to matching
(which is then guaranteed to fill the order completely).

### 4.4 Resting

Append the order to the tail of the FIFO queue at its price level on its side
and emit `Rested{order_id, cl_ord_id, session, side, price, leaves_qty}`.

### 4.5 Continuous matching

Incoming order `A` (limit price `pA` or market). A resting price `p`
**crosses** iff `A` is a market order, or `A` is a buy and `pA >= p`, or `A`
is a sell and `pA <= p`.

`R` is a **self-match** iff `A.stp != None`, `A.owner != 0` and
`A.owner == R.owner`. (The aggressor's STP mode governs; the resting order's
mode is irrelevant.)

```
while A.leaves > 0 and the opposite side is non-empty
      and the opposite best price crosses:
    R = head of the opposite best level
    if self-match(R):
        CancelResting:   emit Canceled{R, SelfTradePrevention, R.leaves, was_resting=true};
                         remove R; continue
        CancelAggressor: emit Canceled{A, SelfTradePrevention, A.leaves, was_resting=false};
                         A is done (does not rest); stop
        CancelBoth:      emit Canceled{R, ...was_resting=true}, then
                         emit Canceled{A, ...was_resting=false}; remove R; A is done; stop
    q = min(A.leaves, R.leaves)
    execute q at R.price: both legs' leaves -= q, cum += q
    emit Fill{match_id = next, price = R.price, qty = q,
              aggressor = A.side, buy/sell legs with post-fill leaves/cum}
    last_trade_price = R.price
    if R.leaves == 0: remove R (no further event)
```

### 4.6 Disposition after matching

* `A` cancelled by STP → done.
* `A.leaves == 0` → done (no further event).
* `A` is market, IOC (or FOK — cannot happen) → emit
  `Canceled{A, ImmediateOrCancel, canceled_qty = A.leaves, was_resting = false}`.
  A market order's TIF is ignored here: it never rests. A market FOK still
  runs the 4.3 pre-check first, so it is killed with `FillOrKill` unless the
  whole opposite side can fill it.
* otherwise → rest (4.4).

## 5. CancelOrder

1. no live order for `(session, orig_cl_ord_id)` → `Rejected{cmd = Cancel,
   reason = UnknownOrder, cl_ord_id = cmd.cl_ord_id, orig_cl_ord_id =
   cmd.orig_cl_ord_id, order_id = 0, instrument = 0}`.
2. otherwise emit `Canceled{reason = UserRequested, request_cl_ord_id =
   cmd.cl_ord_id, canceled_qty = leaves, was_resting = true}` and remove the
   order. Allowed in every phase.

If CancelOrder.reduce_to_total_qty is nonzero, compute target leaves as
max(total - cum, 0), using a comparison before unsigned subtraction. If target
>= old leaves, do nothing and emit nothing. Otherwise remove old leaves minus
target and emit Canceled with leaves_qty=target. If target is positive, retain
the order ID, token and FIFO position; update stored qty=cum+target. No lot or
price validation applies to a cancellation reduction. If target is zero, remove
the order. This path is allowed in every phase, unlike ReplaceOrder.

## 6. ReplaceOrder

Let `O` be the live order for `(session, orig_cl_ord_id)`; `new_token =
cmd.cl_ord_id != 0 ? cmd.cl_ord_id : orig_cl_ord_id`. Rejections are
`Rejected{cmd = Replace, cl_ord_id = cmd.cl_ord_id, orig_cl_ord_id,
order_id = O.order_id (0 if not found), instrument = O.instrument (0 if not
found)}`. Checks in order:

1. `O` not found → `UnknownOrder`
2. `cmd.cl_ord_id > kMaxClOrdId` → `InvalidClOrdId`;
   `new_token != orig_cl_ord_id` and `(session, new_token)` is live → `DuplicateClOrdId`
3. quantity invalid → `InvalidQuantity`
4. price invalid → `InvalidPrice`
5. phase `Closed` → `MarketClosed`; `Halted` → `InstrumentHalted`
6. `O.post_only`, phase `Continuous`, and the new price would cross the opposite
   best → `PostOnlyWouldCross`

Then:

* **`cmd.qty <= O.cum_qty`**: the order is done. Emit
  `Canceled{reason = UserRequested, request_cl_ord_id = cmd.cl_ord_id,
  canceled_qty = O.leaves, was_resting = true}` (with the order's *old* token)
  and remove it.
* **Priority kept** iff `!cmd.force_priority_loss`, `cmd.price == O.price` and
  `cmd.qty - O.cum_qty <= O.leaves`. Update in place (token, qty, leaves; FIFO
  position unchanged) and emit `Replaced{new_order_id = old_order_id,
  priority_kept = true}`.
* **Priority lost** otherwise: remove `O` from the book, assign a new `OrderId`,
  update token/price/qty/leaves (cum is preserved), emit
  `Replaced{old_order_id, new_order_id, priority_kept = false}`. Then, in
  `Continuous`, match it as an aggressor (4.5, with its own owner/STP mode)
  followed by disposition (4.6, it is DAY/GTC so a remainder rests); in a call
  phase it rests directly (4.4).

`Replaced` fields: `old_price`/`old_leaves_qty` are pre-replace values; `qty`,
`leaves_qty = qty - cum_qty`, `cum_qty`, `price` are post-replace values.

## 7. MassCancel

Cancels every live order of `session` (restricted to `instrument` if non-zero)
in **ascending OrderId order**, each with `Canceled{reason = MassCancel or
CancelOnDisconnect (per cmd.cancel_on_disconnect), request_cl_ord_id =
cmd.cl_ord_id, was_resting = true}`, then emits `MassCancelDone{session,
request_cl_ord_id, canceled_count, reason}` with `instrument = cmd.instrument`.
Never rejected (an unknown instrument simply matches no orders). Allowed in
every phase.

## 8. SetPhase

If `cmd.instrument != 0` and unknown → `Rejected{cmd = SetPhase, reason =
UnknownInstrument, session = 0, cl_ord_id = 0, ...}`. If `cmd.phase` is out of
range → `Rejected{..., InvalidField}`. Otherwise apply to the instrument (or to
every instrument in ascending id order when `instrument == 0`):

1. if new phase == old phase: nothing.
2. if new phase ∈ {`Continuous`, `Closed`} and the book is crossed
   (`best_bid >= best_ask`): run the **uncross** (9).
3. if new phase == `Closed`: expire every live `Day` order of the instrument
   in ascending OrderId order: `Canceled{reason = EndOfDay, was_resting = true}`.
   GTC orders remain.
4. set the phase and emit `PhaseChanged{old_phase, new_phase}`.

## 9. Auction uncross

Only resting limit orders exist in a call phase, so the uncross considers
limit orders only. Let `bb`/`ba` be the best bid/ask (`bb >= ba`).

**Candidate prices**: every distinct price in `[ba, bb]` at which at least one
order (either side) rests.

For candidate `p`: `B(p)` = Σ leaves of buys with price `>= p`;
`S(p)` = Σ leaves of sells with price `<= p`; `V(p) = min(B, S)`;
`I(p) = B(p) - S(p)`.

**Uncross price** `p*`: maximise `V`; ties → minimise `|I|`; ties → minimise
`|p - auction reference price|`; ties → the lower price.

**Execution**: repeatedly pair the highest-priority remaining buy (highest
price, then FIFO) with the highest-priority remaining sell (lowest price, then
FIFO) while the buy's price `>= p*` and the sell's price `<= p*`; execute
`q = min(leaves)` at `p*`, emitting `Fill{aggressor = None, price = p*}` and
removing exhausted orders. **STP is not applied** in the uncross. Afterwards
emit `AuctionSummary{price = p*, matched_qty = V(p*), match_id = next}` and set
`last_trade_price = p*`. The resulting book is never crossed.

`auction_indicative()` reports the same computation without executing:

* crossed: `crossed = true`, `price = p*`, `paired_qty = V(p*)`,
  `imbalance_qty = |I(p*)|`, and `imbalance_side` is `Buy` if `I(p*) > 0`,
  `Sell` if `I(p*) < 0`, else `None`;
* not crossed: `crossed = false`, `price = 0`, `paired_qty = 0`,
  `imbalance_qty` is the unsigned difference between the total buy and total
  sell leaves of the instrument, and `imbalance_side` is the side with the
  larger total (`None` if equal).

## 10. Event ordering summary

| Command | Events in order |
|---|---|
| NewOrder rejected | `Rejected` |
| NewOrder accepted | `Accepted`, then [FOK: `Canceled(FillOrKill)` and stop], then per match step `Fill` / STP `Canceled`s, then one of: `Rested`, `Canceled(ImmediateOrCancel)`, nothing |
| Cancel | `Canceled`, `Rejected`, or nothing (a reduction to at least the current leaves, 5) |
| Replace | `Rejected`, or `Canceled` (qty <= cum), or `Replaced` [, `Fill`/STP `Canceled`s, then `Rested` / nothing] |
| MassCancel | `Canceled`* (ascending OrderId), `MassCancelDone` |
| SetPhase | per instrument: [`Fill`*, `AuctionSummary`] [`Canceled(EndOfDay)`*] `PhaseChanged` |

Every event's `instrument` is the processed instrument, including PhaseChanged
and AuctionSummary (`MassCancelDone`: cmd.instrument; rejections: as specified).

## 11. Invariants (checked by tests)

* In `Continuous` and `Closed` (after any command), the book is
  never crossed: `best_bid < best_ask` whenever both exist.
* Every level holds ≥ 1 order; level qty = Σ leaves; `leaves + cum == qty`.
* Live current total = cumulative fills + leaves. Across an amendment chain,
  executed + current leaves + Canceled quantities = initial total + sum of
  (actual replacement total minus previous total). A replace that cancels
  because qty <= cum contributes no amendment delta, only its Canceled event.
* `live_order_count()` equals the number of resting orders.
* Engine output is a pure function of the command stream (determinism).

## 12. Foundation boundaries (normative)

Halted preserves crossed books inherited from call phases without matching.
Entering Continuous or Closed uncrosses regardless of previous phase.
All phase/auction events identify the processed instrument, even with no orders.

EngineConfig construction rejects zero limits, max_instruments > 65535 and
max_orders > UINT32_MAX. `required_event_capacity(config)` validates these
bounds and computes checked `2*max_orders + 2*max_instruments + 4` slots.
Each match or STP step removes at least one live order or terminates the
aggressor; phase batches additionally have at most one summary and one phase
change per instrument plus one expiration per live order. This bound therefore
covers every command, including empty all-instrument phase transitions.
Instrument setup also rejects invalid Phase values, reference prices outside
the valid price grid, and unrepresentable ladder/allocation sizes. Each engine
must check its own layout's byte sizes before allocation. Max order quantity
must be at least lot size. Side-volume/FOK/auction/level sums use uint64, never
Quantity; these config bounds ensure max_orders*UINT32_MAX fits uint64.
Imbalance uses unsigned subtraction of smaller from larger. Price distances
subtract smaller positive valid price from larger, never signed abs overflow.

EventBuffer allocates only during construction. Its fixed capacity includes
already-appended events; clear retains capacity. Before any command mutation,
including ID counters, engine checks remaining capacity against the conservative
bound above or a proven tighter read-only bound. Insufficient room returns
HandleResult::InsufficientOutput and leaves engine AND output unchanged; no
rejection event is emitted. Caller drains/retries with a startup-sized buffer.
Otherwise handle returns Applied, including semantic rejections. A Command
whose type is none of the five CommandType values changes no state and emits
no events (journal readers and gateways reject unknown tags before the
engine). push returns
false when full and never grows: every engine must honor preflight and treat
an unexpected push failure as an implementation defect, never silently drop it.
The server must not dispatch or journal an unapplied command as completed.
No exception or vector growth after mutation is an exhaustion strategy.

Active event payload fields are all mandatory, with these defaults:
Accepted echoes all NewOrder fields plus assigned order ID; Rested echoes the
stored order and its remaining quantity. Fill has positive qty, next match ID,
trade price, correct aggressor, and both complete post-fill legs (including
owner/session). Replaced uses pre-amend old fields and post-amend new fields,
with cum preserved. Canceled echoes stored side/price/token/session/ID and cum, including the
verbatim ignored price of a market order; leaves_qty is post-cancel leaves
(zero except partial CancelOrder reductions). The request token is zero
except on the Canceled that a Cancel, Replace (qty <= cum, 6) or MassCancel
itself requests, which echoes the command's token. Self-trade prevention
cancels emitted while a Replace that lost priority matches follow 4.5 and
carry request token zero, like those of a NewOrder. Rejected always
echoes request session, request token and target token for Cancel/Replace;
NewOrder target/ID are zero. Unknown Cancel/Replace instrument/ID are zero;
known Replace rejection uses target instrument/ID. SetPhase rejection echoes
requested instrument, command type and reason, with session, tokens and ID zero.
MassCancelDone echoes request fields, count and selected reason. PhaseChanged
uses actual old/new phases. AuctionSummary uses uncross price, aggregate volume
and a fresh global match ID allocated AFTER all constituent fills. No summary
or match ID is emitted for a noncrossed transition. Unmentioned request-token
fields are zero. Every event copies command ts; inactive union storage and
padding have no meaning. Rejections consume no order/match IDs.

Replay equality means ordered active field equality. Journals require a
versioned serializer with fixed widths, byte order, tag and active fields;
raw Command/Event memory and memcmp/hashing padding are prohibited. The
journal/oracle owner must implement and test that encoding before replay claims.
