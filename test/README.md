# Tests

From the repository root:

```sh
make -j4 test
```

This runs three suites:

| Command | Coverage |
| --- | --- |
| `make -j4 unit` | Matching, books, codecs, gateways, config, JSON, and core policies |
| `make -j4 integration` | Sockets, HTTP, feeds, and API-managed exchange processes |
| `make -j4 exchange-test` | Standalone order, matching, query, and ITCH replay checks |

Integration tests use loopback TCP/UDP and real child processes. They check lifecycle, trading, feed recovery, conflicts, isolation, and shutdown. Replay compares 100,000 synthetic messages with a reference book.

## Debug and filtering

```sh
make -j4 test MODE=debug
make -j4 unit FILTER=matching
make -j4 integration FILTER=net_
```

Debug builds use AddressSanitizer and UBSan. `FILTER` selects native unit and integration cases by substring. It does not filter the standalone suite or the Python API tests.

Builds go to `build/<mode>/`. CI runs all three suites on macOS and Ubuntu in release and debug modes.

## Scope

Fuzz campaigns and old end-to-end suites are deferred. Inactive FIXP/SBE code remains under ignored `legacy/`. Removing old reports or tests does not resolve the findings they described.

The active benchmark suite lives in [benchmark/](../benchmark/README.md). Replay lives in `src/utils/itch_replay/`.

Book builders, the TCP test client, and admin test helpers are linked into test binaries. Historical `arena_*` and `vector_*` test names still exercise the current matching engine.
