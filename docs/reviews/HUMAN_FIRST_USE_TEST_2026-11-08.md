# Human first-use validation packet

Status: READY FOR HUMAN EVALUATION on frozen frontend75cf28f/native360ad69. All reported current-workflow findings are closed within documented expert scopes. Actual human results: NOT RUN. This is a local test release, not a production deployment.

## Participant task sheet

Start from the experiment library with a clean browser profile. Do not read docs or ask for click instructions. Think aloud about what you expect each action to do. All activity must use a separate local test environment, never installed research data. The facilitator records timing, wrong turns and assistance without coaching.

1. You need to buy 500 shares. Compare a TWAP schedule with immediate execution over the same three market seeds. Explain completion, unexecuted quantity and cost. Decide whether either result is preferable and why. Launch the first useful experiment within five minutes; comparison and one explained fill within fifteen minutes.
2. Investigate why one parent order did not finish. Also shorten a simulation so it ends before its execution window, and explain whether the result still belongs in the same comparison. Find the responsible schedule decisions and an associated child-order lifecycle. Distinguish a completed simulation from a fully observed execution window and a fully filled parent.
3. Test whether widening market-making quotes improves results. Change only quote width across common seeds, compare net P&L, inventory risk and post-fill price movement. Explain one fill with the evidence available to the strategy at the time.
4. Test an inventory-aware market maker and a change in feed or cancel latency. Explain which assumption changed, what improved/worsened and what the synthetic market cannot establish.
5. Save the evidence another researcher needs to reproduce a result. Explain exact versus derived values, unavailable observations, paired sample counts, changed assumptions and any excluded or failed work.
6. Find an active/paused run, step it, resume it and recover from an injected temporary connection error without duplicating work. Return to the originating experiment and compare it with its variant.

## Facilitator scoring sheet

Freeze the final build, API binary identity, dataset and protocol before recruiting participants. Keep the answer key separate from the task sheet. Recruit representative execution and market-making researchers unfamiliar with this UI. Record each participant separately; do not average away failure of a critical task.

| Capability | Critical | Found unaided | Completed unaided | Interpreted correctly | Seconds / wrong turns / assistance |
| --- | --- | --- | --- | --- | --- |
| Choose execution task and configure valid parent/seeds | Yes | | | | |
| Review assumptions and launch | Yes | | | | |
| Create a variant preserving market assumptions | Yes | | | | |
| Select baseline and candidate in the intended direction | Yes | | | | |
| Interpret completion, residual, cost scope and signs | Yes | | | | |
| Recognize censored observation windows | Yes | | | | |
| Explain residual using schedule and child evidence | Yes | | | | |
| Inspect fill lifecycle and delivered versus exchange view | Yes | | | | |
| Compare market-making quote width | Yes | | | | |
| Interpret P&L, fees, inventory risk and markout eligibility | Yes | | | | |
| Configure inventory-aware skew | No | | | | |
| Configure one latency sensitivity variant | No | | | | |
| Interpret paired samples, dispersion and confounding assumptions | Yes | | | | |
| Locate failed/missing work and recover safely | Yes | | | | |
| Pause, step and resume a run | No | | | | |
| Export provenance and reproduce/share evidence | Yes | | | | |
| Recognize synthetic-world limits and unavailable features | Yes | | | | |
| Navigate results back to experiment and comparison | No | | | | |
| Change timing units without changing assumptions | No | | | | |
| Use keyboard navigation for key tasks | No | | | | |

Success requires at least 16 of these 20 capabilities found, completed and interpreted unaided, and every critical capability passing. Assistance means that capability did not pass unaided. Record observed behavior and the participant's interpretation, not the facilitator's guess. Timing targets are additional product targets, not a substitute for correct interpretation. Do not count unavailable/unimplemented capabilities as passed; readiness requires addressing or explicitly revising the agreed scope before the denominator is frozen.

Human results: NOT RUN. Frozen identities, verified reference outcomes, fixture preparation and error-injection protocol are in the separate HUMAN_FACILITATOR_KEY_2026-11-08.md. Give participants only the task sheet above. Do not expose that key or reference outcomes before scoring.

## Isolated session setup

The facilitator can start a verified copied release with a brand-new participant directory:

```sh
python3 scripts/start_human_review.py --release output/releases/human-review-75cf28f --session output/human-participant-02-75cf28f
```

The default participant URL is `http://127.0.0.1:46072/experiments`; API is loopback46073. Use a new directory for every participant, and different explicit ports for simultaneous sessions. The launcher verifies release inventory and Node version, refuses an existing session directory, checks direct/proxied readiness, records identities/logs/data under that directory, and stops only its owned services on Ctrl+C while preserving records. `session.json` intentionally says human_result NOT RUN; score the actual participant separately.

Participant01 is already running at http://127.0.0.1:46072/experiments with an empty experiment library, new data `output/human-participant-01-75cf28f/data` and retained session logs/identities in its parent directory. The launcher is supervised by the current tool session; if it is no longer running, start a NEW session directory with the command above. Do not reuse a participant directory. Stop the current session using the supervisor PID recorded in the facilitator key, or Ctrl+C in its owning terminal. This stops owned test services and preserves evidence.

No participant has used this environment. Start a clean browser profile, start the timer only when the participant begins, and keep participant success separate from automated readiness. Expert/reference services have been stopped and their data retained. No installed production service was changed.
