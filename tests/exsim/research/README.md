# Position accounting

`PositionLedger` records authoritative fills for one initially-flat position in
one instrument, with FIFO cost basis. One quantity unit is one share; the
instrument multiplier is one. All cash, price, fee and P&L units are 1/10000 of
the quote currency. Signed per-share fees support rebates. Fees are charged on
every fill, including opening fills; gross realized P&L excludes all fees.

`equity = cash + position * mark`; `net_pnl = equity - initial_cash`;
`net_pnl = gross_realized_pnl + unrealized_pnl - fees`.

The caller supplies a positive mark from the appropriate observation time.
Invalid marks or arithmetic overflow return no valuation. There is no invented
zero mark and no internal clock. The caller must deliver each authoritative
fill exactly once; replayed gateway acknowledgments must not be counted again.
This component does not own risk checks, session routing, market-data marks,
strategy scheduling, or instrument-specific fee selection.

Open lots are held in a startup-allocated ring. Consecutive same-side fills at
the same price coalesce. Fills preflight lot capacity and checked arithmetic
before committing. All failure statuses leave both the ledger and lot ring
unchanged. The caller must fail a research run if an already-executed fill
cannot be recorded. It must not discard the fill or call this an exchange
rejection. Position magnitudes and arithmetic intermediates must fit int64.

Run `make -j4 test FILTER=position_ledger` and the debug equivalent for fixed
examples and a 20000-fill accounting-identity check. The allocation guarantee
for the ledger (capacity failure, crossing through flat, valuation, ring reuse)
and for the run loop of every compiled strategy is the `research_allocations`
suite: `make -j4 test FILTER=research_allocations`. It uses the shared
`testfw::AllocationScope` and is skipped where allocation counting is
unavailable (sanitizer builds), so the release build enforces it. Direct C
allocation functions are not intercepted.
