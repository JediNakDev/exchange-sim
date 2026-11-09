#!/bin/sh
set -eu
PGPASSWORD=$(cat /run/secrets/app_password)
export PGPASSWORD
exec /usr/local/bin/exsim_apid "$@"
