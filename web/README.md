# Frontend

Next.js app using Bun, TypeScript, Tailwind CSS and shadcn/ui.

```sh
bun install
bun run dev
```

This starts the C++ API and frontend. `bun run dev:mock` uses the test service instead.

## Testing

Tests live in `test/`, grouped by unit, browser, e2e and review checks.

```sh
bun run test
bun run typecheck
bun run lint
bun run test:e2e
```

For browser tests, build the frontend and native tools first:

```sh
bun run build
make -C .. build/release/exsim_api build/release/exsim_research
bunx playwright install chromium --only-shell
bun run test:browser
```

## API Types

The contract lives in `config/api/openapi.yaml` at the repository root.
Run `bun run generate-api-types` after changing it.
