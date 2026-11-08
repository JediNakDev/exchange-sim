# Bot implementation verification

Date: 2026-11-02
Checkout: /Users/jedi/.t3/worktrees/lob/t3-b900e409
Branch: t3/simulated-liquidity-bots
Compiler: Apple clang 21.0.0, arm64-apple-darwin27.0.0
Standard/warnings: C++20, -Wall -Wextra -Wpedantic -Wshadow

Commands and exact relative logs:

- `make test FILTER=sim_bots`: focused-release.log, 21/21 passed.
- `make test MODE=debug FILTER=sim_bots`: focused-debug.log, 21/21 passed.
- `make test`: release.log, 66/66 passed (45 existing + 21 new).
- `make test MODE=debug`: debug.log, 66/66 passed with ASan + UBSan.
- Log scan: no compiler warnings, errors, failed tests, ASan errors, or UBSan runtime errors.

The allocation guard replaces ordinary, array, sized-delete, aligned, and
nothrow C++ allocation functions and forwards to malloc/free (posix_memalign
for aligned allocation). Its thread-local counter is enabled only around the
active bot test window, after bot/context setup. The window includes 1000
active ticks, matching/fills, partial replenishment/replaces, taker IOC and DAY
arrivals/expiry, halt/resume, and stop/cancel. Observed allocations: zero.
The test context pre-reserves command recording and uses fixed order/event arrays.
This proves C++ new-family allocation absence for the tested paths, not allocation
absence in a future server context or unrelated C library calls.

Focused coverage: exact first ladder, lot rounding, idle convergence, exact
one-tick replacement counts, full fills, partial replenishment TOTAL quantities,
phase transitions/call phases, stop/start, STP/EOD cancellation, rejection
recovery, delayed callbacks and delayed rejected moves, price bands, bounded
capacity, halt quote retention, total-quantity budget rollover, deterministic
maker/taker streams, fixed-seed exponential rate bounds, passive cancellation,
transactional config, registry/session limits, table lookup/ID progression,
same-price priority-preserving decreases, exact large-jump reuse counts,
UnknownOrder recovery, foreign instrument filtering, and inventory skew.

The fake exchange covers this component's consumed contracts, uses per-session
order lookup and price/FIFO matching, emits Accepted/Fill/Rested/IOC-cancel and
replace/STP events, and supports inline or deferred delivery. It is deliberately
not an auction implementation or replacement for the separately owned oracle.

Integration actions: wire all-event fanout, timer invocation, exclusive session
assignment, option-parser hooks, and immutable instrument lookup. Revalidate
against the canonical repaired API and production engine when available. See
include/exsim/sim/bots/README.md for public API, keys, model choices, timestamp
contract, capacity behavior, and the half-spread wording ambiguity.
No changes were made to shared headers, normative docs, Makefile, or framework.
No production MatchingEngine link was required. No subagents were used.
