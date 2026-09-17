# ODBCPP Conformance and Hardening Audit

This document tracks behavior verified against the ODBC contract. A function
being exported or having a happy-path test is not considered complete.

Baseline: `e4542db` (2026-09-09). Primary target: PostgreSQL through unixODBC,
iODBC, and the Windows Driver Manager. Redshift-specific behavior remains a
later compatibility pass.

Authoritative references:

- [ODBC function conformance](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/function-conformance)
- [Core interface conformance](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/core-interface-conformance)
- [ODBC state transition tables](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/appendix-b-odbc-state-transition-tables)
- [Environment, connection, and statement attributes](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/environment-connection-and-statement-attributes)

## Status rules

- **Verified**: supported behavior, failure behavior, diagnostics, and relevant
  state transitions have direct tests.
- **Partial**: useful behavior exists, but the accepted values, states,
  diagnostics, or tests are incomplete.
- **Unsupported**: intentionally rejected with an appropriate diagnostic and
  not advertised as supported.
- **Unaudited**: implementation exists but has not yet been checked against the
  specification.

## Inventory

The shared library currently exports 76 ODBC symbols: 49 base operations and
27 wide-character variants. The older roadmap count of 66 was stale.

| Operation | Variants | Status | Existing evidence | Principal remaining work |
|---|---:|---|---|---|
| `SQLAllocHandle` | A | Partial | unit failure injection, integration, DM | Root/parent types, null outputs, ODBC version, open-connection state, output clearing, and HY001 are covered; optional pooling info tokens remain |
| `SQLFreeHandle` | A | Partial | unit, integration, DM | Type identity, parent/child ordering, connected state, descriptor ownership/detachment, and failed-free retention are covered; optional pooling info tokens remain |
| `SQLConnect` | A/W | Partial | unit failure, integration, DM | Reconnect is rejected with 08002; A/W lengths and malformed wide input are covered, API credentials override DSN credentials, and authentication failures return 28000; timeout/cancellation and remaining connection-failure injection remain |
| `SQLDriverConnect` | A/W | Partial | unit, integration, DM | The supported noninteractive surface has A/W validation, explicit input lengths, length-only and terminator-only outputs, exact output/truncation rules, connected-state preservation, and unknown-keyword warnings; interactive prompting remains unsupported |
| `SQLDisconnect` | A | Partial | unit, integration, DM | Disconnected, active-transaction, and child-invalidation paths covered; async execution remains |
| `SQLSetConnectAttr` | A/W | Partial | unit, integration | Common defaults, catalog selection, connection timeout, quiet-mode pointer width, invalid values, read-only fields, and recognized unsupported modes are classified; platform-specific pooling attributes remain |
| `SQLGetConnectAttr` | A/W | Partial | unit, integration | Common numeric values, connection timeout, current catalog buffers, pointer-valued quiet mode, read-only attributes, connected-state checks, and cached broken-link detection are covered; platform-specific pooling attributes remain |
| `SQLEndTran` | A | Partial | unit, integration | Connection and environment scopes, multi-connection commit/rollback, inactive environments, and invalid completion codes are covered; transaction-failure injection remains |
| `SQLExecDirect` | A/W | Partial | unit failure, integration, DM | Open cursors are protected and direct execution replaces prepared SQL; cancellation remains |
| `SQLPrepare` | A/W | Partial | integration, DM | Open cursors are protected, old result state is retired, and PostgreSQL errors can surface through pre-execution result metadata discovery; eager prepare-time validation remains |
| `SQLExecute` | A | Partial | integration | Unprepared execution returns HY010 and open-cursor re-execution returns 24000; parameter arrays and data-at-execution remain |
| `SQLFetch` | A | Partial | unit, integration, DM | Never-executed and no-result states return HY010/24000; row arrays and full state matrix remain |
| `SQLFetchScroll` | A | Partial | unit, integration, DM | Only `SQL_FETCH_NEXT` is supported; keep other orientations honest |
| `SQLMoreResults` | A | Partial | unit, integration, DM | Result/update-count traversal and close-time discard are covered; error-result sequences remain |
| `SQLGetData` | A | Partial | integration, DM | Target types and per-row/switching-column offsets are covered; complete conversion/chunking matrices remain |
| `SQLBindCol` | A | Partial | unit, integration | Invalid C types and negative lengths are covered; row arrays, row-wise binding, and full type/conversion matrix remain |
| `SQLBindParameter` | A | Partial | unit, integration | Direction/C/SQL type and length diagnostics covered; input arrays, data-at-execution, and full conversion matrix remain |
| `SQLNumParams` | A | Partial | unit, integration, DM | Prepared statements are server-validated without execution; state, null/output preservation, complex markers, direct execution, and IPD count agreement are covered; cancellation and communication-failure injection remain |
| `SQLNumResultCols` | A | Partial | unit, integration | Prepared metadata, result sets, update counts, exhausted/closed cursors, delayed PostgreSQL errors, null outputs, and output preservation are covered; cancellation and communication-failure injection remain |
| `SQLRowCount` | A | Verified | unit, integration | Allocated, prepared, update-count, result-set, fetched/exhausted, closed, failed-execution, null-output, and output-preservation cases are covered |
| `SQLDescribeCol` | A/W | Partial | unit, integration | Prepared/executed/closed and no-result states, untouched error outputs, and ANSI/wide names are covered; bookmark column and communication/cancellation failures remain |
| `SQLColAttribute` | A/W | Partial | unit, integration | Prepared-state discovery, count/name/label/core and type-derived IRD fields, destination isolation, exact field diagnostics, truncation, and wide byte lengths are covered; origin-name fields and remaining state/error paths need enrichment or explicit negative tests |
| `SQLDescribeParam` | A | Partial | unit, integration | Prepared/executed/exhausted/closed/direct states, exact diagnostics, fixed-size PostgreSQL types, bound precision/scale, cache revision, and untouched errors are covered; unknown variable precision and failure injection remain |
| `SQLSetStmtAttr` | A/W | Partial | unit, integration | Common scalar modes, escape scanning, descriptor attachment, offset/operation pointers, and precise value diagnostics work; multirow arrays and optional cursor modes remain |
| `SQLGetStmtAttr` | A/W | Partial | unit, integration | Common defaults, escape scanning, descriptor handles/pointers, row positioning, and recognized unsupported attributes are covered; platform-specific ODBC 3.8 fields remain |
| `SQLCloseCursor` | A | Verified | unit, integration, DM | Allocated, prepared, open-empty, exhausted, closed, update-count, failed-execution, and pending-result states are covered |
| `SQLFreeStmt` | A | Verified | unit, integration, DM | SQL_CLOSE idempotence, ARD unbinding, APD reset, invalid-option preservation, SQL_DROP ownership, and pending-result discard are covered |
| `SQLGetTypeInfo` | A/W | Verified | unit, integration, DM | All 19 fields are checked across the ordered supported-type inventory, including PostgreSQL-version-aware numeric scale ranges; valid-empty and HY004 filters, disconnected/open/exhausted cursor states, legacy plus ODBC 3 datetime identifiers, and A/W Driver Manager calls are covered |
| `SQLTables` | A/W | Partial | unit, integration, DM | Null versus empty arguments, wildcard escaping, value-list filtering, catalog/schema/type enumerations, local temporary tables, state errors, malformed wide input, and A/W paths are covered; ODBC 2 catalog-pattern behavior and restricted-user visibility remain |
| `SQLColumns` | A/W | Partial | unit, integration, DM | Null/empty, ordinary catalog, escaped patterns, PostgreSQL UUID/JSONB/array/domain type identities, text octet length, cursor states, and A/W paths are covered; complete size/precision metadata and restricted-user visibility remain |
| `SQLPrimaryKeys` | A/W | Partial | unit, integration, DM | Required table, literal/empty arguments, escaping, key ordering, cursor states, and A/W paths are covered; restricted-user visibility remains |
| `SQLForeignKeys` | A/W | Partial | unit, integration, DM | PK/FK filter combinations, ordinary/empty arguments, primary-target filtering, same-name constraints, rule/deferrability mapping, ordering, cursor states, and A/W paths are covered; restricted-user visibility remains |
| `SQLStatistics` | A/W | Partial | unit, integration, DM | Ordinary/empty arguments, uniqueness filtering, quick statistics, expression/partial/hash/clustered classification, included columns, ordering, cursor states, and A/W paths are covered; exact cardinality remains |
| `SQLProcedures` | A/W | Partial | unit, integration, DM | Ordinary catalog, null/empty and escaped patterns, procedure/function distinctions, overloads, all PostgreSQL argument-mode counts, cursor states, and A/W paths are covered; restricted-user visibility remains |
| `SQLProcedureColumns` | A/W | Partial | unit, integration, DM | Ordinary catalog, null/empty and escaped patterns, unnamed columns, parameter modes/order, return rows, PostgreSQL domain type identity/base metadata, cursor states, and A/W paths are covered; result-set discovery and complete size/precision metadata remain |
| `SQLSpecialColumns` | A/W | Partial | unit, integration, DM | Ordinary/empty arguments, conservative scope, nullable modes, primary-key preference, safe unique-index fallback, composite keys, PostgreSQL UUID/domain identities, row-version empty results, cursor states, and A/W paths are covered; complete size/precision metadata remains |
| `SQLGetDiagRec` | A/W | Verified | unit, integration, DM | Retrieval is nondestructive; handle type, record number, absent records, null destinations, native codes, exact-fit, one-short, and terminator-only A/W buffers are covered, including mixed-width iODBC translation |
| `SQLGetDiagField` | A/W | Partial | unit, integration | All standard header/record identifiers, return provenance, origins, ANSI/wide lengths, and truncation are covered; row/column-specific server errors remain |
| `SQLError` | A/W | Verified | unit, integration, DM | ANSI/wide calls share a per-handle cursor, return every diagnostic in order, preserve records for SQLGetDiagRec, reset on a new stack, select statement/connection/environment precedence, retain position after errors, and run through mixed-width iODBC mapping |
| `SQLGetInfo` | A/W | Verified | unit, integration, DM | Every driver-owned standard type is classified; positive PostgreSQL claims execute end to end, and Driver Manager-only mappings are covered separately |
| `SQLGetFunctions` | A | Verified | shared-library export audit, unit, integration, DM | Connection state, exact single-function results, ODBC 2 array, ODBC 3 bitmap, null output, and invalid identifiers are enforced against all advertised exports |
| `SQLNativeSql` | A/W | Verified | unit, integration, DM | Advertised ODBC escapes translate to PostgreSQL syntax; A/W lengths, truncation, diagnostics, literals/comments, and execution-path agreement are covered |
| `SQLSetEnvAttr` | A | Verified | unit, integration, DM | ODBC 2/3/3.8 version selection, mandatory null-terminated output, invalid values, sequencing, and Driver Manager-owned pooling boundaries are classified |
| `SQLGetEnvAttr` | A | Verified | unit, DM | Version, null-terminated output, iODBC Unicode negotiation, ignored numeric buffer lengths, null outputs, invalid attributes, output preservation, and storage widths are covered |
| `SQLGetDescField` | A/W | Partial | unit, integration, DM | Standard header/record fields, PostgreSQL type characteristics, wide byte lengths, and truncation work; statement association and origin-name enrichment remain |
| `SQLGetDescRec` | A/W | Partial | unit, integration, DM | Core fields, datetime subtype, null outputs, record bounds, truncation, and mixed-width iODBC translation are covered; full associated-statement state matrix remains |
| `SQLSetDescField` | A/W | Partial | unit | Header/record mutability, descriptor-kind restrictions, type consistency, unbinding, and pointer fields are covered; bookmarks and complete type-derived defaults remain |
| `SQLSetDescRec` | A | Partial | unit, DM | Atomic core-field updates, pointer fields, descriptor growth, type/subtype consistency, and IRD rejection are covered; bookmarks and all interval combinations remain |
| `SQLCopyDesc` | A | Partial | unit concurrency | Opposite-direction copies are serialized without deadlock and IRD targets return HY016; remaining consistency and state matrix remain |

No operation is promoted to **Verified** until its audit row has explicit
negative and state-transition evidence. This deliberately resets optimistic
historical “complete” labels without discarding working behavior.

## Attribute coverage

### Environment

Implemented: `SQL_ATTR_ODBC_VERSION`, mandatory null-terminated output through
`SQL_ATTR_OUTPUT_NTS`, and iODBC driver Unicode negotiation.
Connection pooling remains a Driver Manager responsibility, but the audit must
verify which environment attributes are expected at the driver boundary and
which should be rejected.

### Connection

Implemented: `SQL_ATTR_LOGIN_TIMEOUT`, `SQL_ATTR_CONNECTION_TIMEOUT`,
`SQL_ATTR_AUTOCOMMIT`,
`SQL_ATTR_TXN_ISOLATION`, pre-connect `SQL_ATTR_CURRENT_CATALOG`, read/write
access-mode default, ODBC async-off default, metadata-ID false default,
automatic-IPD false reporting, open-connection `SQL_ATTR_CONNECTION_DEAD`
reporting, and iODBC application `SQLWCHAR` negotiation. Unsupported read-only,
async-on, and metadata-ID true modes are rejected rather than silently ignored.

PostgreSQL packet sizing is explicitly rejected as unsupported before connect
and as too late while connected. Not yet implemented or fully classified:
actual broken-link detection for connection-dead status, genuine asynchronous
ODBC function execution, metadata identifier semantics, and platform/Driver
Manager-owned tracing or cursor-library attributes.

### Statement

Stored: query timeout and maximum rows. Row/parameter array sizes, bind types,
status pointers, and processed-count pointers share their descriptor header
state rather than maintaining duplicate statement-only values.
Default-only behavior is reported for forward-only/non-scrollable cursors,
unspecified sensitivity, read-only concurrency, single-row and
single-parameter arrays, column-wise binding, retrieve-data on, bookmarks off,
automatic IPD off, zero keyset/maximum length, metadata-ID false, and ODBC
async off. Fetch and prepared execution update status outputs on success,
truncation, no-row, local validation errors, and PostgreSQL errors.

Explicit APD/ARD handles can be attached, shared within their connection,
reset to the statement's automatic descriptors, and are automatically detached
when freed. Foreign, invalid, implementation, and another statement's implicit
descriptors are rejected with precise diagnostics.

Fetch and prepared execution consume those descriptor headers and reject
unsupported multirow, bind-offset, row-wise, and operation-array settings.
Bind-offset and operation pointers round-trip through the active application
descriptors, and the current row number is available only while the cursor is
positioned on a fetched row.
`SQLBindCol` and `SQLBindParameter` populate the associated ARD/APD/IPD records.
The same records are the source of truth for fetch and prepared execution, so
applications can bind through the descriptor APIs and parameter bindings
survive `SQLPrepare`. Arrays larger than one, nonzero maximum length,
metadata-ID true, no-scan behavior, and genuine asynchronous ODBC function
completion remain.

### Descriptors

Four implicit descriptor handles, explicit descriptor allocation, application
descriptor attachment, cross-statement sharing, null reset, and free-time
automatic fallback exist. Statement attributes, descriptor header fields, and
execution status use the same header state. Binding calls populate their
descriptor records, including parameter direction, and direct record edits
drive fetch and prepared execution. IRD record/header mutation and copy targets
return HY016, except for the writable row-status and rows-processed header
pointers. Remaining descriptor-kind rules, consistency checks, and the full
field matrix remain partial.
The IRD record count and common name, type, length, precision, scale, and
nullability fields are populated from PostgreSQL result metadata and cleared
when the cursor closes.

## Logging coverage

The logging backend is off by default; configuration precedence, file rotation,
asynchronous delivery, structured escaping, and password non-disclosure are
tested. The shared C API boundary now records every warning and error associated
with a configured connection, including the exact ODBC operation, return code,
SQLSTATE, native error, duration, and connection identity. This covers
metadata, attributes, descriptors, transactions, fetches, and unexpected
exceptions without copying instrumentation into each wrapper. Nested A/W
delegation emits one record for the application-visible operation, and no
arguments or bound values are included. Diagnostic-retrieval calls, invalid
handles with no owning connection, success tracing, and disabled-logging
overhead benchmarks remain; logging is **implemented but partial** and never a
substitute for ODBC diagnostics.

## Maintainability snapshot

- `odbc_api.cpp` is 2,688 lines and contains all 76 exported wrappers;
  `odbc_handles.cpp` is 3,831 lines and combines connection, statement,
  descriptor, conversion, metadata, and registry responsibilities.
- Audit batch 56 removes the unused `AsyncDatabaseConnection` facade, whose
  detached threads fabricated query rows and never participated in the ODBC
  production path. Its permissive and disabled tests and misleading example
  are gone. The small protocol double needed by a real liveness test now lives
  under `tests/`; platform async transports and their focused deadline,
  cancellation, saturation, and real PostgreSQL coverage remain intact.
- Audit batch 1 removes a pass-through string helper and obsolete statement
  descriptor stubs whose only test asserted that they failed. This is the
  first targeted deletion pass; it is not a blanket rewrite.
- Audit batch 7 adds one shared operation lease at the C ABI boundary. Calls
  on the same handle or connection-owned handle graph are serialized in a
  stable lock order, while distinct connections remain concurrent. This
  replaces ad hoc locking rather than adding locks throughout API bodies.
- Audit batch 9 implements the standard `SQLGetDiagField` header and record
  families, including statement dynamic-function and row-count provenance,
  row/column defaults, correct standard origins, and ANSI/wide byte-length and
  truncation behavior. PostgreSQL execution tests cover INSERT and SELECT.
- Audit batch 10 replaces eight repeated ANSI catalog-argument readers with
  one validated helper parallel to the wide reader. Shared wide-output length
  validation and direct A/W parity tests cover catalog arguments,
  `SQLDescribeCol`, `SQLColAttribute`, and string versus numeric `SQLGetInfo`.
- Audit batch 11 fills the commonly queried connection-attribute defaults and
  implements pre-connect current-catalog selection for PostgreSQL. It tests
  ANSI and wide byte lengths, truncation, null and malformed lengths,
  read-only attributes, unsupported values, and connection-state behavior.
  `SQL_ATTR_CONNECTION_DEAD` reports a known-open connection and returns 08003
  when no connection is open; actual link-loss probing remains explicit work.
- Audit batch 12 loads the built shared driver and verifies every advertised
  base symbol and wide export. It exhaustively compares
  the ODBC 2 array and ODBC 3 bitmap with the supported-function set, checks
  individual queries, rejects invalid handles and null outputs, and preserves
  an explicit false result for unknown function identifiers. Windows uses an
  exact module-definition export list rather than exporting internal symbols.
- Audit batch 13 implements the useful scalar subset of rowset and parameter
  array reporting without claiming multirow execution. It stores and returns
  the four status/count pointers, updates them for successful, truncated,
  exhausted, locally invalid, and server-rejected operations, distinguishes
  invalid zero sizes from unsupported larger arrays, and reports incomplete
  parameter sets before protocol I/O.
- Audit batch 14 extends the existing C ABI guard into the common failure-log
  boundary. One compact path emits operation, return code, SQLSTATE, native
  error, microsecond duration, and connection identity for all connection-owned
  API warnings and errors. Tests cover ANSI and nested wide calls, truncation,
  unknown attributes, secret exclusion, and duplicate-record suppression.
- Audit batch 15 makes unsupported asynchronous execution, batches, arrays,
  bookmarks, positioned operations, and non-forward cursor modes explicit in
  `SQLGetInfo`; fixes accessibility overclaims; and returns the required HY096
  for unknown information types. The remaining positive flags are tied to
  transaction, multiple-result, forward-fetch, and any-column/order tests.
- Audit batch 16 implements explicit APD/ARD association within a connection,
  supports sharing one user descriptor across statement roles, restores the
  original automatic descriptor on null assignment or descriptor free, and
  rejects invalid, foreign, and implicit descriptor handles.
- Audit batch 17 removes duplicate statement status storage. Scalar array and
  bind settings now round-trip through descriptor headers; fetch and execute
  read the same status/count pointers and reject unsupported descriptor header
  modes before touching application buffers or issuing prepared protocol I/O.
- Audit batch 18 makes `SQLBindCol` and `SQLBindParameter` populate the active
  ARD/APD and IPD records, including buffer, length/indicator, C/SQL type,
  precision/scale, and input/output direction fields. Descriptor API tests
  retrieve those fields through the public handles rather than private state.
- Audit batch 19 removes the duplicate column- and parameter-binding vectors.
  Fetch and prepared execution now consume the active descriptor records
  directly, preserve bindings across `SQLPrepare`, and are exercised through
  descriptor-only bindings against PostgreSQL. `SQL_UNBIND` and
  `SQL_RESET_PARAMS` clear the corresponding application descriptor records.
- Audit batch 20 accepts parameter bindings above the prepared marker count,
  as required by ODBC, while prepared execution consumes only the marker-count
  prefix. PostgreSQL coverage proves that the extra binding is ignored and
  that a genuinely missing in-range binding still reports 07009 before I/O.
- Audit batch 21 assigns explicit roles to implicit descriptors and protects
  the implementation row descriptor. `SQLSetDescField` and `SQLCopyDesc`
  reject attempts to modify it with HY016, while its row-status and
  rows-processed header pointers remain writable and tested.
- Audit batch 22 populates IRD records from each PostgreSQL result's column
  metadata and clears them with the cursor. Integration coverage retrieves the
  record count, names, SQL types, declared length, numeric precision, and scale
  through `SQLGetDescField` rather than the statement metadata helpers.
- Audit batch 23 validates `SQLBindCol` buffer descriptions before mutating the
  ARD: negative lengths return HY090, invalid C type identifiers return HY003,
  and recognized but unsupported C types return HYC00. Unit and exported-API
  integration tests assert the exact diagnostics and successful recovery.
- Audit batch 24 validates `SQLBindParameter` before mutating APD/IPD state.
  Invalid direction, C type, SQL type, length, and missing input pointers return
  HY105, HY003, HY004, HY090, and HY009 respectively; recognized unsupported
  types or output directions return HYC00. Prepared execution also resolves
  `SQL_C_DEFAULT` from the declared SQL type and now handles its smallint and
  real mappings.
- Audit batch 25 corrects core statement sequencing. Fetch before execution now
  returns HY010 without changing row-status outputs; fetch after a successfully
  executed update with no result set returns 24000; and execute without prepare
  returns HY010. Exhausting a real result set still returns `SQL_NO_DATA`, and
  unsupported row-array settings are tested after a real execution so state
  validation cannot mask them.
- Audit batch 26 makes both `SQLCloseCursor` and `SQLFreeStmt(SQL_CLOSE)`
  discard every pending result and row count from a PostgreSQL batch. A later
  `SQLMoreResults` now remains at `SQL_NO_DATA` instead of reviving a result
  that the application explicitly discarded.
- Audit batch 27 protects open cursors from `SQLPrepare`, `SQLExecDirect`, and
  prepared re-execution with 24000. A successful direct execution now replaces
  any older prepared statement, reports zero parameters through `SQLNumParams`,
  and leaves a later `SQLExecute` in HY010 instead of running stale SQL.
- Audit batch 28 makes ANSI `SQLDescribeCol` and `SQLColAttribute` match their
  wide variants on short output buffers: both preserve the full source length,
  null-terminate the truncated value, and return `SQL_SUCCESS_WITH_INFO` with
  01004 instead of silently succeeding.
- Audit batch 29 validates `SQLGetData` target identifiers before conversion.
  Invalid identifiers return HY003, incompatible valid conversions remain
  07006, and `SQL_ARD_TYPE` resolves the active application row descriptor's
  concise type instead of being rejected as an unknown target.
- Audit batch 30 replaces per-column `SQLGetData` offset storage with the one
  active column/offset pair defined by ODBC. Switching to another column now
  invalidates a partial offset, so switching back restarts that column while
  same-column calls still retrieve successive chunks.
- Audit batch 31 preserves conversion outcomes through the shared text-protocol
  converter. Numeric overflow now reports 22003, invalid date/time input reports
  22007, invalid numeric text remains 22018, and fractional numeric loss returns
  `SQL_SUCCESS_WITH_INFO` with 01S07. Unit tests classify converter outcomes and
  PostgreSQL integration tests verify the public `SQLGetData` diagnostics.
- Audit batch 32 completes the first variable-length buffer-boundary pass.
  Zero-byte and sub-`SQLWCHAR` character buffers now return 01004 without
  consuming data, empty values still require terminator space, exact-fit and
  one-unit-short retrieval are covered, and failed calls no longer discard a
  different column's partial offset. The same tests run with two- and four-byte
  driver-side `SQLWCHAR` builds.
- Audit batch 33 completes the `SQLNumResultCols` and `SQLRowCount` state
  matrices. Prepared statements are described with PostgreSQL Parse/Describe/
  Sync without execution, using bound IPD types as protocol hints; delayed
  server errors surface as 42000 and deadlines as HYT00. Real PostgreSQL tests
  cover allocated, prepared, update-count, result-set, fetched/exhausted,
  closed, and failed states, verify untouched error outputs, and prove that
  describing `INSERT ... RETURNING` causes no side effect. Cached descriptions
  are revision-bound to the IPD, so direct descriptor type changes force fresh
  server metadata instead of leaving stale result types.
- Audit batch 34 routes `SQLDescribeCol` and `SQLColAttribute` through the same
  cached prepared-statement description used by `SQLNumResultCols`. It
  distinguishes no-result statements (07005), bad column numbers (07009),
  unknown fields (HY091), and known unsupported fields (HYC00), while leaving
  application outputs untouched on errors. `SQL_DESC_COUNT` now ignores the
  column number, name and label fields isolate character destinations from
  numeric ones, and the core type/length/precision/scale/nullability/unnamed
  fields are available before execution. Wide column attributes report byte
  lengths and reject misaligned buffers; real PostgreSQL tests cover prepared,
  executed, exhausted, and closed transitions without requiring a preceding
  `SQLNumResultCols` call.
- Audit batch 35 completes the first parameter-metadata state and type pass.
  `SQLNumParams` and `SQLDescribeParam` now validate lazily prepared SQL through
  PostgreSQL Parse/Describe/Sync without executing it, cache the returned
  parameter OIDs, and preserve application outputs on every error. Allocated,
  prepared, executed, exhausted, closed, direct-execution, invalid-index, and
  delayed-server-error cases have exact return-code and SQLSTATE coverage. The
  IPD count is replaced even for zero-parameter statements, extra bindings do
  not become statement markers, and direct execution cannot expose stale IPD
  records. Fixed-size PostgreSQL types use driver-owned size/scale metadata;
  bound precision and scale are retained when the server confirms the same
  type, while unavailable variable-length precision remains explicitly zero.
- Audit batch 36 adds the missing ODBC 3 `SQLGetDescRec`, `SQLGetDescRecW`, and
  `SQLSetDescRec` exports and advertises them through `SQLGetFunctions`.
  `SQLGetDescField` now returns the standard descriptor header and record
  families, including data type names, datetime subtypes, radix, transfer
  octet length, display size, searchability, signedness, and writable status.
  Mutations enforce descriptor-kind and read-only restrictions, validate
  type/subtype consistency, avoid retaining validation-only IPD data pointers,
  and unbind records when non-deferred fields change.
  Unit and PostgreSQL tests cover bounds, diagnostics, null destinations,
  truncation, atomic multi-field updates, and IRD rejection. Driver Manager
  coverage exercises both multi-field APIs and records iODBC's negotiated
  two-byte-driver/four-byte-application name-buffer translation.
- Audit batch 37 implements the connection-wide request timeout with the ODBC
  default of no deadline and applies it to connection-level server requests.
  PostgreSQL packet sizing is explicitly classified as unsupported before
  connection and as immutable after connection, with output preservation and
  ANSI/wide entry-point coverage.
- Audit batch 38 records failed transport trips in the database connection and
  exposes that cached state through `SQL_ATTR_CONNECTION_DEAD` without a
  server round trip. Communication failures now report 08S01 instead of being
  misclassified as SQL syntax errors. A deterministic transport test and a
  real PostgreSQL backend-termination test cover the transition.
- Audit batch 39 verifies `SQLDriverConnect` with no-prompt, complete, and
  complete-required modes against PostgreSQL. The driver returns the exact
  input connection string and required length, reports 01004 on truncation,
  and preserves both output destinations when a connected handle is reused.
  Interactive prompting remains explicitly unsupported with HYC00.
- Audit batch 40 reports the required 01S00 warning when an otherwise usable
  ANSI or wide connection string contains an unknown keyword. The completed
  connection is retained, the input string is still returned unchanged, and
  the shared narrow output writer replaces a duplicate buffer-copy path.
- Audit batch 41 implements environment-scoped `SQLEndTran`. It visits every
  connected child, commits or rolls back each active transaction, ignores
  inactive connections, preserves per-connection diagnostics, and reports
  25S01 on aggregate failure. Two independent PostgreSQL sessions prove both
  rollback and commit behavior; empty-environment and invalid-code cases are
  covered without server I/O.
- Audit batch 42 preserves the full native pointer width for
  `SQL_ATTR_QUIET_MODE`, accepts the disabled default for connection-level
  asynchronous execution, and distinguishes recognized-but-unsupported ODBC
  connection attributes (HYC00) from invalid identifiers (HY092). Read-only
  attributes remain rejected on the setter path, and failed getters preserve
  application outputs.
- Audit batch 43 completes the common forward-only statement-attribute matrix.
  Cursor scrollability and sensitivity report their conservative defaults;
  valid unsupported values are separated from invalid values. Application
  descriptor bind-offset and operation pointers round-trip through their
  descriptor headers, while keyset, maximum-length, automatic-IPD, and
  bookmark defaults are exposed without overstating unsupported execution
  modes. `SQL_ATTR_ROW_NUMBER` now observes the cursor-position rules, and a
  PostgreSQL regression test also proves that fetching past the end invalidates
  the prior row for `SQLGetData`.
- Audit batch 44 implements the mandatory `SQL_ATTR_OUTPUT_NTS` environment
  contract: true is the default and accepted value, false is explicitly
  unsupported, other values are invalid, and changes after DBC allocation are
  rejected. Unit coverage runs against both unixODBC and iODBC headers.
- Audit batch 45 parses and retains PostgreSQL startup `ParameterStatus`
  messages with malformed-frame rejection, then uses the negotiated
  `server_version` and resolved connection settings for the dynamic
  `SQLGetInfo` identity fields. Disconnected calls return 08003 without
  changing outputs, and the integration path verifies ANSI values plus wide
  byte lengths.
- Audit batch 46 enforces the `SQLGetInfo` connection-state contract for every
  information type except `SQL_ODBC_VER`. Capability assertions now run over
  a real PostgreSQL connection, including ANSI/wide byte lengths, numeric
  storage widths, truncation, and invalid-type diagnostics; disconnected calls
  preserve application outputs and return 08003.
- Audit batch 47 classifies the remaining common ODBC 2.x/3.x `SQLGetInfo`
  families instead of returning HY096 for defined information types. Unknown
  limits and unverified optional syntax, conversion, and scalar functions use
  the standard conservative zero/empty values. A connected PostgreSQL matrix
  verifies result types, storage widths, and values under both unixODBC and
  iODBC headers, then executes the advertised grouping, alias, join, insert,
  index, union, escape-clause, and integrity syntax end to end.
- Audit batch 48 exercises the Driver Manager-owned `SQL_DRIVER_HENV`,
  `SQL_DRIVER_HDBC`, and `SQL_DRIVER_HSTMT` mappings through the external
  application path. unixODBC must produce nonzero native handles; iODBC's
  accepted zero-valued mappings are recorded as a manager limitation.
  `SQL_DRIVER_HDESC` is likewise covered where the Driver Manager implements
  it; current iODBC forwards its manager handle to the driver and is excluded
  from that assertion because the driver cannot safely translate it.
- Audit batch 49 exhaustively checks every implemented static string and
  numeric information type, plus all five dynamic identity strings, through
  ANSI and wide entry points. Length-only calls, numeric null outputs, narrow
  and wide truncation, misaligned wide byte buffers, disconnected state, and
  invalid types have direct return-code, output-preservation, and SQLSTATE
  assertions. With the header comparison leaving only aliases and
  Driver Manager-owned types, `SQLGetInfo` is promoted to Verified.
- Audit batch 50 rechecks `SQLGetFunctions` state and diagnostics rather than
  relying only on its earlier export bitmap. Calls before connection now return
  HY010 without changing outputs, invalid identifiers return HY095, and null
  outputs return HY009. The single-function, ODBC 2 array, and ODBC 3 bitmap
  matrices run on a real PostgreSQL connection and share one export inventory
  with the dynamic-library test, eliminating the duplicated capability list.
- Audit batch 51 completes the driver-owned environment-attribute matrix.
  ODBC 2, 3, and (where defined) 3.8 versions round-trip before child
  allocation; numeric buffer lengths are ignored as required; null outputs,
  invalid values and attributes, pre-version `SQL_ATTR_OUTPUT_NTS`, and
  post-DBC mutations have exact diagnostics and preserve outputs. Connection
  pooling attributes remain correctly owned by the Driver Manager rather than
  being duplicated inside the driver, so both environment APIs are Verified.
- Audit batch 52 replaces the `SQLNativeSql` pass-through with a small,
  quote-aware ODBC escape translator shared by `SQLNativeSql`, direct
  execution, and preparation. Date/time/timestamp, scalar function, outer
  join, LIKE escape, and procedure-call clauses are covered without rewriting
  braces inside literals, identifiers, dollar strings, or comments. ANSI and
  wide length-only, explicit-length, zero-length, truncation, malformed input,
  output preservation, Driver Manager, and real PostgreSQL execution tests
  make the advertised surface Verified; unsupported function-return calls stay
  explicit with HYC00.
- Audit batch 53 implements `SQL_ATTR_NOSCAN` as real statement state. The
  default OFF path translates escapes before PostgreSQL sees them, ON preserves
  the original SQL for both direct and prepared execution, invalid modes return
  HY024 without changing state, and SQLNativeSql remains statement-independent.
- Audit batch 54 closes the cursor-lifecycle pair. `SQLCloseCursor` now has
  explicit coverage for allocated, prepared, open-empty, exhausted, already
  closed, update-count, failed-execution, and pending-result states.
  `SQLFreeStmt` proves SQL_CLOSE is harmless without a cursor, SQL_UNBIND and
  SQL_RESET_PARAMS mutate the active descriptors, invalid options preserve
  bindings, and deprecated SQL_DROP releases the statement plus its implicit
  descriptor subtree. Both APIs are Verified for the synchronous surface.
- Audit batch 55 closes the ordinary handle-type matrix. Environment handles
  are enforced as registry roots, every child type rejects a wrong parent and
  clears its output on failure, mismatched frees return SQL_INVALID_HANDLE, and
  failed frees leave the correctly typed handle usable. The only remaining
  allocation/free gap is the optional Driver Manager pooling info-token path.
- Audit batch 57 fixes SQLConnect credential semantics and authentication
  diagnostics. Non-null user and authentication arguments now override values
  stored in a DSN, while omitted arguments continue to use DSN defaults.
  PostgreSQL rejects deliberately invalid ANSI and wide credentials with 28000,
  and both paths recover on the same connection handle. Unit tests cover all
  three invalid length positions and malformed wide DSN, user, and password
  inputs without attempting network I/O. Login-deadline expiration is also
  classified as HYT00 rather than the connection-request HYT01 state.
- Audit batch 58 replaces a conditional, network-dependent SQLGetDiagRec test
  with exact ANSI and wide boundary assertions. Length-only, terminator-only,
  one-unit-short, and exact-fit retrieval preserve the full required length,
  populate independent SQLSTATE/native destinations, terminate output, and do
  not consume the record. The Driver Manager path now calls SQLGetDiagRecW
  directly, including the four-byte iODBC application ABI, so the ordinary
  synchronous handle surface is Verified.
- Audit batch 59 fixes legacy SQLError sequencing. The prior implementation
  always requested record one and then erased the complete diagnostic stack,
  losing every later record. Each handle now keeps a resettable legacy cursor;
  ANSI and wide calls consume successive records from that cursor while
  SQLGetDiagRec remains nondestructive. Tests retrieve two records across mixed
  A/W calls, reach SQL_NO_DATA, prove both records are still directly
  addressable, and verify that replacing the diagnostic stack resets the cursor.
- Audit batch 60 closes the remaining SQLError compatibility matrix. Tests
  enforce statement-before-connection-before-environment handle precedence,
  invalid/null handles, negative and terminator-only buffers, no cursor advance
  on SQL_ERROR, advance on SQL_SUCCESS_WITH_INFO, and ANSI/wide Driver Manager
  calls. The wide mapping also runs through the four-byte iODBC application ABI,
  so SQLError is Verified for the exported synchronous surface.
- Audit batch 61 completes the supported `SQLDriverConnect` boundary matrix.
  ANSI and wide entry points now cover null inputs, invalid input and output
  lengths, invalid and interactive completion modes, malformed wide text,
  invalid handles, explicit-length input, length-only output, and the
  terminator-only truncation boundary. PostgreSQL integration verifies that
  both character widths report the complete required output length and 01004
  consistently. Simultaneous truncation and unknown-keyword warnings retain
  both diagnostic records instead of overwriting the first; interactive
  prompting remains deliberately unsupported.
- Audit batch 62 tightens `SQLGetTypeInfo` without broadening advertised type
  support. Invalid type identifiers now return HY004, valid but unsupported
  types return an empty 19-column result, and `SQL_ALL_TYPES` returns an
  ordered inventory that includes both legacy and ODBC 3 date/time/timestamp
  identifiers. Disconnected and open-cursor state errors, invalid handles,
  direct ANSI/wide calls, and the external Driver Manager path are covered.
- Audit batch 63 verifies every `SQLGetTypeInfo` result column for every
  advertised PostgreSQL type. Literal delimiters, creation parameters,
  nullability, case sensitivity, signedness, scales, datetime subtypes,
  radices, and null fields are checked alongside names, sizes, and ordering.
  Non-character types now report basic predicate support instead of incorrectly
  claiming LIKE support. Exhausted-cursor behavior is covered and the API is
  promoted to Verified.
- Audit batch 64 corrects `SQLTables` pattern and PostgreSQL type semantics.
  Explicit empty catalog, schema, and table patterns no longer behave like null
  unrestricted arguments, while the standard catalog, schema, and table-type
  enumeration forms still require their companion empty strings. PostgreSQL
  temporary tables are reported and filterable as `LOCAL TEMPORARY`, including
  in the advertised type enumeration. Tests cover wildcard and escaped-
  wildcard matching, quoted case-insensitive type lists, result ordering and
  null columns, open and disconnected states, malformed wide input, and A/W
  enumeration parity.
- Audit batch 65 corrects the `SQLColumns` argument classes. `CatalogName` is
  now a case-sensitive ordinary argument instead of a wildcard pattern, while
  schema, table, and column names retain ODBC pattern matching and the
  advertised backslash escape. Explicit empty arguments no longer collapse
  into null unrestricted searches. PostgreSQL tests distinguish `_` from
  escaped underscores, prove literal `%` catalog handling, verify an exact
  current-catalog match, cover every null-versus-empty position and open-cursor
  diagnostics, and exercise the same escaped patterns through `SQLColumnsW`.
- Audit batch 66 corrects `SQLPrimaryKeys` ordinary-argument handling. Present
  empty catalog and schema names now match only catalogless or schemaless
  tables instead of becoming unrestricted. A quoted PostgreSQL table whose
  name contains both a dot and an apostrophe proves that table names are
  literal, safely escaped, and never split into multipart identifiers. Tests
  also reject wildcard-looking catalog, schema, and table names, verify
  constraint rather than physical column ordering, preserve an open cursor,
  enforce the required table pointer with HY009, cover malformed wide input,
  and repeat exact catalog/schema/table selection through `SQLPrimaryKeysW`.
- Audit batch 67 corrects `SQLForeignKeys` scope and ordinary arguments.
  References to unique constraints are no longer misreported as references to
  a primary key, and present empty catalog/schema arguments constrain the
  result instead of disappearing. PostgreSQL tests cover primary-only,
  foreign-only, and combined filters; literal wildcard-looking values on both
  sides; foreign-table ordering; all five update/delete rules; all three
  deferrability values; required-pointer diagnostics; malformed wide input;
  and exact six-argument `SQLForeignKeysW` matching.
- Audit batch 68 tightens `SQLStatistics` metadata instead of inventing
  precision the server does not expose. Index cardinality is now null rather
  than the unrelated table-row estimate, page counts come from the index
  relation rather than the table, expressions are returned as column text,
  and non-orderable hash columns no longer claim ascending order. Empty and
  wildcard-looking ordinary arguments are literal, `SQL_INDEX_UNIQUE` is
  verified, unsupported exact `SQL_ENSURE` statistics return HYC00, and the
  distinct invalid accuracy option now returns HY101 rather than HY100. A/W
  PostgreSQL tests cover quoted names, partial, expression, and hash indexes.
- Audit batch 69 corrects `SQLProcedures` argument classes. The catalog is a
  case-sensitive ordinary argument rather than a pattern, while schema and
  procedure names preserve pattern semantics even when explicitly empty.
  PostgreSQL tests distinguish wildcard and escaped underscores, retain both
  overload rows, classify functions separately from procedures, validate the
  core count/type columns, protect an open cursor, reject malformed wide text,
  and repeat exact catalog/schema/pattern selection through `SQLProceduresW`.
- Audit batch 70 corrects `SQLProcedureColumns` filtering and required result
  values. Catalog matching is literal while schema, procedure, and column
  patterns retain null-versus-empty behavior. Unnamed parameters and function
  returns now use the required empty `COLUMN_NAME`, which also makes an empty
  column pattern select only those rows. Results include column type in their
  standard ordering. PostgreSQL tests cover input, input/output, output, and
  return rows; ordinal positions; wildcard escaping; overload-like name
  matches; open cursors; malformed wide input; and A/W parity.
- Audit batch 71 makes `SQLSpecialColumns` scope claims conservative. Ordinary
  catalog, schema, and table values now preserve explicit empties, and primary
  key columns report `SQL_SCOPE_CURROW`; requests requiring transaction or
  session stability return an empty result because PostgreSQL permits key
  updates and the driver cannot guarantee those wider scopes. `SQL_ROWVER`
  remains an explicit empty result because PostgreSQL has no general-purpose
  automatically updated row-version column. Tests cover composite key order,
  nullable modes, literal wildcard-looking values, exact A/W selection,
  malformed wide text, open cursors, and HY097/HY098/HY099 option diagnostics.
- Audit batch 72 replaces `SQLForeignKeys`' ambiguous information-schema join
  with PostgreSQL object-identity joins. PostgreSQL permits the same foreign
  key constraint name on multiple tables; joining by schema and name could
  cross-pair those unrelated constraints. The query now follows relation and
  attribute OIDs, zips each foreign/referenced column pair by ordinality, and
  locates the referenced primary-key constraint by its exact column set. The
  integration fixture deliberately reuses a constraint name across two child
  tables while retaining unique-constraint exclusion and ordering checks.
- Audit batch 73 completes `SQLSpecialColumns` row-identifier selection beyond
  primary keys. A primary key remains the preferred candidate, followed by the
  shortest usable unique index in stable object order. Invalid, not-ready,
  partial, and expression indexes are excluded; included columns are not
  reported as key columns. `SQL_NO_NULLS` rejects a whole candidate if any key
  column is nullable instead of returning an invalid key subset. PostgreSQL
  tests cover nullable selection, non-null fallback, included-column exclusion,
  and the no-qualifying-index empty result.
- Audit batch 74 aligns `SQLStatistics` with PostgreSQL index structure. Hash
  indexes now report `SQL_INDEX_HASHED`, indexes marked as the table's cluster
  index report `SQL_INDEX_CLUSTERED`, and other access methods remain
  `SQL_INDEX_OTHER`. Included attributes remain visible as index columns with
  their actual ordinal positions but return null sort direction because they
  are stored values rather than ordering keys. Invalid or dropping indexes are
  excluded. PostgreSQL tests distinguish key and included attributes and keep
  `SQL_INDEX_UNIQUE` filtering independent of covering-index storage.
- Audit batch 75 verifies `SQLProcedures` parameter counts against PostgreSQL's
  full argument-mode model. Real functions now prove that `INOUT` contributes
  to both input and output counts, `VARIADIC` contributes to the input count,
  and every `TABLE` result column contributes to the output count. This closes
  the prior count gap without adding duplicate production logic: the existing
  catalog query already handled the server's `i`, `o`, `b`, `v`, and `t` modes
  correctly.
- Audit batch 76 shares PostgreSQL type classification across `SQLColumns`,
  `SQLProcedureColumns`, and `SQLSpecialColumns`. Catalog rows now identify
  UUID, JSONB, arrays, and domains by their PostgreSQL type names instead of
  generic information-schema labels; domain rows use their underlying ODBC
  data type and base integer size. The text `CHAR_OCTET_LENGTH` now has the
  driver's declared large-text limit rather than a spurious null. Real
  PostgreSQL tests cover the three catalog paths, numeric precision/scale,
  domain sizes, and ANSI/wide parity. Variable-length size/precision details
  and nested-domain result/parameter metadata remain explicit follow-ups.
- Audit batch 77 reports UUID's canonical 36-character PostgreSQL text form
  consistently as `SQL_VARCHAR` metadata. `SQLDescribeCol` now reports a
  column size of 36, and `SQLColumns`, `SQLProcedureColumns`, and
  `SQLSpecialColumns` report 36 for their corresponding character size and
  buffer-length fields. A real PostgreSQL test checks all four paths. Other
  variable-length and nested-domain metadata remain open.
- Audit batch 78 derives time and timestamp `COLUMN_SIZE` from PostgreSQL's
  declared fractional-second precision in result descriptions, `SQLColumns`,
  and `SQLSpecialColumns`; the maximum unzoned timestamp size is corrected to
  26 in `SQLGetTypeInfo` and inferred `SQLDescribeParam`. It also reports the
  ODBC default-C-type transfer
  lengths (6 bytes for date/time, 16 for timestamp) in `SQLColumns`,
  `SQLProcedureColumns`, and `SQLSpecialColumns`, and supplies temporal
  `DECIMAL_DIGITS` for special columns. PostgreSQL tests cover precision 0,
  explicit fractional precision, time-zone variants, primary-key metadata,
  and routine arguments. Routine argument `COLUMN_SIZE` remains the default
  maximum because PostgreSQL's routine argument type OIDs do not carry typmods.
  See the ODBC [column-size](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/column-size)
  and [transfer-length](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/transfer-octet-length)
  rules and PostgreSQL's [information-schema precision](https://www.postgresql.org/docs/current/infoschema-columns.html).
- Audit batch 79 makes the iODBC Driver Manager descriptor test account for
  both Unicode ABI paths: direct four-byte `SQLWCHAR` calls report the name
  length in characters, while iODBC's two-byte-driver to four-byte-application
  conversion reports it in bytes. Both paths still verify the full wide name,
  terminator, and type. This is a test expectation for observed manager
  behavior, not a claim that the mixed-width byte count follows ODBC's
  [character-count contract](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdescrec-function).
- Audit batch 80 resolves PostgreSQL domain chains in `SQLColumns`,
  `SQLProcedureColumns`, and `SQLSpecialColumns` through recursive `pg_type`
  links. A two-level integer domain now retains its outer type name while
  reporting `SQL_INTEGER`, fixed size and transfer length, scale, radix, and
  SQL data type. Real PostgreSQL tests cover table columns, a primary key,
  input and return parameters, and `SQLColumnsW`. Typmod-bearing nested
  domains and result/parameter descriptors are not yet covered. The query
  relies on the documented [`typbasetype` link](https://www.postgresql.org/docs/current/catalog-pg-type.html),
  not the newer `pg_basetype` function.
- Audit batch 81 resolves unknown PostgreSQL parameter type OIDs through
  recursive `pg_type.typbasetype` links before populating `SQLDescribeParam`
  and the implementation parameter descriptor. It only queries the catalog
  for unrecognized OIDs and caches their base type for the statement; known
  built-in parameters incur no extra query. A real PostgreSQL test covers a
  two-level integer domain before execution, its result-column description,
  and execution after metadata inspection. PostgreSQL already reports the
  integer base type for that domain in result-column metadata. Domain
  constraints and typmods (such as a domain over `varchar(n)` or
  `numeric(p,s)`) are not yet reflected in parameter size or scale because
  `ParameterDescription` supplies a type OID but no typmod.
- Audit batch 82 extends the domain parameter lookup to retain the nearest
  declared `typtypmod` while walking nested domains. `SQLDescribeParam` and
  the implementation parameter descriptor now report length for nested
  `varchar(13)` and precision/scale for `numeric(8,3)`. The real PostgreSQL
  test covers duplicate parameter OIDs in one statement and both descriptor
  fields. It also confirms that PostgreSQL's prepared result metadata already
  supplies the same modifiers to `SQLDescribeCol` without another lookup.
  This does not infer typmods for ordinary non-domain parameters,
  which PostgreSQL's `ParameterDescription` does not provide, nor does it
  expose domain constraints or names through the parameter descriptor.
- Audit batch 83 carries the effective typmod through the recursive
  PostgreSQL catalog type join used by `SQLColumns`. Nested `varchar(13)` and
  `numeric(8,3)` domains now report the same data type, column and transfer
  size, scale, radix, and character octet length as plain columns. A real
  PostgreSQL test compares all six fields, including `SQLColumnsW` parity for
  the character domain. Routine argument and special-column typmod coverage
  remains separate work.
- Audit batch 84 applies that effective typmod to `SQLSpecialColumns` and
  `SQLProcedureColumns` using shared character-length, octet-length, numeric
  precision, buffer-length, and scale expressions. A real PostgreSQL test
  compares a composite primary key and routine input/return arguments over
  nested `varchar(13)` and `numeric(8,3)` domains with the corresponding
  `SQLColumns` values, including ANSI/wide parity. Non-domain routine
  arguments still use conservative sizes because `pg_proc` carries type OIDs
  but not argument typmods.
- Audit batch 85 decodes PostgreSQL's signed numeric scale from the low
  11 typmod bits instead of interpreting the encoded value as a positive
  scale. A PostgreSQL 17 integration test covers `numeric(8,-2)` on plain
  and nested-domain columns in `SQLColumns` and `SQLDescribeCol`, plus
  nested-domain `SQLSpecialColumns`, `SQLProcedureColumns`, and
  `SQLDescribeParam`, the parameter descriptor, and `SQLColumnsW`. The test
  skips PostgreSQL versions before 15, when [negative numeric scales were
  introduced](https://www.postgresql.org/docs/15/datatype-numeric.html).
  Catalog fallback values from `information_schema` are normalized as well,
  since they expose the encoded value for this case.
- Audit batch 86 exposes the type-derived IRD fields already calculated for
  result columns through `SQLColAttribute` and `SQLColAttributeW`, including
  type names, literal syntax, display and octet lengths, and numeric/type
  characteristics. A prepared PostgreSQL query checks these values against
  `SQLGetDescField`, explicit numeric expectations, legacy field aliases,
  ANSI truncation diagnostics, and wide string byte lengths. ODBC 3
  `SQL_DESC_PRECISION` now uses the IRD value while the ODBC 2
  `SQL_COLUMN_PRECISION` retains column-size semantics. Origin-name
  fields remain unsupported because the driver does not yet populate them.
- Audit batch 87 makes `SQL_DESC_UNSIGNED` return `SQL_TRUE` for nonnumeric
  result types while keeping signed PostgreSQL numeric types `SQL_FALSE`,
  as required by the [ODBC column-attribute definition](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolattribute-function).
  The real PostgreSQL test checks both the IRD and `SQLColAttribute` for
  numeric and character columns.
- Audit batch 88 sizes numeric display and transfer buffers for PostgreSQL
  scales outside the ordinary ODBC range. Negative scales can add integer
  digits beyond precision, and scales at least as large as precision require
  a leading zero before the decimal point. Real PostgreSQL values exercise
  both cases and check the resulting `SQLColAttribute` display/octet sizes
  against the IRD where applicable. Standard positive-scale values retain
  the [ODBC precision-plus-two rule](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/display-size).
- Audit batch 89 reads ODBC 3 `SQLColAttribute` values from the active IRD
  record instead of reconstructing a second descriptor from cached column
  metadata. ODBC 2 column-size and decimal-digit identifiers keep their
  separate legacy values. This also ensures later IRD origin-name enrichment
  will not diverge from `SQLColAttribute`.
- Audit batch 90 tests that changing an IPD parameter type invalidates the
  prepared result shape for type-name attributes as well as `SQLDescribeCol`.
  Stale `ColumnInfo` comments that incorrectly equated concise SQL type and
  decimal digits with different descriptor fields were removed.
- Audit batch 91 advertises PostgreSQL 15+'s `numeric`/`decimal` minimum
  scale of -1000 in `SQLGetTypeInfo`, while older servers retain zero. The
  all-types integration matrix and `SQLGetTypeInfoW` check the value against
  the connected server version, matching [PostgreSQL's documented scale range](https://www.postgresql.org/docs/17/datatype-numeric.html).
- Audit batch 92 closes the underlying socket after a failed TLS handshake or
  certificate verification in both sync and async transports, so a failed
  upgrade cannot leave a writable plaintext connection. Sync reconnect and
  raw-socket upgrade also discard prior TLS state; timeout and certificate
  rejection have direct no-plaintext-I/O regression checks. Socket closure
  avoids writing TLS close-notify to a peer that may have already gone away.
- Audit batch 93 retries interrupted plain-socket send/receive calls while
  rechecking the original deadline. A POSIX signal-interruption regression
  verifies that `DeadlineModel=SocketTimeout` receives a timeout diagnostic
  rather than an unrelated I/O failure.
- Audit batch 94 rebuilds the synchronous TLS context on the next connection
  after verification, CA location, or minimum-protocol settings change. A
  reconnect regression verifies that switching from unverified TLS to peer
  verification rejects a self-signed certificate instead of reusing the old
  no-verification context.
- Audit batch 95 checks IPv4/IPv6 hosts against IP subject-alternative names
  in both sync and async TLS transports; DNS hosts continue to use DNS/CN
  matching. A certificate with a numeric DNS SAN cannot impersonate a valid
  IP address. See [OpenSSL's X.509 matching contract](https://docs.openssl.org/3.0/man3/X509_check_host/).
- Audit batch 96 omits SNI for literal IPv4/IPv6 addresses in both TLS
  transports while preserving it for DNS names. Loopback servers inspect the
  actual ClientHello behavior. [RFC 6066](https://datatracker.ietf.org/doc/html/rfc6066#section-3)
  disallows literal IP addresses in the SNI `HostName` field.
- Audit batch 97 rejects embedded NUL bytes in host strings before DNS
  resolution in sync, epoll, and IOCP transports. This prevents the resolver
  from silently connecting to the truncated prefix; a rejected reconnect also
  closes the previous socket.
- Audit batch 98 aligns synchronous TLS zero-length send/receive with plain
  and async transports, avoiding a zero-length `SSL_read` and unnecessary
  deadline checks. TLS I/O also caps each OpenSSL `int` length argument at
  `INT_MAX`, with boundary tests that do not allocate giant buffers. See the
  [OpenSSL write](https://docs.openssl.org/3.2/man3/SSL_write/) and
  [read](https://docs.openssl.org/3.6/man3/SSL_read/) signatures.
- Audit batch 99 rejects embedded NUL bytes in TLS upgrade hostnames even
  when certificate verification is disabled, and closes the underlying plain
  socket. Sync and async loopback tests verify the rejection and that no
  plaintext I/O remains available after failure.
- Audit batch 100 publishes a synchronous TLS context only after protocol
  bounds and trust-store configuration succeed. Invalid minimum protocol
  versions fail explicitly. Failed CA-file loading is retried on reconnect
  rather than leaving a partially configured context in use; a two-connection
  regression checks both failures.
- Audit batch 101 requires `getsockopt(SO_ERROR)` itself to succeed before a
  pending TCP connection is accepted. A Windows CI regression revealed that
  polling only for writability can consume the whole deadline on a refused
  endpoint, preventing fallback from IPv6 localhost to IPv4. Windows CI also
  timed out with `select` watching both write and exception sets. Pending
  Winsock connects now register `FD_CONNECT` before calling `connect` and
  await its completion event and error code using
  [Microsoft's `WSAEventSelect` contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect).
  Because the Windows runner still gave no completion event for a refused
  endpoint before its deadline, a shared deadline is divided among remaining
  resolved addresses so one stalled address cannot starve IPv4 fallback.
  Refused-port and IPv4-only localhost tests cover failure cleanup and
  address fallback; the prompt-refusal timing assertion applies only on POSIX.
- Audit batch 102 parses transport settings only after merging driver-wide,
  DSN, and connection-string layers. A valid higher-priority value now
  overrides an invalid lower-priority value. Async size settings use strict
  decimal parsing, rejecting signs, whitespace, trailing characters, zero,
  and overflow. Unit tests cover both precedence and malformed values.
- Audit batch 103 adds IPv4-only localhost fallback tests for Epoll and IOCP.
  Windows CI exposed the same first-address starvation in IOCP: an unfinished
  `ConnectEx` could consume the whole login deadline. IOCP now budgets each
  pending endpoint, cancels and closes a stalled socket, then attempts the
  next address while preserving the overall deadline and one-shot completion.
- Audit batch 104 applies the per-endpoint budget to Linux Epoll as well, so a
  silent first address does not consume the deadline before later addresses
  are tried. Both native async backends have expired-connect recovery tests
  and IPv4-only localhost fallback coverage.
- Audit batch 105 makes synchronous TCP connect failure close any socket left
  by an exception during socket configuration or post-connect I/O setup. The
  refused-port regression now begins from a live connection and checks that
  the failed reconnect leaves no native socket behind.
- Audit batch 106 parses semicolon-delimited connection attributes without
  splitting braced values, decodes escaped closing braces, and rejects
  malformed braced values. This prevents embedded text such as
  `PWD={secret;SERVER=other}` from changing connection options. Unit tests
  cover escaped braces, whitespace, malformed input, and option precedence;
  ANSI and wide Driver Manager connections verify that a semicolon inside a
  braced description does not produce a spurious 01S00 warning. The syntax
  follows [Microsoft's ODBC connection-string grammar](https://learn.microsoft.com/en-us/openspecs/sql_server_protocols/ms-odbcstr/55953f0e-2d30-4ad4-8e56-b4207e491409).
- Audit batch 107 applies the first occurrence of a repeated connection-string
  keyword, as specified by [SQLDriverConnect](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldriverconnect-function).
  A unit test checks the parsed value, and ANSI/wide Driver Manager connections
  prove that a later, invalid password cannot override the first valid one.
- Audit batch 108 validates `PORT` as an entire unsigned decimal value in the
  TCP range 1–65535 before narrowing it to 16 bits. This prevents `65536` from
  wrapping to zero and rejects signs, suffixes, empty values, and overflow.
  ANSI/wide API tests verify `SQL_ERROR`, `HY000`, and a PORT-specific message
  without making a network connection.
- Audit batch 109 parses `SSL` as a boolean option instead of treating every
  unrecognized value as false. Values such as `SSL=require` now fail before
  connecting instead of silently using plaintext. ANSI/wide API tests cover
  malformed values, error diagnostics, and braced text; Driver Manager tests
  connect with accepted `off` and `no` values. The README documents accepted
  spellings and the no-downgrade behavior.
- Audit batch 110 rejects embedded NUL bytes in explicit-length connection
  strings, connection identities and credentials, and PostgreSQL startup and
  authentication fields before a packet can be sent. PostgreSQL defines these
  fields as [NUL-terminated strings](https://www.postgresql.org/docs/current/protocol-message-types.html),
  so an embedded NUL could change how the server interprets subsequent bytes.
  Parser and ANSI/wide ODBC tests cover connection strings, users, passwords,
  database names, and startup parameter names and values.
- Audit batch 111 rejects embedded NUL bytes in simple and extended SQL text
  before PostgreSQL's NUL-terminated query fields are framed or a transaction
  begins. `SQLExecDirect`/`SQLPrepare` and their wide forms return 42000;
  PostgreSQL integration confirms the statement handle can execute a valid
  query afterward. The simple-query path now converts parser exceptions to
  `InvalidParameter`, matching prepared and description requests.
- Audit batch 112 applies the same embedded-NUL rejection to `SQLNativeSql`
  and `SQLNativeSqlW`. Explicit-length input now returns 42000 before escape
  translation, leaving the caller's output buffer and output length untouched;
  PostgreSQL integration covers both character widths.
- Audit batch 113 treats malformed PostgreSQL query frames as fatal to the
  logical connection. Parser and result-decoding failures return a protocol
  error, mark the connection dead, and prevent a subsequent query from using
  the untrusted stream. The ODBC error mapper classifies protocol errors as
  communication-link failures (08S01). A synthetic server-response test covers
  a truncated `RowDescription` followed by `ReadyForQuery`.
- Audit batch 114 separates malformed PostgreSQL authentication frames from
  credential rejection. Parser failures during startup now return a protocol
  error rather than an authentication failure; synthetic backend tests cover
  both a truncated authentication request and a server `ErrorResponse` that
  rejects the credentials.
- Audit batch 115 validates the [PostgreSQL `ReadyForQuery` message](https://www.postgresql.org/docs/current/protocol-message-formats.html):
  it must carry exactly one `I`, `T`, or `E` status byte. Malformed markers
  during startup return a protocol error without opening the connection;
  malformed query-completion markers invalidate an established connection.
- Audit batch 116 validates [PostgreSQL error fields](https://www.postgresql.org/docs/current/protocol-error-fields.html)
  and the terminating byte. Missing required fields, duplicate fields,
  unterminated values, and trailing bytes are protocol errors. Query execution
  tracks the presence of an `ErrorResponse` independently of its message text,
  so a malformed or empty error cannot be mistaken for success; a synthetic
  backend regression covers the missing-message case.
- Audit batch 117 validates fixed-size startup frames. AuthenticationOk and
  AuthenticationCleartextPassword must carry exactly four payload bytes;
  BackendKeyData must carry eight in the driver's negotiated PostgreSQL 3.0
  protocol, rather than the variable-length key permitted by 3.2. Malformed
  lengths fail startup with a protocol error instead of opening a connection.
- Audit batch 118 enforces the [PostgreSQL startup sequence](https://www.postgresql.org/docs/current/protocol-flow.html):
  `ReadyForQuery` cannot complete a connection before `AuthenticationOk`.
  A server `ErrorResponse` before authentication remains a credential failure;
  after authentication it is classified by the server SQLSTATE: class `28`
  remains an authentication failure (including a nonexistent role after
  `AuthenticationOk`), while other errors are startup/connection failures.
- Audit batch 119 rejects out-of-phase PostgreSQL startup frames. A second
  authentication request after `AuthenticationOk` cannot trigger another
  credential response, and `ParameterStatus` or `BackendKeyData` before
  authentication succeeds cannot be mistaken for normal startup metadata.
- Audit batch 120 rejects unexpected backend frames during PostgreSQL startup
  instead of silently skipping them until `ReadyForQuery`. Query-phase frames
  cannot precede the first query, while a post-authentication `NoticeResponse`
  remains permitted by the [startup flow](https://www.postgresql.org/docs/current/protocol-flow.html).
- Audit batch 121 validates [PostgreSQL `NoticeResponse` fields](https://www.postgresql.org/docs/current/protocol-error-fields.html)
  with the same field decoder used for `ErrorResponse`. Missing mandatory
  fields, duplicates, and unterminated notices fail startup with a protocol
  error instead of being silently ignored.
- Audit batch 122 retains [asynchronous `ParameterStatus` changes](https://www.postgresql.org/docs/current/protocol-flow.html)
  received during a query, so connection metadata stays current after `SET`
  and other changes. Startup and query responses share the same payload
  validator; a malformed query-time status invalidates the connection.
- Audit batch 123 preserves successful PostgreSQL results before a later
  batch error. The first result remains accessible, then `SQLMoreResults`
  reports the aborted batch with `SQL_ERROR` as required by the
  [ODBC multiple-results contract](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/multiple-results).
  Server-specific query SQLSTATE mapping remains a separate diagnostic gap.
- Audit batch 124 preserves PostgreSQL class-22 data-exception SQLSTATEs in
  immediate and deferred query errors. Real `SQLExecDirect`, `SQLExecute`,
  and `SQLMoreResults` division-by-zero paths now report `22012`; other
  server-specific classes retain the existing conservative mapping pending
  an ODBC-by-PostgreSQL SQLSTATE audit. Malformed five-character server codes
  are rejected as protocol errors.
- Audit batch 125 maps PostgreSQL class-23 integrity-constraint errors to
  ODBC `23000` as documented for [SQLExecDirect](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function).
  A real duplicate-key test covers direct execution, prepared execution,
  deferred `SQLMoreResults`, and subsequent connection recovery.
- Audit batch 126 maps PostgreSQL `42P01` (undefined table) and `42703`
  (undefined column) to ODBC `42S02` and `42S22` using the
  [SQLExecDirect diagnostics](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function).
  Other class-42 errors retain the existing `42000` fallback. Real
  PostgreSQL tests cover direct, prepared, and deferred-result paths.
- Audit batch 127 maps PostgreSQL `42P07` (duplicate table) and `42701`
  (duplicate column) to ODBC `42S01` and `42S21`, respectively, using the
  [PostgreSQL error-code list](https://www.postgresql.org/docs/current/errcodes-appendix.html)
  and [SQLExecDirect diagnostics](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function).
  A real PostgreSQL test covers direct, prepared, deferred-result, and
  recovery paths; other class-42 errors retain the `42000` fallback.
- Audit batch 128 narrows `42P07`: PostgreSQL also uses that code for a
  duplicate index name. Direct and prepared `CREATE INDEX` errors now map to
  ODBC `42S11`, while `CREATE TABLE`/`CREATE VIEW` map to `42S01`. A deferred
  batch error without an identified statement retains `42000` rather than
  claiming the wrong object kind. Real PostgreSQL tests cover duplicate
  table/index direct, prepared, and deferred errors plus connection recovery.
- Audit batch 129 maps PostgreSQL `42704` (undefined object) to ODBC `42S12`
  only for a known `DROP INDEX` statement, following the
  [SQLExecDirect diagnostics](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function).
  Other undefined objects and deferred errors without statement identity
  retain `42000`. Real PostgreSQL tests cover direct/prepared/deferred errors
  and recovery.
- Audit batch 130 preserves PostgreSQL `3F000` (invalid schema name), which
  matches [ODBC `3F000`](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function).
  A real nonexistent-schema test covers direct, prepared, and deferred
  execution plus connection recovery.
- Audit batch 131 translates PostgreSQL `22P02` (invalid text representation)
  to ODBC `22018` (invalid character value for cast specification) for query
  errors. Other class-22 states remain unchanged. A real invalid numeric cast
  test covers direct, prepared, deferred, and recovery paths using the
  [SQLPrepare diagnostics](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprepare-function).
- Audit batch 132 prevents a cancelled thread-pool receive from writing into
  the caller's buffer after cancellation returns. The worker receives into
  owned storage and copies under the completion/cancellation lock only when
  completion wins. A delayed loopback reply and a queued-work barrier verify
  that cancellation leaves the buffer unchanged and calls back once. Native
  epoll and IOCP receive-buffer cancellation remain separate lifetime audits.
- Audit batch 133 gives epoll receives owned storage too. Completion copies
  into the caller's span only while winning the operation-state lock, so a
  cancellation cannot return and then race with a reactor write. Existing
  loopback round-trip and cancellation tests exercise both paths; IOCP remains
  to be audited independently.
- Audit batch 134 gives IOCP receives owned storage for the duration of the
  overlapped Winsock operation. The completion transfers successful bytes to
  the caller before marking the operation complete; cancelled receives do not
  copy. Windows loopback tests check normal round-trip and cancellation
  followed by a successful receive into a different buffer.
- Audit batch 135 makes the async TLS receive completion and cancellation
  decision atomic with the copy into the caller's buffer. A cancellation that
  wins the operation-state lock reports cancellation and leaves the buffer
  unchanged; a completed copy makes a later cancellation a no-op. The TLS
  cancellation test now checks the buffer as well as callback cardinality,
  and the round-trip checks a zero-length receive before ordinary I/O.
  The TLS loopback suite also runs on macOS with the thread-pool transport,
  instead of compiling to an empty test binary there.
- Audit batch 136 makes zero-length socket and TLS I/O validate connection
  state and the deadline before reporting a successful no-op, matching the
  native async engines. Tests cover disconnected, expired, and connected
  no-op calls in Strict and SocketTimeout modes, plus synchronous and async
  TLS paths. The earlier expired-deadline TLS no-op test was updated to assert
  the strict deadline contract while retaining the connected no-op check.
- Audit batch 137 skips thread-pool connect, send, and receive work when its
  operation was cancelled while queued. Before the fix, deterministic
  one-worker tests showed a cancelled send reaching the peer, a cancelled
  receive consuming the next byte, and a cancelled connect closing the live
  socket. The same tests now confirm those queued tasks execute no socket I/O.
  Cancellation after a worker starts remains best-effort because the
  synchronous socket call may already be in progress.
- Audit batch 138 reaps completed epoll requests when admitting new work, so
  a cancelled request releases its queue slot immediately even while the
  reactor is occupied by a callback. A Linux-only test holds that callback,
  cancels a queued receive at depth one, and submits a replacement before
  releasing the reactor. IOCP cancellation remains completion-driven and is
  not covered by this immediate-release rule.
- Audit batch 139 distinguishes an unannounced TCP close from authenticated
  TLS shutdown in the async receive path. Raw EOF now returns a TLS error;
  only OpenSSL's `SSL_ERROR_ZERO_RETURN` reports clean EOF after a
  `close_notify`, as [OpenSSL documents](https://docs.openssl.org/3.4/man3/SSL_shutdown/).
  Loopback tests cover both abrupt closure and a real `close_notify`.
- Audit batch 140 applies the same distinction to synchronous TLS receive.
  [OpenSSL's version-specific unexpected-EOF reporting](https://docs.openssl.org/1.1.1/man3/SSL_get_error/)
  is handled in both the 1.1 and 3.x forms. Protocol failures now carry the
  TLS error category; underlying socket failures remain network errors and
  deadline expiration remains a timeout. Loopback tests cover both closure
  paths, including a real `close_notify`.
- Audit batch 141 removes direct stderr writes from synchronous TLS setup
  failures. The first OpenSSL error now reaches the caller through the TLS
  diagnostic, and the error queue is drained before subsequent TLS operations.
  The invalid trust-store test asserts the diagnostic retains OpenSSL detail
  on repeated connection attempts.
- Audit batch 142 makes the synchronous TLS receive error classifier directly
  testable. Unit cases cover both pre-3.0 and 3.x unexpected-EOF forms, while
  preserving network classification for syscall failures and nonempty error
  queues; the loopback tests continue to cover real TLS shutdown behavior.
- Audit batch 143 adds a SocketTimeout-mode regression test for the maximum
  representable deadline. It exercises both connection setup and the
  connected no-op receive, which applies the platform socket timeout, without
  waiting for that long deadline to elapse.
- Audit batch 144 reclaims canceled thread-pool requests when a full queue
  admits new work. The one-worker, depth-one test first reproduced a rejected
  replacement while the worker was held in a callback, then confirms the
  canceled request releases capacity immediately and a genuinely full live
  queue still rejects overflow. Only full-queue submissions scan pending work.
- Audit batch 145 also reclaims deadline-expired thread-pool requests at
  admission. Expiration callbacks run outside the queue lock, so an expired
  request reports a timeout and releases its slot even if the sole worker is
  blocked in another callback. A deterministic depth-one test reproduced the
  prior rejected replacement and delayed timeout callback before the fix.
- Audit batch 146 distinguishes thread-pool queue rejection from caller
  cancellation, matching epoll and IOCP. A full queue now completes the
  operation once with a queue-full network error while `is_cancelled()` stays
  false; the depth-one overflow test first reproduced the old cancellation
  status and generic message before the change.
- Audit batch 147 extends that saturated-queue regression to connect and
  receive as well as send. Each rejected operation completes without a false
  cancellation flag, and the rejected receive leaves the caller buffer intact.
  The deterministic test passed five repeated local runs.
- Audit batch 148 removes typed stores to application-owned `SQL_C_WCHAR`
  buffers in `SQLGetData`. A PostgreSQL-backed test positions the output one
  byte off `SQLWCHAR` alignment; UBSan first reported misaligned copy and
  terminator stores, then passed with strict fail-on-error after byte-wise
  copies. The test runs in both two- and four-byte driver builds.
- Audit batch 149 applies the same alignment rule to bound-column wide output
  in `TextDataConverter`. A unit test first reproduced a misaligned store under
  strict UBSan; byte-wise copies now handle both complete and truncated output.
  A PostgreSQL-backed `SQLBindCol` test verifies the real fetch path with an
  unaligned application buffer in both two- and four-byte builds. Linux CI
  additionally caught a null source pointer on zero-byte copies for empty
  strings; those copies are skipped.
- Audit batch 150 removes typed stores to application-owned integral and
  floating-point result buffers. Strict UBSan first reported a misaligned
  `SQL_C_SLONG` store; byte-wise copies now cover integer and floating result
  conversions. Unit tests exercise complete, fractional-truncation, and
  invalid-input behavior with unaligned buffers; a PostgreSQL-backed
  `SQLBindCol` test covers integer and double fetches on that path.
- Audit batch 151 handles date, time, and timestamp structs the same way.
  Strict UBSan first reported misaligned `SQL_DATE_STRUCT` member access;
  locally assembled structs are now copied as bytes to application buffers.
  Unit tests cover all three types plus an invalid-date no-write case, and a
  PostgreSQL-backed `SQLBindCol` test exercises all three on unaligned output.
- Audit batch 152 applies byte-wise reads to numeric bound parameters during
  prepared execution. A PostgreSQL-backed test first reproduced a strict UBSan
  misaligned `SQL_C_SLONG` load, then passed with unaligned integer, float, and
  double input buffers; existing parameter tests cover null, type, and state
  failures separately.
- Audit batch 153 makes the shared SQLWCHAR-to-UTF-8 decoder read code units
  through byte-wise copies. A unit test first reproduced a strict UBSan
  misaligned span access, then covered explicit lengths, `SQL_NTS`,
  supplementary characters, and malformed surrogate input. A PostgreSQL-backed
  prepared-parameter test verifies an unaligned wide input round-trip.
- Audit batch 154 removes typed stores to `SQLGetInfo` numeric output buffers.
  A PostgreSQL-backed test first reproduced a strict UBSan misaligned
  `SQLUSMALLINT` store, then verified two- and four-byte numeric values through
  the A/W entry points and output preservation on an unknown information type.
  Windows CI also caught a platform macro collision with the test's `small`
  variable; the variable was renamed before rerunning CI.
- Audit batch 155 removes typed stores to `SQLGetDiagField` numeric output
  buffers. A unit test first reproduced a strict UBSan misaligned `SQLINTEGER`
  store, then checked unaligned header and record values through the A/W entry
  points, including no-data output preservation.
- Audit batch 156 removes typed stores from `SQLGetDiagRec` numeric outputs and
  from the shared wide-string output helpers. A unit test first reproduced a
  strict UBSan misaligned native-error store, then covered A/W diagnostic
  records, unaligned wide SQLSTATE and message buffers, the wide diagnostic
  field's byte-length variant, and no-data output preservation.
- Audit batch 157 copies narrow `SQLGetDiagField` string lengths byte-wise.
  A focused unit test first reproduced a strict UBSan misaligned
  `SQLSMALLINT` store, then verified the length-only query on an unaligned
  output pointer.
- Audit batch 158 copies lengths from the shared narrow-string writer and
  numeric `SQLGetInfo` branches byte-wise. A unit test first reproduced a
  strict UBSan misaligned string-length store; PostgreSQL-backed tests cover
  two- and four-byte numeric values through A/W entry points and preserve the
  output length on an unknown information type.
- Audit batch 159 copies `SQLGetEnvAttr`, `SQLGetConnectAttr`, and
  `SQLGetStmtAttr` output lengths byte-wise. A strict UBSan unit test first
  reproduced a misaligned environment-attribute length store, then exercised
  environment, wide connection, and statement attributes plus error-path
  preservation on the same unaligned buffer.
- Audit batch 160 removes typed stores from `SQLGetEnvAttr` numeric values.
  A strict UBSan unit test first reproduced a misaligned ODBC-version store,
  then covered the ODBC version, output-termination flag, iODBC Unicode type,
  and output preservation for an unsupported attribute.
- Audit batch 161 removes typed stores from `SQLGetConnectAttr` numeric and
  pointer-sized values. A strict UBSan unit test first reproduced a misaligned
  login-timeout store, then covered narrow numeric and wide quiet-mode output
  plus preservation of the output buffer for an unsupported attribute.
- Audit batch 162 removes typed stores from `SQLGetStmtAttr` numeric and
  pointer-valued outputs. A strict UBSan unit test first reproduced a
  misaligned `SQLULEN` store, then covered narrow and wide numeric attributes,
  descriptor and binding pointers, and error-path output preservation.
- Audit batch 163 copies `SQLGetFunctions` results from aligned local storage
  into caller buffers. A PostgreSQL-backed strict UBSan test first reproduced
  a misaligned `SQLUSMALLINT` store, then checked single-function, ODBC 2 array,
  and ODBC 3 bitmap outputs plus no-write behavior for an invalid function id.
- Audit batch 164 copies `SQLNumResultCols` and `SQLRowCount` results from
  aligned locals into caller buffers. A PostgreSQL-backed strict UBSan test
  first reproduced a misaligned column-count store, then checks both outputs
  and preserves their buffer on statement-sequence errors.
- Audit batch 165 copies `SQLDescribeCol` and `SQLDescribeParam` numeric
  outputs byte-wise. A PostgreSQL-backed strict UBSan test first reproduced
  a misaligned column-name-length store, then checks every numeric output of
  both APIs and no-write behavior for invalid column and parameter numbers.
- Audit batch 166 copies `SQLColAttribute` string lengths, column counts, and
  descriptor-derived numeric results byte-wise. A PostgreSQL-backed strict
  UBSan test first reproduced a misaligned string-length store, then checks
  each output category and no-write behavior for an invalid field identifier.
- Audit batch 167 copies the `SQLNumParams` result byte-wise. A PostgreSQL-
  backed strict UBSan test first reproduced a misaligned parameter-count
  store, then checks the copied count and preserves the output for an
  unprepared statement.
- Audit batch 168 copies `SQLGetDescRec` numeric outputs and `SQLGetDescField`
  header, record, pointer, and string-length outputs byte-wise. A strict
  UBSan unit test first reproduced a misaligned descriptor name-length
  store, then checks representative fields and preserves output on invalid
  record and field identifiers.
- Audit batch 169 copies parameter-set processed counts and status values
  byte-wise at execution start and completion. A PostgreSQL-backed strict
  UBSan test first reproduced a misaligned processed-count store, then
  verifies both success and execution-error status outputs.
- Audit batch 170 copies fetched-row counts and row status values byte-wise
  across success, truncation, conversion error, and no-data paths. A
  PostgreSQL-backed strict UBSan test first reproduced a misaligned
  rows-fetched store, then checks all four outcomes with unaligned outputs.
- Audit batch 171 copies bound-column and `SQLGetData` length indicators
  byte-wise, including the shared text conversion path. A PostgreSQL-backed
  strict UBSan test first reproduced a misaligned NULL-indicator store, then
  checks NULL and non-NULL values plus no-write behavior before fetching.
- Audit batch 172 loads prepared-parameter NULL and length indicators
  byte-wise. A PostgreSQL-backed strict UBSan test first reproduced a
  misaligned input-indicator load, then checks text, binary, wide-text, and
  NULL parameter round-trips with unaligned indicators.
- Audit batch 173 copies numeric outputs from the `SQLGetDescRecW` wrapper
  and `SQLNativeSql` A/W length paths byte-wise. Strict UBSan tests first
  reproduced misaligned wide-descriptor and narrow native-SQL stores, then
  checked success, zero-length truncation, and error-path preservation.
- Audit batch 174 copies `SQLAllocHandle` output handles byte-wise, both when
  clearing the result and after registration. A strict UBSan unit test first
  reproduced a misaligned handle store, then checked successful allocation
  and the cleared result after an invalid handle type.
- Audit batch 175 parses plain signed integers and fixed-point decimal integer
  parts exactly before conversion to C integral types, and bounds the remaining
  floating-point fallback by an exclusive power-of-two limit. Strict UBSan
  first reproduced an out-of-range `SQL_C_SBIGINT` cast at the maximum value;
  unit and PostgreSQL tests now cover both 64-bit limits, fractional
  truncation, overflow diagnostics, and unchanged error outputs.
- Audit batch 176 parses ordinary decimal and scientific notation as decimal
  digits before converting to C signed integers, eliminating floating-point
  rounding at the 64-bit boundary. A failing unit test first reproduced the
  positive maximum being rejected; unit and PostgreSQL-backed tests now cover
  both signed limits, fractional truncation, overflow and invalid-exponent
  diagnostics, extreme exponent magnitudes, and untouched outputs on error.
- Audit batch 177 retains accepted leading whitespace while sending the
  remaining decimal/scientific text through the exact integer parser. A
  failing strict UBSan unit test reproduced rejection of a whitespace-prefixed
  64-bit maximum. Unit and PostgreSQL-backed tests cover both signed limits,
  fractional warning, overflow diagnostics, and unchanged outputs on error.
- Audit batch 178 follows the ODBC character-to-C conversion rule that outer
  spaces are ignored for numeric and date/time targets. Exact integer parsing,
  floating-point parsing, and date, time, and timestamp parsing now accept
  surrounding whitespace. Unit and PostgreSQL-backed tests cover valid values,
  malformed suffixes, empty input, diagnostics, and untouched error outputs.
- Audit batch 179 reports `01S07` when converting a value with nonzero
  fractional seconds to `SQL_C_TIME`, or when a `SQL_C_TIMESTAMP` value loses
  nonzero digits beyond its nine-digit fractional field. Unit tests first
  reproduced the missing time warning; PostgreSQL-backed tests cover time,
  timestamp-to-time, and long text-to-timestamp conversions.
- Audit batch 180 accepts valid timestamps for `SQL_C_DATE`, preserving the
  date and reporting `01S07` only when nonzero time fields are discarded.
  Unit and PostgreSQL-backed tests cover zero and nonzero time, subnanosecond
  fractions, malformed date/time text, and untouched outputs on error.
- Audit batch 181 accepts dates for `SQL_C_TIMESTAMP` and initializes all
  time fields to zero, as required by the ODBC date conversion table. Unit and
  PostgreSQL-backed tests verify leap dates, outer spaces, invalid dates,
  diagnostic classification, and untouched outputs on error.
- Audit batch 182 reuses exact decimal-to-integer conversion for numeric text
  requested as `SQL_C_BIT`. Values strictly between zero and two truncate to
  a bit with `01S07`; negative or at-least-two values return `22003`, and
  malformed text returns `22018`. Unit and PostgreSQL-backed tests cover
  values near the upper boundary and preserve native PostgreSQL `t`/`f` input.
- Audit batch 183 accepts time-only values for `SQL_C_TIMESTAMP`, using the
  current local date from platform thread-safe time conversion. Unit and
  PostgreSQL-backed tests allow a midnight rollover while checking the date,
  time fields, malformed input, `22007`, and untouched outputs on error.
- Audit batch 184 rounds positive submillisecond deadline remainders up to the
  next millisecond across transport backends. A deterministic clock test first
  reproduced a still-future deadline being reported as expired, then checked
  exact and just-past millisecond boundaries without relying on scheduling.
  The full matrix exposed that rounding up can overflow the clock when deriving
  a per-address deadline from `Deadline::max()`; synchronous, epoll, and IOCP
  connection attempts now use the saturating deadline helper.
- Audit batch 185 uses the same exact ceil-to-millisecond calculation for epoll
  and IOCP reactor waits. Their former add-one-millisecond expression both
  overshot exact boundaries and risked clock-duration overflow near its limit.
- Audit batch 186 rechecks the absolute deadline after a capped poll or Winsock
  event wait times out. A shortened-wait loopback test first reproduced an
  early timeout while the server still had time to send a reply, then verified
  that the socket becomes ready within the original deadline.
- Audit batch 187 enables `SO_NOSIGPIPE` on macOS for both newly connected and
  adopted sockets. Two platform regressions first observed the option disabled,
  then verified it is enabled; this complements `MSG_NOSIGNAL` on Linux and
  prevents a closed peer from terminating the driver process during a send.
- Audit batch 188 exercises an actual send to a closed macOS socket peer in a
  child process with default SIGPIPE handling. The child must return a network
  error normally rather than terminate from the signal; process isolation
  keeps a regression from killing the entire transport test executable.
- Audit batch 189 distinguishes an abrupt peer close during the TLS handshake
  from a deadline timeout in both Strict and SocketTimeout modes. The new
  loopback test also verifies the failed upgrade leaves no plaintext send path;
  ten repeated runs passed before the full local matrix.
- Audit batch 190 retains OpenSSL's handshake failure context in the TLS
  diagnostic instead of returning only a generic message. The abrupt-close
  regression first reproduced the missing `SSL_connect` context in both
  deadline models, then verified the connection remains unusable after failure.
- Audit batch 191 reuses that formatter for TLS send and receive failures,
  preserving OpenSSL operation and error detail while retaining the existing
  timeout, TLS, and network error categories. The abrupt-close receive test
  first reproduced the missing `SSL_read` context, then verified it is present.
- Audit batch 192 classifies an OpenSSL `SSL_ERROR_SYSCALL` as a socket timeout
  only when the OpenSSL call returned a negative result. A zero result can mean
  an unannounced peer close on older OpenSSL and must not inherit a stale
  timeout code; deterministic cases cover zero, negative timeout, and network
  failure for handshake, read, and write's shared decision.
- Audit batch 193 captures `SSL_get_error` immediately after async TLS
  handshake, read, and write calls, before memory-BIO flushing can change the
  thread-local OpenSSL error queue. The loopback regression injects an error
  during transport send and first reproduced a false handshake failure, then
  completed a TLS echo round trip. The worker also clears unrelated queued
  errors before each TLS call, per [OpenSSL's contract](https://docs.openssl.org/3.0/man3/SSL_get_error/).
- Audit batch 194 releases async TLS queue capacity immediately when a queued
  request is canceled, completing its callback outside the queue lock. A
  deterministic depth-one test holds the worker in a callback, cancels the
  queued request, and first reproduced a false queue-full rejection for its
  replacement. The canceled callback now completes before the worker resumes.
- Audit batch 195 reclaims expired async TLS requests when a full queue or a
  pending connect would otherwise reject new work. Expiration callbacks run
  outside the queue lock and preserve a concurrent cancellation outcome. A
  depth-one test first reproduced delayed timeout and false queue-full errors
  while the worker was held in a callback, then verified prompt timeout and
  replacement admission.
- Audit batch 196 clears the thread-local OpenSSL error queue before each
  synchronous TLS handshake, read, and write call, as required by the
  [SSL_get_error contract](https://docs.openssl.org/3.0/man3/SSL_get_error/).
  Loopback tests inject a stale error before handshake, send, and receive, then
  check connection success, timeout classification, and an empty error queue. The
  installed OpenSSL already passed those cases before the guard; this hardens
  behavior across supported OpenSSL versions rather than claiming a local bug.
  The macOS loopback server suppresses SIGPIPE if a client times out during its
  handshake, keeping that expected test race from terminating the suite.
- Audit batch 197 completes already-expired thread-pool requests at admission,
  outside the queue lock. Before the fix, a busy worker delayed their timeout,
  and a full queue mislabeled them as network errors. Red/green tests cover both
  cases, plus expired connect/send/receive parity and an untouched receive
  buffer; the focused tests passed 50 repeated runs after the fix.
- Audit batch 198 applies the same admission-time deadline rule to the async
  TLS wrapper. Already-expired requests complete outside its queue lock, ahead
  of queue-full or pending-connect rejection. Red/green tests cover a blocked
  worker, full queue, connect/send/receive error parity, and an untouched
  receive buffer; the focused tests passed 50 repeated runs after the fix.
- Audit batch 199 saturates negative relative deadlines to an already-expired
  time point before converting milliseconds to the clock's finer unit. This
  avoids overflow for extreme negative durations. A red/green test covers
  negative one millisecond and the minimum milliseconds value, then passed 20
  repeated runs in both normal and sanitizer builds.
- Audit batch 200 treats backslash as an ordinary character inside PostgreSQL
  double-quoted identifiers, consistent with the
  [lexical syntax](https://www.postgresql.org/docs/current/sql-syntax-lexical.html).
  The parameter-marker scanner previously mistook a backslash before the
  closing quote for an escape and missed a later `?` marker. A red/green parser
  test and a real prepared-statement test cover that identifier shape.
- Audit batch 201 distinguishes a dollar sign inside an unquoted PostgreSQL
  identifier from the start of a dollar-quoted string. The scanner previously
  treated `foo$tag$bar` as opening a string and hid a later `?` parameter.
  A red/green parser test also preserves genuine `$tag$...$tag$` strings; a
  prepared-statement integration test exercises the identifier against
  PostgreSQL. This follows the same
  [lexical rules](https://www.postgresql.org/docs/current/sql-syntax-lexical.html).
- Audit batch 202 ends `--` line comments at either CR or LF, matching the
  [PostgreSQL lexer](https://github.com/postgres/postgres/blob/master/src/backend/parser/scan.l).
  Previously, a CR-only line ending hid the next `?` parameter. A red/green
  parser test and a real prepared-statement test cover the boundary.
- Audit batch 203 makes the ODBC SQL escape translator honor PostgreSQL's
  nested block comments and CR-only line endings. Previously, an escape inside
  the outer comment could be rewritten after the inner `*/`. Red/green unit
  tests cover both boundaries and real PostgreSQL tests cover `SQLNativeSql`
  and execution through a nested comment.
- Audit batch 204 keeps backslash-escaped quotes inside PostgreSQL `E'...'`
  strings while scanning for ODBC escapes. Previously, an embedded escape
  could be rewritten as SQL, and a later real escape left unchanged. A
  red/green unit test and real PostgreSQL translation/execution test cover
  both sides of the quote boundary.
- Audit batch 205 enforces digit-only hours, minutes, and seconds in ODBC
  time/timestamp escapes, as required by the
  [ODBC datetime grammar](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/date-time-and-timestamp-escape-sequences).
  Previously, two-character negative fields such as `-1` and `-0` passed
  range validation. Red/green unit tests cover all three fields; connected
  tests verify SQLSTATE 22007, output preservation, and direct/prepared
  recovery.
- No local `clang-tidy` or `cppcheck` executable was available for this pass.
  Compiler warnings, sanitizers, focused source inspection, and behavioral
  tests were used instead; CI should add a pinned static-analysis tool later.

## Priority findings

### P0 — correctness and safety

1. **C entry-point exception containment:** audit batch 4 routes all 76 exported
   ODBC symbols through one exception barrier. Unexpected failures return
   `SQL_ERROR`; non-diagnostic calls attach `HY000` when their handle remains
   usable. Audit batch 8 injects `std::bad_alloc` through the real
   `SQLAllocHandle` path and verifies `SQL_ERROR`, a null output, `HY001`, and
   successful recovery on the next allocation.
2. **Handle concurrency:** audit batch 2 changes registry lookup to pin shared
   ownership for each exported call. Audit batch 7 adds connection-domain
   operation leases in stable handle order, covers same-handle and sibling
   serialization, proves independent connections remain concurrent, and
   exercises opposite-direction descriptor copies for deadlock regressions.
3. **Parent/child lifetime:** audit batch 3 records the handle graph, pins a
   statement's connection, rejects out-of-order parent frees, and recursively
   invalidates subordinate handles on statement free or successful disconnect.
   Audit batch 7 serializes child calls with connection disconnect/free and
   allocation transitions through their shared connection lease.
4. **Diagnostic lifecycle:** audit batch 2 clears prior records at the start of
   non-diagnostic handle calls. Audit batch 5 makes diagnostic retrieval itself
   non-mutating and validates handle type, record number, and buffer length.
   Audit batch 6 records return-code provenance at the shared ABI boundary;
   audit batch 9 covers the standard header/record field identifiers and
   statement execution provenance. Rowset- and parameter-array-specific
   row/column diagnostics remain with those unsupported features.
5. **Input validation parity:** audit batch 1 covers execution/preparation
   boundaries. Audit batch 10 centralizes catalog string validation and wide
   output validation, then directly compares ANSI/wide behavior for connection
   strings, catalog arguments, column metadata, and `SQLGetInfo`. Remaining
   pairs stay explicitly listed as partial in the inventory until their full
   value and state matrices are covered.

### P1 — conformance and interoperability

1. Complete the connection/statement/descriptor attribute matrices.
2. Keep the verified `SQLGetInfo` matrix synchronized with newly implemented
   driver features and platform headers; do not turn conservative zero values
   into capability claims without executable PostgreSQL evidence.
3. Exercise every exported wide symbol directly on two-byte and four-byte
   `SQLWCHAR` Driver Manager paths.
4. Add state-machine tests for allocated, connected, prepared, executed,
   fetched, exhausted, closed, disconnected, and freed states.
5. Complete truncation, NULL, overflow, malformed UTF, numeric conversion,
   date/time, binary, and very-large-value matrices.
6. Add failure injection for DNS, connect, authentication, TLS, mid-query
   disconnect, timeout, malformed protocol frames, and allocation failures.

### P2 — observability and maintainability

1. Extend the common failure logger to diagnostic-retrieval calls where an
   owning connection is still valid, without mutating diagnostic state.
2. Benchmark disabled and asynchronous logging before claiming the roadmap's
   `<1%` overhead target.
3. Split the two 2,000-plus-line ODBC implementation files by responsibility.
4. Remove obsolete descriptor stubs, pass-through helpers, speculative
   comments, duplicated ANSI/wide validation, and tests that lock in temporary
   stubs instead of contractual behavior.

## Test policy for closing an audit row

Each supported behavior needs:

1. A positive unit test.
2. Null, invalid-length/value, truncation, and invalid-state tests where
   applicable.
3. Exact return code and SQLSTATE assertions.
4. ANSI/wide parity tests for string APIs.
5. A real PostgreSQL test when server behavior is involved.
6. A Driver Manager test when ABI, registration, or manager translation is
   involved.
7. Sanitizer coverage and all-platform CI.

Unsupported behavior must return a documented diagnostic, remain absent from
capability advertisements, and have a negative test proving both facts.

## AI-code quality gate

Authorship cannot be reliably inferred from source code. The review therefore
uses observable maintainability criteria:

- delete abstractions that only forward or rename values;
- reject comments that merely restate code or claim unproven completeness;
- consolidate duplicated validation only when the shared contract is truly
  identical;
- keep functions focused and expose state explicitly rather than through
  scattered booleans;
- require tests for failure behavior, not test-count claims;
- measure complexity, binary size, compile time, and logging overhead before
  accepting generalized infrastructure;
- prefer small, specification-linked commits that are easy for a human
  reviewer to validate.

The audit is complete only when the matrix is accurate, not when every optional
ODBC feature is implemented. Unsupported features may remain unsupported as
long as that behavior is standards-compatible, tested, logged appropriately,
and advertised honestly.
