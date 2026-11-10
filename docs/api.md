# API specs

Build the two C++23 processes from the repository root:

```sh
make -j4 apps
./build/release/exsim_api --bind 127.0.0.1 --port 7600 --data-dir output/exchanges
```

The API saves exchange configs in the data directory. It holds live process state in RAM. Starting the API reloads definitions as stopped; it does not resume orders or automatically restart exchanges. The data directory is locked to one manager.

Create a definition using the sample INI config:

```sh
python3 - <<'PY' > output/exchange-request.json
import json
from pathlib import Path
print(json.dumps({"config": Path("test/fixtures/exchange.conf").read_text()}))
PY
curl -sS -H 'Content-Type: application/json' \
  --data-binary @output/exchange-request.json http://127.0.0.1:7600/v1/exchanges
```

Use the returned ID:

```sh
curl -sS -X POST http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/start
curl -sS http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/connections
curl -sS http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/status
curl -sS -X POST http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/stop
curl -sS -X DELETE http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID
```

`PUT /v1/exchanges/{id}` replaces a stopped definition using the same `{"config":"..."}` request. Responses redact passwords; send your original/full config for replacement. Updating or deleting a running exchange returns `409`. Start waits for readiness and stop waits for exit; both return `200`, including repeated calls. Requests to a failed startup return its failure, with the definition retained for correction and retry.

Order entry supports OUCH 4.2 over SoupBinTCP and FIX 4.4, sharing the same books and ITCH 5.0 over MoldUDP64 public feed. Set each participant's `protocol` to `ouch42` or `fix44`. The exchange opens a TCP listener for each protocol; `ouch_port` and `fix_port` configure them, and connection details return `orderEntryPort` and `fixOrderEntryPort`. Set unique market-data destinations for simultaneously running exchanges. Listener port `0` requests an OS-assigned TCP/recovery port. Market-data destination port must be explicit and nonzero. Query `connections` for the actual ports and fresh session identity on each start.

Use a specific local IPv4 `bind_address`. Use a reachable LAN address for trader computers on another machine. Multicast also needs a working interface/network path. A unicast market-data destination supports one receiving address, not many subscribers. The HTTP API has no authentication and binds loopback by default.

You can also start an unmanaged exchange directly:

```sh
./build/release/exsim_exchanged --config test/fixtures/exchange.conf
```

Stop it with SIGINT or SIGTERM. Managed exchanges also stop if their manager disappears. Every new process starts with empty books. Journaling is available to standalone exchanges through the existing INI settings; managed journaling is not enabled in this first API.

See [OpenAPI](openapi.json) for the HTTP contract and [architecture](architecture.md) for process boundaries. Docker and Compose are not part of this checkout; run the native processes on macOS or Linux.
