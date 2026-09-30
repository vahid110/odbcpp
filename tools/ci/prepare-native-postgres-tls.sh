#!/usr/bin/env bash
# Configure a stopped disposable native PostgreSQL cluster for TLS qualification.
set -euo pipefail
pg_data="${1:?PostgreSQL data directory required}"
tls="${2:?Absolute TLS fixture directory required}"
[[ "$pg_data" = /* && "$tls" = /* && -f "$pg_data/PG_VERSION" ]] || {
  echo 'Existing absolute PostgreSQL data and absolute fixture paths required' >&2
  exit 1
}
[[ ! -f "$pg_data/postmaster.pid" ]] || {
  echo 'TLS setup requires a stopped disposable PostgreSQL cluster' >&2
  exit 1
}
mkdir -p "$tls"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=odbcpp-ci-postgres' -addext 'subjectAltName=IP:127.0.0.1' \
  -keyout "$tls/server.key" -out "$tls/server.crt" >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=odbcpp-ci-unrelated' \
  -keyout "$tls/wrong.key" -out "$tls/wrong.crt" >/dev/null 2>&1
chmod 600 "$tls/server.key" "$tls/wrong.key"
cp "$tls/server.key" "$pg_data/server.key"
cp "$tls/server.crt" "$pg_data/server.crt"
chmod 600 "$pg_data/server.key"
printf "\nssl=on\nssl_cert_file='server.crt'\nssl_key_file='server.key'\n" >> "$pg_data/postgresql.conf"
