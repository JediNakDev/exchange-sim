# Foundation review, recovered 2026-11-02

Status: findings awaiting implementation and integration verification. This report is evidence from the earlier independent GPT review, not a statement that fixes have landed. The reviewed commits are listed below. Read the product decision in PRODUCT.md alongside these findings.

Reviewed `35da879` and `4b03a5d`. No repository files changed. Builds and probes ran in a scratch copy. The four existing hash-map tests pass; targeted probes reproduced the crashes, tracker corruption, and compilation failure below.

1. **High - [flat_hash_map.hpp:53](/Volumes/PortableSSD/Documents/programming/lob/include/lob/flat_hash_map.hpp:53): `find(0)` returns an empty slot as a valid entry.**
   The equality check precedes the empty-sentinel check. Consequently, `contains(0)` returns true, and `OrderBook::cancel_order(0)` dereferences a null `Order*`. ASan/UBSan reproduced the crash at `src/order_book.cpp:390`. After deletion, the returned value can instead be stale.
   **Fix:** Return `nullptr` immediately for `key == 0`; test zero lookup before insertion, after insertion, and after erase/clear.

2. **High - [GATEWAYS.md:55](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:55): OUCH replace and cancel quantities are wrong.**
   U/X Shares denote total intended quantity. At cum=100, U(500) must leave 400, not 500; X(450) must leave 350.
   **Replacement:** “U.qty=shares; X targets `max(shares−cum,0)` leaves, never increasing exposure.” [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

3. **High - [GATEWAYS.md:81](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:81): FIX application messages execute before missing predecessors.**
   “Higher → process it” violates ordered application processing. If sequence 10 is a cancel and sequence 11 a replacement/new order, receiving 11 first changes trading outcomes before recovery.
   **Replacement:** “For an application message above expected sequence, buffer or discard it and request recovery; do not submit its command until preceding sequence numbers are received or validly gap-filled. Suppress already-processed duplicates.” Define session-message exceptions separately. [FIX session specification](https://www.fixtrading.org/wp-content/uploads/download-manager-files/FIX_Transport_1.1.pdf)

4. **High - [exsim_bench/main.cpp:144](/Volumes/PortableSSD/Documents/programming/lob/tools/exsim_bench/main.cpp:144): the live-order tracker corrupts itself after priority-losing replaces.**
   The order is already tracked. `Replaced` may remove/add its token, then `Rested` adds it again. With an unchanged token, `Rested` directly duplicates the existing entry. Removing the order later removes only one copy, leaving an unindexed ghost. The generator can subsequently dereference an empty `find_order()` result at line 234.
   **Reproduced:** One live order → replace plus rest → tracker size 2 → cancel → tracker size 1.
   **Fix:** On priority loss, remove the old token and wait for `Rested` before adding the replacement. On priority retention, update only the token mapping. Make duplicate additions fail loudly.

5. **High - [command.hpp:4](/Volumes/PortableSSD/Documents/programming/lob/include/exsim/command.hpp:4): trivial copyability does not make raw journals deterministic or portable.**
   Commands/events contain padding and differently sized union alternatives. Constructing `Command(CancelOrder{...})` does not initialize the entire union storage. Two equivalent commands or events can therefore have different byte representations, contradicting “written verbatim” and “bit-identical event stream.” Raw records also depend on ABI and endianness.
   **Fix:** Define a versioned, explicit field encoding with fixed byte order. Compare active event fields in differential tests. Replace the guarantee with: “Replay produces identical ordered event field values; journal representation uses the canonical serializer.”

6. **High - [event.hpp:144](/Volumes/PortableSSD/Documents/programming/lob/include/exsim/event.hpp:144): output buffering invalidates the unconditional no-allocation guarantee.**
   `EventBuffer::push()` grows a vector. A default buffer holds 4,096 events, while a legal mass cancel can emit over a million. An allocation failure can also occur after the engine has already changed state and emitted only part of a command’s events.
   **Fix:** Specify a caller-owned capacity requirement and expose a maximum-event/reservation API. Check available output capacity before mutation. Proposed contract: “`handle()` requires sufficient remaining output capacity for the complete command and never grows the buffer.”

7. **High - [GATEWAYS.md:65](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:65): OUCH dead acknowledgements contradict subsequent reports.**
   A(D) permits no later execution/cancel reports. A fully filled IOC here produces A(D), E. Emit A(L) before subsequent reports; reserve D for atomic cancellation. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

8. **High - [GATEWAYS.md:55](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:55), [SPEC.md:195](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:195): the API cannot express OUCH replacement priority.**
   OUCH U always loses priority; same-price reductions here retain it. Add `force_priority_loss=true` for U; keep X reductions in place so replaced orders cannot overtake untouched peers. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

9. **High - [GATEWAYS.md:43](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:43): OUCH retransmission can create another order after completion.**
   OUCH tokens are day-unique; repeated submissions are ignored. Engine uniqueness covers only live orders. Retain a per-day used-token table and silently suppress duplicates before engine submission. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

10. **High - [SPEC.md:226](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:226): legal phase transitions violate the uncrossed-book invariant.**
    In PreOpen, accept buy 110 and sell 100, then transition to Halted. Section 8 neither uncrosses nor rejects the transition, but section 11 forbids crossed Halted books. Implementations may preserve, reject, or execute this book.
    **Replacement:** “Halted preserves the book without matching and may contain crossed orders inherited from a call phase. The uncrossed-book invariant applies to Continuous and Closed. Entering either phase uncrosses a crossed book, regardless of the previous phase.”

11. **High - [event.hpp:120](/Volumes/PortableSSD/Documents/programming/lob/include/exsim/event.hpp:120), [GATEWAYS.md:123](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:123): ITCH Cross Trade lacks its required match identifier.**
    `AuctionSummaryEvent` provides price and volume but no match number. Using a constituent fill’s ID, allocating another ID, and maintaining a publisher-only counter produce incompatible feeds. ITCH Q requires a day-unique match number.
    **Fix:** Add `AuctionSummary.match_id` and specify its allocation relative to constituent fills and the global counter. [ITCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)

12. **High - [GATEWAYS.md:120](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:120): priority-losing replaces can add the replacement twice to ITCH.**
    A non-crossing reprice produces `Replaced`, then `Rested`. Row 126 emits U, which creates the new reference; row 120 then emits A for that same reference.
    **Replacement:** “When a Replaced/Rested pair is represented by U, consume the corresponding Rested event without emitting A. Otherwise emit D for the old reference and A for the eventual remainder. Emit exactly one creation message per new reference.”

13. **High - [SPEC.md:18](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:18), [GATEWAYS.md:111](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:111): valid engine values need not fit the wire formats.**
    The engine permits positive `int64` prices beyond unsigned 32-bit wire prices and opaque `uint64` timestamps beyond ITCH’s six-byte, midnight-relative timestamp. Copying them “as-is” truncates valid events.
    **Fix:** Define supported instrument price limits and a gateway timestamp conversion with an explicit trading-day origin; reject unrepresentable configuration before accepting orders. [ITCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)

14. **High - [SPEC.md:42](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:42): instrument/configuration validation omits values needed for defined execution.**
    `initial_phase = static_cast<Phase>(255)` and `reference_price = INT64_MIN` satisfy the listed checks. The former has no phase semantics; the latter can overflow auction distance calculations. `max_instruments > 65535` also permits ID wrap into reserved zero.
    **Replacement:** “Reject invalid initial phases, reference prices outside the instrument’s valid tick grid, instrument counts exceeding `InstrumentId` capacity, and configurations whose ladder size or allocation-size arithmetic is unrepresentable.”

15. **High - [SPEC.md:74](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:74), [SPEC.md:221](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:221): event-field defaults are insufficiently normative for identical streams.**
    New-order rejection examples omit `session`; SetPhase rejection uses `...`, leaving `instrument`, IDs, and tokens unspecified. Section 10 describes instrument through an “order involved,” although phase/auction events need no order. Market-order cancellation prices are also unstated.
    **Replacement:** Define every active payload field per event: rejected order-command session echoes the command; SetPhase session/tokens/order ID are zero; invalid-instrument rejection echoes the requested instrument; phase/auction events identify the processed instrument; cancellation price is the order’s stored price, including the verbatim market-order price.

16. **High, pre-existing - [sharded_engine.cpp:99](/Volumes/PortableSSD/Documents/programming/lob/src/sharded_engine.cpp:99), [sharded_engine.cpp:158](/Volumes/PortableSSD/Documents/programming/lob/src/sharded_engine.cpp:158): different symbols trade against each other.**
    Symbols route by modulo shard count, but each shard owns one `OrderBook`, and the worker ignores `op.symbol`. A buy for symbol 1 and sell for symbol 3 match when there are two shards.
    **Fix:** Keep separate books per instrument within each shard, or route commands into the new multi-instrument engine. Add a same-shard, different-symbol isolation test before treating this as simulator infrastructure.

17. **Medium - [test.hpp:89](/Volumes/PortableSSD/Documents/programming/lob/tests/framework/test.hpp:89): comparison macros can retain dangling references.**
    `CHECK_EQ(engine.find_order(...)->leaves_qty, 5)` binds `testfw_a_` through a temporary optional’s `operator->`. The optional dies before the comparison. An equivalent probe produced ASan stack-use-after-scope.
    **Fix:** Perform capture, comparison, and diagnostics inside a helper-function invocation, keeping operand temporaries alive through the full expression; alternatively capture scalar values by value.

18. **Medium - [flat_hash_map.hpp:35](/Volumes/PortableSSD/Documents/programming/lob/include/lob/flat_hash_map.hpp:35): defaulted moves leave a null table with nonempty metadata.**
    After moving, the source retains `mask_`, `size_`, and `max_entries_` while `slots_` becomes null. Calling `find`, `clear`, or inserting below the stale capacity dereferences null; even `reserve(old_capacity)` returns without repairing it. A sanitizer probe confirmed this.
    **Fix:** Implement moves that reset the source to the default empty state, or delete moves.

19. **Medium - [flat_hash_map.hpp:44](/Volumes/PortableSSD/Documents/programming/lob/include/lob/flat_hash_map.hpp:44): reserve arithmetic can silently underallocate or loop forever.**
    On this 64-bit host, `reserve(size_t{1} << 63)` returned a map with capacity for only eight entries because `max_entries * 2` wrapped to zero. Other oversized requests can shift `capacity` to zero and loop indefinitely.
    **Fix:** Check multiplication, power-of-two rounding, and `sizeof(Slot) * capacity` before allocation. Reject impossible sizes deterministically.

20. **Medium - [flat_hash_map.hpp:137](/Volumes/PortableSSD/Documents/programming/lob/include/lob/flat_hash_map.hpp:137): const iteration exposes mutable keys and values.**
    `unique_ptr<T[]>::operator[] const` returns `T&`. A visitor accepting references can set a key to zero through a const map, losing an entry without changing `size_`. Reproduced: size remains 1 while the sole key becomes unfindable.
    **Fix:** Bind each visited slot as `const Slot&`. If mutable value iteration is needed, provide a separate overload that still exposes keys as const.

21. **Medium - [exsim_bench/main.cpp:42](/Volumes/PortableSSD/Documents/programming/lob/tools/exsim_bench/main.cpp:42): allocation counting misses over-aligned allocation.**
    An `alignas(64)` hot structure allocated through aligned `operator new` bypasses these overrides, allowing a false “zero allocations” result. Direct `malloc`/`calloc` also bypass the guard.
    **Fix:** Cover aligned scalar/array allocation and matching deallocation forms. State whether the guarantee covers C allocation, and add a guard self-test that deliberately allocates an over-aligned object.

22. **Medium - [GATEWAYS.md:139](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:139): “engine depth after every packet” is impossible for multi-packet commands.**
    A mass cancel of thousands of orders exceeds one datagram. After the first packet, the subscriber correctly retains orders whose deletes are in later packets, while the engine has completed the whole command.
    **Replacement:** “Compare final engine depth after consuming every packet for the command batch; alternatively compare each packet against engine state reconstructed from the corresponding event prefix.”

23. **Medium - [GATEWAYS.md:130](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:130), [matching_engine.hpp:55](/Volumes/PortableSSD/Documents/programming/lob/include/exsim/matching_engine.hpp:55): NOII mapping is not defined by the available query.**
    ITCH I distinguishes near, far, and current-reference prices. `AuctionIndicative` exposes only one price and returns all zeros for noncrossed books, losing information about one-sided interest.
    **Fix:** Define a documented simplified NOII model and every field’s calculation, or extend the query with the required auction/reference statistics. [ITCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)

24. **Medium - [GATEWAYS.md:54](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:54): unsupported OUCH instructions silently change meaning.**
    TIF=1 means one second, but maps to Day. Add deterministic expiry or reject unsupported TIFs; explicitly reject unsupported Display, Minimum Quantity, and Cross Type instructions. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

25. **Medium - [GATEWAYS.md:68](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:68): auction liquidity flags are incorrect.**
    C denotes closing-cross liquidity; opening fills need O and halt-cross fills H. Map using the instrument’s pre-transition phase instead of labeling every auction C. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

26. **Medium - [GATEWAYS.md:66](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:66): plain post-only rejection uses a midpoint-specific reason.**
    W means invalid midpoint-post-only price. Use a documented general fallback such as O for the simulator’s plain `PostOnlyWouldCross` rejection. [OUCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf)

27. **Medium - [GATEWAYS.md:85](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:85): malformed FIX framing is conflated with semantic rejection.**
    Bad checksum/body length should be treated as garbled input, without advancing the expected inbound sequence. Sending Reject and consuming the sequence can permanently discard an order instead of recovering it.
    **Replacement:** “Discard garbled messages without sequence advancement; recover the gap on the next valid message. Send session Reject for parseable messages with applicable semantic errors.” [FIX session specification](https://www.fixtrading.org/wp-content/uploads/download-manager-files/FIX_Transport_1.1.pdf)

28. **Medium - [GATEWAYS.md:77](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:77): inbound FIX recovery cannot complete from this contract.**
    Outbound gap fills are specified, but inbound SequenceReset/GapFill handling is absent. A peer recovering missing heartbeats with a gap fill cannot advance this session’s expected sequence; the literal implementation repeatedly requests the same gap.
    **Fix:** Specify validated `NewSeqNo` advancement, duplicate-gap-fill suppression, and rejection of attempts to lower the expected sequence. Also define matching TestRequest/Heartbeat handling. [FIX session specification](https://www.fixtrading.org/wp-content/uploads/download-manager-files/FIX_Transport_1.1.pdf)

29. **Medium - [GATEWAYS.md:68](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:68), [GATEWAYS.md:122](/Volumes/PortableSSD/Documents/programming/lob/docs/GATEWAYS.md:122): one Fill can produce two messages without a defined order.**
    With STP disabled, both legs can belong to the same session. Buy-first and sell-first implementations then generate different OUCH/FIX byte streams and ExecID assignments. Auction ITCH C messages have the same ambiguity.
    **Replacement:** “For each Fill, emit buy-leg output before sell-leg output, including when both target the same session. Assign outbound sequence numbers and ExecIDs in that order.”

30. **Medium - [SPEC.md:108](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:108), [SPEC.md:242](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:242): aggregate arithmetic widths are unspecified.**
    Individual quantities fit `uint32`, but FOK availability and auction totals need not. With max quantity `UINT32_MAX`, adding two resting quantities can wrap a `Quantity` accumulator before the FOK comparison. Signed auction imbalance introduces another overflow boundary.
    **Replacement:** “Availability and side-volume sums use checked unsigned 64-bit arithmetic; imbalance magnitude uses subtraction of smaller from larger. Reject configurations exceeding supported aggregate bounds. Price-distance calculations must not overflow.”

31. **Medium - [SPEC.md:279](/Volumes/PortableSSD/Documents/programming/lob/docs/SPEC.md:279): conservation is undefined across amendments.**
    Accept 500, fill 100, replace total quantity with 300, then cancel 200. Initial quantity, current quantity, amendment reductions, and terminal cancellation cannot all use the single word `qty` in the stated equation. This invites false invariant failures.
    **Replacement:** “For live orders, current total quantity equals cumulative fills plus leaves. Across a replacement chain: executed quantity + current leaves + quantities in Canceled events = initial quantity + Σ(actual replacement total − previous total).”

32. **Medium - [Makefile:67](/Volumes/PortableSSD/Documents/programming/lob/Makefile:67): incremental builds do not track compiler or flag changes.**
    After a build, requesting the same target with changed `EXTRA_CXXFLAGS` reports “up to date.” Changing `CXX`, feature macros, or sanitizer flags within one mode can therefore execute stale or mixed-configuration objects.
    **Fix:** Add per-mode compile/link configuration stamps as prerequisites, updating their timestamps only when their contents change. Track the source list too, so removing a translation unit forces relinking.

33. **Medium - [matching_engine.hpp:125](/Volumes/PortableSSD/Documents/programming/lob/include/exsim/matching_engine.hpp:125): `for_each_order` rejects ordinary free-function callbacks.**
    `engine.for_each_order(fn)` deduces a function reference, then attempts a forbidden `void*`-to-function-pointer cast. Compilation failure reproduced.
    **Fix:** Pass the address of a local object wrapper capturing `f` to `visit_orders`, then cast back to that wrapper type. This also preserves constness without casting away const.

34. **Medium - [exsim_bench/main.cpp:266](/Volumes/PortableSSD/Documents/programming/lob/tools/exsim_bench/main.cpp:266): larger runs silently benchmark capacity rejection.**
    `passive_add` starts with 2,000 orders and has fixed `max_orders = 2^21`. Beyond 2,095,152 timed additions, commands reject instead of adding. A larger `--ops` can thus appear faster by measuring an increasing proportion of rejects.
    **Fix:** Size capacity from setup plus requested operations, or reject unsupported operation counts. Assert expected acceptance/rejection and fill counts during generation and replay.

35. **Medium - [exsim_bench/main.cpp:375](/Volumes/PortableSSD/Documents/programming/lob/tools/exsim_bench/main.cpp:375): percentiles include unmeasured timer overhead and uncontrolled scheduling.**
    Per-command samples include the counter-read boundary cost, while throughput includes `clear()` and event counting that latency excludes. No empty-boundary baseline or affinity information is reported, making small claimed engine improvements difficult to distinguish from measurement effects.
    **Fix:** Report timer-boundary overhead and sample quantization, offer CPU affinity, and explicitly distinguish handle-only latency from complete-loop mean time. Do not silently subtract overhead from quantized samples.

36. **Medium - [src/order_book.cpp:12](/Volumes/PortableSSD/Documents/programming/lob/src/order_book.cpp:12), [src/order_book.cpp:298](/Volumes/PortableSSD/Documents/programming/lob/src/order_book.cpp:298): retaining the fill vector does not bound its allocation.**
    A sweep through 1,025 one-share resting orders exceeds the 1,024 reserved fills and reallocates, including in `LOB_DETERMINISTIC_POOL` mode. The span change fixes repeated allocation after ordinary matches, not the claimed allocation-free boundary.
    **Fix:** Reserve against the supported maximum number of resting-order matches, or use a bounded caller-owned output buffer with explicit exhaustion behavior.

37. **Medium, pre-existing - [sharded_engine.cpp:159](/Volumes/PortableSSD/Documents/programming/lob/src/sharded_engine.cpp:159): fully executed orders leak client-ID mappings.**
    New resting orders enter `client_to_book_order`, but resting orders exhausted by subsequent fills never leave it. Repeated buy/sell pairs grow this map indefinitely despite a nearly empty book. The newly explicit growth setting allows recurring allocations.
    **Fix:** Remove completed resting-order mappings while consuming fills, using a reverse lookup or stored client handle. Size capacity against live orders, not lifetime order submissions.

38. **Low - [exsim_bench/main.cpp:384](/Volumes/PortableSSD/Documents/programming/lob/tools/exsim_bench/main.cpp:384): `--ops 0` reaches invalid percentile indexing.**
    The tick vector is empty; `ticks.size() - 1` underflows, and both `ticks[i]` and `ticks.back()` are invalid. An unknown workload name also exits successfully without running anything.
    **Fix:** Validate a positive operation count and recognized workload name before generation; fail explicitly if a workload produces zero timed commands.
