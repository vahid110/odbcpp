# PostgreSQL beta work inventory — M0 working baseline

Inventory: 2026-09-25, code baseline `4f6de2a`. This is a finite candidate
checklist, not a claim that G0 or PG-BETA has passed. D1 application families were selected on 2026-09-26: SQL Server linked server /
OPENQUERY, Power BI Desktop (Windows), and Excel (macOS). Versions, access and
detailed workflows remain open; see RELEASE_PLAN.md. Redshift access does not block
this inventory. Scheduler remains paused.

## Finite work packages and estimates

Engineering working days, one stream, before contingency; provisional estimates
from code inspection, not measured velocity. Do not add these again to M1.

| Work | Locations / bounded outcome | Base days |
|---|---|---:|
| A1 | `ODBCConnection::connect`, `DatabaseFactory`: selected backend creation preserves configured transport, ownership and failed-login/TLS behavior | 0.5–1 |
| A2 | `ODBCStatement::prepare`, `statement_sql`, `SQLNativeSql`, `sql_escape.cpp`: marker processing and dialect ownership; preserve literal/comment/escape behavior | 1–2 |
| A3 | `postgres_type_info`, domain lookup (extracted in batch 4), `get_type_info`, eight catalog methods in `odbc_handles.cpp`: native types/domain/catalog semantics behind backend | 4–7 |
| A4 | `QueryResult`, `IDatabaseConnection`, `IProtocolParser`; transaction SQL in `ODBCConnection`, capability values in `odbc_api.cpp`: normalized/native contract, capabilities, errors/deadlines/ownership | 1.5–2 |
| Architecture verification | Small fake backend through shared orchestration, success/NULL/error/unsupported; PostgreSQL and Driver Manager regressions | 2–3 |
| Supported behavior review/fixes | Finite conversion/state/metadata/attribute matrix below; fix demonstrated supported-path gaps, not every permutation | 2–3 |
| Evidence integrity | Fail release validation on absent endpoint, capture actual server identity, name/review skips, preserve CI platform coverage | 1–2 |
| Application and delivery | Named application workflow plus clean beta installation, limitations, versioned artifact | 2–3 |
| **M1 total** | **Architecture 9–15 days plus behavior/evidence/delivery 5–8 days** | **14–23** |

M0 remains 2–3 days. M1's previous 7–12 days did not have a sized architecture
inventory and is superseded by 14–23 days. 30% contingency remains separate;
architecture is required scope, not reserve consumption. Re-estimate after
A3's type/catalog contract is reviewed and after the application is selected.
The application expansion on 2026-09-26 supersedes the one-application
assumption: this range is a pre-expansion baseline, pending separate sizing of
the three tracks in RELEASE_PLAN.md. Added scope is not contingency.

## All 38 partial operations: proposed scope and evidence

Each row is classified once. Evidence names refer to existing test executables;
they are starting points for reviewing cases, not assertions that every required
case is present or passed. All rows remain Partial in the conformance audit.
Residuals are conditional candidates under RELEASE_PLAN.md: incorrect supported
results, visibility/security defects, crashes and hangs never qualify for deferral.
A/W coverage applies wherever exported. G6 will revisit backend semantics for Redshift.

| Operation | Gate | PostgreSQL beta review surface | Conditional residual | Existing evidence |
|---|---|---|---|---|
| `SQLAllocHandle` | G3/G4/G5 | Lifecycle, ownership, failure/output preservation | Pooling tokens T1 | test_api_validation, it_handle_lifecycle |
| `SQLFreeHandle` | G3/G4/G5 | Lifecycle, ownership, failure/output preservation | Pooling tokens T1 | test_api_validation, it_handle_lifecycle |
| `SQLConnect` | G1/G4 | Noninteractive A/W login, diagnostics, teardown, bounded failure | Prompts T5; async/cancel T1/T2 | test_connection_liveness, test_connection_string, it_handle_lifecycle |
| `SQLDriverConnect` | G1/G4 | Noninteractive A/W login, diagnostics, teardown, bounded failure | Prompts T5; async/cancel T1/T2 | test_connection_liveness, test_connection_string, it_handle_lifecycle |
| `SQLDisconnect` | G1/G4 | Noninteractive A/W login, diagnostics, teardown, bounded failure | Prompts T5; async/cancel T1/T2 | test_connection_liveness, test_connection_string, it_handle_lifecycle |
| `SQLSetConnectAttr` | G5 | Advertised scalar attributes and explicit rejection of optional modes | Pooling/arrays/scroll/async T1 | test_attribute_apis, it_handle_lifecycle |
| `SQLGetConnectAttr` | G5 | Advertised scalar attributes and explicit rejection of optional modes | Pooling/arrays/scroll/async T1 | test_attribute_apis, it_handle_lifecycle |
| `SQLEndTran` | G3/G4 | Commit/rollback/autocommit plus failed-transaction recovery | Unclaimed transaction modes T1 | it_handle_lifecycle, it_diagnostics |
| `SQLExecDirect` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLPrepare` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLExecute` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLFetch` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLFetchScroll` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLMoreResults` | G3/G4 | Direct/prepared forward-only state matrix; errors and pending results | Cancel/streamed input T2; arrays/scroll T1; eager prepare validation not promised | it_prepared_statements_real, it_handle_lifecycle, it_diagnostics |
| `SQLGetData` | G2/G8 | Frozen scalar families; NULL, lengths, truncation, range/errors, recovery | Full conversion cross-product T3; arrays T1; streamed input T2 | it_bind_col_real, it_prepared_statements_real, test_redshift_data_conversion |
| `SQLBindCol` | G2/G8 | Frozen scalar families; NULL, lengths, truncation, range/errors, recovery | Full conversion cross-product T3; arrays T1; streamed input T2 | it_bind_col_real, it_prepared_statements_real, test_redshift_data_conversion |
| `SQLBindParameter` | G2/G8 | Frozen scalar families; NULL, lengths, truncation, range/errors, recovery | Full conversion cross-product T3; arrays T1; streamed input T2 | it_bind_col_real, it_prepared_statements_real, test_redshift_data_conversion |
| `SQLNumParams` | G3/G4/G8 | Core metadata in prepared/executed/closed states; failed discovery preserves outputs | Unused origin/variable precision fields T4; bookmarks T1; cancel T2 | it_metadata_real, it_prepared_statements_real, test_metadata_functions |
| `SQLNumResultCols` | G3/G4/G8 | Core metadata in prepared/executed/closed states; failed discovery preserves outputs | Unused origin/variable precision fields T4; bookmarks T1; cancel T2 | it_metadata_real, it_prepared_statements_real, test_metadata_functions |
| `SQLDescribeCol` | G3/G4/G8 | Core metadata in prepared/executed/closed states; failed discovery preserves outputs | Unused origin/variable precision fields T4; bookmarks T1; cancel T2 | it_metadata_real, it_prepared_statements_real, test_metadata_functions |
| `SQLColAttribute` | G3/G4/G8 | Core metadata in prepared/executed/closed states; failed discovery preserves outputs | Unused origin/variable precision fields T4; bookmarks T1; cancel T2 | it_metadata_real, it_prepared_statements_real, test_metadata_functions |
| `SQLDescribeParam` | G3/G4/G8 | Core metadata in prepared/executed/closed states; failed discovery preserves outputs | Unused origin/variable precision fields T4; bookmarks T1; cancel T2 | it_metadata_real, it_prepared_statements_real, test_metadata_functions |
| `SQLSetStmtAttr` | G5 | Advertised scalar attributes and explicit rejection of optional modes | Pooling/arrays/scroll/async T1 | test_attribute_apis, it_handle_lifecycle |
| `SQLGetStmtAttr` | G5 | Advertised scalar attributes and explicit rejection of optional modes | Pooling/arrays/scroll/async T1 | test_attribute_apis, it_handle_lifecycle |
| `SQLTables` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLColumns` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLPrimaryKeys` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLForeignKeys` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLStatistics` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLProcedures` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLProcedureColumns` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLSpecialColumns` | G5/G8 | Advertised catalog shapes/filtering and selected-user visibility; review known precision gaps | Extra precision/statistics/result-set discovery T4 only if unpromised by chosen workflow | it_metadata_real, test_metadata_functions, it_driver_manager |
| `SQLGetDiagField` | G4/G5 | Supported header/record provenance, fields and buffer contracts | Array row/column diagnostics T1 | test_diagnostics_apis, it_diagnostics |
| `SQLGetDescField` | G3/G5 | Association, mutability, core fields, atomic updates and copy consistency | Bookmarks/arrays T1; intervals T3; optional origin fields T4 | test_descriptor_apis, test_thread_safety, it_driver_manager |
| `SQLGetDescRec` | G3/G5 | Association, mutability, core fields, atomic updates and copy consistency | Bookmarks/arrays T1; intervals T3; optional origin fields T4 | test_descriptor_apis, test_thread_safety, it_driver_manager |
| `SQLSetDescField` | G3/G5 | Association, mutability, core fields, atomic updates and copy consistency | Bookmarks/arrays T1; intervals T3; optional origin fields T4 | test_descriptor_apis, test_thread_safety, it_driver_manager |
| `SQLSetDescRec` | G3/G5 | Association, mutability, core fields, atomic updates and copy consistency | Bookmarks/arrays T1; intervals T3; optional origin fields T4 | test_descriptor_apis, test_thread_safety, it_driver_manager |
| `SQLCopyDesc` | G3/G5 | Association, mutability, core fields, atomic updates and copy consistency | Bookmarks/arrays T1; intervals T3; optional origin fields T4 | test_descriptor_apis, test_thread_safety, it_driver_manager |

## Candidate test manifest PG-BETA-1

Retain all 36 current executables (26 unit, 10 integration). This manifest is a minimum regression set,
not a cap on new tests required by code changes. In G0 add exact selected
application/server/OS/Driver Manager versions and case-level evidence for the
surfaces above; until then this is not a frozen release profile.

- `test_api_validation`
- `test_async_tls_transport`
- `test_async_transport`
- `test_attribute_apis`
- `test_c_api_guard`
- `test_connection_liveness`
- `test_connection_pool`
- `test_connection_string`
- `test_datarow`
- `test_descriptor_apis`
- `test_diagnostics_apis`
- `test_driver_capabilities`
- `test_driver_logging`
- `test_epoll_transport`
- `test_iocp_transport`
- `test_catalog_queries`
- `test_metadata_functions`
- `test_native_types`
- `test_pg_protocol_parser`
- `test_redshift_data_conversion`
- `test_scram_sha256`
- `test_sql_escape`
- `test_thread_safety`
- `test_transport_deadlines`
- `test_transport_options`
- `test_unicode`
- `it_bind_col_real`
- `it_connection_pool`
- `it_diagnostics`
- `it_get_info`
- `it_handle_lifecycle`
- `it_metadata_real`
- `it_native_sql`
- `it_prepared_statements_real`
- `it_redshift_real`
- `it_driver_manager`

Run the complete set on PostgreSQL, iODBC UTF-16/UCS-4, and ASan/UBSan before
an implementation batch push; then follow the one resulting CI run (Linux,
Windows units, sanitizers, both macOS widths). Record skips by reason; Linux
IOCP and Windows epoll exclusions are not absent-database waivers. Local macOS
leak-sanitizer limitations must be recorded separately from Linux leak coverage.

## Current batch and next dependency

- A1: route ODBC construction through the existing factory with transport
  transfer; test real-parser startup/query, authentication error/timeout, TLS
  refusal and unsupported selection ownership. This does not close G9a.
- Marker counting now dispatches through the selected backend, with PostgreSQL
  lexical edge cases and a differing fake-parser dialect tested. The 2026-09-27 batch moves escape translation behind the backend too;
  next is the A3 type/catalog contract after full batch validation.
- Independent dependency: pin versions, available test machines and workflows for
  the three D1 targets before application acceptance and the final G0 freeze.

## Batch 1 validation — 2026-09-25

- PostgreSQL 17.11 (Homebrew), local macOS arm64: all 34 executables pass in
  `build-postgresql`, `build-iodbc-bridge` (UTF-16 driver/UCS-4 application),
  `build-iodbc` (UCS-4), and `build-sanitize` (unixODBC, ASan/UBSan).
- Each configuration reports 740 Google Test cases, zero skipped cases, plus
  the standalone Driver Manager executable. These are regression observations,
  not 740 independent release requirements or real Redshift evidence.
- The sanitizer Driver Manager test initially failed to find its registration
  because the temporary unixODBC configuration used a full `ODBCINSTINI` path.
  Rerunning that test with `ODBCINSTINI=odbcinst.ini` and the configuration directory
  in `ODBCSYSINI` passed. The other 33 sanitizer executables already passed;
  no production change or test suppression was needed.
- Local ASan uses `detect_leaks=0` (macOS limitation); UBSan halts on its first
  finding. Linux leak detection remains required in CI. The batch completion
  report records the resulting CI run after the single batch push.
- Focused checks: six factory/backend-boundary tests and all 36 parser tests
  passed. Existing prepared-statement and Driver Manager regressions exercise
  the shared ODBC path, including quoted markers, errors and recovery.
- G0 remains open for the named application and case-level evidence review;
  G9a remains open for the remaining dialect/type/catalog/capability extraction.

## Application-host reminder

Requested 2026-09-27: remind the user at the start of G8 application acceptance,
or when that work is next and blocked on hosts. Needed then: Windows with SQL
Server/Power BI, and a Mac with Excel. Existing machines and recorded manual
runs suffice initially; dedicated self-hosted runners are optional. A daily
checkpoint-only reminder is active; the implementation scheduler stays paused.
The implementation owner should also raise this at the checkpoint during active
work, rather than waiting for the next daily check.

## Batch 2 scope — 2026-09-27

G9a/A2: share Unicode validation independently of ODBC, move PostgreSQL escape
syntax into its backend and route NativeSql/direct/prepared translation through
that contract. This completes the planned A2 implementation; A3/A4 and overall
G9a remain open. The batch does not add new database/application support claims.

The checkpoint reminder checks daily and only notifies when application hosts
become actionable. It does not resume automated implementation. Product targets
are selected; G0 still needs versions, detailed workflows and evidence review.

Batch 2 local validation: all 34 executables pass in PostgreSQL, iODBC UTF-16
bridge, iODBC UCS-4, and ASan/UBSan configurations against PostgreSQL 17.11.
Each configuration reports 745 Google Test cases with zero skips, plus the
standalone Driver Manager test. Local macOS ASan has leak detection disabled;
Linux CI retains its leak-detection gate. Focused suites: 11 Unicode tests,
11 PostgreSQL escape tests, 8 factory/dialect tests and 18 native-SQL integration
tests pass. Translation semantics were preserved during the move. A2 is closed
on local evidence, subject to the single batch CI run; A3/A4 are next.

## Batch 3 scope — 2026-09-27

G9a/A3: normalize native scalar metadata in the backend, then use it for ODBC
result columns, prepared parameters and known-type classification. The new
`test_native_types` executable is added to PG-BETA-1, taking its minimum to
35 executables (25 unit, 10 integration). No older tests are removed.

This batch closes scalar interpretation extraction only. Next: backend domain
lookup and catalog/type-info construction, followed by the remaining capabilities
and contract acceptance. Application-host reminder is not due yet.

Batch 3 validation: all 35 executables pass in PostgreSQL, iODBC UTF-16 bridge,
iODBC UCS-4 and ASan/UBSan configurations against PostgreSQL 17.11. Each reports
750 Google Test cases, zero skips, plus the standalone Driver Manager executable.
The four new core type-contract tests and the complete metadata/prepared suites
also passed focused checks. Local macOS leak detection remains disabled;
Linux leak detection is covered by the ensuing CI run. The batch completion
report records the CI link for the single push.

## Batch 4 scope — 2026-09-27

G9a/A3: move domain discovery and native catalog-row interpretation into the
PostgreSQL connection; cache normalized metadata in shared ODBC statements.
A related regression test found stale inferred parameter types/dimensions during
reprepare. The fix preserves explicit application bindings while refreshing
inferred metadata. The batch is split into backend extraction, ODBC consumption,
and the tested reprepare correction.

Next: catalog/type-info construction, then remaining capabilities and shared-layer
contract acceptance. A3/G9a remain open. Application-host reminder is not due yet.

Batch 4 validation: all 35 executables pass in PostgreSQL, iODBC UTF-16 bridge,
iODBC UCS-4 and ASan/UBSan configurations against PostgreSQL 17.11. Each reports
758 Google Test cases, zero skips, plus the standalone Driver Manager executable.
Focused checks pass all 10 native-type tests, 8 factory/dialect tests and the
complete metadata/prepared integration suites. Local macOS leak detection remains
disabled; Linux CI retains its leak-detection gate. The batch completion report
records the ensuing GitHub Actions result.

## Batch 5 scope — 2026-09-27

G9a/A3: extract table, primary-key and foreign-key catalog construction behind
the selected backend. The shared layer retains ODBC enumeration rules, argument
handling and execution state. PostgreSQL query semantics remain unchanged.
`test_catalog_queries` adds six backend contract cases; a live integration case
checks quoted Unicode names across all three operations, unknown table types and
reuse after an empty result. PG-BETA-1 now contains 36 executables.

Next: extract the remaining five catalogs and type-info construction, followed
by capability and complete shared-layer contract acceptance. A3/G9a remain open.
Application-host reminder is not due yet.

Batch 5 validation: all 36 executables pass in PostgreSQL, iODBC UTF-16 bridge,
iODBC UCS-4 and ASan/UBSan configurations against PostgreSQL 17.11. Each final
run has 766 Google Test cases, zero skips, plus the standalone Driver Manager
executable. Focused backend and complete metadata suites pass. An initial full
run caught a disconnected-catalog SQLSTATE change; it was repaired to preserve
existing behavior, covered for both key operations, then all four gates passed.
Local macOS leak detection remains disabled; Linux CI retains leak detection.
The completion report records the ensuing GitHub Actions result.

CI follow-up: the first push passed four jobs but macOS iODBC hit the existing
`AsyncTransportTest.TimeoutHandling` scheduling/network-dependent assertion.
The test now uses an already-expired deadline, waits for callback completion and
requires the exact timeout code instead of accepting cancellation after a fixed
sleep. The corrected case passes 100 repetitions on iODBC UCS-4; the complete
transport unit suite is rerun in all four local configurations before the repair
push. Production transport code is unchanged.

## Batch 6 scope — 2026-09-27

G9a/A3: extract the remaining five catalog builders and their shared PostgreSQL
type/domain SQL helpers. All eight catalog operations now use backend requests.
The batch preserves catalog behavior and removes native catalog SQL from shared
ODBC code. Two new integration cases cover column/index/special-column metadata
and routine metadata with domain types, quoted Unicode names and empty-result
reuse; five additional backend cases cover filters and options. Unsupported
backend coverage now includes every catalog request alternative.

Next: SQLGetTypeInfo construction and descriptor type names, then A4 capabilities
and full shared-layer contract acceptance. A3/G9a remain open. Application-host
reminder is not due yet.

Batch 6 validation: all 36 executables pass in PostgreSQL, iODBC UTF-16 bridge,
iODBC UCS-4 and ASan/UBSan configurations against PostgreSQL 17.11. Each reports
773 Google Test cases, zero skips, plus the standalone Driver Manager executable.
Focused checks pass all 11 backend catalog tests, metadata API tests and the
complete metadata integration suite. A source comparison confirms all five
extracted builders retain their SQL string literals in order. Local macOS leak
detection remains disabled; Linux CI retains leak detection. The completion
report records the ensuing GitHub Actions result.

## Batch 7 — backend type catalogs and descriptor properties

Completes the A3 extraction: PostgreSQL owns advertised type definitions and the
server-version numeric-scale rule; shared ODBC code consumes normalized definitions
for SQLGetTypeInfo and descriptor names/properties. Synthetic type-info result
columns no longer require PostgreSQL IDs. Legacy temporal aliases, filtering,
ordering and NULL properties remain unchanged. Preconnection descriptor APIs use
the configured backend's unconnected catalog. A4 and shared-layer acceptance
remain open; G9a is not complete. Next-work entry points and local validation
commands are maintained in DEVELOPMENT_HANDOFF.md.

Validation: all 36 executables pass in PostgreSQL, iODBC UTF-16 bridge, iODBC
UCS-4 and ASan/UBSan against PostgreSQL 17.11: 778 Google Test cases per
configuration, zero skips, plus the standalone Driver Manager executable.
Focused native-type/descriptor suites and the complete metadata integration suite
also pass. Local macOS leak detection remains disabled; Linux CI retains it.
The completion report records the ensuing CI run.

## Batch 8 — A4 transaction boundary

The implementation routes begin/commit/rollback and session isolation
through typed backend methods using the original caller deadline. PostgreSQL owns
transaction command text and its transaction/isolation capabilities. Shared ODBC
code derives SQL_TXN_CAPABLE, SQL_DEFAULT_TXN_ISOLATION and SQL_TXN_ISOLATION_OPTION
from those capabilities, rejects unsupported settings, and retains autocommit,
cursor behavior, diagnostics and state transitions. Other capability fields and
the broader result/error/recovery contract remain A4 work.

Five new backend unit cases cover commands, capability/isolation agreement,
invalid enums without I/O, preserved errors/deadlines and unsupported backends.
Two new live tests cover all advertised isolation levels, active-transaction
attribute rejection, and transactional DDL rollback/commit.

Full local validation completed on 2026-09-28 after full execution access was
restored: PostgreSQL, iODBC UTF-16 bridge, iODBC UCS-4, and ASan/UBSan all pass.
Each configuration runs 36 executables (26 unit, 10 integration), 785 GoogleTest
cases with zero skips, plus the standalone Driver Manager executable. All seven
new regression cases executed successfully. Six focused suites also passed in
each configuration during development. `git diff --check` passes.
Local macOS leak detection remains disabled; Linux CI retains it. The completion
report records the ensuing CI run. A4 and G9a remain open for the bounded work
identified above.

## Windows live gate — 2026-09-29

The standard Windows Server 2025 GitHub-hosted runner now starts a disposable
SCRAM-authenticated PostgreSQL cluster using its preinstalled PostgreSQL binaries.
The standalone Driver Manager executable builds against Windows odbc32 and loads
the registered driver DLL. All nine in-process integration executables run too;
the job rejects missing XML reports and skipped integration cases. Test and
server logs are retained for seven days. Native registry DSN attribute reading
remains a delivery gap: the fixture supplies the driver's current INI files as
well as registry registration for DLL discovery.

Local regression evidence: all four complete configurations pass, 36 executables,
785 GoogleTest cases each, zero skips. The completion report must record the
Windows CI result; local macOS runs alone cannot establish Windows compatibility.
SQL Server/OPENQUERY and Power BI acceptance remain unexecuted and are described
in RELEASE_PLAN.md's hosted Windows approach. A4/G9a remain open.
