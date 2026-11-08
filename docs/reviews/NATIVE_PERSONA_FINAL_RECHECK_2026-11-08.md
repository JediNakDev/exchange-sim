# Independent final native persona recheck, 2026-11-08

Verdict: **scoped native pass for exact corrected source `360ad69529aa57b5e8856391812cc3d1d93c6896`**. NATIVE-R1/R2/R3 are closed. The two previously missing actual-scenario requirements now have independent native evidence. No new scoped objection was demonstrated. This report does not clear future code, browser behavior, a release, or actual human first-use success.

## Identity and method

New review-only harness checkout `/Users/jedi/.t3/worktrees/lob/t3-6baeba53`, starting at the exact reviewed source. Read WORK_PLAN current rollup/recent persona checkpoints, the four original persona reports, repair ledger/native implementation handoff, prior NATIVE_PERSONA_RECHECK and relevant PRODUCT, ARCHITECTURE, SPEC replacement semantics, API and RESEARCH_RUNNER contracts. Shared WORK_PLAN, schema and production code were not edited.

Before using artifacts, independently checked empty native source diffs (`src`, `include`, `tools`, Makefile) against `360ad69` both here and in `/Volumes/PortableSSD/Documents/programming/lob`. The supplied immutable API is byte-identical to the parent's corrected API and its delivery inventory. Actual bytes:

- `/tmp/exsim-persona-review-360ad69/api/exsim_api`: SHA256 `10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5`.
- `/Volumes/PortableSSD/Documents/programming/lob/build/persona-race/exsim_research`: SHA256 `598ff719788963c03d3d94ddaf3afaea7025434dfea14e53574b7e5cb8ecc442`.

Hashes were checked again after use. Selected source hashes are committed alongside the probes. Source equality and artifact hashes identify this review; they are not an independent reproducible-build attestation. SourceRevision is explicitly supplied, while executableSha256 is checked against the actual executable bytes. No inference of correctness is made from either declaration.

The wrapper starts its own API with port0 and fresh disposable storage, stops it, then starts another owned ephemeral instance on that same owned store to check restart behavior. It terminates/waits each server in finally. No requests or modifications to shared45872/45873, installed services or private/deployed services.

## Finding dispositions

| Finding / requirement | Status | Independent evidence |
| --- | --- | --- |
| NATIVE-R1 / MM01 actual race | **closed** | Actual native slow-cancel100ms seed103 order7: submit48 at63099715ns, final39-share fill69 at105073965ns, arrival104 at163120271ns, reject105 unknown_order. Arrival exchange ID is absent in HTTP (native zero). Lifecycle now reports related fill69 and transit100020556ns. `race.json`. |
| R1 no-fill unknown_order counterexample | **closed** | Actual own order24: submit130, arrival152, reject153 unknown_order, related fill null. Unknown-order alone is not promoted to a fill race. Its complete lifecycle is in race.json. |
| R1 zero-ID leg safety | **closed** | New independent boundary probe runs the real projection with an unrelated fill whose unused sell leg is0, for submitted target7 and target0. Both return null. These two deliberately constructed boundary inputs are separately labeled in engine_queue.cpp; they are not used as actual MM02 scenario evidence. |
| NATIVE-R2 / EXEC01 CLI | **closed** | CLI compares actual fresh native CENSORED_NATIVE and FULL_WINDOW_PARTIAL directories, both orientations: exit6, paired_seeds empty, execution_window_censored exclusion, reproduction_checked true. Full-window cap10 self-comparison exits0 and pairs101 despite residual260. `cli.json`. |
| EXEC01 API parity | **closed** | Actual API excludes that censored parent with execution_window_censored; full-window partial remains paired. The main probe also compares ordinary partially completed parents and retains selected-attempt receipts. |
| NATIVE-R3 / EXEC02 partial integrity | **closed** | Fresh paused TWAP stepped600 canonical events then cancelled, valid persisted trace3 rows. Append invalid JSON after those rows: limits1,2,1000 all return409 evidence_unavailable. Repeat with malformed scalar, boolean, quantity and enum rows, always off-page for limit1. All27 requests reject consistently. Restore original bytes in finally: pages1 and1000 return valid recorded_terminal_partial evidence. `partial-guards.json`. |
| R3 valid-trace compatibility | **closed** | The actual writer's decision/report rows, including native unused-field defaults, remain readable after restart and restoration. No arbitrary extra semantic parsing rules were added by this review. Source inspection matches validation before page retention. This does not certify every possible valid historical trace. |
| MM02 actual replacement queue | **closed** | New independent compiled quoting scenario drives real Simulation/MatchingEngine and production CanonicalEventBuilder/write_fifo_evidence. Priority-losing quote2 ->3 at2ms emits reset event12 with unavailable queue, then rest13 behind30 shares/1 order, total40. Same-price size10 ->5 at3ms emits priority-kept event19, still behind30, total35. Direct queries to the simulation's actual engine at both transitions independently assert FIFO order and quantities. No hand-authored canonical queue events. `engine-evidence.jsonl`, engine_queue.cpp. |
| FIRSTUSE01 actual measured zero | **closed** | Actual native sweep seed101, one buy share at99.95, fill1000112013ns. At100ms horizon, recorded mid99.95 gives exact zero. Native markout row and Decimal formula agree. Last delivered two-sided book is99.94/99.96, and no intervening exchange book-changing event occurs through the probe time. Distinct identical-config experiment has measured zero too. API comparison n1/tied1, exact zero difference. `zero.json`, `zero-native.json`. |
| FIRSTUSE01 fully unobserved | **closed** | Same actual fully observed parent has5s horizon censored, measured shares0 and both summary values null. Comparison n0/tied0/mean null, distinct from the eligible measured-zero horizon. |

The queue probe uses a small independent compiled market-making strategy that posts a quote, reprices it, then reduces size, with a scripted competing bid. It is an actual native engine scenario of the kind explicitly permitted by the brief. It is not a claim that the built-in fixed-spread strategy naturally produced this exact command sequence in a seeded market study. Both the simulation engine and canonical builder's real shadow engine agree; projection snapshots are also checked against direct simulation-engine queries. The initial lack of ahead shares at the old price does not weaken the nonzero30-share check at the replacement's new price.

The zero scenario removes the noise taker, sets background fair-value step interval100ms, uses a one-share sweep starting1s with100ms parent duration and2s simulation end. It keeps the baseline's remaining assumptions. The bounded search allows101..112; seed101 already supplies the required actual measured zero. This is a deterministic scenario, not a profitability sample or calibrated market model.

## Cheap corroboration of affected existing flows

The main process probe is adapted explicitly from the prior independent review's helper/probes; the corrected assertions and new queue scenario are new review work. It runs fresh native simulations and interrogates their actual API/artifact outputs, rather than consuming old recorded successes or running implementation tests.

| Native scope | Status on this source | Evidence / limits |
| --- | --- | --- |
| EXEC02 ordinary residual causality | **closed, native scope** | seed102 fills483,72 trace rows, strategy-known delivered-fill accumulation reconciles row by row. cap10 seed101 fills240,78 rows,24 submitted ten-share children plus missing-touch skips. Deadline31s differs from final target30s. Exact contiguous13-row pagination. |
| EXEC03 / MM05 neutral ties | **closed, native scope** | Independently count exact-zero per-seed differences for every neutral metric and assert tiedCount equals that count; assert at least one neutral difference is nonzero and no invented better/worse counts. Source corroborates explicit positive/negative/zero classification. Fresh measured-zero case separately establishes real equality. |
| EXEC04 / FIRSTUSE02 field issue | **closed, native scope** | Zero interval preview returns valid=false with exact editable `/config/strategy/sliceIntervalNs`; preview200 is not launch success. Browser focus/error association remains outside this review. |
| MM01 request counts | **closed, native scope** | Actual slow study gives new/cancel/replace rejects3/0/505, replace reasons504 unknown_order and1 post_only_would_cross. Correct causal race separately demonstrated above. |
| MM04 eligible denominators | **closed, native scope** | Fresh baseline seed103 measured shares135665/133167/124664. Decimal total/eligible-shares matches per-share summary within six-decimal rounding; comparison includes coverage metrics. |
| VALIDATION01 persisted identity | **closed for affected-flow corroboration** | Fresh provenance digest equals API bytes, supplied source equals360ad69, omitted release null. Same old provenance survives own process restart. Prior independent distinct-actual-build check remains its own historical evidence; not repeated here or upgraded to different-release portability. |
| VALIDATION02 complete export/native verify | **closed for affected-flow corroboration** | Fresh audit contains13382 native event rows, complete inventory/download and actual corrected CLI verify exit0. Cancelled/failed packages preserve evidence and do not claim reproducible cancellation timing. Invalid chunk offset400. No browser download/unpack certification here. |
| VALIDATION03 restart-pinned cohorts | **closed for affected-flow corroboration** | Fresh retry changes latest selection, original pin stays stable across retry and own API restart. Attempt ledger includes unlaunched/cancelled/failed attempts. Prior corrupt-pin/distinct-build probes not rerun. |
| Old/new trace limits | **closed for affected-flow corroboration** | Remove own completed execution-decisions sidecar: native reproduction still returns the trace. Restore bytes afterward. New terminal partial survives restart. Limit1001 remains422. Requested row retention bounded while whole file is validated. |
| FIFO500/export oversize limits | **untested anew** | FIFO source still explicitly caps500 and flags truncation; prior independent report already distinguishes implementation-fixture corroboration. Prior symlink/2GiB rejection and giant-export checks remain historical scoped evidence. No need to claim a new large-memory or500-transition run. |
| EXEC05, VALIDATION04, MM03 and other first-use presentation | **untested here** | Parent's separate frontend reviewers own browser labels, selected sibling-fill identity, keyboard/mobile/charts/context and export handoff. Native stage/order identities are retained in evidence, without a browser clearance claim. |

## Reproducers, checks and limits

Committed evidence/probes: `docs/reviews/NATIVE_PERSONA_FINAL_RECHECK_2026-11-08/`.

From this checkout:

```sh
python3 docs/reviews/NATIVE_PERSONA_FINAL_RECHECK_2026-11-08/run.py
python3 docs/reviews/NATIVE_PERSONA_FINAL_RECHECK_2026-11-08/build_engine.py
```

Final process run: `/var/folders/99/cjkytqh130n5xq3vzsr3f17c0000gn/T/native-final-360ad69-_t77byl4`. Both phases exit0, final ALL PASS; owned servers stopped. Full raw request responses, native directories, downloaded packages and logs remain there. Compact critical evidence and logs are committed. Exact IDs, nanosecond times, quantities and hashes remain strings in captured native/API evidence.

Final engine run: `/var/folders/99/cjkytqh130n5xq3vzsr3f17c0000gn/T/native-final-engine-jszwhyvm`. One review-owned translation unit compiled serially (one compiler, below the allowed-j2 ceiling), linked read-only corrected native objects; compile exit0 with no warnings, scenario exit0. No parent output was written. Object hashes are recorded in engine-objects.json. This is actual native linkage, not an independently rebuilt entire engine/toolchain. Compiler slot released.

Probe development encountered reviewer-only assumptions: no-fill case was not on order7 and guessed order12 was nonexistent, so discovery now uses actual order listing (counterexample order24); API forbids self-comparison, so measured-zero comparison now uses a distinct experiment; canonical API has delivered market data rather than a published event kind, so midpoint corroboration explicitly checks source position and absence of intervening exchange mutations. Initial fixture compilation needed the concrete ApiJsonWriter include. Failed drafts are not counted as passing checks. Final scripts were rerun after these corrections.

No implementation tests or broad suites were used as independent clearance. No install, Docker, benchmark, delegates, performance claim, calibrated-realism claim, deployed-service access, production/schema/WORK_PLAN change or actual human test. No sanitizers, cross-platform build or new cross-build portability claim. Existing prior independent clearances remain bounded by their reports. Future parent code changes require a NEW review; this reviewer did not self-fix production or pre-clear future repairs.

Parent may use this report to close the scoped native objections and actual-scenario gaps for360ad69. Overall non-human readiness still depends on the separately commissioned frontend rechecks. Stop before actual human testing; human80% success remains unrun.
