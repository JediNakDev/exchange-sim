# SPEC conformance review (matching engine)

Independent QA review of `docs/SPEC.md` against the reference oracle
(`tests/exsim/reference/reference_engine.hpp`) and production engine B
(`src/exsim/matching_engine.cpp`). Differential fuzzing proves the two agree;
this review checks that what they agree on is what the spec says.

Method: every normative rule was extracted from SPEC.md (including the
section 12 repairs and FOUNDATION_REPAIRS.md contract details). For each rule
the expected events of new scenarios were derived by hand from the spec text
before running either implementation. Scenarios were then run against both
engines individually (`exsim_scenario` built for each) and through the suite.

Reviewed at base commit `e18fd42`.

## Results

| Item | Value |
|---|---|
| Normative rules extracted | 82 |
| Rules asserted exactly by new spec scenarios | 74 |
| Rules outside the scenario DSL (C++ tests only) | 4 (R9, R73, R75, R76) |
| Rules with a spec ambiguity | 4 (R32, R68, R79, R82), plus doc inconsistencies D1 to D3 |
| Rules exactly asserted by the 23 pre-existing scenarios | 9 (others asserted only partially, see H2) |
| New scenarios | 16 files `tests/scenarios/spec_*.scn`, about 330 event and state assertions |
| Pending scenarios (spec right, implementation wrong) | 0. `tests/scenarios_pending/` not created |
| `make test EXTRA_CXXFLAGS=-DEXSIM_TEST_PRODUCTION` | 272/272 passed (release) |
| `make test MODE=debug EXTRA_CXXFLAGS=-DEXSIM_TEST_PRODUCTION` | 270/270 passed (ASan/UBSan) |
| Mutation check | Changing one hand-derived value (auction price 101 to 99.5) fails both `Oracle.scenarios` and `ProductionEngine.scenarios` |

No behavioural divergence from SPEC.md was found in either implementation for
any reachable command stream I constructed. All findings below are spec
ambiguities, harness weaknesses or code-level hazards found by reading.

Every new scenario uses `expect count` on each command it checks, so extra or
missing events fail. Most assertions list every field that the rule fixes.

## New scenarios

| File | Rule areas |
|---|---|
| `spec_validation_order.scn` | 4.1 check order 1 to 8, every rejection reason, token bounds 0 / 2^48-1 / 2^48, session 0, enum range, post-only combos, lot / max / band-edge / off-grid values, ts echo, rejected orders consume no OrderId, verbatim negative market price |
| `spec_capacity_boundaries.scn` | 4.1.9 slot needed even for a fully filling IOC or market order, post-only (8) before capacity (9), replace needs no slot, slot freed by cancel |
| `spec_stp_level_modes.scn` | 4.5 own / foreign / own at one level with CancelResting, CancelAggressor after partial fill, CancelBoth leaves worse levels, owner 0 never self-matches, resting mode irrelevant |
| `spec_fok_stp_walk.scn` | 4.3 walk with each STP mode, self-match after sufficient liquidity, walk limited by price, exact multi-level fill, kill event fields |
| `spec_fok_market.scn` | 4.2 / 4.3 market FOK (literal reading, ambiguity A1) |
| `spec_continuous_matching.scn` | 4.5 / 4.6 trade at resting price, sell aggressor leg orientation, IOC no-cross / partial / full, sweep then rest at limit, token reuse after completion, per-session duplicate across instruments |
| `spec_market_band_edges.scn` | market order sweeps min and max band levels, GTC market never rests, verbatim price on IOC cancel |
| `spec_replace_partial_fill.scn` | 6 kept vs lost on partially filled orders, token re-key, FIFO retained, qty == cum cancel with old token and request token, price change matches as aggressor, qty increase loses priority, call-phase replace rests, last trade price as auction reference, post-only replace |
| `spec_replace_rejections.scn` | 6 check order and fields (UnknownOrder per session, InvalidClOrdId, Duplicate, qty, price, phase), checks before qty <= cum, Halted / PreOpen / Closed behaviour, PreOpen to Closed uncross then expiry |
| `spec_replace_stp_and_ids.scn` | 6 priority-loss replace uses its own STP mode (CancelAggressor and CancelResting), session 65535 with token 2^48-1 does not alias |
| `spec_partial_cancel_reduction.scn` | 5 target = max(total - cum, 0), no-op when target >= leaves, total below cum, total == cum, FIFO retained, no lot validation, Closed phase |
| `spec_mass_cancel_order.scn` | 7 ascending OrderId including a later priority-loss id, instrument filter, unknown instrument, all instruments, reason selection, Halted, session 0 |
| `spec_phase_transitions.scn` | 8 rejection order and fields, same-phase no-op, all-instrument ascending batch, call-to-call and call-to-Halted keep the cross, Halted to Continuous uncross with STP not applied, noncrossed transition consumes no MatchId, Closed expiry ascending OrderId not price |
| `spec_halted_cross_close.scn` | 12 PreOpen to Halted to Closed, minimum imbalance beats reference distance, GTC remainder survives |
| `spec_auction_ties.scn` | 9 equal and opposite imbalance resolved by reference then lower price, touching book, candidates only at resting prices inside [ba, bb], FIFO pairing with partial legs, same-owner trades, all-instrument batch order |
| `spec_volume_u32_sums.scn` | 12 level, auction and FOK sums above 2^32 |

## Rule table

Status: `new` = asserted exactly by a new spec scenario; `covered` = an
existing scenario already asserts it exactly; `gap` = not expressible in the
scenario DSL; `ambiguous` = the spec text admits two readings (see findings).
"Existing" lists pre-review scenarios that touch the rule (often partially).

| R | Section | Rule | Existing | New spec scenarios | Status |
|---|---|---|---|---|---|
| R1 | 1 | Every event copies the command `ts` | none | validation_order, stp_level_modes, phase_transitions, mass_cancel_order, replace_rejections | new |
| R2 | 1 | OrderId starts at 1, +1 per assignment, engine-global | replace | all | new |
| R3 | 1 | Rejected commands consume no OrderId | none | validation_order, capacity_boundaries | new |
| R4 | 1 | Priority-loss replace assigns a new OrderId before Replaced | replace | replace_partial_fill, mass_cancel_order | covered, new |
| R5 | 1 | MatchId +1 per Fill and AuctionSummary, global across instruments | auction, auction_volume | auction_ties, phase_transitions | covered, new |
| R6 | 1 | ClOrdId 1..2^48-1, unique per session among live orders only | validation | validation_order, continuous_matching, replace_stp_and_ids | new |
| R7 | 1 | SessionId 0 invalid for orders, 1..65535 valid | none | validation_order, replace_stp_and_ids | new |
| R8 | 1 | OwnerId 0 never self-trades | none | stp_level_modes | new |
| R9 | 2, 12 | add_instrument config validation | none | DSL throws on invalid instrument | gap |
| R10 | 2 | Price valid iff in [min, max] and on tick | validation | validation_order, replace_rejections | new |
| R11 | 2 | Quantity valid iff > 0, lot multiple, <= max | validation | validation_order, replace_rejections | new |
| R12 | 2 | last_trade_price updated by every Fill | none | replace_partial_fill | new |
| R13 | 2, 9 | Auction reference = last trade price if set else configured | auction_distance | replace_partial_fill, auction_ties | new |
| R14 | 3 | Closed rejects NewOrder with MarketClosed | phases | validation_order | covered, new |
| R15 | 3 | Closed rejects Replace with MarketClosed | none | replace_rejections | new |
| R16 | 3 | Halted rejects NewOrder / Replace with InstrumentHalted | phases | phase_transitions, replace_rejections | new |
| R17 | 3 | Cancel and MassCancel allowed in every phase | partial_cancel | replace_rejections, mass_cancel_order, partial_cancel_reduction | new |
| R18 | 3 | Call phase rejects market, IOC, FOK with NotAllowedInPhase | phases | phase_transitions (FOK) | covered, new |
| R19 | 3, 6 | Replace allowed in call phase; rests without matching | none | replace_partial_fill, replace_rejections | new |
| R20 | 4.1 | Checks run in order 1..9; first failure only | none | validation_order, capacity_boundaries | new |
| R21 | 4.1, 12 | NewOrder Rejected fields (session, token echoed; orig 0, id 0, instrument = cmd.instrument) | none | validation_order | new |
| R22 | 4.1.2 | Session 0, token 0, token > max give InvalidClOrdId | none | validation_order | new |
| R23 | 4.1.3 | Live duplicate gives DuplicateClOrdId (before enum / qty) | validation | validation_order | new |
| R24 | 4.1.4 | Out-of-range side/type/tif/stp, post-only with market/IOC/FOK give InvalidField | none | validation_order | new |
| R25 | 4.1.6 | Market order price ignored by validation | none | validation_order, market_band_edges | new |
| R26 | 4.1.8 | Post-only rejected only in Continuous, buy >= best ask / sell <= best bid | post_only | capacity_boundaries, phase_transitions, replace_partial_fill | new |
| R27 | 4.1.9 | Capacity: slot needed even if fully filling; reject iff live == max | capacity | capacity_boundaries | new |
| R28 | 4.2, 12 | Accepted echoes all fields, verbatim price incl. market | none | validation_order, market_band_edges | new |
| R29 | 4.2 | Call phase: rest, no matching, no cross check | none | phase_transitions | new |
| R30 | 4.3 | FOK kill emits only Canceled{FillOrKill, qty, cum 0, not resting} | fok | fok_stp_walk | covered, new |
| R31 | 4.3 | FOK walk skips CancelResting self-matches, stops on other modes, stops at avail >= qty, limited to crossing prices | fok_stp | fok_stp_walk | new |
| R32 | 4.2, 4.3, 4.6 | FOK pre-check for market FOK orders | none | fok_market (literal reading) | ambiguous (A1) |
| R33 | 4.4 | Rest at FIFO tail; Rested fields | fifo | continuous_matching, all | new |
| R34 | 4.5 | Crossing definition; trade at resting price | sweep | continuous_matching, market_band_edges | new |
| R35 | 4.5, 12 | Fill: aggressor side, buy/sell orientation, complete post-fill legs | none | continuous_matching, stp_level_modes | new |
| R36 | 4.5 | Self-match uses aggressor mode and owner only | stp_cancel_* | stp_level_modes | new |
| R37 | 4.5 | CancelResting cancels and continues (several at one level) | stp_cancel_resting | stp_level_modes | new |
| R38 | 4.5 | CancelAggressor cancels remainder after partial fills | stp_cancel_aggressor | stp_level_modes, replace_stp_and_ids | new |
| R39 | 4.5 | CancelBoth: resting cancel then aggressor cancel | stp_cancel_both | stp_level_modes | new |
| R40 | 4.6 | Market / IOC remainder: Canceled{ImmediateOrCancel}; market TIF ignored; none if fully filled | ioc, market | continuous_matching, market_band_edges | covered, new |
| R41 | 4.6 | DAY / GTC remainder rests at its limit | sweep | continuous_matching | new |
| R42 | 5 | Cancel of unknown order: Rejected with instrument 0, id 0, tokens echoed | none | replace_partial_fill | new |
| R43 | 5 | Full cancel fields (request token, canceled = leaves, was_resting) | none | replace_rejections, partial_cancel_reduction | new |
| R44 | 5 | Reduction target = max(total - cum, 0) without unsigned wrap | partial_cancel, amend_filled | partial_cancel_reduction | covered, new |
| R45 | 5 | target >= leaves: nothing emitted | partial_cancel | partial_cancel_reduction | covered, new (see D2) |
| R46 | 5 | Partial reduction keeps id, token, FIFO; qty = cum + target; no lot check; any phase | partial_cancel | partial_cancel_reduction | new |
| R47 | 6 | Replace check order and Rejected fields (target id / instrument when known) | none | replace_rejections, replace_partial_fill | new |
| R48 | 6 | qty <= cum: Canceled with old token and request token, after all checks | amend_filled | replace_partial_fill, replace_rejections | new |
| R49 | 6 | Priority kept iff no force, same price, qty - cum <= leaves | replace | replace_partial_fill | new |
| R50 | 6 | Priority lost: new id, cum preserved, aggressor with own STP in Continuous, rest in call phase | replace | replace_partial_fill, replace_stp_and_ids | new |
| R51 | 6, 12 | Replaced old_* pre-replace, others post-replace | none | replace_partial_fill | new |
| R52 | 6 | Post-only replace cross check only in Continuous | none | replace_partial_fill, replace_rejections | new |
| R53 | 7 | Mass cancel ascending OrderId, filter, reason, Done fields, instrument = cmd.instrument, never rejected | mass | mass_cancel_order | new |
| R54 | 8 | SetPhase rejections (unknown instrument before invalid phase) and fields | none | phase_transitions | new |
| R55 | 8.1 | Same phase emits nothing | none | phase_transitions | new |
| R56 | 8 | instrument 0 applies to all in ascending id | none | phase_transitions, auction_ties | new |
| R57 | 8.2 | Uncross only when entering Continuous / Closed with a crossed book | auction, halt_auction | phase_transitions, halted_cross_close | new |
| R58 | 8.3 | Closed expires DAY orders ascending OrderId after the uncross; GTC remain | expiry | phase_transitions, halted_cross_close, replace_rejections | new |
| R59 | 8.4 | PhaseChanged last, actual old/new phases | none | phase_transitions | new |
| R60 | 9 | Candidates: distinct resting prices in [ba, bb] only | none | auction_ties (DDD) | new |
| R61 | 9 | Maximise V | auction_volume | phase_transitions (CCC), auction_ties (EEE) | covered, new |
| R62 | 9 | Then minimise abs(I) | auction_imbalance | halted_cross_close, auction_ties | new |
| R63 | 9 | Then minimise distance to reference | auction_distance | auction_ties, replace_partial_fill | new |
| R64 | 9 | Then lower price | auction | auction_ties, phase_transitions | covered, new |
| R65 | 9 | Pairing best buy with best sell (price then FIFO) at p*, aggressor None | none | auction_ties (EEE) | new |
| R66 | 9 | STP not applied in uncross | none | phase_transitions, auction_ties | new |
| R67 | 9, 12 | Summary after fills with matched_qty = V(p*), fresh match id; last trade = p* | auction | auction_ties, replace_rejections | new |
| R68 | 9 | auction_indicative fields | none | not expressible in DSL | ambiguous (A2), gap |
| R69 | 10 | Event order per command type | partial | all | new |
| R70 | 10 | Event instrument field rules | none | mass_cancel_order, phase_transitions, validation_order | new |
| R71 | 11 | Book never crossed in Continuous / Closed | none | depth checks after every uncross | new |
| R72 | 11 | Level qty = sum of leaves; leaves + cum = qty | none | depth / order checks | new |
| R73 | 11 | Determinism | none | fuzz and replay tests | gap |
| R74 | 12 | Halted keeps an inherited cross; Continuous / Closed uncross from any phase | halt_auction | phase_transitions, halted_cross_close | new |
| R75 | 12 | EngineConfig bounds, required_event_capacity | none | C++ tests | gap |
| R76 | 12 | Preflight: InsufficientOutput leaves state and output unchanged | none | C++ tests; code read (finding E3) | gap |
| R77 | 12 | uint64 side / FOK / auction / level sums | none | volume_u32_sums | new |
| R78 | 12 | Unsigned imbalance and price distance | auction_distance | auction_ties, halted_cross_close | new |
| R79 | 12 | Canceled request token zero except Cancel / Replace / MassCancel | none | asserted for unambiguous cases only | ambiguous (A3) |
| R80 | 12 | SetPhase Rejected: session, tokens, id zero; instrument echoed | none | phase_transitions | new |
| R81 | 12 | No summary or match id on a noncrossed transition | none | phase_transitions (BBB) | new |
| R82 | 12 (silent) | Out-of-range CommandType | none | not constructible in DSL | ambiguous (A5) |

## Findings, ranked by severity

No finding is a confirmed spec violation by an implementation. Severity
reflects the risk to the project if left alone.

### Resolutions (2026-11-04, after engine C became production)

| Finding | Resolution |
| --- | --- |
| H1 | `ProductionEngine.scenarios` runs every file and fails with `path: line N: event field ...` for each failing one. Checked with the repro below. |
| A1 | SPEC 4.6 states that a market FOK runs the 4.3 pre-check (reading 1). |
| A2 | SPEC 9 lists the crossed and not-crossed `auction_indicative` fields separately (reading 1). |
| A3 | SPEC 12: only the Canceled that a Cancel, Replace or MassCancel itself requests echoes the token; STP cancels during a Replace carry zero (reading 1). |
| A5 | SPEC 12: an unknown CommandType changes nothing and emits nothing. Engine C and the oracle already behave this way; engine B is no longer production. |
| D1, D2, D3 | Fixed in SPEC 2, 10 and 12. SPEC 2 also notes the production engine's 2^24 price-level limit at setup. |
| E1 | Applied to engine B; engine C had the same cost and now keeps per-instrument order lists (`docs/engine-variants/C_INDEX_ARENA.md`). |
| E2 | Engine B only. Engine C's `blank_event` starts the lifetime of the correct union member before any field is written. |
| E3, H2, H3 | Informational; unchanged. |

### Medium

**H1. Production scenario test hides which scenario failed.**
`tests/exsim/differential/engine_diff_tests.cpp:12-16` calls `run_scenario`
without catching, so a failure prints only `FAILED ProductionEngine.scenarios`
with no file, line or event (observed during the mutation check), and the
first failing file stops the loop, hiding later failures. `Oracle.scenarios`
(`tests/exsim/reference_tests.cpp:12-20`) wraps each file and reports
`path: line N: event field ...`. Fix: mirror the oracle loop. Repro: any
scenario with a wrong expectation, for example change `price=101` to
`price=99.5` in the `auction_summary` line of `spec_halted_cross_close.scn`.

**A3. Request token of Canceled events emitted while processing a Replace.**
SPEC.md:336: "request token is zero except for Cancel/Replace/MassCancel, whose
request token is echoed." A priority-loss Replace can produce STP Canceled
events (for the replaced order with CancelAggressor/CancelBoth, or for resting
orders with CancelResting) during the same command. Reading 1: only the
Canceled that the Replace itself requests (qty <= cum, section 6) echoes the
token; STP cancels follow section 4.5 with 0. Reading 2: every Canceled emitted
by a Replace command echoes `cmd.cl_ord_id`. Both implementations use reading 1.
This matters for FIX tag 11 on the resulting execution report (GATEWAYS.md:111).
`spec_replace_stp_and_ids.scn` deliberately does not assert this field.
Recommend adding one sentence to section 12 or 4.5.

### Low

**A1. FOK pre-check for market orders.** SPEC.md:103 runs the pre-check "if
`tif == FOK`"; section 4.6 says "A market order's TIF is ignored: it never
rests." Reading 1 (both implementations, documented in SCENARIOS.md): market
FOK runs the pre-check and is killed with FillOrKill when the whole side is
insufficient. Reading 2: market TIF is ignored entirely, so a market FOK
behaves like market IOC (partial fill then ImmediateOrCancel). Scenario
`spec_fok_market.scn` asserts reading 1 and can be moved to
`tests/scenarios_pending/` if the spec owner picks reading 2.

**A2. auction_indicative imbalance when crossed.** SPEC.md:265-268: "(and
`crossed = false`, price/paired=0 when not crossed; imbalance is the unsigned
difference between total buy and sell leaves, with its surplus side)". It is
unclear whether the imbalance clause applies only when not crossed. Reading 1
(both implementations, and the header comment "at price if crossed; total side
difference otherwise"): crossed uses abs(I(p*)), not crossed uses total
difference. Reading 2: always total buy minus total sell leaves. Feeds ITCH
NOII. Not reachable from the scenario DSL. Recommend moving the imbalance
clause out of the parenthesis and stating both cases.

**A5. Out-of-range CommandType.** SPEC.md does not define it ("Anything not
specified here is a bug in this document"). Engine B aborts the process
(`src/exsim/matching_engine.cpp:953`); the oracle returns Applied with no events
(`tests/exsim/reference/reference_engine.hpp:116`). Not reachable from journals
(the reader rejects unknown tags) or the fuzzer, so the divergence is untested.
A gateway bug that forged a bad tag would crash the server instead of being
rejected. Recommend specifying either a no-op or a Rejected with InvalidField.

**E2. Event payloads written through references to inactive union members.**
`src/exsim/matching_engine.cpp:277` (and the other `emit_*` helpers at about
lines 296, 306, 320, 520, 653, 683, 724, 849) value-initialize `Event` (which
starts the lifetime of `accepted`) and then assign fields through
`CanceledEvent& canceled = event.canceled;`. Assigning through a reference
does not start the lifetime of another union member under [class.union], so
this is formally undefined behaviour; current compilers produce the intended
bytes and UBSan does not flag it. The oracle assigns the whole member
(`e.canceled = CanceledEvent{...}`), which is well defined. Recommend the
same pattern in engine B. Determinism is unaffected today.

**E1. All-instrument close scans all live orders once per instrument.**
`expire_day_orders` (`src/exsim/matching_engine.cpp:731`) walks the global
age list for each instrument, so `SetPhase{instrument=0, Closed}` costs
O(instruments x live orders) (64 x 1M = 64M visits at the default config).
Output is correct and ordered; this is a latency risk at session close only.

**D2. Section 10 table contradicts section 5 for no-op reductions.**
SPEC.md:276 says Cancel emits "`Canceled` or `Rejected`"; SPEC.md:173 says a
reduction with target >= leaves emits nothing. GATEWAYS.md:58 confirms "no
effect, no response", so section 5 is intended. Both implementations and
`partial_cancel.scn` / `spec_partial_cancel_reduction.scn` follow section 5.
Recommend adding "or nothing (reduction no-op)" to the table.

### Info

**E3. Engine B preflight is tighter than the oracle's.** Engine B checks
`live + 2` (NewOrder, Replace), `1` (Cancel), `live + 1` (MassCancel) and
`2*live + 2*instruments + 1` (SetPhase) at
`src/exsim/matching_engine.cpp:921-941`; the oracle requires the full
`required_event_capacity`. I re-derived each bound: every match or STP step
removes a resting order except the final aggressor-terminating step, so a
NewOrder emits at most Accepted + live + 1; uncross fills each exhaust an order
and expiry cancels each remove one. The bounds are sound and allowed by
section 12 ("or a proven tighter read-only bound"). Consequence: HandleResult
differs between the engines for undersized buffers, so differential tests must
only compare HandleResult with startup-sized buffers (they currently do).

**H2. Pre-existing scenarios assert subsets.** Of the 23 original files, most
omit `expect count` and assert one or two fields (for example
`stp_cancel_aggressor.scn`, `auction_distance.scn`, `halt_auction.scn`), so
missing or extra trailing events and wrong leg fields would pass. The new
spec scenarios cover the same rules exactly; the originals were left untouched
per ownership rules.

**H3. Scenario DSL blind spots.** It cannot assert `auction_indicative`,
`last_trade_price`, absence of an order (`expect order` only checks presence),
FIFO order inside a level (`level_orders`), `HandleResult`, or
`add_instrument` rejection (an invalid instrument line throws). Those rules
(R9, R68, R73, R75, R76) rely on C++ tests, several of which were written by
the engine author (`tests/exsim/engine/vector_*`). FIFO order was asserted
indirectly through which order the next aggressor hits.

**D1. Wrong oracle path.** SPEC.md:6 names `tests/exsim/reference_engine.hpp`;
the file is `tests/exsim/reference/reference_engine.hpp`.

**D3. Garbled clause and incomplete list.** SPEC.md:335-336 attaches
"including the verbatim ignored market price" to the `leaves_qty` clause; it
belongs to "Canceled echoes stored ... price". SPEC.md:42-45 says
`add_instrument` returns 0 "unless" a list that omits the section 12
conditions (reference price on grid, valid phase, max_order_qty >= lot_size).
Both implementations apply the full `valid_instrument_config`.

## Code reading notes with no finding

* FlatHashMap uses backward-shift deletion and key 0 as the empty sentinel;
  client keys `(session << 48) | token` and OrderIds are never 0, and capacity
  is reserved for `max_orders`, so the `std::abort()` on insert failure
  (`src/exsim/matching_engine.cpp:230-231, 622`) is unreachable.
* Mass cancel and expiry iterate an age list whose append order equals
  OrderId order (ids are assigned and registered within one command, and a
  priority-loss replace re-registers at the tail with the newest id), so
  ascending OrderId order holds without sorting.
* No unordered container is iterated to produce output; `incoming_order` and
  `OrderInfo` are zeroed before use; aggregate sums are uint64 throughout;
  reference distance and imbalance use unsigned subtraction of smaller from
  larger in both implementations.
* Engine B's single-pass uncross candidate scan
  (`compute_uncross_price`) was checked by hand to compute B(p) and S(p) over
  exactly the resting prices in [ba, bb]; the oracle recomputes per candidate.
