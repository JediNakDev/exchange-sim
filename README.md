# exsim

A simple exchange simulator for testing trading strategies with various trading protocal supported.

## Architecture

Our architecture is composed by Next.js frontend app, C++ backend server, trader and exchange worker for testing trading strategies jobs, Postgres database, and local file (should be replaced by s3 or equivalent in production).

```mermaid
flowchart TD
    FE[Frontend] <-->|HTTP| BE[Backend]
    BE --> DB[Database]

    subgraph SIM[Simulation]
        direction LR
        T[Trader Process]
        E[Exchange Process]

        T <-->|UNIX Socket| E
    end

    BE <-->|UNIX Socket| SIM
```

## Getting Started

From the repository root:

```sh
cd web
bun install
bun run dev
```

This builds the C++ backend and starts both backend and frontend:

- Frontend: http://127.0.0.1:3000
- Backend: http://127.0.0.1:7600

By default, this uses file-backed metadata. To use an existing PostgreSQL database set this variable in your environment:

```dotenv
EXSIM_DATABASE_URL=postgresql://USER:PASSWORD@localhost:5432/exsim
```

Or you can skip all that and run it via docker compose.

```sh
sh docker/create-secrets.sh
docker compose up -d --build --wait
```

## Project Layout

- `lib/` and `include/`: reusable C++ code.
- `src/`: API, trader and exchange daemons.
- `tools/`: protocol clients, research CLI, audit and deployment helpers.
- `test/`: native tests, benchmark, fuzz, scenarios and validation tools.
- `web/`: frontend app, with frontend tests in `web/test/`.
- `config/`: runtime configs, OpenAPI contract and SBE schema.

## Testing

```sh
make -j4 test
make -j4 test-tools
cd web
bun run test
```

`make bench` builds the Google Benchmark suite. It needs google-benchmark installed.
Python helpers handle audit files, packaging and integration checks. Simulation stays in C++.

## Key Design Decisions

### Efficiency is not compromised.

This was previously a limited order book project, as such the exchange simulator choices of data structures and algorithms were chosen to prioritize limited order book operations efficiency.
We use hash map (Order Id -> Order) and another hash map for price level (Price -> Order List) with order linked list inside.
This resulted in expected O(1) time complexity for order lookup, non-crossing order insertion and quantity updates, FIFO append/unlink price-level operations and best-bid/ask queries.
And match an incoming order at expected O(F + S) with F = fills produced; S = total bitmap words scanned.

### Try to simulate the real trader-exchange relationship.

The trader and exchange worker are two different processes that communicate through UNIX socket with swappable exchange protocol.
The experiments is also as repeatable as possible. The engine has no clock, I/O or randomness. The scheduler supplies time and seeded inputs.

### Exchange protocol is extensible at the core.

The exchange protocol is designed to seperate from the worker, it is used as a function to translate struct to bytes.
As a result the new protocol can be introduced and the old protocal can be updated in a very convenient fasion.

## Limitations & Further Reading

- This is a very early alpha, expects bugs.
- Uploaded strategies are not implemented. Only templated strategy are available.
- Latency is simulated using configurable delays and jitter.
