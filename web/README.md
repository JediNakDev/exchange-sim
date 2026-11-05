# exsim research UI

The browser research interface for exsim: **Experiments, then Compare, then Inspect**, plus a read-only Connections view (see [docs/PRODUCT.md](../docs/PRODUCT.md)). It talks to the research service only through the OpenAPI contract in [docs/api/openapi.yaml](../docs/api/openapi.yaml). Conventions (units, decimals, pagination, idempotency, errors, polling, versioning) are in [docs/API.md](../docs/API.md).

Stack: Bun, Next.js (App Router), strict TypeScript, Tailwind CSS, shadcn/ui and Recharts through the shadcn chart component. Types come from `openapi-typescript`, and calls go through `openapi-fetch`. There is no tRPC, because the server is C++.

## Scripts

| Command | What it does |
| --- | --- |
| `bun install` | install dependencies |
| `bun run dev` | build the C++ research service (`make MODE=release build/release/exsim_api` in the repository root), start it on `EXSIM_API_PORT` (default 7600) with its data in `EXSIM_API_DATA_DIR` (default `../build/exsim-api-data`), and start `next dev` with `EXSIM_API_BASE_URL` pointing at it; Ctrl+C stops both |
| `bun run dev:mock` | the same with the TypeScript mock service instead of the C++ service (also `bun run dev -- --mock` or `EXSIM_DEV_BACKEND=mock`) |
| `bun run dev:next` | `next dev` only, against whatever `EXSIM_API_BASE_URL` points to |
| `bun run mock-server` | the mock API on its own |
| `bun run build` | production build |
| `bun run start` | serve the production build |
| `bun run typecheck` | `tsc --noEmit` |
| `bun run lint` | ESLint (Next.js rules, plus the decimal rule below) |
| `bun run test` | `bun test`: decimal helpers, contract conformance of every operation, fixture reconciliation and comparison math |
| `bun run test:e2e` | build `exsim_api`, start it on an ephemeral port with a temporary data directory, and run `e2e/exsim-api-binary.e2e.ts` against it (every operation, schema-validated) |
| `bun run generate-api-types` | regenerate `src/lib/api/generated-schema.d.ts` from `../docs/api/openapi.yaml`; run it after changing the contract and commit the result |

## Configuration

| Variable | Default | Used by |
| --- | --- | --- |
| `EXSIM_API_BASE_URL` | `http://127.0.0.1:7600` | the Next.js server-side proxy |
| `EXSIM_API_PORT` | `7600` | `dev`: port of the research service (C++ or mock) |
| `EXSIM_API_DATA_DIR` | `../build/exsim-api-data` | `dev`: experiments, runs and artifacts of the C++ service |
| `EXSIM_MOCK_PORT` | `7600` | `mock-server` on its own |
| `EXSIM_API_BINARY` | `../build/release/exsim_api` | `test:e2e` |

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

## Research service

`bun run dev` runs the real service, `tools/exsim_api` in the repository root. It executes every run with the deterministic research runner in C++ and serves the contract on loopback; see the "Alignment with the deterministic research runner" section of [docs/API.md](../docs/API.md) for how runner output maps onto the contract. Its baselines (`fixed-spread-2-ticks`, `fixed-spread-4-ticks`) are the runner's own configurations, and runs take well under a second, so the Run page usually shows a completed run on its first poll; create a run with **start paused** to step through it.

## Mock API server

`mock-server/` implements the whole contract in memory, so the UI can be developed and tested without the C++ service. It is a TypeScript stand-in, not the reference simulation.

- **Fixtures:** two experiments, fixed spread at 2 ticks (`exp_fixedspreadw2`) and at 4 ticks (`exp_fixedspreadw4`, child of the first), each completed on master seeds 101, 202, 303, 404 and 505. Runs are named `run_fsw<width>seed<seed>`.
- **Simulation:** each run is a seeded synthetic market (a background market maker and a noise taker in one price-time FIFO book) plus the research strategy, with separate feed, order, ack and cancel latencies. The fills, ledger, markouts, series, lifecycle and FIFO evidence all come from the same event log, so they reconcile.
- **New runs:** a run created from the UI advances in wall-clock time (queued for about 0.8 s, then running for about 8 s) and supports pause, resume, step and cancel.
- **Strategy parameters:** the mock reads the runner's fixed spread parameters but approximates its quoting: it requotes on every strategy-visible mid change, at most once per `requoteIntervalNs`, at `halfSpreadTicks` each side, and ignores `requoteThresholdTicks` and `postOnly`.
- **Contract checks:** every response is validated in tests against `docs/api/openapi.yaml` (`mock-server/openapi-schema-validator.ts`).

## Tests

- `tests/mock-api-contract.test.ts`: all 23 operations, with status, header and schema checks; errors and field issues; idempotent replay and conflict; the run state machine.
- `tests/fixture-reconciliation.test.ts`: for each of the 10 fixture runs, fills explain position, cash and fees, the equity and net P&L identities hold, markouts are censored exactly past run end, and FIFO and lifecycle evidence is consistent.
- `tests/compare-math.test.ts`: per-seed comparison values equal the run summaries, differences are exact, dispersion matches an independent recomputation (to 1e-6), and aligned series means are consistent.
- `tests/exact-decimal-format.test.ts`: the decimal helpers.
- `e2e/exsim-api-binary.e2e.ts` (`bun run test:e2e`): the real `exsim_api` binary on an ephemeral port. It creates experiments, runs one to completion and reads every artifact view, pauses, steps and resumes another, cancels a third, checks idempotent replay, compares two experiments paired by seed, and restarts the service to read stored runs. Every response is validated against the contract.
