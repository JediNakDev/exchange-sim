# Focused Linux execution and durability validation

Completed 2026-11-07 (Asia/Singapore). GCC and Clang each passed all 64 focused
C++ tests, built the API executable, and returned 2 for an unmatched filter.
The Python delivery fixtures passed 40/40 once on Linux. No compiler or runtime
regression was found; no production source or public contract was changed.

## Source and isolation

- Harness task `926e3cf2-2e6d-49c0-b157-640c064cb592` (Linux validation).
  Full child thread identifier:
  `thread:delegated-task:command%3Amcp%3Adbfd70d4-c620-498e-9be2-f818534d0cbb%3Adelegate-task%3A926e3cf2-2e6d-49c0-b157-640c064cb592`.
- Dependent checkout `/Users/jedi/.t3/worktrees/lob/t3-f2c04d3b`, branch
  `t3/linux-execution-durability`. Validated parent source
  `7ca10141852a56a6a784cb4c5aa58213efa57d96`, not `main`.
- Read `AGENTS.md`, `docs/WORK_PLAN.md` and the integration checkpoint before
  running checks. Existing-image and running-container discovery was read-only.
- `git archive --format=tar 7ca10141852a56a6a784cb4c5aa58213efa57d96`
  produced `source-7ca1014.tar`, SHA-256
  `846533bb5b155638c21ee9ab511c251bf340dc4ce7d4bbb1ee33955dec99557c`.
  Docker copied that archive into the disposable container, then extracted it
  into `/validation/source`. No host checkout/data mount or published port.
- Reused existing `exsim-dev:latest`, pinned to image ID
  `sha256:2a9d943fdf4aadc80e562347444ba001adcb77059c088e7309f1c04a38ff63be`.
  No image build or pull. The image lacked Python; only Debian's Python
  interpreter/stdlib and dependencies were installed in this disposable
  container to run the required fixtures. Package installation is logged.
- Container `exsim-linux-execution-926e3cf2`, ID
  `8938f51ac7cfb4b895f11eea6c2b525a0325a644032125073d136677e1de2082`.
  `--cpus=2`, `NanoCpus=2000000000`, cgroup `cpu.max=200000 100000`.
  Every make invocation used `-j2`. GCC completed before Clang started.
- Compiler objects/binaries are separate: `/validation/build/gcc` and
  `/validation/build/clang`. No source directory was cleaned. Complete file
  checksum manifests before and after validation match byte for byte.

## Platform and results

Debian GNU/Linux 13.6 (trixie), native `aarch64`/ELF AArch64, OrbStack Linux
`7.2.12-orbstack-00394-gbecdd6130068`, overlayfs temporary storage. GCC
`14.2.0-19` (14.2.0); Debian Clang `19.1.7 (3+b1)`, target
`aarch64-unknown-linux-gnu`; libstdc++ `14.2.0-19`; glibc `2.41-12+deb13u4`;
GNU Make 4.4.1; Python 3.13.5. This is actual Linux execution, not macOS evidence.

| Gate | GCC | Clang |
| --- | --- | --- |
| Registered focused cases | 64 | 64 |
| Unfiltered focused binary | 64/64, exit 0 | 64/64, exit 0 |
| Durability / execution cases included | 17 / 11 | 17 / 11 |
| Unmatched filter | exit 2 | exit 2 |
| Only tool built: `exsim_api` | Passed | Passed |
| Compiler warnings / errors, `-Werror` | 0 / 0 | 0 / 0 |

The test sources are exactly `tests/main.cpp` and all five
`tests/exsim/api/*.cpp` files. `--list` is asserted to contain exactly 64
entries, including 17 `api_durability` and 11 `execution_api` cases. The
execution binary receives no filter. The intentionally unmatched filter is
`__exsim_linux_unmatched_926e3cf2__`; its diagnostic and exit status are checked.
Both compiler builds use release `-O2 -g`, C++20, the project's warning flags
and `-Werror`. The script asserts `64/64 tests passed` as well as exit 0.

Python: `python3 -B scripts/deployment/test_research_delivery.py` passed
40/40 in 5.284 seconds, once, after both compiler gates. Its fixture loopback
servers, backups, restores and packages are disposable test resources.

## Portability conclusions and limits

- Successful reservation creation, response replacement and invalid-request
  reservation removal exercised `sync_file` on regular files and the
  idempotency directory on Linux overlayfs. The implementation opens with
  `O_RDONLY | O_CLOEXEC`, calls `fsync`, and propagates errors; both compiler
  runs accepted the directory handling. Store obstruction is rejected before
  mutation; mutation persistence failure retains the reservation.
- The fork/pipe/`RLIMIT_FSIZE`/ignored-`SIGXFSZ` probe passed under both
  compilers: the resource was committed, response persistence failed with
  503, abrupt child exit occurred, and restart refused duplicate execution
  while retaining one resource. The malformed-startup fork probes also passed.
- Restart tests reject malformed experiment/run metadata, torn legacy records,
  corrupt or misnamed new records, capacity overflow, missing terminal evidence
  and missing successful resources. Successful replay, uncertain step retries
  and terminal failure diagnostics survive the tested restarts.
- Execution tests cover both sides of TWAP/sweep through the real service,
  pause/step/resume versus uninterrupted reproduction, exact native-to-API
  accounting/cost mapping including half price units, residual and censored
  windows, no liquidity, cancellation, reload comparisons, and rejection of
  substituted native seed/configuration/strategy evidence.

These results cover the existing focused assertions on Linux arm64 and this
filesystem. They do not establish power-loss recovery, another filesystem's
directory-fsync support, multi-writer safety, Linux x86_64 portability, or an
independent correctness review. No full suite, sanitizer/TSan, benchmark,
frontend or browser gate was rerun. Installed services/data and the parent
checkout were untouched. Parent review and integration remain the next action.

## Evidence and reproduction

All paths below are under the exact host evidence root
`/Users/jedi/.t3/worktrees/lob/t3-f2c04d3b/output/linux-execution-durability-2026-11-07/`.
Long logs and the source archive remain there, outside the committed report.

| Evidence | Path relative to that root |
| --- | --- |
| Final driver, versions, limits, exit 0 | `logs/driver.log`, `container-console.log` |
| Full initial GCC compilation and API link | `initial-attempt/gcc-test-build.log`, `initial-attempt/gcc-api-build.log` |
| Final GCC make invocations (reused same GCC output) | `logs/gcc-test-build.log`, `logs/gcc-api-build.log` |
| Full Clang compilation and API link | `logs/clang-test-build.log`, `logs/clang-api-build.log` |
| Unfiltered runs | `logs/gcc-tests.log`, `logs/clang-tests.log` |
| Exact registrations and source selection | `logs/gcc-test-list.log`, `logs/clang-test-list.log`, `logs/test-sources.txt` |
| Unmatched-filter diagnostics | `logs/gcc-empty-filter.log`, `logs/clang-empty-filter.log` |
| Python fixtures / package setup | `logs/python-delivery.log`, `logs/python-install.log` |
| Source immutability | `logs/source-before.sha256`, `logs/source-after.sha256` |
| Docker configuration and exit state | `container-inspect.json`, `image-inspect.json` |
| Input and executed final script | `source-7ca1014.tar`, `validation-driver.sh` |
| Evidence checksums | `SHA256SUMS` |

The first driver attempt passed GCC64, the unmatched-filter assertion and API
build, then exited 127 because the image lacks the optional `file` utility.
The driver now uses installed binutils `readelf -h`. The initial logs are
preserved in `initial-attempt/` and `initial-container-console.log`. The final
driver reused the same completed GCC objects, reran its 64 cases, then built
and ran Clang and ran Python once. This was a driver correction, with no C++
source fix. The final container exited 0 without OOM and was removed after
evidence extraction.

The committed sibling script
`validate_linux_execution_durability_2026-11-07.sh` reproduces the gates inside
a disposable container. On the host, create the archive from the full SHA
above, create a container with `--pull=never --cpus=2` and the pinned image ID,
and use `bash /tmp/exsim-validation.sh` as its command. Copy the archive to
`/tmp/exsim-source.tar` and the script to `/tmp/exsim-validation.sh`, then
`docker start -a <container>`. Inspect its exit code and copy out
`/validation/logs` before removing it. The script builds only `run_tests` and
`exsim_api`, using the separate compiler build paths above, with 180-second
runtime timeouts. Package networking is needed only if Python is missing.
