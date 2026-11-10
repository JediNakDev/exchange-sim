# Historical LOB results

| CSV version | Original iteration | Commit | Date |
|-------------|--------------------|--------|------|
| 0.1.0 | 1.0.0 | c2edf085 | 2026-01-28 |
| 0.1.1 | 1.1.0 | 0a171e3a | 2026-02-04 |
| 0.1.2 | 1.2.0 | 753300d8 | 2026-03-01 |
| 0.1.3 | 1.3.0 | 88511ee2 | 2026-03-25 |
| 0.1.4 | 1.4.0 | 404feb54 | 2026-05-11 |

Files use `benchmark_result_<version>.csv`. The first four files retain
the original CSV contents unchanged.

`benchmark_result_0.1.4.csv` contains the single MixedWorkload result recovered
from [the console capture](../benchmark/profiling/macos_capture/bench_stdout.txt).
Its values retain the capture's displayed precision; the sample count is blank
because it was not printed. This is a transcription of a historical run,
not a new measurement or a complete suite. The original run still used the
old 1.3.0 output filename, and the tagged CSV was unchanged from 0.1.3.

The ITCH replay measurements are in [benchmark/profiling/](../benchmark/profiling/README.md).
