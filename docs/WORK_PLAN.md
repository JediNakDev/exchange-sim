# Testing infrastructure continuation

This checkout did not contain WORK_PLAN.md before the canonical sync. This
record covers only this testing/simulation-infrastructure task; the parent
coordination record must be reconciled on integration.

- Checkout: /Users/jedi/.t3/worktrees/lob/t3-0fb59d8d
- Branch: t3/build-exsim-test-harness
- Source task: delegated testing infrastructure continuation from the interrupted
  implementation. No subagents or new checkouts were started.
- Canonical foundation imported from 9a2eaa5, committed separately as ffd1790.
  All explicitly named foundation paths were restored exactly; no engine
  variant was imported. Existing untracked infrastructure was preserved.
- Implementation: independent oracle, canonical field visitors and encoding,
  journal/replay, differential generator/minimizer, scenario DSL/23 scenarios,
  independent contract gates and opt-in production allocation/differential
  tests. See the commit containing this record.
- Original temporary shim preserved untracked at
  tests/exsim/local/reference_shim_DO_NOT_MERGE.cpp.disabled, outside automatic
  source discovery. It has the obsolete API and is not part of this delivery.
- Engine remains absent. Default tests validate the reference independently;
  production tests require EXSIM_TEST_PRODUCTION and a real engine.
- Checks: make test, make test MODE=debug; reference-mode fuzz/replay/scenario
  tools linked and exercised. Production-mode harness, allocation guard and
  tool/benchmark objects compiled in release and sanitizer modes without
  linking an engine. No full production make tools claim.
- Evidence: /tmp/exsim-harness-logs/, summarized in HARNESS_VALIDATION.md.
- Next action: apply this infrastructure to each engine variant; execute the
  production commands in SCENARIOS.md, including allocation and retry gates.
  Resolve the listed contract interpretations with the integrator if needed.
