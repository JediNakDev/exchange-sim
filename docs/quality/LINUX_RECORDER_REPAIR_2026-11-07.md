# Linux recorder-repair validation

GCC 14.2 and Clang 19.1.7 each pass **71/71** focused API cases and build
`exsim_api` under `-Werror`, with zero compiler warnings or errors. Both
unmatched-filter probes return 2. This is fresh evidence for the recorder and
cache-bound repair, not a reuse of the earlier Linux64 receipt.

## Source and execution

- Archived commit `5c97846` includes production repair `8ee86f1`, its seven new
  regressions, and the independent repair report. The archive SHA-256 is
  `f247204057dc0f9d6586d9ae1cea0d9f56d0cd8c7c80bc88203a06b6602b7bb6`.
- Native Linux `aarch64`, OrbStack kernel `7.2.12-orbstack-00394-gbecdd6130068`.
  Debian GCC `14.2.0-19`; Debian Clang `19.1.7 (3+b1)` with libstdc++.
- Reused image
  `sha256:2a9d943fdf4aadc80e562347444ba001adcb77059c088e7309f1c04a38ff63be`.
  No image pull/build or package installation. Container
  `exsim-linux-recorder-8ee86f1` had `--cpus=2 --network=none`, no host mounts
  and no published ports. It exited 0 without OOM and was removed after logs
  and its configuration were copied out.
- Executed the sibling
  [validation driver](validate_linux_recorder_repair_2026-11-07.sh), SHA-256
  `ae8e6db9466c31fcc7d42d8af07a149b42345f7662dd01b7718baae98c76b8cf`.
  GCC completes before Clang starts; every make invocation uses `-j2`.
  Separate build roots are `/validation/build/gcc` and `/validation/build/clang`.
  Source checksum inventories before/after execution match byte for byte.

| Gate | GCC | Clang |
| --- | --- | --- |
| Registration | 71; includes 5 I/O and 2 cache-bound cases | Same |
| Unfiltered focused tests | 71/71, exit 0; 5 s | 71/71, exit 0; 5 s |
| Empty filter | Exit 2 | Exit 2 |
| Focused test build | 22 s | 15 s |
| API executable build | Passed, 1 s | Passed, under 1 s |
| Compiler warnings / errors | 0 / 0 | 0 / 0 |

The focused binary includes `tests/main.cpp` and all five
`tests/exsim/api/*.cpp` files. Tests exercise native recorder errors, child-only
file-size limits, failed queued startup, healthy-run progress, restart and safe
cache rejection. Sparse oversized fixtures avoid constructing large payloads.
The executable and tests have verified ELF AArch64 headers.

## Evidence and reproduction

Evidence root: `/tmp/exsim-linux-recorder-8ee86f1/`.

- `container-console.log`, `logs/driver.log`: versions, limits, timing and exit.
- `logs/{gcc,clang}-test-build.log`, `logs/{gcc,clang}-api-build.log`: builds.
- `logs/{gcc,clang}-tests.log`: all 71 cases without a filter.
- `logs/{gcc,clang}-test-list.log`, `logs/test-sources.txt`: exact registration.
- `logs/{gcc,clang}-empty-filter.log`: failed empty selections.
- `logs/source-{before,after}.sha256`: source immutability.
- `container-inspect.json`, `container-id.txt`: isolation and exit state.
- `source-5c97846.tar`, `validation-driver.sh`: exact executed inputs.

To reproduce, create a new disposable container from the pinned image with
`--cpus=2 --network=none`. Copy `git archive --format=tar 5c97846` into
`/tmp/exsim-source.tar` and the sibling script into `/tmp/exsim-validation.sh`.
Run `bash /tmp/exsim-validation.sh`, assert container exit 0, export
`/validation/logs`, then remove that container. Do not mount a host checkout or
real data. The old 64-case script remains unchanged as historical evidence.

No full engine/protocol suite, sanitizer/TSan repetition, benchmark, Python
delivery rerun, frontend/browser gate, installed-service change or real-data
mutation occurred in this gate. This establishes focused Linux arm64 behavior
on the tested container filesystem, not power-loss recovery or a total-memory
bound. Independent frontend clearance and release restore/promotion remain.
