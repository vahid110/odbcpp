# ADR 0002: Library layout

Status: Proposed (2026-10-30). Phase 1 of the 2026-10-28 architecture review.

## Context

All product code compiles into two artifacts (`odbcpp_core`, `odbcpp_driver`),
each built twice from the same sources. Layering is enforced by configure-time
regex scanning of includes against hand-kept manifests
(`cmake/internal-headers/*.txt`, `sdk/header_owners.txt`), header copying into
the build tree, source partitions and a Python header projection. It costs
configure time and readability and has blocked legitimate design. The "SDK" is
a header-only interface; nothing can link it without the whole driver. One
configure builds one product, which doubles CI and packaging.

Goals the layout must serve:

- Reusable libraries for other drivers, in particular ADBC drivers for AWS
  products: authentication must be usable without any database or ODBC code.
- Three products from shared code: PostgreSQL (with RDS), Redshift, and the
  planned MySQL.
- Layering enforced by the compiler and linker, not by text scanning.

Measured include dependencies (2026-10-30) already follow this shape, apart
from two edges noted below.

## Decision

Each library is a CMake target with its own directory, public include root
(`libs/<name>/include/odbcpp/<name>/`), private sources (`libs/<name>/src/`),
and explicit `PUBLIC`/`PRIVATE` link dependencies. Private headers stay in
`src/` and are invisible to dependents.

| Library | From today | Depends on | Notes |
|---|---|---|---|
| `odbcpp::base` | `sdk/*/util` (except logging) | std only | Result/errors, Deadline + cancellation, UTF-8, hex/base64, `SecretBytes` + secure wipe, `LoginMaterial` and HTTPS/trust config value types |
| `odbcpp::logging` | `sdk/*/util/driver_logging*` | base; spdlog (private) | Keeps spdlog out of every other library |
| `odbcpp::tls` | `sdk/*/security` | base; OpenSSL or AWS-LC (private) | TLS client, trust stores, crypto provider |
| `odbcpp::transport` | `sdk/*/transport` | base, tls | Sockets, TLS transport, deadlines |
| `odbcpp::db` | `sdk/include/odbcpp/database`, `sdk/*/session` | base; transport interface | Provider-neutral contracts and session lifecycle; no backend fields (moved in Phase 2) |
| `odbcpp::auth` | `sdk/*/auth` (core) | base | Credential providers, resolver, cache; never depends on `db` |
| `odbcpp::auth_aws` | `sdk/*/auth/aws` | auth; AWS SDK (private, hidden symbols) | Rebuilt in Phase 2a; until then holds the existing modules |
| `odbcpp::backend_pg` | `core/database/postgres`, `generic_database_connection`, parsers | db, transport, tls, logging | PostgreSQL wire protocol; Redshift is a profile of it |
| `odbcpp::backend_mysql` | `core/database/mysql` | db, transport, tls | Built and unit-tested; in no product until the MySQL driver ships |
| `odbcpp::auth_bridge_pg` | `sdk/src/adapters`, PG auth consumer | auth, backend_pg | The only place auth meets a backend |
| `odbcpp::odbc` | `odbc/` (not setup) | db, base, logging | ODBC adapter; no backend or auth includes |
| `odbcpp::odbc_setup` | `odbc/setup*` | odbc | DSN setup UI |
| products | `product/` | odbc + one backend (+ auth bridges) | One shared library per product with its export list |

Products: a single configure builds every product driver
(`odbcpp_postgresql`, `odbcpp_redshift`, later `odbcpp_mysql`) from the shared
libraries; each product is a small composition file plus its export list.
`TARGET_DATABASE` remains only as a convenience filter. Tests link the
libraries they test; product-specific tests are built per product.

Enforcement: the link graph above is the rule. One small CMake architecture
test checks the graph (allowed edges only) and that `auth` never reaches `db`.
The regex include scanner, header manifests, header copying, source partitions,
legacy header projection and their self-tests are deleted as each library
moves. Each library is installable as a CMake package (`find_package(odbcpp
COMPONENTS base auth ...)`); an out-of-tree sample consumer built against the
installed packages runs in CI to prove the boundary.

Namespaces become `odbcpp::<library>` in a separate mechanical step after the
moves, so move diffs stay reviewable.

Known edges to remove during the moves: the two PostgreSQL bridge headers that
live under `auth` move to `auth_bridge_pg`; the PostgreSQL backend's direct use
of the auth core for native IdP material goes through the bridge until the
Phase 2a redesign removes it.

## Migration

Leaves first, one library per PR, every step green on the required checks.
Libraries first become CMake targets in place; files move once, at the end, so
the tools and checks that hard-code source paths change only once.

1. `base` and `logging` as targets, with the configure-time graph check and its
   script test.
2. `tls`, then `transport`.
3. `db` (contracts and session).
4. `auth`, `auth_aws`, `auth_bridge_pg` (targets only; redesign is Phase 2a).
5. `backend_pg` and `backend_mysql`.
6. `odbc` and `odbc_setup`; then delete the partition, manifest, header
   staging and projection machinery. Until then, library sources still pass
   the existing internal-include check.
7. Move files to `libs/<name>/{include,src}` with the new include spelling, as
   one scripted pure move.
8. Products: one configure builds every driver; update presets, packaging and
   CI (the per-product CI matrix collapses to one build per platform).
9. Installable packages and the out-of-tree sample consumer.
10. Namespace rename (separate mechanical step, as above).

Also in Phase 1, separately from the moves: clang-format and clang-tidy (one
formatting commit), `-Wconversion`, compiler hardening flags, fuzz harnesses
for the wire, connection-string and AWS response parsers, and pinned
dependencies.

Estimated effort: 6–9 person-weeks.

## Consequences

- Includes and links show the architecture; IDEs and compiler errors point at
  real files.
- `auth` and `db` are consumable by other drivers without ODBC.
- Large one-time churn (file moves). Moves are kept separate from logic changes
  so history and review stay legible.
- Packaging and CI change once in step 8; until then the current product
  matrix keeps working.
