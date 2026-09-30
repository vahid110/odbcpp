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

## Windows delivery addition

Required W1–W4 in RELEASE_PLAN.md cover native registry DSNs, connection-string
interoperability, the minimal ODBC Administrator GUI and x64 beta installation.
Subtotal 4.5–8 base days plus separate 30% contingency; replaces overlapping
Windows delivery allowance and does not make the old M1 total a current forecast.
A4/G9a implementation and acceptance coverage are complete in batch 12; confirm
its exact-revision CI before proceeding to W1. W1/W2 precede Windows application acceptance; W3/W4 and packaged
application reruns precede PG-BETA. Interactive SQLDriverConnect prompting stays
out of the frozen scope unless a chosen application demonstrates a need.

Hosted Windows evidence: [CI 36555245322](https://github.com/vahid110/odbcpp/actions/runs/36555245322)
passed at `b5f9d0b`, including all 365 live GoogleTest cases without skips and the
standalone odbc32 Driver Manager executable. PostgreSQL setup took 10 seconds;
live tests took 21 seconds. This does not close native DSN/GUI/installer acceptance.

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

The first Windows run passed all nine direct integration executables but exposed
an 08003 failure during Driver Manager connection loading. The repair permits
SQL_DRIVER_ODBC_VER before backend connection (ANSI and wide), while retaining
preconnection rejection for session/database information. Two regression cases
exercise version output and buffer validation. All four full local gates pass
again with 787 GoogleTest cases each and zero skips; Windows confirmation follows.

## Batch 9 — A4 advertised backend capabilities

Backend-neutral capability snapshots now supply SQLGetInfo's database identity,
identifier rules/limits, SQL and schema feature claims, terminology and NULL rules.
PostgreSQL owns the values; generic backends inherit no PostgreSQL feature claims.
Shared ODBC output and conservative unsupported-driver masks remain unchanged.
Two backend cases cover metadata without I/O and empty/unsupported profiles;
two live cases verify quoted identifier boundaries and NULL/set-operation semantics.
Existing A/W metadata, truncation, invalid input and numeric-width tests remain.

Focused native-type, attribute and live GetInfo suites pass. All four complete
local gates pass: 36 executables, 791 GoogleTest cases each, zero skips; macOS
ASan/UBSan runs with the documented leak-detection limitation. The completion
report records the ensuing CI run, including mandatory Windows live tests.
A4/G9a remain open for normalized/native result, error/deadline/lifetime contract
review and a differing fake backend through shared ODBC orchestration. W1–W4
follow architecture closure and precede final Windows application acceptance.

## Batch 10 — A4 diagnostics and ownership contract

Native server SQLSTATE interpretation now belongs to the selected backend;
shared ODBC retains transport/timeout mapping and validates normalized states.
PostgreSQL direct/prepared/deferred diagnostics and conservative ambiguous DDL
fallbacks are preserved. Four unit cases cover mappings, invalid/unknown/context
edges, a generic backend and owned diagnostic storage. One live case exercises
mapped/fallback errors across execution paths with subsequent recovery. A scripted
session case verifies retained NULL/empty/text rows, metadata and a deferred error
after backend destruction. Result/parameter/lifetime/deadline and session reuse
contracts are recorded in BACKEND_BOUNDARY.md and the public internal headers.

Focused native-type, connection-liveness and live metadata suites passed.
All four complete local gates passed: 36 executables and 797 GoogleTest cases
per configuration, zero skips. Sanitizers use the documented macOS leak-detection
setting; Linux CI retains leak coverage. The completion report records the pushed
SHA's five-job CI result, including hosted Windows live integration.

A4/G9a remains open: binary/boolean value and binary parameter encodings, native
command-tag interpretation, and differing fake-backend ODBC acceptance are the
bounded remaining architecture work. W1–W4 follow architecture closure.

## Batch 11 — A4 binary parameters and completed-statement kinds

Binary parameters cross the backend boundary as raw bytes; PostgreSQL owns bytea
wire encoding. Shared ODBC uses plain hexadecimal conversion only for character
input. Normalized StatementKind metadata now supplies completed-result dynamic
function diagnostics, including multiple results, without parsing native tags.

Five new cases cover parameter framing and type distinctions, plain-hex edge cases
and all octets, command-tag boundaries/unknown metadata, server-observed binary
bytes with NULL/empty/reuse, and per-result diagnostics. Existing ANSI/wide, binary
binding, length/truncation and prepared-query tests remain required.
Focused parser/native-type and live parameter/metadata/diagnostic suites passed.
All four full local gates passed: 36 executables, 802 GoogleTest cases each,
zero skips. macOS sanitizer settings retain the documented leak-detection limit;
Linux CI continues leak coverage. The completion report records the ensuing
five-job CI result for the pushed SHA, including Windows live integration.

A4/G9a remains open for result-side binary/boolean conversion and differing fake
backend acceptance. Binary parameter and native command-tag ownership extraction
are implemented; no Redshift-specific behavior is introduced.

## Batch 12 — A4 implementation and G9a acceptance complete

PostgreSQL owns native binary/boolean result normalization. Shared ODBC consumes
raw binary bytes and normalized bits for bound fetch and chunked SQLGetData.
The independent fake backend uses different IDs, encodings, capabilities and
errors through the real shared ODBC APIs; it covers backend selection, prepared
parameters, NULL/empty results, errors/recovery, unsupported features, deadlines,
retirement and reconnect. The final ownership/dependency review and its explicit
boundary decisions are in BACKEND_BOUNDARY.md. A1–A4 have no remaining planned
extraction items; exact-revision CI confirmation is the final G9a closure check.

All eight focused suites passed. All four full local gates passed: 37 executables
(27 unit, 10 integration), 811 GoogleTest cases each, zero skips. This includes
the new seven-case fake-backend suite and backend codec regressions, without
removing the existing PostgreSQL conversion, metadata, protocol or lifetime gates.
ASan/UBSan uses the documented macOS leak setting; Linux CI retains leak coverage.
The completion report must identify the green five-job CI run for the pushed
revision, including Windows live PostgreSQL/Driver Manager coverage.

After that confirmation, G9a is closed and W1 native Windows DSNs is next.
W2–W4 and G8 application acceptance still precede PG-BETA. This historical
checkpoint does not certify Redshift G9b or an SDK product.

## Batch 13 — W1 native Windows DSNs

Native User/System DSN and driver-default reading uses the process registry view.
User DSNs shadow same-named System DSNs without attribute merging or fallback on
malformed/denied entries. UTF-16 strings, empty values, numeric DWORD metadata and
explicit attribute/alias precedence are covered. WINDOWS_DSN.md documents source
selection, legacy explicit INI fallback and failure behavior.

Focused connection-string, transport-resolution and logging suites passed. All
four full local gates passed: 37 executables, 812 GoogleTest cases each, zero
skips, with the documented macOS sanitizer settings. Four additional native
registry cases compile/run only on Windows; local results do not validate them.
Windows CI now uses registry-only DSNs for live integration, with a conflicting
32-bit entry, and runs the native Driver Manager suite under LocalSystem while
a same-named runner User DSN has unusable settings. Exact-revision Windows CI
confirmation is required to close W1. SQL Server/OPENQUERY remains a separate G8
application gate, and W2 A/W connection-string acceptance is next afterward.

## Batch 14 — W2 native Windows connection strings

W1 closed with all five CI jobs green at d1e8d0b (run 36570790247).
W2 preserves the shared parser and adds native DM A/W acceptance: DSN-less and
DSN overrides, normalization, braced/escaped passwords, explicit input lengths,
completed lengths, Unicode W credentials, empty/wrong-password rejection without
credential disclosure, and successful recovery on the same handle. Eleven happy
paths and four rejection/recovery sequences run under both runner and LocalSystem
identities. Two direct parser cases retain normalization and empty-first-duplicate
behavior independently of Windows DM rewriting.

Focused parser/resolution tests passed. All four full local gates passed with
37 executables, 814 GoogleTest cases each, zero skips and the documented macOS
sanitizer settings. Native Windows cases require exact-revision CI confirmation;
local execution cannot validate them. On green CI, W2 closes; the pre-W3 effort
review and W3/W4 delivery follow. Actual G8 application acceptance remains open.

## Batch 15 — W3 native Administrator setup

W2 closed at 5ed113a with all five jobs green (CI 36572088585). W3 adds a separate
native setup DLL with Connection/Authentication tabs, installer A/W entry points,
validated fields, transient masked passwords, test login and cancel without writes.
The persistence allowlist excludes credentials and removes legacy PWD/PASSWORD on
Save; partial edits retain other values, and ownership/scope checks protect other
DSNs. Windows tests exercise the real installer, dialog and Driver Manager.

Six portable model cases passed focused testing. All four local gates passed:
38 executables, 820 GoogleTest cases each, zero skips with the existing macOS
sanitizer settings. Windows-only installer/dialog tests and captured rendering
require exact-revision CI validation before W3 closes. Packaging, clean-machine
Administrator execution and G8 application acceptance remain W4/G8 work.

## Batch 16 — W4 Windows beta packaging

W3 closed at 29a22a4 (CI 36584852239, all five jobs green; rendered tabs reviewed).
W4 implements a pinned x64 MSI build with driver/setup registration, app-local
runtime DLLs, dependency/license inventory and SHA-256 checksums. Input checks
reject x86 and malformed PE payloads. A separate fresh-runner job validates the
installed DLL/dependency paths, real Administrator entry, live ODBC connections,
failed-upgrade rollback, successful upgrade, downgrade rejection, foreign
registration protection and uninstall preserving DSNs/unrelated values.

All four local gates passed: 38 executables, 820 GoogleTest cases each, zero skips,
with the established macOS sanitizer settings. MSI building and the fresh-runner
lifecycle remain Windows-only acceptance: require all six exact-revision CI jobs
to pass before W4 closure. G8 host/application checks and G11 project licensing/
signing remain open; this package is an unsigned internal beta validation artifact.

W4 closed at c82efd2: CI 36595134585 passed all six exact-revision jobs. The
packaged driver loaded app-local dependencies, opened its two-tab setup dialog
through the 64-bit ODBC Administrator, connected through the native Driver
Manager, survived failed-upgrade rollback, upgraded, rejected downgrade and
foreign ownership, and uninstalled without deleting DSNs or unrelated values.

## Batch 17 — deferred G8 track and truthful Redshift pilot preparation

The first hosted SQL Server linked-server prototype was cancelled before its
45-minute job limit after the opaque SQL Server media/setup phase ran for about
40 minutes. That interruption is neither a product failure nor acceptance
evidence. At the user's direction, SQL Server/OPENQUERY is deferred for now;
Power BI Desktop and Excel application acceptance also remain open. The heavy
linked-server workflow is retained as an explicit manual dispatch and no longer
runs on every push. PG-BETA remains open at G8.

Preparatory Redshift evidence work may continue without claiming the M2 pilot.
`it_redshift_real` is now built only for `TARGET_DATABASE=REDSHIFT`; PostgreSQL
can no longer satisfy a Redshift-named test. The pilot requires a nonempty
`ODBCPP_REDSHIFT_TEST_CONNECTION` with `SSL=1`, requires configured schema/table
fixture names for catalog evidence, and rejects a `version()` result that does
not identify Amazon Redshift. It covers connection, prepared scalar/NULL fetch,
table/column metadata, invalid SQL and recovery. A real endpoint remains required
to execute these cases and close G1; absence fails rather than skips.

Focused PostgreSQL discovery/build checks and the Redshift pilot compile check
passed. Running the pilot without endpoint configuration failed with the required
message. All four complete PostgreSQL local gates passed: 37 executables (28 unit,
9 integration), 813 GoogleTest cases each and zero skips, including ASan/UBSan
with the established macOS leak-detection limitation. The seven previously
counted `it_redshift_real` cases were removed from PostgreSQL evidence; two new
pilot cases compile only in the Redshift build.

The selected capability-profile unit test now derives its expected DBMS identity
from the compiled backend instead of hard-coding PostgreSQL. All 28 Redshift-build
unit executables pass locally. CI builds the Redshift target, runs those unit
tests and verifies that the real pilot cannot pass without endpoint configuration;
it does not run or simulate live Redshift compatibility.

## SDK/MySQL reprioritization — 2026-09-30

W1–W4 and G9a are complete, but PG-BETA remains open because G8 application
acceptance is not complete. The user explicitly deferred the host-dependent SQL
Server/OPENQUERY, Power BI Desktop and Excel runs while other work proceeds.
This preserves every G8 requirement and support-claim boundary.

Live Redshift M2 is waiting on restored AWS/Redshift account access. During that
external wait, the active bounded milestone is MS1 in SDK_PRODUCT_PLAN.md: freeze
the connectivity SDK boundary, preserve PostgreSQL, and use a real MySQL 8 slice
to prove an unrelated backend. ADBC is a future adapter constraint only. This
planning decision changes no PostgreSQL conformance classification, test result
or beta claim.
