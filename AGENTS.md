# Project instructions

## Read only what the task needs

- Start with `README.md`, then inspect the relevant source and tests.
- Keep docs focused on topics that need more depth than the README.
- Match the README's short, direct writing style. Do not add dated reports or long task histories.
- Historical review findings are not fixed merely because the reports were removed.

## User constraints

- Product: API-managed empty exchange processes. External trading engines supply all orders and liquidity.
- No web frontend, built-in trader strategies, or PostgreSQL. Keep live exchange state in RAM, with config files and optional logs on disk.
- Language: C++23 for production builds, tests, and editor tooling.
- Management: an OpenAPI-described C++ HTTP API. Trading engines connect directly to each exchange's protocol endpoints.
- Active verification: unit and integration tests on macOS and local OrbStack Ubuntu. Fuzz and end-to-end suites are deferred.
- Leave README.md to the user; do not rewrite it.
- Simulation, matching, exchange protocols, accounting, and simulated time belong in C++.
- Use GPT/ChatGPT with high reasoning for resumed and new delegates (latest user decision 2026-11-05, "continue and turn all subagent to chatgpt"; this replaces the earlier Claude-only rule). Do not use local Ollama while the user is working on this machine.
- Never include a co-author attribution in commits. Avoid em dashes in responses, comments, and new prose.

## Coordination

- Preserve untracked `.claude/`, `output/`, and unfinished implementation files from interrupted work.
- Do not alter another delegate's owned files. Coordinate public API/spec changes before resuming dependent implementations.
- Record task IDs, checkout paths, commits, checks, and next actions in ignored `output/` task records. Keep long logs in files; return concise findings and exact log paths.
- Use harness-created isolated worktrees for independent implementation. Preserve existing interrupted worktrees and resume their actual child threads when continuing that work.
- Do not poll running delegates. Use T3 completion notifications and follow the tool's `next` instruction.
- Never claim that a design, declaration, partial branch, or successful launch is a working integrated feature.
