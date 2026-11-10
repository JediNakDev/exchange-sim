# Tests

Two active targets:

```sh
make -j4 unit
make -j4 integration
make -j4 test
```

`unit` covers matching, books, protocol codecs, in-memory gateway sessions, config, JSON, and exchange core policies. `integration` covers native socket/HTTP/feed components and the API with real child exchange processes over loopback TCP/UDP. It checks CRUD, session matching, feed recovery, isolation, conflicts, shutdown, and restart. No database or frontend is required.

Use `MODE=debug` for AddressSanitizer and UBSan. Use `FILTER=substring` to select native test cases; API process integration always runs its complete small set. Outputs go to `build/<mode>/`.

For OrbStack Ubuntu, use a separate build directory to avoid mixing macOS and Linux objects:

```sh
orb -m ubuntu make -j4 test BUILD=build/linux-release EXTRA_CXXFLAGS=-Werror
```

Fuzz campaigns, benchmarks, old server end-to-end/review suites, standalone tools, and inactive FIXP/SBE tests live under `legacy/exsim/`. They are excluded from these targets and CI and need migration before becoming active again. Removed strategy/research tests are outside the exchange-only product scope.

Market-data receivers and book builders remain as validators for active publisher/recovery tests. Those implementations and the TCP test client are linked only into test binaries. Tests with historical `arena_*` and `vector_*` names still exercise the current production matching engine.
