# Networking implementation and validation

Checkout: `/Users/jedi/.t3/worktrees/lob/t3-bbf9dbf6`
Branch: `t3/add-networking-layer`

The implementation preserves the interrupted public headers and completes their
source definitions. It adds a TCP client, UDP endpoints, clocks and independent
loopback tests. No production MatchingEngine implementation is needed.

## API

All symbols are in `exsim::net`. Socket operations return 0 or an errno value;
raw byte I/O returns ssize_t with errno. Constructors allocate bounded storage
and can throw on setup failure. Runtime network methods are noexcept.

- `Socket`: move-only descriptor ownership; `fd`, `release`, `close`.
- `Ipv4Endpoint`: host-order address and port; `loopback_endpoint(0)` for tests.
- `parse_ipv4_address`, `format_ipv4_endpoint`: bounded numeric addressing.
- `open_tcp_socket`, `open_udp_socket`: nonblocking, close-on-exec, SIGPIPE-safe.
- `set_non_blocking`, `set_tcp_no_delay`, `set_reuse_address`, `set_reuse_port`.
- `set/get_send_buffer_size`, `set/get_receive_buffer_size`, `set_no_sigpipe`.
- `bind_ipv4`, `listen_for_connections`, `open_tcp_listener`.
- `accept_connection`: configures each accepted descriptor before ownership transfer.
- `start_connect`, `connect_result`, `take_socket_error`.
- `local_endpoint`, `peer_endpoint`: actual port/address queries.
- `send_some`, `receive_some`: one syscall with EINTR retry.
- `Poller`: `open`, `add`, `modify`, `remove`, `wait`, `events`, `close`.
- `PollInterest`: Read, Write, ReadWrite; readiness is level-triggered.
- `PollEvent`: uint64 token plus readable, writable, hangup and error flags.
- `make_poll_token`, `poll_token_tag/value`: optional owner-tag routing.
- `ReceiveBuffer`: `readable`, `consume`, `writable_tail`, `commit`, `receive_from`.
- `SendBuffer`: `reserve`, `commit`, `append`, `pending`, `flush_to`, `overflowed`.
- `TcpConnection`: owns socket and buffers; `attach`, `receive`, `flush`, `close`.
- `TcpConnection::disconnect`: closes the socket while preserving final received bytes.
- `TcpServerConfig`: slot, stream-buffer, socket-buffer and listener limits.
- `TcpServer`: `open`, `handle_event`, `flush_pending`, `close`.
- `TcpServer`: `send`, `reserve_send`, `commit_send`, `connection`, `is_open`.
- `TcpServer`: `close_now`, `close_after_flush`, connection/rejection counters.
- `ConnectionId`: slot plus generation; stale IDs and readiness tokens are ignored.
- `TcpServerHandler`: opened/data/closed callbacks via static dispatch.
- `TcpClient`: `connect`, `complete_connect`, `send`, `receive`, `flush`, `close`.
- `UdpSender`: `open`, `multicast_options`, `send_to`, `fd`, `close`.
- `UdpReceiver`: `open`, `join_multicast`, `receive_from`, `send_to`, `fd`, `close`.
- `monotonic_ns`, `realtime_ns`, `utc_midnight_ns`.
- `LocalClock`: setup-time `refresh`; allocation-free `midnight_ns(unix_ns)`.

## Integration contract

The application must keep Poller alive longer than TcpServer, route owner tags,
consume complete inbound frames in callbacks, and call `flush_pending` after
encoding messages. Adapt gateway `prepare(n)` to `reserve_send(id,n).data()` with
an explicit empty-span check, and `commit(n)` to `commit_send(id,n)`.
Reserve/commit spans must be used immediately, without an intervening buffer
mutation or connection callback. Callbacks must not throw. They may send or close
connections, but must not recursively dispatch another poll batch.

Poller events expire at the next wait. kqueue can return separate read and write
events for one descriptor. Standalone clients manage their own readiness
registration; unregister before disconnecting, or discard stale client tokens.
`ReceiveBuffer::BufferFull` requires consuming data before another read.
TcpServer disconnects when the callback leaves the receive buffer entirely full;
configure capacity for the largest frame. Send overflow is sticky and is handled
by the next server flush. A graceful close drains output even after peer SHUT_WR;
application-requested closes do not produce a closed callback. The application
owns its graceful-drain timeout. Ordinary peer EOF closes the connection.

Generation counters have 24 usable bits under the owner tag. A slot is retired
at exhaustion instead of wrapping. LocalClock refresh belongs at setup/session
boundaries and timezone or DST transitions, outside the hot path. The allocation
check counts C++ heap allocations during normal runtime cycles, not kernel
socket resources. No shared contract or build files were changed.

## Checks

- `make test FILTER=net_`: 16 networking cases.
- `make test`: 40 total cases in this checkout.
- `make test MODE=debug`: 40 cases with ASan and UBSan.
- `tests/exsim/net/check_allocations.sh`: standalone counter, 64 runtime cycles,
  zero C++ allocations after setup; isolated from the shared test binary.
- Same full suites under Linux GCC release/debug; Linux Clang release also run.
- macOS uses kqueue; Linux container uses epoll. Both exercise multicast loopback.
- No test threads, so TSan is not needed. Optional RTT benchmark was not built.

Detailed generated logs are in the task-owned, gitignored directory
`tests/exsim/net/logs/`: `release-focused.log`, `release-all.log`, `debug-all.log`,
`allocations.log`, `linux-checks.log`, and `repeat-focused.log`.

Tests cover split frames, receive compaction, socket ownership and options,
connect success/refusal, readiness tokens and removal, 32 simultaneous slots and
rejection beyond capacity, stale IDs/events/flush entries, orderly EOF/reset,
receive/send exhaustion, client reconnect, server reopen, SIGPIPE suppression,
UDP request/reply, datagram truncation, zero-length datagrams and multicast.
The transfer case forces partial writes with SO_SNDBUF=4096, queues over 4 MiB,
compacts and refills output, verifies byte order and resumes via write readiness
before gracefully closing. Every wait uses a bounded deadline or poll timeout.

## Delivered files

```text
include/exsim/net/clocks.hpp
include/exsim/net/poller.hpp
include/exsim/net/socket.hpp
include/exsim/net/stream_buffers.hpp
include/exsim/net/tcp_client.hpp
include/exsim/net/tcp_connection.hpp
include/exsim/net/tcp_server.hpp
include/exsim/net/udp.hpp
src/exsim/net/poller.cpp
src/exsim/net/socket.cpp
src/exsim/net/stream_buffers.cpp
src/exsim/net/tcp_client.cpp
src/exsim/net/tcp_server.cpp
src/exsim/net/udp.cpp
tests/exsim/net/.gitignore
tests/exsim/net/README.md
tests/exsim/net/allocation_check.cc
tests/exsim/net/check_allocations.sh
tests/exsim/net/network_tests.cpp
```
