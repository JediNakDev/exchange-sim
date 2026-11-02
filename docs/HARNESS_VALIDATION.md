# Harness validation

Scope: oracle and test/simulation infrastructure. No production engine has
been linked or validated. The temporary reference shim is untracked/disabled.

## Independent checks

- Release: make test, 42/42 tests pass.
- ASan/UBSan: make test MODE=debug, 41/41 tests pass.
- The one-test count difference is the journal allocation replacement test,
  deliberately excluded under sanitizers.
- Twenty-three hand-authored scenarios run against the oracle, covering FIFO,
  sweeps, market/IOC/FOK, FOK/STP, every STP mode, post-only, replacement priority,
  partial/full cancels, disconnect cancellation, phase restrictions, auction
  volume/imbalance/reference-distance/lower-price tie breaks, halt/resume,
  expiry, validation and capacity.
- Config bounds, UINT32_MAX aggregates (including crossed auction volume),
  nonempty output preflight/retry with order and match IDs, partial cancellation
  FIFO in every phase, random state invariants and amendment-chain conservation
  run independently of production code.
- Canonical serialization tests cover every command field, negative/extreme
  prices, active event payloads with different inactive bytes, new cancel,
  replacement and summary fields, journal roundtrip, repeatable event hashes,
  corrupt headers/records/footer, partial truncation and whole-record truncation.
- Journal append/flush: 200,000 commands, zero counted C++ allocations.
- Deterministic quick sweep: 40 seeds x 3,000 commands, varied one to three
  instruments and normal/tiny order capacity. Oracle-vs-oracle consistency
  checking does not establish independent semantic correctness.

## CLI checks

Reference-mode tools build with EXSIM_REFERENCE_ONLY. Final long runs:

- Mixed: 2,000,000 commands in 7.65628 s, 261,224 commands/s.
- Tiny: 2,000,000 commands in 6.97927 s, 286,563 commands/s.
- Final FIFO mutation: seed 1, divergence at step 1218, minimized to four
  commands. The independent test confirms no individual command can be removed
  while retaining its divergence. No production shim was mutated or committed;
  the disabled-by-default reference mutation provides the detector experiment.
- Replaying the minimized journal normally reports no divergence; its scenario
  executes successfully. Two fresh replay runs both report four commands, nine
  events, hash 7463904695323254780.

Earlier narrower generator runs minimized to three commands. The final
generator varies instrument count, which changes the seeded command stream.

## Logs and integration gates

Logs are under /tmp/exsim-harness-logs/:

- release.log and sanitizers.log
- tools-reference.log
- production-compile.log and production-compile-sanitizers.log
- fuzz-million.log and fuzz-tiny-million.log
- injection.log, diff-replay.log, scenario-cli.log and replay.log

The release and sanitizer production-mode object compilations cover the
differential/scenario tests, allocation test, fuzz/replay/scenario tools and
canonical benchmark. Compile-only success does not establish matching or
allocation correctness. The real engine is absent, so full make tools and
production runtime checks remain integration gates.

Run the commands in SCENARIOS.md separately for each engine variant. They
enable differential, scenarios, output-preflight retry/counter, partial-cancel
FIFO and 200,000-command production allocation tests. No shared public APIs
or Makefile design was changed beyond the exact canonical sync.

Contract interpretations remaining: unknown top-level command is a no-op
(journal rejects it); unknown-instrument phase query is Closed; market FOK
runs the FOK precheck. See SCENARIOS.md for rationale and adjustment points.
