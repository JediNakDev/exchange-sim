# API specs

Build and start the management API from the repository root:

```sh
make -j4 apps
./build/release/exsim_api --bind 127.0.0.1 --port 7600 --data-dir exchanges
```

This starts the API at `http://127.0.0.1:7600`. It saves exchange definitions on disk and holds live process state in RAM. Restarting the API reloads definitions as stopped.

## Create and start an exchange

Create a request from the sample config:

```sh
mkdir -p output
python3 - <<'PY' > output/exchange-request.json
import json
from pathlib import Path
print(json.dumps({"config": Path("test/fixtures/exchange.conf").read_text()}))
PY
curl -sS -H 'Content-Type: application/json' \
  --data-binary @output/exchange-request.json http://127.0.0.1:7600/v1/exchanges
```

Use the returned ID in place of `EXCHANGE_ID`:

```sh
curl -sS -X POST http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/start
curl -sS http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/connections
curl -sS http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/status
curl -sS -X POST http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID/stop
curl -sS -X DELETE http://127.0.0.1:7600/v1/exchanges/EXCHANGE_ID
```

Every start creates empty books. External trading clients provide the orders.

## Routes

Paths below are relative to `/v1`.

| Method | Path | Result |
| --- | --- | --- |
| GET | `/service` | Supported service profile |
| GET | `/exchanges` | All definitions |
| POST | `/exchanges` | Create a stopped definition, `201` |
| GET | `/exchanges/{id}` | Definition and process state |
| PUT | `/exchanges/{id}` | Replace a stopped definition |
| DELETE | `/exchanges/{id}` | Delete a stopped definition, `204` |
| POST | `/exchanges/{id}/start` | Start and wait for readiness |
| POST | `/exchanges/{id}/stop` | Stop and wait for exit |
| GET | `/exchanges/{id}/status` | State and live status when running |
| GET | `/exchanges/{id}/connections` | Live endpoint details |

Successful calls return `200` unless shown otherwise. Create and replace take an object containing only a `config` string with the full INI.

Responses redact passwords, MDP replay passwords, and iLink HMAC secrets. Keep the original config for replacement; a redacted response cannot be submitted as a config.

Running definitions cannot be replaced or deleted (`409`). Repeated start or stop returns the existing state. Connections requires a running process (`409` otherwise). Invalid configs return `400`; unknown IDs return `404`.

Startup readiness or port conflicts return `409`. Failure to launch the executable returns `503`. The definition remains available for correction and retry. The API currently allows at most 32 definitions.

## Trading connections

Set each user's `protocol` to `ouch42`, `fix44`, or `ilink3`. All gateways share the same books. Public events use ITCH 5.0 over MoldUDP64 by default, or CME MDP 3.0 with `[market_data] protocol=mdp3`.

Configure TCP listeners with `ouch_port`, `fix_port`, and `ilink_port`. The connections response exposes `orderEntryPort`, `fixOrderEntryPort`, and `ilinkOrderEntryPort`. Listener port zero requests an OS-assigned port. iLink is disabled when no iLink users or explicit listener are configured. Query connections after every start for actual ports.

MDP uses `snapshot_port` and `instrument_port` for recovery feeds and optional `backup_port` for a redundant incremental feed. `retransmit_port` is then an authenticated TCP replay listener; `recoveryTransport` reports `tcp`. ITCH recovery remains UDP. See the [CME profile, credentials, limits, and sample config](ilink-and-mdp.md).

Use a specific local IPv4 `bind_address` and nonzero market-data destination ports. Running exchanges need distinct feed address and port pairs, including MDP snapshot, instrument, and backup destinations. A unicast feed sends to one destination address; multicast needs a working network path.

The HTTP API has no authentication and defaults to loopback. Remote traders need a reachable exchange address.

## Shutdown and disk state

Stop the API with Ctrl+C. It stops and reaps its managed exchanges. Children also shut down if their private connection to the manager closes.

The data directory is locked to one API process. Config files retain full credentials with owner-only permissions. Books and sessions are not saved.

Standalone exchanges can use optional journal settings. Managed exchanges currently reject journaling, the legacy Exsim `fixp_sbe` protocol, and public admin listeners. CME iLink 3 has its own FIXP/SBE gateway.

To run an exchange directly:

```sh
./build/release/exsim_exchanged --config test/fixtures/exchange.conf
```

Stop it with SIGINT or SIGTERM. It also starts with empty books.

See [OpenAPI](openapi.json) for the HTTP contract and [architecture](architecture.md) for process boundaries.
