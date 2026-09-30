#!/usr/bin/env bash
# Enable verified TLS on an existing disposable PostgreSQL CI container.
set -euo pipefail
container="${1:?PostgreSQL container ID required}"
tls="${2:?Absolute fixture directory required}"
[[ "$tls" = /* ]] || { echo 'Fixture directory must be absolute' >&2; exit 1; }
mkdir -p "$tls"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$tls/server.key" -out "$tls/server.crt" \
  -subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1'
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout "$tls/wrong.key" -out "$tls/wrong.crt" \
  -subj '/CN=untrusted.invalid'
docker cp "$tls/server.key" "$container:/tmp/server.key"
docker cp "$tls/server.crt" "$container:/tmp/server.crt"
docker exec -u 0 "$container" sh -c '
  cp /tmp/server.key "$PGDATA/server.key"
  cp /tmp/server.crt "$PGDATA/server.crt"
  chown postgres:postgres "$PGDATA/server.key" "$PGDATA/server.crt"
  chmod 600 "$PGDATA/server.key"
  printf "ssl=on\nssl_cert_file=\047server.crt\047\nssl_key_file=\047server.key\047\n" >> "$PGDATA/postgresql.conf"
'
docker restart "$container"
for attempt in {1..30}; do
  if docker exec "$container" pg_isready -h 127.0.0.1 -U postgres; then exit 0; fi
  sleep 1
done
echo 'PostgreSQL did not become ready after TLS configuration' >&2
exit 1
