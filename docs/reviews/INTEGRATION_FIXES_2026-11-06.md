# Integration review fixes, 2026-11-06

Original report: [INTEGRATION_REVIEW_2026-11-06.md](INTEGRATION_REVIEW_2026-11-06.md), review commit `0aa017f`, target `69bb968`. The original report remains a record of that target.

## Changes

- **IR-01:** HTTP comparison re-executes the expected configuration and seed through the same C++ simulation driver as the API. Native files must match that execution byte for byte, excluding wall-clock status. Metrics use a common summary conversion from the fresh ledger/accumulators; series samples and end time also come from that execution. Comparison does not consume the outer HTTP summary, binary samples cache or saved final time. Other read endpoints retain their existing persisted view behavior.
- **IR-02:** Before re-execution, canonical native identity, master seed and saved configuration must match the enclosing API run. Reproduced native artifacts then also validate the evidence contents. An intact native directory for another seed or configuration cannot enter the paired result.
- **IR-03:** Optional shutdown diagnostics were removed from the reactor. `ServerRuntimeOptions.log` applies to startup listener reporting. Normal CLI completion returns its status without a final blocking stderr write. No diagnostic persistence worker or additional logging queue was introduced.

## Regression evidence

Five new cases were compiled against the reviewed pre-fix source in isolated temporary binaries. All failed as intended, with no source replacement in the working checkout:

- Outer summary metrics edited, then service restarted: failed before repair.
- Valid samples cache substituted and saved end time edited, then service restarted: failed before repair.
- Intact native directory substituted from another seed: failed before repair.
- Intact native directory substituted from another configuration at the same seed: failed before repair.
- Full, blocking log pipe during shutdown: failed before repair (child exceeded its 300 ms observation window).

Exact pre-fix logs: `/tmp/exsim-ir-before-fix/{api-cache,api-provenance,runtime-log,compile}.log`. The first four are `api_service` cases in `research_service_tests.cpp`; the fifth is `server_runtime_review.a_full_log_pipe_does_not_block_shutdown`. The native substitutions independently pass the CLI reproduction check before the API rejects them, proving provenance rather than malformed-file rejection.

Final-code verification is recorded in WORK_PLAN.md when complete. Existing limits remain: local service, synthetic market-making UI, TWAP CLI-only, no historical mode, and no new performance claim. Successful journal footer writes are not fsync durability; bounded teardown does not constitute a hard real-time OS guarantee.
