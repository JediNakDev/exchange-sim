# Independent selected-fill repair review, 2026-11-07

**Verdict: FER-01 is repaired in this scope. No remaining wrong explanation or stale-selection defect found.** This is fresh independent clearance of the selected-fill frontend repair, not backend, release or deployment clearance. The previous independent repair task hit a provider limit and supplies no clearance.

Reviewed dependent parent `8ee86f1e2ee9fbe10d916b2bcf1f7e60184412de`, including repair `75d9960e0aff7475ffd3cb64628500f7cd1f4b3d`. There is no `web/` diff between those commits. Reviewer: GPT-6.1 Sol/xhigh, working alone in harness checkout `/Users/jedi/.t3/worktrees/lob/t3-c95c391d`, branch `t3/selected-fill-identity-review`. Task ID: `exsim-20261107-fer01-fresh-review-8ee86f1`. Full harness thread reference: `thread:delegated-task:command%3Amcp%3Ac5871c80-7318-4575-b3f9-3687f8548075%3Adelegate-task%3Aexsim-20261107-fer01-fresh-review-8ee86f1`.

Read AGENTS, WORK_PLAN, PRODUCT, quality/FILL_IDENTITY_REPAIR_2026-11-07 and reviews/FRONTEND_EXECUTION_RECHECK_2026-11-07. Inspected the inspector, resource/cursor hooks, selected-fill search, typed client and relevant tests/configuration. No production source, backend, dependency or original test changes.

## Source assessment

- `web/src/components/research/run-inspect-view.tsx:419`: the selected resource includes run ID, order ID, exact event-position string and processed-event count. Same-order siblings now have different resource identities even on stationary runs.
- `:424`: refreshed evidence has precedence only when both order and event match the selected fill. The fallback searches the current table page by both fields; it does not use the saved selection object's stale ledger. Off-page selection can use the separate selected resource across cursor changes.
- `web/src/hooks/use-api-resource.ts:38`: key changes immediately hide previous-key data/failures. Effect cleanup suppresses both late success and failure, aborts cooperative transport and stops polling. Cancellation is not the only protection against an uncooperative late response.
- `web/src/lib/api/research-workflow.ts:67`: the search forwards AbortSignal, checks it before each page, matches order plus exact event, stops on a match/passed position, rejects missing evidence and bounds cursor cycles. It retains no unrelated-page collection. Finding a late match can still take many pages.
- Inspector `:376` keys the inspection component to run ID, resetting selection, event focus and cursor history on remount. Selecting an own order clears fill selection. Progress changes invalidate selected evidence, table pages and lifecycle independently of the cursor.

## Fresh compiled-renderer evidence

Built this checkout from a frozen Bun installation using `bun run build`, then `bun scripts/prepare-browser-assets.ts`. Actual Next standalone React UI in Chromium, one worker, loopback `127.0.0.1:3762`. All research API requests are intercepted by private in-process synthetic fixtures; the unused server upstream is loopback port 9. No native API was started and ports 3760/7600, real data and installed services were untouched. Playwright stopped its owned server after completion; no listener remained on 3762.

Final command, from `web/`:

```sh
bunx playwright test --config review-tests/fill-identity.playwright.config.ts
```

**9/9 passed in 9.1 seconds**, comprising the following exact cases. These are nine selected cases, not a rerun of the entire ordinary browser suite.

| Cases | Count | Evidence |
| --- | --- | --- |
| Original `FER-01: a second fill of the same order replaces the selected explanation` | 1/1 | Reviewer assertions unchanged; second heading replaces first and a second selected read occurs. |
| Original `sibling fills keep distinct explanations: stationary`, `pending`, `off-page` | 3/3 | Existing file unchanged; committed sibling, observed browser fetch abort, retention across cursor pages. |
| `late uncancelled sibling response cannot replace ledger, markout or related-event links` | 1/1 | Test transport deliberately ignores abort. Second selected response supplies ledger 222, markout +17.25 and links #90/#93; releasing A afterward cannot restore A. |
| `run route change resets selection and cursor and rejects a late colliding run result` | 1/1 | Real Next links through summary/experiment to another run, same document. Page resets to 1 with no selection; B reuses A's order/event/match IDs. B ledger 333 survives A's late result. |
| `selecting an own order clears fill evidence and aborts its pending search` | 1/1 | Selection becomes order-only, pending browser fetch aborts, releasing its old route cannot restore a fill or report an obsolete refresh error. |
| `off-page failed sibling search retries exact identity through cursors and stops at the match` | 1/1 | Selected 503 remains explicit. Leaving its table page shows neither sibling's explanation. Retry rejects a wrong-order decoy at the same event, follows the selected cursor, displays ledger 444, and never reads the cursor after its match. |
| `run progress refresh cancels the prior search and reloads the same selected identity` | 1/1 | Paused-to-completed fixture increments progress. Prior selected fetch aborts; exactly one new selected read replaces table ledger 200 with selected ledger 666 and retains row selection. |

Browser log: `/tmp/exsim-fer01-c95c391d-browser.log`. All nine successful traces: `/tmp/exsim-fer01-c95c391d-browser-results/<case-directory>/trace.zip`. Directory names are emitted by Playwright and can be enumerated under that exact root.

Additional focused checks: **16/16**, 52 assertions, 439 ms, from `bun test ./tests/research-workflow.test.ts ./review-tests/frontend-execution.review.test.tsx`. Of these, six specifically cover selected search early-stop, pre-abort, missing evidence, exact positions above 2^53, in-flight signal propagation and cursor cycles; the other ten are adjacent existing workflow/presentation checks. Log `/tmp/exsim-fer01-c95c391d-search.log`. Full TypeScript check (`bunx tsc --noEmit --incremental false`) and lint of the two new review files passed: `/tmp/exsim-fer01-c95c391d-{types,lint}.log`.

An initial added navigation probe incorrectly assumed `history.pushState` changes Next's `useParams`; the URL changed but the route params did not. Replaced that harness step with real Next links and verified unchanged `performance.timeOrigin`. Initial log `/tmp/exsim-fer01-c95c391d-browser-initial.log` contains 7 passes and this harness failure, not a product finding. An initial fetch-wrapper typing error omitted Bun's extra fetch properties; the final wrapper preserves them. Initial type log `/tmp/exsim-fer01-c95c391d-types-initial.log`. The final nine-case browser and final type logs above are authoritative.

## Build identity and limits

- Reviewed frontend tree: `46773eddc4407b98f2d7f371c5f97a1618397a91`. Fresh build ID: `l0GERnR0u-txND9Nz1FSh`; Bun 1.4.4, Node v26.12.1, Next 16.4.4, Playwright 1.65.0. Browser version is recorded in `/tmp/exsim-fer01-c95c391d-runtime.log`.
- Inspector chunk `.next/static/chunks/2ehw3_09a2m48.js` SHA-256: `e62a524dbf6a3516b4cb8803f3756b4c30c8a5f51160d53db88d66df76bb6d46`.
- Original reviewer file SHA-256: `820d45a8163efd1be7a45170ce70aed37979e0b4550c5d5998f6198b766ca737`; existing three-case file SHA-256: `3da1618e780a89ba41b0583854a0fd08b59e447713d7a03ae1f172746ef6eed6`. Git confirms both files unchanged from parent.
- Full receipt, including standalone server/dependency hashes and checkout/task identity: `/tmp/exsim-fer01-c95c391d-provenance.json`. Frozen-install/build logs: `/tmp/exsim-fer01-c95c391d-{install,build}.log`.

The synthetic fixtures establish UI identity and async behavior, not matching/accounting correctness. Cancellation evidence stops at browser fetch; this review does not establish that a disconnected Next proxy cancels native work. The cross-run browser probe uses actual links through intermediate routes; direct run-param remount protection is also source-inspected. No accessibility, native API, whole C++/browser suite, Docker, benchmark, production-data, release packaging or deployment gate was run. Only new review tests/configuration, this report and WORK_PLAN are committed.

Next: parent may integrate this narrow clearance and review probes, then apply the separate backend/Linux/release gates. The old candidate containing FER-01 remains unsuitable; this independent frontend test build is not a promoted or packaged release.
