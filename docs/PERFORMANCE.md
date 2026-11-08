# Historical performance evidence

Retained from the previous README and its linked reports. These measurements were not repeated for the production deployment or product recording. Hardware and load descriptions below refer to those historical runs.

Matching engine, `exsim_bench --ops 2000000`, perf build (`-O3 -march=native
-flto`), Apple M6 macOS, zero allocations in every workload. The production
engine (C, index arena) and the previous one (B, sorted level vectors) were
alternated three times in one session under the same background load, so the
comparison is fair but the absolute numbers are pessimistic. Median of three:

| Workload | C: Mops/s | C: mean ns | C: p99 ns | B: Mops/s | C / B |
| --- | ---: | ---: | ---: | ---: | ---: |
| passive_add | 11.91 | 84 | 291 | 6.61 | 1.80x |
| cancel | 7.59 | 132 | 458 | 4.28 | 1.77x |
| aggressive | 12.99 | 77 | 291 | 10.38 | 1.25x |
| mixed | 13.11 | 76 | 333 | 7.23 | 1.81x |

On a quieter machine the variant author measured 15.9, 9.0, 16.6 and 17.5
Mops/s for C. A third variant (A, tick ladder) was also benchmarked against C
over seven alternating rounds: C was 1.10x to 1.19x faster on passive,
aggressive and mixed flow and A about 5% faster on pure cancels. Layouts,
measurements and rejected optimizations are recorded in
[docs/engine-variants/](engine-variants/).

Wire codecs, `exsim_codec_bench`, same perf build and machine, one message at
a time as the gateways call them, 256 distinct inputs per case, every decoded
field and encoded byte forced to memory, median of 7 rounds of 2,000,000
messages (three runs agreed within 0.2 ns, and within 1 ns for FIX), zero
allocations:

| Path | Message | Bytes | ns/msg |
| --- | --- | ---: | ---: |
| inbound order | OUCH 4.2 EnterOrder in a SoupBinTCP packet, decode | 52 | 6.0 |
| inbound order | SBE NewOrderSingle in a SOFH frame, decode | 51 | 4.0 |
| inbound order | FIX 4.4 NewOrderSingle, framing, checksum and decode | 162 | 228 |
| outbound fill | OUCH 4.2 OrderExecuted in a SoupBinTCP packet, encode | 43 | 3.6 |
| outbound fill | SBE ExecutionReportTrade in a SOFH frame, encode | 89 | 2.1 |
| outbound fill | FIX 4.4 ExecutionReport, encode | 220 | 155 |
| market data | ITCH 5.0 AddOrder, encode | 36 | 2.1 |
| market data | ITCH 5.0 OrderExecuted, encode | 31 | 1.4 |
| market data | ITCH 5.0 AddOrder, decode | 36 | 2.7 |

The binary protocols cost a few nanoseconds per message. FIX costs 38 to 74
times more than OUCH or SBE for the same order or fill, which makes it the
codec worth optimizing.

Wire to wire, `exsim_w2w_bench`: the real server runtime on its own thread
(OUCH, FIX and admin listeners, the ITCH feed to a local UDP socket, no bots,
no journal) and an OUCH client on another thread of the same process, over
loopback. The client enters a buy order at a fixed rate, cancels it when it
is accepted, and times on its monotonic clock the Enter Order write until
the Order Accepted read (ouch accept) and until the matching ITCH Add Order
arrives on the feed (itch add). Same Apple M6 Mac mini, perf build, macOS
27, no CPU pinning (macOS has none), load average 2.5 to 7 from other work,
microseconds:

| Run | Server poll | ouch accept p50 / p99 / p99.9 | itch add p50 / p99 / p99.9 | Orders/s |
| --- | --- | --- | --- | ---: |
| 1,000 orders/s, 20,000 orders | blocking | 66 / 3,252 / 7,490 | 61 / 3,190 / 7,442 | 1,000 |
| same | busy poll | 63 / 783 / 3,841 | 57 / 764 / 3,811 | 1,000 |
| 20,000 orders/s, 100,000 orders | blocking | 54 / 1,212 / 3,363 | 44 / 1,010 / 2,925 | 19,997 |
| same | busy poll | 54 / 1,596 / 3,932 | 45 / 1,368 / 3,393 | 20,000 |
| as fast as possible, 32 in flight | blocking | 387 / 488 / 691 | 323 / 424 / 595 | 76,126 |
| same | busy poll | 384 / 484 / 772 | 319 / 424 / 603 | 76,258 |

No run lost a feed message, needed a retransmission or saw a reject. The
medians sit at the machine's floor: a plain 64-byte loopback TCP round trip
through a blocking echo thread measured 63 us p50 and 79 us p99 in the same
minute, so the macOS network stack and thread wake-ups dominate, not the
exchange (whose codecs and matching cost well under a microsecond per
order). The millisecond tails are scheduling on a shared, unpinned machine.
The windowed rows measure throughput: with 32 orders in flight latency is
queueing, and the single-threaded client, not the server, is the likely
limit. Tuned Linux numbers (pinned cores, isolated CPUs) are still to be
measured; full method and p90 and max columns are in
[SERVER_RUNTIME.md](SERVER_RUNTIME.md) section 5.


These are historical measurements from their documented setups. The production browser walkthrough does not establish a new latency or speedup claim. Current correctness gates and source revisions are in [WORK_PLAN.md](WORK_PLAN.md).
