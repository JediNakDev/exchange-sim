# Research product walkthrough

[Watch the MP4](exsim-product-walkthrough.mp4) · [Chapter captions](walkthrough.vtt)

The silent walkthrough records the production Next.js frontend connected through Tailscale Serve to the real C++ research service. Every experiment, chart, event and fill shown comes from an executed synthetic simulation. It does not use the mock service or fabricated chart data.

| Approximate time | Scene |
| --- | --- |
| 00:00 | Experiments and completed seed runs |
| 00:03 | Configure a quoting experiment, feed delay and assumptions |
| 00:11 | Start paused, step 1,000 scheduler events, resume to completion |
| 00:17 | Equity, inventory, drawdown and fixed-point accounting |
| 00:22 | Event stepping, a fill's order lifecycle and exchange/strategy views |
| 00:31 | Compare three common seeds and expose the changed quote width |
| 00:39 | Paired metric differences, dispersion and per-seed drill-down |
| 00:45 | Inspect the manifest and return to the summary |

The comparison changes only `strategy.halfSpreadTicks`, from 1 to 2, across seeds 1, 2 and 3. In this particular synthetic experiment the wider quotes have lower net P&L on all three seeds. Three seeds and an uncalibrated synthetic market do not establish a live trading result.

## Screens

[![Paired comparison](paired-comparison.png)](exsim-product-walkthrough.mp4)

![Run accounting and charts](run-summary.png)

![Fill lifecycle and the delivered strategy view](fill-lifecycle.png)

![Experiment configuration and assumptions](experiment-configuration.png)

![Paused run after stepping scheduler events](paused-run.png)

## Capture and checks

Captured on 2026-11-06 at 1440 x 900. The UI was driven in an isolated Chromium session after the preview bridge and desktop browser surfaces were unavailable. Navigation, form entry, run controls, event stepping and fill selection used actual rendered controls. The final pass completed all eight scenes, with 57 API responses, zero failed API responses and zero browser exceptions. The MP4 was decoded end to end after encoding.

The Next.js production build enables standalone output. The C++ research source is the validated integration at `edc13b0`; this publishing change does not alter simulation or matching behavior. Host-specific Tailscale addresses, property lists, logs and run artifacts are not included in these assets. Reproduction and service management: [DEPLOYMENT.md](../DEPLOYMENT.md).
