# iLink and MDP

C++23 simulator profile: CME iLink 3 order entry and MDP 3.0 market data.
All orders and liquidity come from external clients. The exchanges start empty.

## Task list

- [x] **P01: Pin schemas and scope.** iLink schema 8/version 9, MDP schema 1/version 13, Global TCP Recovery schema 9/version 0. Primary iLink sessions, outright index futures, limit DAY/FAK orders, MBP depth 1-10.
- [x] **P02: Define shared mappings.** Separate CME SecurityIDs and full symbols from dense internal IDs. Preserve 20-byte client IDs. Convert PRICE9 exactly, share UTC event timestamps and order/trade IDs across private and public messages.
- [x] **P03: Implement wire codecs.** Generated iLink field layouts and null defaults, handwritten framing and MDP codecs, pinned XML, C++23 regeneration tool, independent CME packet fixtures and malformed-input tests.
- [x] **P04: Implement iLink sessions.** Signed negotiation/establishment, keepalives, termination, sequencing, NotApplied, reconnect, retained replies, previous UUID recovery, and bounded session history.
- [x] **P05: Implement iLink orders.** New/cancel/replace/mass cancel, acknowledgements, rejects, partial/full fills, FAK elimination, ownership checks, static party/account validation, and cancel on disconnect.
- [x] **P06: Implement MDP publication.** Instrument definitions, security status, MBP updates, trades, channel reset, heartbeats, packet/report sequences, event boundaries, and optional redundant A/B destinations.
- [x] **P07: Implement MDP recovery.** Periodic snapshots with consistent incremental boundaries, instrument replay, bounded historical packet storage, authenticated TCP recovery, and explicit unavailable-history replies.
- [x] **P08: Integrate runtime/config.** Register iLink, select ITCH or MDP, configure listeners and destinations, report bound ports, retain state in RAM, and handle faults, capacity limits, and shutdown.
- [x] **P09: Extend management API.** Advertise protocols, publish connection details in OpenAPI, redact HMAC/replay secrets, reject feed conflicts across all destinations, and test managed lifecycle/restarts.
- [x] **P10: Verify and document.** Unit and integration checks pass on macOS and OrbStack Ubuntu in release and ASan/UBSan builds, including existing protocol regressions and the combined private/public CME socket flow. Schema regeneration is reproducible. Fuzz and end-to-end suites remain deferred.

## Run

```sh
make -j4 apps
./build/release/exsim_exchanged --config test/fixtures/ilink3-mdp.conf
```

For managed exchanges, submit that same INI to the existing management API.
Query `/v1/exchanges/{id}/connections` after startup for assigned listener
ports. The example has local test credentials; clients sign their iLink
Negotiate and Establish requests with the configured base64url HMAC key.

## Supported messages

| Interface | Templates |
| --- | --- |
| iLink session | 500-510, 513 |
| iLink requests | 514 new, 515 replace, 516 cancel, 529 mass cancel |
| iLink replies | 521 business reject, 522 new, 523 reject, 524 elimination, 525 outright trade, 531 modify, 534 cancel, 535/536 cancel/replace reject, 562 mass-action report |
| MDP incremental | 4 reset, 12 heartbeat, 30 security status, 46 MBP book, 48 trade summary |
| MDP recovery feeds | 52 snapshots, 54 future definitions |
| Global TCP recovery | ASCII Logon/MarketDataRequest/Logout, SBE 408 login and 409 logout |

## Configuration and mappings

Set `protocol=ilink3` for an iLink user. Configure `ilink_access_key`,
`ilink_secret_key`, `ilink_session` (three characters), `ilink_firm`,
`ilink_party_id`, and `ilink_account`. These are local simulator identities.
The party ID represents a pre-registered record configured before startup.
`ilink_port=0` requests an assigned port when iLink users exist. Without
users or an explicit port, the iLink listener is disabled.

Each instrument has a unique positive `cme_security_id` and optional
`cme_symbol` of up to 20 characters. The INI section name remains its short
internal/OUCH symbol. The futures definition uses XCME, USD, FIFO, and index
points; configure `cme_maturity_date` (YYYYMMDD) and `cme_contract_size`.
Their defaults describe a synthetic contract maturing on 2099-12-31 with
size one. Quantities are contracts.

The matching engine retains its positive four-decimal price grid. Off-grid,
out-of-band, negative, and unrepresentable prices are rejected. PRICE9
conversion never rounds. iLink accepts quantities 1-99999, limit DAY/FAK,
and no hidden quantity, stop, discretion, or reservation price. Replace
quantities include fills already received; OFMOverride must be disabled.
Mass cancel supports instrument, session-wide, and configured market segment
scopes, without side/order-type filters.

For MDP, set `[market_data] protocol=mdp3`, `channel_id`,
`market_segment_id`, `book_depth` (1-10), and distinct destination ports:
`port`, `snapshot_port`, `instrument_port`, plus optional `backup_port`.
All use `address` and the configured multicast interface. Set
`replay_username` and `replay_password`; `retransmit_port` is then a TCP
listener, not MoldUDP64. `max_payload` must be 1400-65507 bytes.
`snapshot_interval_ms` and `instrument_interval_ms` control recovery loops.
ITCH remains the default when `protocol` is omitted.

A/B incremental packets are identical and share sequence numbers. Clients
deduplicate by sequence, queue incrementals during recovery, and apply only
updates newer than a snapshot's packet/report boundary. Each MDP message
uses one UDP packet; an event can span packets and ends with EndOfEvent.
`fault md` applies to the incremental stream before both destinations.

## Retention and limits

`max_tokens_per_session` bounds each user's client-ID and request-ID tables.
`store_messages_per_session` and `store_bytes_per_session` bound iLink
history for each of the current and immediately previous UUIDs. Older UUIDs
are unavailable. Reaching a session history/output limit disconnects that
session. Reconnect uses Establish with the negotiated UUID and exact next
client sequence; new UUIDs need Negotiate first.

Explicit iLink retransmit ranges allow up to 2500 messages. A current-UUID
request with count zero returns up to 2500 retained messages; previous-UUID
requests require an explicit count. Retransmissions preserve application
sequences/UUIDs and set PossRetransFlag. Duplicate inbound sequences close
the connection without executing the request again. Higher sequences cause
NotApplied; clients can resend missing requests or explicitly advance with
Sequence.

`replay_buffer_packets` bounds the MDP packet ring. TCP replay accepts one
range of at most 2000 packets per connection, up to four concurrent
connections, and queues at most 4 MiB per connection. Requests outside the
retained range or exceeding limits receive logout. An aggregate MBP level
that cannot fit the schema's signed 32-bit quantity stops publication and
causes exchange shutdown rather than a truncated update.

Books, UUIDs, IDs, and recovery history disappear on process exit. A restart
starts empty, emits ChannelReset, and requires new session negotiation.
The simulator uses price-time matching and a configured local trading day;
it does not reproduce CME pro-rata algorithms, exchange calendars, or
clearing. MBO, options/spreads, on-demand party registration, backup iLink
connections, advanced order types, and other templates are outside this
profile and are not silently accepted.

## Pinned schemas and regeneration

XML lives in `spec/cme/`. Builds use committed C++ layouts and need no network
or CME credentials. Linux builds require OpenSSL development headers and
libcrypto (`libssl-dev` on Ubuntu); macOS uses system CommonCrypto.

| File | SHA256 |
| --- | --- |
| `ilinkbinary.xml` | `f5c35f28df5e18d76bdcd790de14bcdd85e0bbb605aba36e30aeee7c952916eb` |
| `templates_FixBinary.xml` | `f45ebadf65ccb1a2d416f00ee755480072ecff030970cc170a78540541d78d60` |
| `tcprecovery.xml` | `88bd5c24317975b18c33b622eac6bf3c8674484665dc20c2f7abba7c8d324d7a` |

The iLink XML is the [public version 9 mirror](https://github.com/Open-Markets-Initiative/Directory/blob/43cecf716dd264bd8c859b43b9c30fadc0740c06/Organizations/Cme/Specifications/iLink3/Cme.Globex.iLink3.Sbe.v8.9.xml),
with schema description `20251104`. CME's [version 9 rollout](https://www.cmegroup.com/notices/electronic-trading/2026/02/20260202.html)
identifies version 9 as the production target. MDP and recovery XML came from
CME anonymous FTP under `/SBEFix/Production/Templates/` and
`/SBEFix/Production/GlobalTCPRecovery/Templates/`.

Regenerate fixed-block descriptors and null defaults:

```sh
c++ -std=c++23 src/utils/cme_codegen/main.cpp -o output/cme_codegen
output/cme_codegen spec/cme/ilinkbinary.xml > lib/include/protocol/ilink3_schema.hpp
```

Schema changes also need gateway/MDP behavior review and updated wire tests.
See the [iLink specification](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/714539039),
[MDP specification](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457219613),
and [TCP recovery specification](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457574209).
