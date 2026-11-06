# Production research service

The research deployment consists of the C++ HTTP service and a production Next.js frontend. It does not start the separate real-time exchange protocol listeners.

## Build and package

```sh
make -j4 build/release/exsim_api EXTRA_CXXFLAGS=-Werror
cd web
bun install --frozen-lockfile
bun run build
mkdir -p .next/standalone/.next
cp -R .next/static .next/standalone/.next/
if [ -d public ]; then cp -R public .next/standalone/; fi
```

`next.config.ts` enables standalone output. Deploy the complete standalone directory, its copied static/public assets, and the C++ binary. Node.js runs the standalone Next.js server; Bun manages frontend dependencies and builds. Keep run data outside the release directory.

## Run on loopback

In one terminal, from the repository root:

```sh
./build/release/exsim_api --bind 127.0.0.1 --port 7600 \
  --data-dir "$HOME/.local/share/exsim/research-data"
```

In another, from `web/`:

```sh
NODE_ENV=production HOSTNAME=127.0.0.1 PORT=3760 \
  EXSIM_API_BASE_URL=http://127.0.0.1:7600 \
  node .next/standalone/server.js
```

The browser calls `/api/exsim/v1/...`; Next.js forwards to the loopback C++ service. Verify readiness at `http://127.0.0.1:3760/api/exsim/v1/service`. `/` redirects to `/experiments`.

## Private Tailscale access

On a machine already signed in to Tailscale, use a free Serve port:

```sh
tailscale serve --bg --https=10000 http://127.0.0.1:3760
tailscale serve status
```

Use the HTTPS URL printed by Serve from a device in the same tailnet. This is tailnet-only access; Funnel is not enabled. Existing Serve handlers on other ports should be preserved. The app has no separate user authentication, so anyone your tailnet policy lets reach this endpoint can control its research runs. The C++ port remains loopback-only.

Remove this endpoint with `tailscale serve --https=10000 off`.

## Installed macOS deployment

The demonstration host runs copied release artifacts under `$HOME/Library/Application Support/exsim/releases/`, independent of the external-drive source checkout. The C++ API remains pinned to its validated `production-20261106` bundle; the redesigned frontend uses the `production-20261106-workstation` bundle. Persistent data is at `$HOME/Library/Application Support/exsim/research-data/`.

Two LaunchAgents start at user login and restart on exit:

- `com.jedinak.exsim.api`: C++ service, loopback 7600.
- `com.jedinak.exsim.frontend`: standalone Next.js, loopback 3760.

Their property lists are in `$HOME/Library/LaunchAgents/`; logs are `$HOME/Library/Logs/exsim/{api,frontend}.log`. Host-specific property lists and run data are not committed.

```sh
launchctl print gui/$(id -u)/com.jedinak.exsim.api
launchctl print gui/$(id -u)/com.jedinak.exsim.frontend

# Restart after updating a copied release:
launchctl kickstart -k gui/$(id -u)/com.jedinak.exsim.api
launchctl kickstart -k gui/$(id -u)/com.jedinak.exsim.frontend
```

Stop the jobs with `launchctl bootout gui/$(id -u)/com.jedinak.exsim.api` and the corresponding frontend label. Their property lists remain available for a later `launchctl bootstrap gui/$(id -u) <plist-path>`.

Completed runs persist across service restarts. Runs interrupted while active are marked failed when the service reloads them. Browser comparisons re-execute the expected configuration/seed and do not use unverified outer metric or series caches.
