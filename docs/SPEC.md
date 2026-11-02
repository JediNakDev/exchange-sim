# Matching Engine Specification (normative)

This document defines the observable behaviour of `exsim::MatchingEngine`.
It is precise enough that two independent implementations — the production
engine (`src/exsim/matching_engine.cpp`) and the reference model used for
differential testing (`tests/exsim/reference_engine.hpp`) — produce
**identical event streams** for any command stream. Anything not specified
here is a bug in this document.

Types referenced below live in `include/exsim/{types,command,event}.hpp`.

---

## 1. Units and identifiers

| Thing | Representation |
|---|---|
| Price | `int64`, 4 implied decimals (`$150.25 = 1502500`) |
| Quantity | `uint32` shares |
| Timestamp | `uint64` ns, opaque; every event copies `ts` from the command that produced it |
| `OrderId` | assigned by the engine, starts at 1, +1 per assignment, engine-global |
| `MatchId` | starts at 1, +1 per `Fill` event, engine-global |
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
`tick_size > 0`, `lot_size > 0`, `max_order_qty > 0`, `0 < min_price <= max_price`,
`min_price % tick_size == 0`, `max_price % tick_size == 0`, the symbol is not
already registered, and fewer than `max_instruments` exist.

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
  A market order's TIF is ignored: it never rests.
* otherwise → rest (4.4).

## 5. CancelOrder

1. no live order for `(session, orig_cl_ord_id)` → `Rejected{cmd = Cancel,
   reason = UnknownOrder, cl_ord_id = cmd.cl_ord_id, orig_cl_ord_id =
   cmd.orig_cl_ord_id, order_id = 0, instrument = 0}`.
2. otherwise emit `Canceled{reason = UserRequested, request_cl_ord_id =
   cmd.cl_ord_id, canceled_qty = leaves, was_resting = true}` and remove the
   order. Allowed in every phase.

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
* **Priority kept** iff `cmd.price == O.price` and
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
emit `AuctionSummary{price = p*, matched_qty = V(p*)}` and set
`last_trade_price = p*`. The resulting book is never crossed.

`auction_indicative()` reports the same computation without executing (and
`crossed = false`, zeros when the book is not crossed). `imbalance_side` is
`Buy` if `I(p*) > 0`, `Sell` if `I(p*) < 0`, else `None`.

## 10. Event ordering summary

| Command | Events in order |
|---|---|
| NewOrder rejected | `Rejected` |
| NewOrder accepted | `Accepted`, then [FOK: `Canceled(FillOrKill)` and stop], then per match step `Fill` / STP `Canceled`s, then one of: `Rested`, `Canceled(ImmediateOrCancel)`, nothing |
| Cancel | `Canceled` or `Rejected` |
| Replace | `Rejected`, or `Canceled` (qty <= cum), or `Replaced` [, `Fill`/STP `Canceled`s, then `Rested` / nothing] |
| MassCancel | `Canceled`* (ascending OrderId), `MassCancelDone` |
| SetPhase | per instrument: [`Fill`*, `AuctionSummary`] [`Canceled(EndOfDay)`*] `PhaseChanged` |

Every event's `instrument` is the instrument of the order involved
(`MassCancelDone`: `cmd.instrument`; rejections: as stated above).

## 11. Invariants (checked by tests)

* In `Continuous`, `Closed` and `Halted` (after any command), the book is
  never crossed: `best_bid < best_ask` whenever both exist.
* Every level holds ≥ 1 order; level qty = Σ leaves; `leaves + cum == qty`.
* Quantity conservation: for every order, `qty == cum + leaves + Σ canceled`.
* `live_order_count()` equals the number of resting orders.
* Engine output is a pure function of the command stream (determinism).
