# Research and simulation refactor, 2026-11-08

Opus 5.5/high started this workstream in `/Users/jedi/.t3/worktrees/lob/t3-f5cfef64`, branch `t3/audit-sim-research-api`, from `67bb05b`. Its continuation failed with a Claude API rate limit before tests/report/commit. Parent preserved its work and finished implementation review and integration. No stand-in query build counts as integrated validation.

## Changes

- Measure FIFO queue evidence with the production engine's new read-only level visitor. Removes an OrderId scratch allocation sized to maximum orders and the per-level-order global lookup. The traversal now visits each order at that level once. Preserves quantities ahead, total level quantity and FIFO semantics after partial fills and priority-preserving/losing replacements.
- Keep a dense markout-row index by fill and horizon while importing records and rebuilding restart indexes. Fills-page queries use direct indexing rather than reconstructing a hash table of all markouts on every request. Duplicate pairs retain the last row as before; absent or out-of-range pairs cannot index beyond storage. Index allocations occur in the API control plane under the existing lock, outside the simulation hot path. This trades bounded-by-artifacts resident memory for avoiding repeated query allocations and scans.
- Write existing JSON buffers directly to artifact streams; reuse the execution-decision JSON writer across rows. No schema or field order changes.

Parent rejected a draft change that fsynced every atomic artifact replacement and replaced the explicitly durable idempotency writer. Repeated progress updates should not gain an unconditional disk barrier during this refactor. Existing artifact atomic replacement and idempotency durability semantics are preserved.

## Review and tests

Parent reviewed modified source, the scheduler's fixed-capacity heap and FIFO tie-break, checked FIFO ledger updates, recorder/strategy boundaries and restart-index construction. No changes were needed to scheduler ordering, ledger accounting, strategies/bots or canonical serialization. These components are also exercised by their existing focused correctness, failure-path and allocation tests. This is not a new exhaustive line-by-line audit of every untouched native API function.

New regressions cover exact FIFO evidence after partial fills and replacements with zero hot-path allocations, a full simulation queue failing explicitly without allocation, and paginated fill markouts reconciling to independent summary counts after a run and after restart. Parent corrected the markout test fixture to include its requested seed 4 in the experiment seed set; the original fixture was rejected before executing a run.

Final commands, results and coverage limits are in `EXCHANGE_REFACTOR_INTEGRATION_2026-11-08.md`. No runtime deployment, user-data migration, frontend edits or performance timing claims.
