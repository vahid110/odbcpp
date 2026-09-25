# PostgreSQL beta work inventory — M0 working baseline

Inventory: 2026-09-25, code baseline `4f6de2a`. This is a finite candidate
checklist, not a claim that G0 or PG-BETA has passed. D1 (application/OS) remains
open; no answer is inferred from elapsed time. Redshift access does not block
this inventory. Scheduler remains paused.

## Finite work packages and estimates

Engineering working days, one stream, before contingency; provisional estimates
from code inspection, not measured velocity. Do not add these again to M1.

| Work | Locations / bounded outcome | Base days |
|---|---|---:|
| A1 | `ODBCConnection::connect`, `DatabaseFactory`: selected backend creation preserves configured transport, ownership and failed-login/TLS behavior | 0.5–1 |
| A2 | `ODBCStatement::prepare`, `statement_sql`, `SQLNativeSql`, `sql_escape.cpp`: marker processing and dialect ownership; preserve literal/comment/escape behavior | 1–2 |
| A3 | `postgres_type_info`, `resolve_parameter_base_types`, `get_type_info`, eight catalog methods in `odbc_handles.cpp`: native types/domain/catalog semantics behind backend | 4–7 |
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
The user application's unknown requirements can materially change this range.

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

Retain all 34 existing executables. This manifest is a minimum regression set,
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
- `test_metadata_functions`
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
  lexical edge cases and a differing fake-parser dialect tested. A2 remains open
  for SQL escape translation; next is that remaining dialect work, followed by
  the A3 type/catalog contract.
- Independent blocker: choose D1 application and OS before application acceptance
  and the final G0 freeze. No credentials or cloud provisioning are needed now.

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
