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

The shared library currently exports 73 ODBC symbols: 47 base operations and
26 wide-character variants. The older roadmap count of 66 was stale.

| Operation | Variants | Status | Existing evidence | Principal remaining work |
|---|---:|---|---|---|
| `SQLAllocHandle` | A | Partial | unit failure injection, integration, DM | DBC requires an ODBC version, STMT/DESC require an open connection, and allocation failure returns HY001; complete type/state matrix remains |
| `SQLFreeHandle` | A | Partial | unit, integration, DM | Parent/child free ordering is enforced; complete state matrix remains |
| `SQLConnect` | A/W | Partial | unit failure, integration, DM | Reconnect is rejected with 08002; complete input/state matrix remains |
| `SQLDriverConnect` | A/W | Partial | unit, DM | Completion modes, exact output-string rules, connected-state handling |
| `SQLDisconnect` | A | Partial | unit, integration, DM | Disconnected, active-transaction, and child-invalidation paths covered; async execution remains |
| `SQLSetConnectAttr` | A/W | Partial | unit, integration | Common defaults, catalog selection, invalid values, and unsupported modes covered; connection timeout and packet size remain |
| `SQLGetConnectAttr` | A/W | Partial | unit, integration | Common numeric values, current catalog buffers, read-only attributes, and connected-state checks covered; broken-link detection remains |
| `SQLEndTran` | A | Partial | unit, integration | Environment-wide completion and multi-connection behavior |
| `SQLExecDirect` | A/W | Partial | unit failure, integration, DM | Open cursors are protected and direct execution replaces prepared SQL; cancellation remains |
| `SQLPrepare` | A/W | Partial | integration, DM | Open cursors are protected and old result state is retired; preparation errors and pre-execution metadata remain |
| `SQLExecute` | A | Partial | integration | Unprepared execution returns HY010 and open-cursor re-execution returns 24000; parameter arrays and data-at-execution remain |
| `SQLFetch` | A | Partial | unit, integration, DM | Never-executed and no-result states return HY010/24000; row arrays and full state matrix remain |
| `SQLFetchScroll` | A | Partial | unit, integration, DM | Only `SQL_FETCH_NEXT` is supported; keep other orientations honest |
| `SQLMoreResults` | A | Partial | unit, integration, DM | Result/update-count traversal and close-time discard are covered; error-result sequences remain |
| `SQLGetData` | A | Partial | integration, DM | Target types and per-row/switching-column offsets are covered; complete conversion/chunking matrices remain |
| `SQLBindCol` | A | Partial | unit, integration | Invalid C types and negative lengths are covered; row arrays, row-wise binding, and full type/conversion matrix remain |
| `SQLBindParameter` | A | Partial | unit, integration | Direction/C/SQL type and length diagnostics covered; input arrays, data-at-execution, and full conversion matrix remain |
| `SQLNumParams` | A | Partial | integration, DM | Prepared marker parsing and direct-execution zero count are covered; pre-execution server validation remains |
| `SQLNumResultCols` | A | Partial | unit, integration | State transitions and no-result/update-count cases |
| `SQLRowCount` | A | Partial | unit, integration | Statement-state matrix and all statement classes |
| `SQLDescribeCol` | A/W | Partial | unit, integration | ANSI/wide name truncation is diagnosed; bookmark column and remaining wide edge cases remain |
| `SQLColAttribute` | A/W | Partial | integration | ANSI/wide name truncation is diagnosed; complete field and destination rules remain |
| `SQLDescribeParam` | A | Partial | unit internals, integration | Availability after prepare and complete type metadata |
| `SQLSetStmtAttr` | A/W | Partial | unit, integration | Scalar modes, descriptor attachment, and descriptor-driven execution work; arrays, offsets, and operations remain |
| `SQLGetStmtAttr` | A/W | Partial | unit, integration | Common defaults, descriptor handles, and status pointers covered; row number and remaining attributes need classification |
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
| `SQLSetEnvAttr` | A | Partial | unit, integration, DM | ODBC version is locked after DBC allocation; supported attribute matrix remains |
| `SQLGetEnvAttr` | A | Partial | unit, DM | Supported attribute matrix and buffer/type rules |
| `SQLGetDescField` | A/W | Partial | unit, integration | IRD result metadata and common header/record fields work; complete field matrix remains |
| `SQLSetDescField` | A/W | Partial | unit | Consistency checks, descriptor-kind restrictions, pointer fields |
| `SQLCopyDesc` | A | Partial | unit concurrency | Opposite-direction copies are serialized without deadlock and IRD targets return HY016; remaining consistency and state matrix remain |

No operation is promoted to **Verified** until its audit row has explicit
negative and state-transition evidence. This deliberately resets optimistic
historical “complete” labels without discarding working behavior.

## Attribute coverage

### Environment

Implemented: `SQL_ATTR_ODBC_VERSION` and iODBC driver Unicode negotiation.
Connection pooling remains a Driver Manager responsibility, but the audit must
verify which environment attributes are expected at the driver boundary and
which should be rejected.

### Connection

Implemented: `SQL_ATTR_LOGIN_TIMEOUT`, `SQL_ATTR_AUTOCOMMIT`,
`SQL_ATTR_TXN_ISOLATION`, pre-connect `SQL_ATTR_CURRENT_CATALOG`, read/write
access-mode default, ODBC async-off default, metadata-ID false default,
automatic-IPD false reporting, open-connection `SQL_ATTR_CONNECTION_DEAD`
reporting, and iODBC application `SQLWCHAR` negotiation. Unsupported read-only,
async-on, and metadata-ID true modes are rejected rather than silently ignored.

Not yet implemented or fully classified: connection timeout, packet size,
actual broken-link detection for connection-dead status, genuine asynchronous
ODBC function execution, metadata identifier semantics, and platform/Driver
Manager-owned tracing or cursor-library attributes.

### Statement

Stored: query timeout and maximum rows. Row/parameter array sizes, bind types,
status pointers, and processed-count pointers share their descriptor header
state rather than maintaining duplicate statement-only values.
Default-only behavior is reported for forward-only cursor type, read-only
concurrency, single-row and single-parameter arrays, column-wise binding,
retrieve-data on, bookmarks off, metadata-ID false, and ODBC async off. Fetch
and prepared execution update status outputs on success, truncation, no-row,
local validation errors, and PostgreSQL errors.

Explicit APD/ARD handles can be attached, shared within their connection,
reset to the statement's automatic descriptors, and are automatically detached
when freed. Foreign, invalid, implementation, and another statement's implicit
descriptors are rejected with precise diagnostics.

Fetch and prepared execution consume those descriptor headers and reject
unsupported multirow, bind-offset, row-wise, and operation-array settings.
`SQLBindCol` and `SQLBindParameter` populate the associated ARD/APD/IPD records.
The same records are the source of truth for fetch and prepared execution, so
applications can bind through the descriptor APIs and parameter bindings
survive `SQLPrepare`. Arrays larger than one, maximum length, metadata-ID true,
no-scan behavior, row number, and genuine asynchronous ODBC function completion
remain.

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

- `odbc_api.cpp` is 2,577 lines and contains all 73 exported wrappers;
  `odbc_handles.cpp` is 2,877 lines and combines connection, statement,
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
- Audit batch 12 loads the built shared driver and verifies every one of the 47
  advertised base symbols and all 26 wide exports. It exhaustively compares
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
- No local `clang-tidy` or `cppcheck` executable was available for this pass.
  Compiler warnings, sanitizers, focused source inspection, and behavioral
  tests were used instead; CI should add a pinned static-analysis tool later.

## Priority findings

### P0 — correctness and safety

1. **C entry-point exception containment:** audit batch 4 routes all 73 exported
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
