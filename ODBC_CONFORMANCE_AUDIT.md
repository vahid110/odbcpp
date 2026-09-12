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
| `SQLAllocHandle` | A | Partial | unit failure injection, integration, DM | DBC requires an ODBC version, STMT/DESC require an open connection, and allocation failure returns HY001; complete type/state matrix remains |
| `SQLFreeHandle` | A | Partial | unit, integration, DM | Parent/child free ordering is enforced; complete state matrix remains |
| `SQLConnect` | A/W | Partial | unit failure, integration, DM | Reconnect is rejected with 08002; complete input/state matrix remains |
| `SQLDriverConnect` | A/W | Partial | unit, integration, DM | Noninteractive completion modes, exact output/truncation rules, connected-state output preservation, and ANSI/wide unknown-keyword warnings are covered; interactive prompting remains unsupported |
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
| `SQLColAttribute` | A/W | Partial | unit, integration | Prepared-state discovery, count/name/label/core numeric fields, destination isolation, exact field diagnostics, and wide byte lengths are covered; remaining descriptor fields need values or explicit negative tests |
| `SQLDescribeParam` | A | Partial | unit, integration | Prepared/executed/exhausted/closed/direct states, exact diagnostics, fixed-size PostgreSQL types, bound precision/scale, cache revision, and untouched errors are covered; unknown variable precision and failure injection remain |
| `SQLSetStmtAttr` | A/W | Partial | unit, integration | Common scalar modes, descriptor attachment, offset/operation pointers, and precise value diagnostics work; multirow arrays and optional cursor modes remain |
| `SQLGetStmtAttr` | A/W | Partial | unit, integration | Common defaults, descriptor handles/pointers, row positioning, and recognized unsupported attributes are covered; platform-specific ODBC 3.8 fields remain |
| `SQLCloseCursor` | A | Partial | unit, integration, DM | Missing/open cursor and pending-result discard are covered; complete statement-state matrix remains |
| `SQLFreeStmt` | A | Partial | unit, integration, DM | SQL_CLOSE pending-result discard and basic options are covered; complete option/state matrix remains |
| `SQLGetTypeInfo` | A/W | Partial | integration, DM | All supported types, type filters, wide entry test |
| `SQLTables` | A/W | Partial | unit, integration, DM | Pattern/metadata-ID semantics and privilege visibility |
| `SQLColumns` | A/W | Partial | unit, integration, DM | Pattern semantics and complete PostgreSQL type metadata |
| `SQLPrimaryKeys` | A/W | Partial | unit, integration, DM | Empty/multipart identifiers, ordering, visibility |
| `SQLForeignKeys` | A/W | Partial | unit, integration, DM | PK/FK filter combinations and rule mapping |
| `SQLStatistics` | A/W | Partial | unit, integration, DM | Accuracy/cardinality modes and expression/partial indexes |
| `SQLProcedures` | A/W | Partial | unit, integration, DM | PostgreSQL procedure/function distinctions and overloads |
| `SQLProcedureColumns` | A/W | Partial | unit, integration, DM | Modes, result columns, overloads and type metadata |
| `SQLSpecialColumns` | A/W | Partial | unit, integration, DM | Scope/nullable semantics and row-version behavior |
| `SQLGetDiagRec` | A/W | Partial | unit, integration, DM | Retrieval preserves records and validates type/record/buffer; truncation matrix remains |
| `SQLGetDiagField` | A/W | Partial | unit, integration | All standard header/record identifiers, return provenance, origins, ANSI/wide lengths, and truncation are covered; row/column-specific server errors remain |
| `SQLError` | A/W | Partial | unit, integration | ODBC 2 sequencing and multi-record consumption |
| `SQLGetInfo` | A/W | Partial | unit, integration, DM | Conservative capability matrix is guarded; complete remaining required information types |
| `SQLGetFunctions` | A | Verified | shared-library export audit, unit, DM | Exact single-function, ODBC 2 array, and ODBC 3 bitmap behavior is enforced against all advertised exports |
| `SQLNativeSql` | A/W | Partial | unit, DM | ODBC escape translation; currently effectively pass-through |
| `SQLSetEnvAttr` | A | Partial | unit, integration, DM | ODBC version and null-terminated output are classified and locked after DBC allocation; Driver Manager pooling attributes remain |
| `SQLGetEnvAttr` | A | Partial | unit, DM | ODBC version, null-terminated output, iODBC negotiation, and output validation are covered; Driver Manager pooling attributes remain |
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
- The callback/future methods in `AsyncDatabaseConnection` are experimental
  scaffolding, are not used by the ODBC driver's production connection path,
  and currently simulate query results on a detached thread. Four real async
  integration cases remain disabled. They must not be counted as end-to-end
  async database coverage.
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
6. **Experimental async database facade:** do not expose or advertise the
   simulated callback/future query path as implemented. Either connect it to
   real protocol I/O with cancellation/deadline ownership or remove it after
   the transport layer no longer needs its test scaffolding.

### P1 — conformance and interoperability

1. Complete the connection/statement/descriptor attribute matrices.
2. Complete the required `SQLGetInfo` information matrix. Audit batch 15
   guards the current capability subset against over-advertising, and
   `SQLGetFunctions` remains guarded by the shared-library export test.
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
