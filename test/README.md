# Tests

Native tests use the shared runner. Benchmark and fuzz have separate entry points.
Binaries still go in `build/<mode>/`.

```sh
make -j4 test
make -j4 test MODE=debug
make -j4 test-tools
make bench
```

`make apps` builds the API and its workers. `make test-tools` builds benchmarks, differential fuzz, scenario and replay checks, plus the diagnostic and research CLIs.
The replay and scenario tools use an independent reference engine for validation.
Standalone book examples, ITCH replay and sharding tests live in `legacy/`.

## Fuzz

The committed corpus is replayed by the normal native tests. Campaigns need clang with libFuzzer, available in the development Docker image.

```sh
make -C test/fuzz
make -C test/fuzz run TARGET=itch50 SECONDS=30 JOBS=1 WORKERS=1
make -C test/fuzz seeds
```

Campaign output goes in `build/fuzz/`. `make -C test/fuzz merge TARGET=itch50` updates the committed corpus.

## Integration

Run from the repository root:

```sh
python3 -B test/integration/test_research_audit.py
python3 -B test/integration/test_research_delivery.py
python3 -B test/integration/compose_secrets_test.py
python3 test/integration/process_architecture_test.py
```

The PostgreSQL and Compose integration tests need their services. CI sets up disposable instances.
Frontend tests stay in `web/test/` and run through the Bun package.
