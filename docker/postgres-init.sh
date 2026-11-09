#!/bin/sh
set -eu
EXSIM_APP_PASSWORD=$(cat /run/secrets/app_password)
export EXSIM_APP_PASSWORD
psql --username "$POSTGRES_USER" --dbname postgres --set ON_ERROR_STOP=1 <<'SQL'
\getenv app_password EXSIM_APP_PASSWORD
CREATE ROLE exsim LOGIN PASSWORD :'app_password';
CREATE DATABASE exsim OWNER exsim;
SQL
unset EXSIM_APP_PASSWORD
