# Selected-fill identity repair

Independent re-check `7a5d995`, integrated as `abdf8a5`, found FER-01: after selecting two fills of one stationary order, the second row/event could be selected while the explanation still showed the first fill. The read had changed from an order-wide collection to a single fill, but its resource key still contained only run, order and progress.

The parent added the exact selected event position to the resource key. Consumer precedence additionally requires both event position and order ID to match the current selection. Selection changes therefore clear keyed data and abort the previous cooperative search. Current table evidence can be used while the selected search loads, but another fill's cached evidence cannot take precedence. No C++ behavior, API shape, accounting calculation or persisted data changed.

## Reproduction and focused checks

- The independent Chromium reproducer is preserved unchanged in `web/review-tests/frontend-execution.browser.ts`. Before the fix, it failed because `Fill 7` was absent while `Fill 4` remained. Exact parent reproduction: `/tmp/exsim-fer01-before.log`.
- The rebuilt standalone artifact passes that same reproducer: **1/1**, 896 ms, `/tmp/exsim-fer01-after.log`.
- Three ordinary browser regressions now cover same-order sibling selection after the first response commits, switching while that response is pending (including observed fetch cancellation), and selecting/retaining the second fill across cursor pages: **3/3**, 2.2 s, `/tmp/exsim-fer01-browser.log`. These are synthetic presentation/identity fixtures, not another matching/accounting implementation.
- Six independent presentation/helper probes were copied into normal CI coverage, with currency/fee fixture scales matching their fields and an extra negative-fee assertion. The original review probe file remains unchanged. Ordinary frontend tests: **82/82**, 13,046 assertions, 2.84 s, `/tmp/exsim-fer01-unit.log`.
- Typecheck, warning-free lint and standalone build passed: `/tmp/exsim-fer01-{types,lint,build}.log`. Updated build ID: `slpULgNACN27FJgew-XPE`.

The existing thirteen browser cases and native API release/ASan/Linux64 gates are prior checkpoint evidence, not claimed as rerun for this readonly frontend fix. CI will now discover sixteen ordinary browser cases. The selected-order identity fix was verified using the four targeted browser cases above rather than repeating unrelated engine/protocol suites.

A new independent re-check of this repair is still required. The earlier `production-20261107-candidate-7ca1014` contains FER-01 and must not be promoted; its inventory/readiness checks establish only its old bytes and startup behavior. Rebuild a new candidate after review, preserving source/build identity. The installed production release and private data remain untouched. Integrated API review is still pending separately.
