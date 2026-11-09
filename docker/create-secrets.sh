#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .secrets
chmod 700 .secrets
temporary=
trap 'if [ -n "$temporary" ]; then rm -f "$temporary"; fi' EXIT
trap 'exit 1' HUP INT TERM
for name in postgres_password app_password; do
    if [ -e ".secrets/$name" ] || [ -L ".secrets/$name" ]; then
        if [ -L ".secrets/$name" ] || [ ! -f ".secrets/$name" ] || [ ! -s ".secrets/$name" ]; then
            printf '%s\n' "Refusing invalid existing credential: .secrets/$name" >&2
            exit 1
        fi
    else
        temporary=$(mktemp ".secrets/.$name.XXXXXX")
        openssl rand -hex 32 > "$temporary"
        test -s "$temporary"
        chmod 444 "$temporary"
        # Atomic, non-overwriting publication after successful generation.
        ln "$temporary" ".secrets/$name"
        rm -f "$temporary"
        temporary=
    fi
done
# Compose bind-mounts file secrets without remapping ownership. Allow the
# non-root API to read its mount; the host parent directory stays private.
chmod 444 .secrets/app_password .secrets/postgres_password
printf '%s\n' 'Compose credentials are ready in .secrets/ (existing credentials preserved).'
