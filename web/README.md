# exsim research UI

The browser research interface for exsim: **Experiments, then Compare, then Inspect**, plus a read-only Connections view (see [docs/PRODUCT.md](../docs/PRODUCT.md)). It talks to the research service only through the OpenAPI contract in [docs/api/openapi.yaml](../docs/api/openapi.yaml). Conventions (units, decimals, pagination, idempotency, errors, polling, versioning) are in [docs/API.md](../docs/API.md).

Stack: Bun, Next.js (App Router), strict TypeScript, Tailwind CSS, shadcn/ui and Recharts through the shadcn chart component. Types come from `openapi-typescript`, and calls go through `openapi-fetch`. There is no tRPC, because the server is C++.

## Scripts

| Command | What it does |
| --- | --- |
| `bun install` | install dependencies |
| `bun run dev` | start the mock API on `EXSIM_MOCK_PORT` (default 7600) and `next dev` with `EXSIM_API_BASE_URL` pointing at it; Ctrl+C stops both |
| `bun run dev:next` | `next dev` only, against whatever `EXSIM_API_BASE_URL` points to |
| `bun run mock-server` | the mock API on its own |
| `bun run build` | production build |
| `bun run start` | serve the production build |
| `bun run typecheck` | `tsc --noEmit` |
| `bun run lint` | ESLint (Next.js rules, plus the decimal rule below) |
| `bun run test` | `bun test`: decimal helpers, contract conformance of every operation, fixture reconciliation and comparison math |
| `bun run generate-api-types` | regenerate `src/lib/api/generated-schema.d.ts` from `../docs/api/openapi.yaml`; run it after changing the contract and commit the result |

## Configuration

| Variable | Default | Used by |
| --- | --- | --- |
| `EXSIM_API_BASE_URL` | `http://127.0.0.1:7600` | the Next.js server-side proxy |
| `EXSIM_MOCK_PORT` | `7600` | `mock-server` and `dev` |

The browser never calls the research service directly. It calls the same-origin route `/api/exsim/v1/...` (`src/app/api/exsim/[...pathSegments]/route.ts`). That route forwards only `/v1` GET and POST requests to `EXSIM_API_BASE_URL`, with the `Content-Type`, `Idempotency-Key`, `Exsim-Schema-Version` and `Accept` headers. If the upstream service is unreachable, the route answers `503 service_unavailable` in the contract's error format.

## Pages

| Route | Purpose |
| --- | --- |
| `/experiments` | saved experiments with run-state counts (polled) |
| `/experiments/new` | derive from a baseline, edit scenario, strategy, seeds, latency components, fees, risk, cash, markout horizons and bounds; **Review assumptions** shows the full assumption table and estimated work before **Create and start** |
| `/experiments/[experimentId]` | configuration assumptions, runs per seed, link to compare with the parent |
| `/runs/[runId]` | state, progress and controls (pause, resume, step N, cancel); equity, inventory and drawdown charts; summary (ledger, orders, risk, markouts, latency), manifest, state history and digests |
| `/runs/[runId]/inspect` | event stepper (arrow keys or `j`/`k`; Shift steps 25; Home/End), exchange-truth and strategy-visible books, own orders and fills; focusing a fill or order shows its related events, lifecycle timeline, latency breakdown, FIFO-ahead evidence, ledger effect and markouts |
| `/compare?baseline=&candidate=` | comparability and seed pairing, assumption differences, aligned equity/inventory/drawdown charts (mean or one seed), and paired metric differences with dispersion and better/worse/tied counts |
| `/connections` | read-only OUCH/FIX sessions, ITCH feeds and gaps, fault scenarios |

## Exact decimals

Prices, cash, quantities, ids, sequence numbers and nanosecond times arrive as decimal strings and must stay strings.

- `src/lib/exact-decimal-format.ts` formats, compares and subtracts them with BigInt fixed point.
- `src/lib/chart-derived-floats.ts` is the only module that turns them into JavaScript numbers, and only for plot positions. Chart tooltips show the original strings.
- ESLint enforces this: `Number()`, `parseFloat` and `Number.parseFloat` are errors in `src/` outside the chart module.

## Mock API server

`mock-server/` implements the whole contract in memory, so the UI can be developed and tested without the C++ service. It is a TypeScript stand-in, not the reference simulation.

- **Fixtures:** two experiments, fixed spread at 2 ticks (`exp_fixedspreadw2`) and at 4 ticks (`exp_fixedspreadw4`, child of the first), each completed on master seeds 101, 202, 303, 404 and 505. Runs are named `run_fsw<width>seed<seed>`.
- **Simulation:** each run is a seeded synthetic market (a background market maker and a noise taker in one price-time FIFO book) plus the research strategy, with separate feed, order, ack and cancel latencies. The fills, ledger, markouts, series, lifecycle and FIFO evidence all come from the same event log, so they reconcile.
- **New runs:** a run created from the UI advances in wall-clock time (queued for about 0.8 s, then running for about 8 s) and supports pause, resume, step and cancel.
- **Contract checks:** every response is validated in tests against `docs/api/openapi.yaml` (`mock-server/openapi-schema-validator.ts`).

## Tests

- `tests/mock-api-contract.test.ts`: all 23 operations, with status, header and schema checks; errors and field issues; idempotent replay and conflict; the run state machine.
- `tests/fixture-reconciliation.test.ts`: for each of the 10 fixture runs, fills explain position, cash and fees, the equity and net P&L identities hold, markouts are censored exactly past run end, and FIFO and lifecycle evidence is consistent.
- `tests/compare-math.test.ts`: per-seed comparison values equal the run summaries, differences are exact, dispersion matches an independent recomputation (to 1e-6), and aligned series means are consistent.
- `tests/exact-decimal-format.test.ts`: the decimal helpers.
