# Tests

From the repository root:

```sh
make -j4 test
```

This runs two suites:

| Command | Coverage |
| --- | --- |
| `make -j4 unit` | Matching, books, containers, codecs, gateways, config, JSON, core policies, and ITCH replay |
| `make -j4 integration` | Sockets, HTTP, feeds, and API-managed exchange processes |

Integration tests use loopback TCP/UDP and real child processes. They check lifecycle, trading, feed recovery, conflicts, isolation, and shutdown. The ITCH replay unit test compares 100,000 synthetic messages with a reference book.

CME tests cover signed iLink sessions, sequence gaps, reconnect/retransmission,
order ownership, fills, cancellation, MDP book reconstruction and snapshots,
TCP packet recovery, and API-managed processes. A combined socket test checks
private iLink fills against MDP trade IDs, prices, and event timestamps.

## Debug and filtering

```sh
make -j4 test MODE=debug
make -j4 unit FILTER=matching
make -j4 integration FILTER=net_
```

Debug builds use AddressSanitizer and UBSan. `FILTER` selects native unit and integration cases by substring. It does not filter the Python API tests.

Builds go to `build/<mode>/`. CI runs both suites on macOS and Ubuntu in release and debug modes.

## Scope

Fuzz campaigns and old end-to-end suites are deferred. Inactive FIXP/SBE code remains under ignored `legacy/`. Removing old reports or tests does not resolve the findings they described.

The active benchmark suite lives in [benchmark/](../benchmark/README.md). Replay lives in `src/utils/itch_replay/`. CME schema sources, regeneration, and supported scope are documented in the [CME profile](../docs/ilink-and-mdp.md).

All native tests use the framework in `framework/test.hpp`. Unit tests live under `exsim/` by topic. Protocol clients, the ITCH book builder, the MoldUDP64 receiver, and the journal reader are test support in `exsim/support/`; they and the admin code link only into test binaries.
