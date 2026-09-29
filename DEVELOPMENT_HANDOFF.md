# ODBCPP development handoff

Start here, then inspect `git status --short` and `git log -3 --oneline`.
This is a navigation summary; source, tests and PG_BETA_CHECKLIST.md remain authoritative.

## Current scope

- Hosted Windows live gate added after batch 8: preinstalled PostgreSQL, native
  odbc32 Driver Manager and zero-skip integration enforcement. CI 36555245322
  passed at b5f9d0b: 365 live cases, zero skips plus standalone DM.
  Complete A4/G9a next, then W1–W4 Windows configuration/delivery before final
  G8 acceptance. See RELEASE_PLAN.md for scope, estimates and dependencies.

- **Batch 11 implemented:** raw binary parameter contract and backend-owned
  wire encoding; normalized completed-statement kinds replace shared native tag
  interpretation. Full validation evidence is in PG_BETA_CHECKLIST.md.
  Batch 10 CI 36561452003 passed all five jobs at 379084a.

- Branch: `codex/transport-foundation`. Implementation scheduler stays paused.
- A1/A2 and A3 extraction are implemented after batch 7. G9a is not closed.
- Next: A4 result-side binary/boolean normalization and remaining conversion
  dependencies, then a differing fake backend through shared ODBC orchestration.
  Parameter encoding and native command-tag extraction are implemented. Preserve
  binding/get-data, parameter and diagnostic tests; close G9a only after acceptance.
  BACKEND_BOUNDARY.md records the reviewed lifetime/deadline/error contract.
- PostgreSQL first; live Redshift validation comes later. Application-host
  reminder is not due until G8 acceptance is actionable.
- Graphify was evaluated and declined. Use narrow `rg` searches and source reads.

## Navigation

- Contracts: `core/database/i_database_connection.h`, `query_result.h`,
  `native_type_info.h`, `type_definition.h`, `catalog_request.h`,
  `backend_capabilities.h` and `transaction.h`.
- PostgreSQL: `pg_database_connection.cpp` (domain lookup), `pg_catalog_query.cpp`
  (all eight catalog queries), `pg_type_info.cpp` (native IDs),
  `pg_type_catalog.cpp` (advertised types and version-dependent scales).
- Shared adaptation: `odbc/odbc_handles.cpp`: `get_type_info`,
  `complete_descriptor_record`, `apply_result_metadata`, `execute_catalog`.
- A4 starting points: `ODBCConnection` transaction methods in that file,
  capabilities in `odbc/odbc_api.cpp`, and generic protocol/session error handling.
- Focused tests: `test_native_types`, `test_catalog_queries`,
  `test_descriptor_apis`, `test_connection_liveness`, `it_metadata_real`.

## Existing local gates (macOS)

36 executables: 26 unit, 10 integration. Latest batch evidence is in
PG_BETA_CHECKLIST.md; inspect the CI run for the pushed SHA before declaring done.
Preserve full gates and repair failures. Successful logs stay in /tmp.

Disposable PostgreSQL fixture:

```sh
/opt/homebrew/opt/postgresql@17/bin/pg_ctl -D build-iodbc-bridge/pgdata -l /tmp/odbcpp-pg.log -o '-p 5432 -h 127.0.0.1' -w start
```

For each existing configured build directory (`build-postgresql`,
`build-iodbc-bridge`, `build-iodbc`, `build-sanitize`), run:
`cmake -S . -B BUILD`, `cmake --build BUILD -j6`, then
`ctest --test-dir BUILD --output-on-failure --timeout 90`.
Use ODBCINI=absolute BUILD/odbc.ini, ODBCINSTINI=absolute BUILD/odbcinst.ini,
ODBCSYSINI=absolute BUILD for the first three. For sanitizer, copy PostgreSQL's
INI files into a temporary config directory, replace build-postgresql driver
paths with build-sanitize, disable tracing, and use **ODBCINSTINI=odbcinst.ini**
(basename) plus absolute ODBCINI/ODBCSYSINI paths. Sanitizer environment:
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1. Linux CI retains leak detection.
PGHOST=127.0.0.1, PGPORT=5432, PGDATABASE=postgres; credentials are in the existing
local test configuration. Use a fresh GTEST_OUTPUT XML directory per run to avoid
counting old result files. Stop the fixture with `pg_ctl -D build-iodbc-bridge/pgdata -m fast -w stop`.

Push each completed batch once, with separate reviewable commits. CI repairs may
require another push. Follow the exact pushed SHA; prefer a completion watch over
repeated full job queries. Do not skip tests based on assumed impact.
