# Research daemon processes

Each active UI run has two independent C++ worker processes:

```text
Frontend -> HTTP -> exsim_apid
                      | control and status (UDS)
                 exsim_exchanged
                      | observations and strategy actions (UDS)
                 exsim_traderd
```

`src/exsim_apid/`, `src/exsim_exchanged/`, and `src/exsim_traderd/` contain
separate executable entry points. Trader callbacks and exchange worker control
runtimes also live in their respective daemon directories. Shared code lives in `lib/src/`; public
headers, strategy classes and pure protocol adapters live in `lib/include/`.
The old `exsim_api` build target is a compatibility copy of `exsim_apid`.
Always distribute `exsim_exchanged` and `exsim_traderd` beside the API binary.
The CLI no longer offers an in-process research fallback.

The API supervises exchange workers and owns PostgreSQL metadata and
idempotency. Each exchange worker creates, owns and reaps its trader process.
`--max-live-runs` bounds run pairs, including paused runs. A cold inspection
worker can reconstruct completed artifacts without a live trader; reproduction
that actually reruns a strategy creates a fresh trader process.

## Execution and transport

The four existing strategy classes execute only in the trader daemon during
production research runs. The exchange owns matching, background market agents,
accounting, canonical recording and the existing simulated-time scheduler.
Test-only `Simulation<Strategy>` remains an independent in-process reference.

The exchange advances the existing event queue until a strategy callback is
due. It sends the callback and delivered strategy-visible state to the trader.
The trader invokes its strategy class and encodes its actions back over the
socket. The exchange acknowledges each action and sends updated visible state
before the next action. A callback-complete frame lets scheduling continue.
This preserves the previous enqueue order, latency model, pause/step semantics,
risk checks, diagnostics and random seed derivation. Risk/admission checks and
the authoritative own-order view remain in the exchange host; the trader only
receives the causally delivered view, never matching-engine state.

The transport is an inherited Unix-domain stream socket pair. There is no
shared pointer, C++ object layout serialization, fixed worker port, or path
lookup per message. Library codecs accept domain structs and produce bounded
byte frames. Integers are explicit big endian, booleans are validated, frames
are length-prefixed and limited to 1024 payload bytes. Visible orders are sent
as separate bounded frames, not an unbounded aggregate allocation. Only startup
allocates strategy order storage. A versioned startup handshake carries the
built-in strategy parameters, risk capacity and derived RNG seed input.

This research synchronization protocol is not standard OUCH/ITCH framing.
It retains research-specific observations, risk responses and diagnostics
without changing their meaning. Existing OUCH/FIX/FIXP and ITCH codecs remain
reusable libraries under `lib/include/exsim/protocol/`; the separate real-time
`exsim_server` and its connectivity tools retain their exchange protocols.
There is no arbitrary/custom strategy loader or protocol selector in research.

Active message transfers have ten-second deadlines. Idle traders wait for the
next callback without a deadline, so pausing a run does not kill the trader.
A trader exit, truncated frame, protocol mismatch or timeout fails the run;
there is no in-process fallback. The exchange closes, kills and reaps its
trader on run teardown, including failures. API control traffic is separate
from the trader/exchange event socket. Suspending the API does not halt an
already running exchange/trader pair.

Build provenance records the API, exchange and trader executable SHA-256
values. The child checks its release directory and verifies the immutable
provenance before acknowledging startup. Queued restart recovery must match
all executable identities.

## Ownership and publication

The API commits a launch reservation before spawning a writer. The child
validates its actual executable and release provenance against the immutable
manifest before activation. A reservation is never silently returned to queued
after execution may have started. Failed runs require a new run ID to retry.

The private control descriptor is fd 3. The artifact-root ownership descriptor
is deliberately inherited as fd 4. Other application descriptors are closed on
exec, including the API's database session. Database credentials are removed
from the child's environment. This is process isolation, not a security sandbox.

The inherited file lock keeps a new API from acquiring the artifact root while
an orphaned worker can still write it. On parent death, control-channel EOF
stops the worker. Startup allows up to two seconds for that cleanup; an orphan
that is stopped or unresponsive retains ownership and causes startup to fail
rather than allowing a competing writer. This is a local single-owner fence,
not a distributed lease protocol.

Workers save state changes to `.worker-record.json`. That file is diagnostic,
not authoritative metadata. The API publishes acknowledged snapshots to
PostgreSQL or, in legacy mode, to `run.json`. Imported original metadata is not
overwritten by the child. Final artifacts and directories are flushed before
the child acknowledges completion. The API commits a terminal snapshot before
returning terminal state to an HTTP reader, so an immediate audit reads the
same committed result. PostgreSQL audit exports synthesize authoritative
`run.json`, including its size, hash and chunked contents.

## Failure behavior

Pause, step and cancel retain their synchronous event-boundary semantics.
The API durably reserves the idempotency key before sending a mutation. It
saves the acknowledged state before saving the successful response. A lost
acknowledgment leaves the key uncertain and reserved; it never executes again
automatically. Known worker validation/state rejections release the reservation.

API control frames are bounded to 64 MiB. Control transfers have finite 15-second deadlines.
Any transport/decode failure poisons and closes the channel before another
request can reuse it. A late reply therefore cannot acknowledge a later
operation. The supervisor stops/reaps a failed worker before freeing its slot
and publishing failure. Until then, status can return the last acknowledged
view, while other unavailable inspections return 503. Other workers continue
advancing independently; mutation/supervisor operations may wait behind a
timed-out control request.

An unacknowledged worker may die between individual cache-file renames. Those
bytes are retained in `interrupted_*` directories after the writer has stopped,
including during API restart recovery. They are not advertised as a complete
inspection snapshot. Failure diagnostics identify worker state as unavailable
and inventory as the last acknowledged value, rather than claiming recovery
of in-memory accounting. Completed/cancelled acknowledged artifacts retain
their existing integrity checks.

Graceful API shutdown requests a final worker failure snapshot, persists it and
reaps the process. Abrupt API death stops workers and active runs fail on the
next startup; queued runs survive subject to the existing build-provenance
check. An independent supervisor that keeps work alive across API death,
horizontal worker hosts, transparent retries and hard OS resource quotas are
future changes. Authentication and multi-user hosting are still separate work.

## Verification

The PostgreSQL integration suite exercises real child PIDs, independent
controls, worker crash/capacity recovery, API suspension, orphan fencing,
partial-cache preservation, timeout poisoning, cold query workers and shutdown
reaping. Canonical native artifacts are compared byte-for-byte against the
test-only in-process reference for all four strategies and two seeds each. Inspection caches use native struct
padding, so their public field values are compared instead of treating raw
cache bytes as a canonical encoding. Existing allocation guards remain the
evidence for declared matching/gateway hot paths; no latency claim is made.

```sh
make -j1 WITH_POSTGRES=1 build/release/exsim_api build/release/run_tests
python3 tests/integration/process_architecture_test.py
python3 tests/integration/postgres_api_test.py
build/release/run_tests process_simulation
build/release/run_tests strategy_wire
build/release/run_tests api
build/release/run_tests alloc
```

See the current worker receipt under `docs/quality/` for exact checked scope
and log paths. No installed release or existing research dataset is migrated
automatically by these changes.
