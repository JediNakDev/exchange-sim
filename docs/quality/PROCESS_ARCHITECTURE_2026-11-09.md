# Trader and exchange process separation, 2026-11-09

User requested independent API, trader and exchange processes, reusable strategy
and adapter libraries, removal of custom-strategy setup, and corrected Docker
packaging. Work is in the shared parent checkout; no delegates, commit, push,
installed-service changes or data migration.

## Implementation

- `src/exsim_apid/main.cpp`: HTTP orchestration, metadata and bounded run slots.
- `src/exsim_exchanged/{main,runtime}.cpp`: exchange worker bootstrap and control.
  Matching, simulated-time scheduling, background liquidity, accounting and
  artifacts are owned by this worker through the shared research library.
- `src/exsim_traderd/{main,runtime}.cpp`: invokes exactly one of the four existing
  strategy classes. Observations and actions cross a real UDS socket pair.
- `lib/include/exsim/` and `lib/src/exsim/`: relocated shared implementation,
  strategy classes, existing protocol codecs, and bounded research wire codec.
  Legacy LOB implementation sources also moved to `lib/src/`; its public
  headers remain under `include/lob/`.
- `ProcessSimulation` preserves scheduler ordering and latency semantics while
  requesting callbacks from traderd. The in-process `Simulation<Strategy>` is
  retained as the independent test reference. Production API has no
  `--in-process-runs` fallback. `exsim_api` remains a compatibility build alias.
- Worker startup verifies API, exchange and trader SHA-256 provenance. The API
  owns exchange lifetimes; each exchange owns and reaps its trader. Pausing has
  no idle socket deadline; active transfers have a ten-second deadline.
- Custom strategy placeholders and their shared frontend component are removed.
  There was no custom-strategy loader in the backend. Generic real-time
  protocol clients remain connectivity tools, not custom research execution.
- Docker API image includes all three daemon binaries. Per-run pairs run inside
  that container using private sockets, without mounting the Docker socket.
  Release packaging requires both worker binaries, copies and inventories them,
  and checks source mutation. Existing bundle verification remains supported.

## Protocol scope

Research IPC uses an explicit versioned research protocol with fixed-width
big-endian fields and bounded frames. It is **not** standard OUCH/ITCH framing.
It transports the existing research observations, actions, risk acknowledgments
and diagnostics. No raw C++ object layouts or pointers are sent. Existing
OUCH/FIX/FIXP/ITCH codecs and the real-time protocol server remain library/tool
facilities. Research remains on the existing simulated-time model; no new
clock configuration or additional user workflow was introduced.

Risk/admission checks and the authoritative strategy-visible order view remain
in the exchange host; the trader receives only delivered information. This is
an explicit initial ownership choice, not a claim that all brokerage/client
risk state has been moved to traderd. The API does not proxy trading messages.

## Validation

All final checks below passed. Native builds use `-j1` and `-Werror`.

| Check | Result | Log under `output/process-architecture-2026-11-09/logs/` |
| --- | --- | --- |
| Complete release build, tools and examples | pass | `exsim-process-all-build.log` |
| Process equivalence, crash/reap, step/resume and parent allocation guard | 7/7 | `exsim-process-equivalence-tests.log` |
| Explicit field encoding, malformed values and stable decision reasons | 3/3 | `exsim-process-wire-tests.log` |
| Native API | 99/99 | `exsim-process-api-tests.log` |
| Research guarantees | 68/68 | `exsim-process-research-tests.log` |
| Allocation selection | 26/26 | `exsim-process-allocations-tests.log` |
| ASan/UBSan focused process/wire tests | 10/10 | `exsim-process-sanitizer-tests.log` |
| Actual three-daemon HTTP integration | pass | `exsim-process-native-integration.log` |
| HTTP/OpenAPI contract, all 23 operations | 9/9 | `exsim-process-http-contract-tests.log` |
| Ordinary frontend tests | 123/123 | `exsim-process-web-unit-tests.log` |
| Combined Bun discovery | 129 pass, 5 existing skips, 0 fail | `exsim-process-web-tests.log` |
| Frontend types and scoped lint | pass | `exsim-process-web-types.log`, `exsim-process-web-lint.log` |
| Release packaging | 42/42 | `exsim-process-delivery-tests.log` |
| Compose credential helper | 4/4 | `exsim-process-compose-secrets-tests.log` |
| Both Docker images | build pass | `exsim-process-docker-{api,web}-build.log` |
| Disposable real Compose/PostgreSQL deployment | pass | `exsim-process-compose-integration.log` |
| Standalone 60-second TWAP seed42 and independent reproduction | completed, replay verified, reproduced | `twap-run.log`, `twap-verify.log` |

The artifact-equivalence cases compare nine canonical output files for every
built-in strategy at seeds7 and42, including commands, events, ledger,
lifecycle, strategy decisions, series and summaries. They require nonzero
strategy order submission. The in-process reference invokes the original
strategy class directly; the candidate invokes a separate trader executable.
The process allocation guard covers the exchange side after initialization;
sanitizer builds disable allocation counting rather than establishing zero
allocations under ASan.

The HTTP integration confirms three distinct daemon PIDs, a pause longer than
the active transfer deadline, step/resume/completion, cold book inspection,
and run failure after killing traderd while the API remains available. The
intentional trader kill produces the expected transport-failure diagnostic.

The Compose check confirms all three daemon names in the process tree,
production frontend HTML/assets and proxy, non-root apps, private API/DB,
actual controls/artifacts, PostgreSQL metadata, and exact replay/result
persistence after full container recreation. The disposable project's
containers, network and volumes were removed. No installed app was promoted.

The combined Bun run initially exposed an existing review mock missing
`usePathname`, which polluted subsequent ordinary tests. Added the missing
mock export without changing review assertions. Combined discovery now passes;
its five existing review skips remain. No new browser or human usability
clearance is claimed.

Runtime image tags are `exsim-api:process-separation` and
`exsim-web:process-separation`; default local image tags were preserved.
Image IDs and native daemon hashes are recorded in
`output/process-architecture-2026-11-09/validation-identities.json`. The verified
TWAP artifacts remain in `output/process-architecture-2026-11-09/twap-seed42/`.
All final logs are retained under the output directory above; original tool
logs remain at the corresponding `/tmp/exsim-process-*.log` paths.

No full native sanitizer suite, full PostgreSQL ownership suite, distributed
worker support, browser WebSocket delivery, or standard OUCH/ITCH research
session is claimed. Existing production/dev processes and data were preserved.
There are no remaining implementation steps for the recorded UDS research
process split. Changes are uncommitted and have not been deployed.

## PR3 GCC portability repair

After source15434f5 was pushed to PR3, GitHub GCC release/debug jobs failed at
`research_service_durability_tests.cpp:126`: glibc's fortified `write()` is
annotated `warn_unused_result`, and a void cast does not consume that result.
The repair checks that the complete status was written before the child exits.
Two equivalent readiness writes in runtime review probes get the same check.
No production behavior or test assertions change.

Linux GCC14 with `-D_FORTIFY_SOURCE=3 -Werror` compiles both affected translation
units in release and ASan/UBSan modes. Local focused durability17/17 and
runtime-review7/7 pass. Logs retained under the output log directory:
`exsim-pr3-gcc-{release,debug}-failed.log`,
`exsim-pr3-gcc-fix-{build,linux-compile,durability-tests,runtime-tests}.log`.
CI rerun and main merge remain pending at this checkpoint.
