# ODBCPP development handoff

Start here, then inspect `git status --short` and `git log -3 --oneline`.
This is a navigation summary; source, tests and PG_BETA_CHECKLIST.md remain authoritative.

## Current scope

- A4/G9a is closed: batch 12 CI 36565977327 passed all five jobs at 160cb23.
- Branch: `codex/transport-foundation`. Implementation scheduler stays paused.
- W1 closed at d1e8d0b: CI 36570790247 passed all five jobs, including registry-only
  live connections and LocalSystem visibility.
- W2 closed at 5ed113a: CI 36572088585 passed all five jobs.
- W3 closed at 29a22a4: CI 36584852239 passed all five jobs and both rendered
  tabs were reviewed. Native setup/API/dialog acceptance passed.
- W4 closed at c82efd2: CI 36595134585 passed all six jobs, including the real
  Administrator path and fresh-runner package lifecycle.
- G8 application acceptance remains open. The first hosted SQL Server prototype
  was cancelled before its 45-minute bound, so it supplies no pass/fail evidence;
  its job is manual-only while that track is deferred. Power BI and Excel remain
  unexecuted. Do not claim PG-BETA from the packaged-driver evidence alone.
- Batch 17 prepares truthful Redshift pilot evidence without starting a live
  compatibility claim: PostgreSQL builds exclude `it_redshift_real`; Redshift
  builds require an explicit TLS connection and reject non-Redshift identity.
  The Redshift target's 28 unit executables run in CI, followed by a check that
  the live pilot fails when no endpoint is supplied. A real endpoint plus
  schema/table fixture is still required to close G1.
- The user reported AWS/Redshift account access unavailable on 2026-09-30.
  Live M2 waits for restored access; never substitute PostgreSQL evidence.
- **Active planned engineering milestone: MS1 SDK foundation and bounded MySQL
  proof.** S1 is complete; start with the S2 migration order in
  SDK_ARCHITECTURE.md. PostgreSQL and MySQL are sibling
  backends; Redshift is a PostgreSQL-family specialization. ODBC remains the
  implemented client API. ADBC constrains the boundary but is not implemented.
- G12 is the internal SDK/MySQL proof. G13 owns later public SDK packaging,
  stability, licensing and ADBC product decisions. Do not expand MS1 into a
  MySQL beta.
- G12 also has an explicit engineering-quality bar: S1 architecture review,
  automated forbidden-dependency checks and an S4 clean-room backend exercise.
  Undocumented internal knowledge or any shared ODBC workflow edit required by
  the sample blocks closure even when functional tests pass.
- S1 must also freeze pooling/reuse and cache lifecycles. Driver Manager reuse
  correctness is required; an SDK pool remains optional/internal. Existing
  metadata caches need invalidation evidence. New prepared caching and tuning
  wait for G10 measurements; query-result caching is out of scope.
- S1's four read-only audits are consolidated in SDK_ARCHITECTURE.md and
  SECURITY_MODEL.md. First S2 batch: secure transport/authentication and trusted
  configuration prerequisites. Omitted TLS currently disables encryption while
  PostgreSQL cleartext-password auth is accepted; do not bury that fix in a
  mechanical refactor.
- Graphify was evaluated and declined. Use narrow `rg` searches and source reads.

## Navigation

- Active SDK scope, estimates and stops: `SDK_PRODUCT_PLAN.md`; milestone order:
  `ROADMAP.md`; gates and deferrals: `RELEASE_PLAN.md`.
- Accepted S1 contracts: `SDK_ARCHITECTURE.md`; threat model and security gates:
  `SECURITY_MODEL.md`.
- Contracts: `core/database/i_database_connection.h`, `query_result.h`,
  `native_type_info.h`, `type_definition.h`, `catalog_request.h`,
  `backend_capabilities.h` and `transaction.h`.
- PostgreSQL: `pg_database_connection.cpp` (domain lookup), `pg_catalog_query.cpp`
  (all eight catalog queries), `pg_type_info.cpp` (native IDs),
  `pg_type_catalog.cpp` (advertised types and version-dependent scales).
- Shared adaptation: `odbc/odbc_handles.cpp`: `get_type_info`,
  `complete_descriptor_record`, `apply_result_metadata`, `execute_catalog`.
- Redshift pilot: `tests/integration/it_redshift_real.cpp`; it is release evidence
  only in a `TARGET_DATABASE=REDSHIFT` build with explicit endpoint variables.
- Focused tests: `test_native_types`, `test_catalog_queries`,
  `test_descriptor_apis`, `test_connection_liveness`, `it_metadata_real`.

## Existing local gates (macOS)

37 executables: 27 unit, 10 integration. Latest batch evidence is in
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
