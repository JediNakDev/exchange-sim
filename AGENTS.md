# Project instructions

## Read only what the task needs

- Start with `docs/WORK_PLAN.md` for current state, dependencies, and delegation records.
- Read `docs/PRODUCT.md` for trader workflows and product/architecture decisions.
- For C++ changes read `docs/ARCHITECTURE.md` and the relevant normative spec (`docs/SPEC.md`, `docs/GATEWAYS.md`, `docs/SERVER.md`).
- `docs/FOUNDATION_REVIEW.md` is the recovered independent review. Findings are not fixed merely because the report exists.

## User constraints

- The first product workflow is execution and market-making research.
- Frontend: Bun, Next.js, TypeScript, Tailwind CSS, shadcn/ui. Prefer existing shadcn components and meaningful names. Do not substitute another framework or package manager.
- The tRPC choice is under evaluation. Current recommendation is an OpenAPI-described C++ HTTP API with generated TypeScript types, without a tRPC wrapper. Do not treat the global tRPC default as a final decision for this C++ backend.
- Simulation, matching, exchange protocols, accounting, and simulated time belong in C++.
- Use GPT/ChatGPT for resumed and new delegates with high reasoning (user decision 2026-11-04, after Claude hit its usage limit; this replaces the earlier Claude-only rule). Do not use local Ollama while the user is working on this machine.
- Never include a co-author attribution in commits. Avoid em dashes in responses, comments, and new prose.

## Coordination

- Preserve untracked `.claude/`, `output/`, and unfinished implementation files from interrupted work.
- Do not alter another delegate's owned files. Coordinate public API/spec changes before resuming dependent implementations.
- Record task IDs, checkout paths, commits, checks, and next actions in `docs/WORK_PLAN.md`. Keep long logs in files; return concise findings and exact log paths.
- Use harness-created isolated worktrees for independent implementation. Preserve existing interrupted worktrees and resume their actual child threads when continuing that work.
- Do not poll running delegates. Use T3 completion notifications and follow the tool's `next` instruction.
- Never claim that a design, declaration, partial branch, or successful launch is a working integrated feature.
