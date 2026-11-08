# HTTP backend efficiency and ownership refactor

Scope: `include/exsim/api/http_{message,server}.hpp`, `src/exsim/api/http_{message,server}.cpp`, `tests/exsim/api/http_tests.cpp`. The research API's HTTP control plane only. Matching, gateways, the order-entry network layer, simulation semantics and the frontend are untouched. The server keeps its blocking thread-per-connection design.

## What changed

**Header parse once per request.** `parse_http_request_head()` is the old parser up to and including the body-size check. It returns the head length and declared body length, with the same validation and the same rejection order, and copies no body. `parse_http_request()` keeps its signature and result, and now calls the head parser and then copies the body. `HttpParseResult` gains `body_bytes`. The connection loop calls the head parser until it succeeds, remembers the head and body lengths, and on later reads checks only whether the body is complete. Before, every 16 KiB read of an incomplete body reset the `HttpRequest` and re-parsed the request line, query, and all headers.

**No front erase per pipelined request.** Serving a request advances `request_start` inside the receive buffer. Consumed bytes are dropped once, right before the next `recv`, and only the unserved partial request is moved. Before, each pipelined request did `erase(0, n)` on the remaining bytes.

**Connection slots instead of mutex plus list.** `start()` allocates `max_connections` slots. Only the accept thread, and `stop()` after joining it, claim, join and close slots, so the lock and `std::list` are gone. A connection thread never closes its own descriptor. That keeps `stop()` from shutting down a descriptor number the kernel has already reused. At exit, the connection thread calls `shutdown(SHUT_WR)`, so the peer sees FIN right away instead of up to 100 ms later at the next reap.

**Lifecycle fixes:**

| Defect in the baseline | Now |
|---|---|
| A second `start()` overwrote the listen descriptor, which leaked it, and assigned to a joinable `std::thread`, which calls `std::terminate`. | It throws `std::runtime_error` and leaves the running server unchanged. |
| If thread creation in `start()` failed, the listen socket leaked. | Any failure after `socket()` closes it and rethrows. |
| If creating a connection thread failed, `std::system_error` escaped the accept thread (`terminate`), the list held an entry that never finished, and the active count stayed high. | The slot is released, the count is restored, and the client gets 503. |
| A handler that threw a non-`std::exception`, a throwing rejection responder, or a `bad_alloc` in a connection thread called `terminate`. | A non-standard handler exception becomes a 500. Any other exception drops only that connection. The accept-thread 503 path is guarded too. |
| `send()` returning 0 looped forever. | It counts as failure. |
| `stop()` keyed off the `stopping_` flag. | `stop()` keys off the running accept thread; it is a no-op when not running, and start, stop, start is tested. |

Unchanged on purpose: the order of validation and rejection, early 413/431/501 on the head before any body is read, limits, keep-alive and request-count rules, 408 for an incomplete request at idle timeout, 503 when over capacity, response serialization, and the public API (additions only).

## Evidence

The same new server tests were run against the baseline sources (`4200598`), with only the two new parser tests removed because they call the new function. Full log: `/tmp/exsim_http_refactor_old_code_tests.log`.

| Test | Baseline | Refactor |
|---|---|---|
| `fragmented_body_parses_the_head_once`: allocations on the connection thread while reading one request, 16 B body vs 512 KiB body (32+ reads) | 7 vs **173**, fails `<= small + 16` | 7 vs **13** |
| `restart_and_failed_start_leave_no_descriptors` | aborts (`terminate` on second `start()`) | pass |
| `throwing_handler_or_responder_drops_one_connection_only` | aborts (uncaught `int`) | pass |
| fragmented pipelines, early rejection, slot capacity and reuse | pass | pass |

The allocation test runs on the server's own connection thread. The first request's handler opens a thread-local `AllocationScope`, and the second request's handler reads it. The count therefore covers exactly the receive and parse work for one request. In the refactor, the 6 extra allocations for a 512 KiB body are receive-buffer doublings and the body copy, and the count grows with log(body). In the baseline, it grew by about 5 allocations per read, from re-parsing the head. Refactor measurement: `/tmp/exsim_http_refactor_new_code_allocation_measurement.log`. No timing speedup is claimed.

New tests:

- the head parser stops before the body, and head and full parsing give the same rejection precedence;
- fragmented and byte-at-a-time pipelines keep request boundaries;
- an oversized, malformed or Transfer-Encoding head is rejected on arrival, well before the 5 s idle timeout, including after a served request;
- the connection-thread allocation regression above;
- 4 rounds of filling 2 slots, a 503 over capacity, release and reuse;
- duplicate start, failed start on a taken port, double stop and restart, all with no change in open descriptors;
- a throwing handler or rejection responder affects one connection only.

## Checks (all `-j1`)

- Release build: `/tmp/exsim_http_refactor_release_build.log`. HTTP tests **17/17**: `/tmp/exsim_http_refactor_release_http_tests.log`. Server tests repeated 20 times, 0 failures: `/tmp/exsim_http_refactor_release_http_repeat20.log`.
- Release API consumers (`api_` 69/69, `execution_api` 26/26) and the existing allocation guards (engine, journal, FIX, FIXP/SBE, OUCH, market data, network, server core, `server_e2e_allocation` single test, research, bots, order book): all pass. Log: `/tmp/exsim_http_refactor_release_api_and_alloc_guards.log`.
- ASan/UBSan (`MODE=debug`, warning-free build `/tmp/exsim_http_refactor_debug_asan_build.log`): HTTP tests **17/17** (`/tmp/exsim_http_refactor_debug_asan_http_tests.log`), server tests repeated 10 times with 0 failures (`/tmp/exsim_http_refactor_debug_asan_http_repeat10.log`), and API consumers `api_` 69/69 and `execution_api` 26/26 (`/tmp/exsim_http_refactor_debug_asan_api_tests.log`). The allocation regression skips itself under ASan by design.
- Not run: the full E2E suite, benchmarks and TSan. This is HTTP evidence only and does not clear the whole system.

## Limitations

- While the head is still incomplete, each read rescans the head from its start. This costs at most `max_header_section_bytes` (16 KiB) of `find` per read, with an idle timeout between reads. That timeout is not an absolute request deadline; a client that continually supplies bytes can extend the request lifetime. An offset resume would have to repeat the limit logic in the server.
- After a large body, the receive buffer keeps its capacity for the rest of that connection. It is bounded by the limits plus one 16 KiB read. The buffer is not reserved up front from `Content-Length`, so a client that only declares a body cannot make the server allocate it.
- The path where thread creation fails is reviewed but not exercised by a test. Forcing `std::thread` creation to fail would need process limits that would disturb the test runner.
- The descriptors are not `FD_CLOEXEC`, as in the baseline. The API process does not spawn children.
- An `accept()` failure such as `EMFILE` still retries on every `poll` wake-up, as in the baseline.

## Parent review and integration

Integrated delegate `bac7ec9` as `9254238`, preserving both sides of the append-only WORK_PLAN conflict. Parent reviewed request framing, size arithmetic, buffer-view lifetime, slot ownership and join ordering. One additional failure-path finding: `std::thread` construction may throw `std::bad_alloc` as well as `std::system_error`; catching only the latter still lets an allocation failure terminate the accept thread. Parent changed the creation guard to catch all exceptions and restore the slot/count before the existing guarded 503/close path. Also placed the new `body_bytes` member after the existing aggregate fields to retain existing three-field aggregate initialization source compatibility. No external binary ABI compatibility is promised.

Parent independently compiled the actual integrated HTTP sources with the repository warning flags, once in release and once under ASan/UBSan, using `tests/main.cpp`, `tests/framework/alloc_counter.cpp`, `tests/exsim/api/http_tests.cpp` and the two HTTP sources. Both builds were warning-free and both runs passed 17/17 HTTP tests. Logs `/tmp/exsim-parent-http-{release,debug}-{build,tests}.log`, binaries `/tmp/exsim-parent-http-{release,debug}`. Sanitizer allocation-count test skips by design. Thread-creation failure itself remains reviewed rather than fault-injected. These focused parent checks establish HTTP readiness within scope; whole-simulator integration is still pending the other workstreams.
