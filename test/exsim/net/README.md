# Networking tests

The tests use loopback sockets and ephemeral ports. They cover TCP connections,
partial writes, bounded buffers, stale readiness tokens, UDP and multicast.
macOS uses kqueue; Linux uses epoll.

```sh
make test FILTER=Network
make test MODE=debug FILTER=Network
```

The allocation check counts C++ heap allocations after setup. Sanitizer builds
skip allocation counting; they still check memory safety.

## Integration

Keep the Poller alive longer than the TcpServer. Route readiness events by
owner tag, consume complete frames in callbacks, then call `flush_pending`.

Gateway `prepare(n)` maps to `reserve_send(id, n).data()` with an explicit
empty-span check. `commit(n)` maps to `commit_send(id, n)`.
Use reserved spans immediately, before another buffer mutation or callback.
Callbacks must not throw or recursively dispatch another poll batch.

Readiness events expire at the next wait. Unregister clients before disconnect,
or discard stale tokens. A full receive buffer needs parsing before another
read. Send overflow closes the connection on the next flush.

Graceful close drains pending output. The application owns the drain timeout.
