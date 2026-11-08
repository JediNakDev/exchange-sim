# Server runtime independent review

Original target: `12a349d`, server runtime merged in `e9495a0`, including gateway QA merges `6227a51` and `3df3527`. Continued in the original checkout `/Users/jedi/.t3/worktrees/lob/t3-6663db04`, branch `t3/review-exsim-server-runtime`. Parent fast-forwarded through `4440d8f`; those intervening changes are coordination documentation only. This remains the original review, not a review of the later FIXP/SBE runtime. SBE is excluded.

Report-only ownership: reproduction tests and this report. No production fixes, public API changes, pushes or parent merges.

## Findings

| ID | Severity | Status | Finding |
| --- | --- | --- | --- |
| REVIEW-SRV-01 | Medium | CONFIRMED | Newline-terminated oversized admin commands bypass the 4096-byte limit and execute. |
| REVIEW-SRV-02 | Medium | CONFIRMED | Fixed-rate benchmark excludes missed send-deadline delay from both latency distributions. |
| REVIEW-SRV-03 | High | CONFIRMED | Synchronous journal flush can block the only reactor thread without a deadline. |
| REVIEW-SRV-04 | High | CONFIRMED | MarketDataFeed accepts forged retransmission packets from an unconfigured UDP sender. |
| REVIEW-SRV-05 | Medium | PLAUSIBLE | Unbounded retransmission request draining can starve other reactor work under sustained flooding. |

### REVIEW-SRV-01

Where: `include/exsim/server/runtime/admin_endpoint.hpp:46-64`.

Complete lines go directly to `core.execute_line` before their size is checked. The only size check examines the unfinished tail after all complete lines have been erased. Sending 5000 spaces followed by `bots off\n` in one loopback write returns `OK` and executes the command. Behavior depends on TCP segmentation: the same command can be rejected if a read ends before its newline. This violates the endpoint's stated maximum line length. The example is bounded and does not demonstrate unbounded memory growth.

Reproducer: `server_runtime_review.oversized_complete_admin_line_is_rejected`. Expected `ERR line too long`; observed `OK`. Fix: check every line's length before whitespace trimming or execution, and apply the same limit to incomplete tails. Close using the existing flush path.

### REVIEW-SRV-02

Where: `tools/exsim_w2w_bench/main.cpp:164-169,179-184,315-340`; claims in `docs/SERVER_RUNTIME.md` section 5 and README's wire-to-wire discussion.

A fixed-rate order has an intended deadline `start_ns + sent_orders * interval_ns`. The recorder retains only a timestamp immediately before encoding/queueing the actual send. A client pause before that timestamp is absent from the distribution; the 50000-outstanding gate also defers arrivals. The tool catches up later, but latency measured from actual sends does not include the wait against the intended schedule. This is coordinated omission relative to an open-loop fixed-rate workload. Actual-send response latency remains a useful metric, with that limitation.

Reproducer: `server_runtime_review.fixed_rate_latency_includes_missed_send_deadline` includes the actual benchmark recorder in the test translation unit, inserts a 100 ms pre-send scheduling pause, and calls its actual accept callback. Observed scheduled latency 110338000 ns and reported latency 0 ns in the final release log. This is a deterministic bookkeeping demonstration, not a new end-to-end performance measurement. No benchmark throughput or tail numbers were gathered under the shared-host build load.

Fix: keep intended and actual send timestamps, report both schedule-to-response and actual-send-to-response, and quantify late/missed sends. Document that actual timestamps precede encoding, send-buffer queueing and flush, while response timestamps occur in decode callbacks, rather than at NIC or kernel boundaries.

Related checks: CLOCK_MONOTONIC is used on the client for both ends. Warmup exclusion uses the order index, which is correct. ITCH Add matching by ordinal is valid for the configured single client, no bots, all-resting accepted buys and loss-free delivery. It is not safe after an order rejection or declared MD loss; the tool can then time out because completion still demands the original number of Adds/cancels. Its printed reject/loss counters are necessary validity conditions. The measured clock on macOS has microsecond-scale quantization visible in the raw results; nanosecond units are not proof of nanosecond resolution. Claims attributing tails solely to scheduling or stating that the server was not saturated are inferences, not established by this benchmark's counters.

### REVIEW-SRV-03

Where: `include/exsim/server/server_runtime.hpp:129` invokes `core.timer`; `include/exsim/server/exchange_core.hpp:273-277` flushes the journal synchronously; `src/exsim/sim/journal.cpp:10-15,35-45` uses blocking `write_all`. Append buffer exhaustion and shutdown also flush synchronously.

Journal paths accept FIFOs and descriptor paths. The reproducer opens a pipe-backed journal, fills the pipe after server startup, then executes one busy-poll reactor iteration. Its periodic journal footer write never returns while the reader remains open without consuming. The child must be killed after the bounded 300 ms observation period. This stalls order entry, admin, timers and signal-requested shutdown alike. The 3-second session drain timeout cannot run while blocked in write. Regular-file storage stalls have the same synchronous architectural risk, although this test deliberately uses a pipe for reproducibility. The core/journal code itself was reviewed separately; this finding concerns its use on the runtime's sole reactor thread.

Reproducer: `server_runtime_review.journal_backpressure_does_not_block_reactor`. The child reports that setup and pipe filling completed before the parent starts its deadline. It exits with `_exit` when successful; the parent always reaps it. Fix: decide and document the persistence contract. Reject unsupported special-file destinations at startup, and if the runtime requires bounded service time with journaling enabled, use a preallocated bounded queue to a persistence worker with an explicit overload policy. Rejecting special files alone does not make regular-file I/O nonblocking.

### REVIEW-SRV-04

Where: `include/exsim/client/exchange_client_sessions.hpp:237-247`.

The wrapper reads both its multicast/unicast feed socket and its dedicated request socket, obtains the peer endpoint, then discards it. Thus any local UDP sender that knows the request port and public session label can insert a MoldUDP64 packet at the expected sequence, including before any actual request. Subsequent authentic messages at that sequence are treated as duplicates. This is the same class of problem previously fixed in the standalone MD client (`5ee5097`), but that fix does not cover this reusable wrapper used by tools and E2E tests. The request socket is loopback-bound, limiting this particular path to local senders.

Reproducer: `server_runtime_review.retransmission_socket_ignores_unconfigured_sender`. Configure trusted endpoint A, send a sequence-1 data packet from endpoint B to `feed.request_fd()`'s local endpoint. Observed one delivered message and next sequence 2, expected zero and 1. A one-byte payload suffices because the transport wrapper passes arbitrary Mold payloads to its handler. Fix: on the request socket, require both source address and port to match `retransmission_server_` before passing packets to the receiver. Keep feed-source policy separate because multicast feed source ports need not equal the destination port.

### REVIEW-SRV-05

Where: `include/exsim/server/runtime/market_data_runtime.hpp:162-180`.

`handle_retransmission_requests()` drains until EAGAIN, including continuing on oversize datagrams. There is no per-iteration packet or time budget. A sustained stream that keeps the socket readable can therefore hold the reactor inside this handler, delaying order entry, shutdown and timers. Similar drain-to-empty loops exist in client-side UDP pumping and TCP accept. This is a static finding, not a demonstrated starvation failure on this host. No flood or throughput experiment was started under the shared-host resource restriction.

Concrete follow-up recipe: continuously send valid session/sequence-1/max-count requests to the retransmission socket from several producers while issuing timed `status` and shutdown requests on another connection. Check timer and admin progress rather than merely total UDP throughput. Fix: cap requests processed per reactor turn; level-triggered readiness will bring the socket back on the next poll. Keep the existing per-response count and payload bounds. Deferred until a controlled resource slot is available.

## Verification and results

The opt-in failures are gated by `EXSIM_RUN_SERVER_REVIEW_FAILURES=1`. Without it those four test bodies return immediately; their normal `ok` entries must not be interpreted as defect fixes. The generation boundary check and two `server_runtime_checks` tests always run.

Commands:

```
./build/release/run_tests server_runtime_
EXSIM_RUN_SERVER_REVIEW_FAILURES=1 ./build/release/run_tests server_runtime_review
```

Release review checks: 7/7 normal entries pass, including the generation boundary, duplicate-login/binding/slot-reuse and busy-poll day-end tests. Explicit reproducers: four expected failures, generation check passes. Logs: `/tmp/exsim-review-server/checks-release.log`, `/tmp/exsim-review-server/reproducers-release-final.log`.

Recovered original evidence: `/tmp/exsim-review-server/build_release.log` ends in exit 0. The preserved baseline is `/tmp/exsim-review-server/run_tests_release_baseline`. All 30 original release E2E iterations pass 12/12: `/tmp/exsim-review-server/flaky/release_run_1.log` through `release_run_30.log`. Reused because the parent integration changed no runtime code. No release flake was observed.

Final sanitizer gates: focused ASan/UBSan 12/12 existing E2E tests, focused TSan 12/12 existing E2E tests, with no sanitizer diagnostics. Logs: `/tmp/exsim-review-server/focused-debug.log`, `/tmp/exsim-review-server/focused-tsan.log`. Normal review entries and always-on checks pass 7/7 under each: `/tmp/exsim-review-server/checks-debug.log`, `/tmp/exsim-review-server/checks-tsan.log`. Four opt-in failing bodies are skipped in those normal runs.

Debug flakiness: all 10 ASan/UBSan E2E repetitions pass 12/12, `/tmp/exsim-review-server/flaky/debug_run_1.log` through `debug_run_10.log`. No debug flake was observed. Focused binaries are `build/review-debug/run_tests` and `build/review-tsan/run_tests`; modes retain the Makefile's sanitizer flags.

Process checks: SIGINT and SIGTERM after the running log both produce orderly end-of-day output and exit 0 with no connected clients: `/tmp/exsim-review-server/signals-final.log`. An initial signal harness used buffered readline with select and timed out despite server startup; it was corrected to use raw descriptor reads. The initial harness log `/tmp/exsim-review-server/signals.log` is not a server failure.

Linux `g++ -Werror` attempt: `/tmp/exsim-review-server/linux-gcc.log` fails while compiling unrelated fuzz regression tests because my temporary source copy omitted `fuzz/targets/fuzz_support.hpp`. This is a validation setup failure, not a production finding. Linux verification of the new tests remains incomplete. No additional Docker build was started after the parent requested resource coordination. The original integrated runtime's Linux evidence is recorded in WORK_PLAN, but is not a substitute for compiling these new tests. A later focused Linux gate should copy the whole repository source excluding build/output/.git and override TEST_SRCS with tests/main.cpp, tests/framework/alloc_counter.cpp, the three server/e2e .cpp files and this review .cpp file. Original broad sanitizer builds were stopped in favor of focused binaries containing the existing three E2E translation units, framework, review tests and production library. No production source or existing E2E file was edited. Every new build uses `-j4`. Host build contention is not latency evidence.

## Checked behavior and limits

Validated by existing E2E tests: OUCH and FIX order entry, replace/cancel, cross-protocol trades, bots, MD depth reconstruction with drop/dup/reorder and retransmission, admin phase/book/status, reject and short delay ordering, OUCH reconnect/drop-out recovery, FIX ResendRequest, admin disconnect cancellation, cancel-on-disconnect enabled and disabled, session/feed end-of-day, and canonical journal replay count/hash equality. These claims apply to those scenarios, not to every possible hostile input.

Additional executable checks here: second OUCH login rejected without disrupting first binding, first connection's order canceled on disconnect, reconnect on a reused slot, busy-poll trading-day termination, and connection-ID roundtrips at the transport's maximum generation. The suspected generation truncation was dismissed: `TcpServer` retires slots at `0x00ffffff`, so larger transport generations cannot occur.

Static tracing: peer close, read error, receive-buffer full and send-buffer overflow call the transport close callback, which runs endpoint `finish_close` and gateway `on_closed`. Gateway-requested closes also call `finish_close`. Stale transport callbacks compare generations. Partial FIX tails stay buffered; OUCH framing keeps its own state. Delay marks are bounded at 256, merge overflow into the newest mark using the later deadline, and release in FIFO order. Listener port 0 resolves via getsockname; MD destination port 0 disables sends without disabling the retransmission ring. Shutdown first closes phases through the core, sends gateway end-of-day messages and MD end-of-session, then progresses connection draining. Signal handlers only set an atomic flag; main requests shutdown at the next loop iteration. Journal failure state causes the server binary to return 1. Retransmission server parsing bounds malformed/count-zero/sequence-zero/wrong-session requests and clips response size and future ranges; a range beginning outside the retained ring gets no answer.

The existing allocation test counts on the server thread using a thread-local guard and acquire/release handshakes; client `std::function`, strings and vectors are outside that guard. The traffic includes OUCH/FIX trades, replaces/cancels, bots and MD. Gateways, buffers, packet-fault storage and session state are preallocated. No new definite heap allocation in the counted hot path was identified. This does not close these coverage gaps: no explicit counted reconnect, saturated 256-delay-mark queue, fault-active retransmission traffic, guaranteed heartbeat/timeout interval, or call-phase NOII. Allocation counters are disabled under sanitizers. The test's post-warmup design also does not establish the strict claim of zero allocations starting at the first poll.

Unproven stress cases: many-connection saturation, socket-level partial writes and a persistently slow consumer, cancel-on-disconnect on every kernel error, saturated delay marks with fragmented FIX frames, malformed/old/future/huge retransmission requests through the actual runtime socket, runtime request floods, and journal failure injected through the server process. These remain coverage gaps, not passing claims. SBE remains out of scope. Signal process checks cover idle shutdown without clients; signal-time active-session drain and persistence-failure interaction remain untested. The report makes no new tuned performance claims.

## Integration handoff

All confirmed defects remain open, by report-only instruction. No production fixes or public API changes. The parent should route the endpoint and MD-wrapper fixes to their owners and decide the journal persistence and benchmark reporting contracts. The test file includes the benchmark tool implementation with its main renamed so it can test the real recorder; future benchmark changes can require corresponding review-test updates. Do not run the opt-in failures as a normal green gate until fixes land. REVIEW-SRV-05 is deferred and unconfirmed.
