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
| `SQLAllocHandle` | A | Partial | unit, integration, DM | DBC requires an ODBC version and STMT/DESC require an open connection; allocation-failure injection remains |
| `SQLFreeHandle` | A | Partial | unit, integration, DM | Parent/child free ordering is enforced; complete state matrix remains |
| `SQLConnect` | A/W | Partial | unit failure, integration, DM | Reconnect is rejected with 08002; complete input/state matrix remains |
| `SQLDriverConnect` | A/W | Partial | unit, DM | Completion modes, exact output-string rules, connected-state handling |
| `SQLDisconnect` | A | Partial | unit, integration, DM | Disconnected, active-transaction, and child-invalidation paths covered; async execution remains |
| `SQLSetConnectAttr` | A/W | Partial | unit, integration | Attribute matrix and pre/post-connect restrictions |
| `SQLGetConnectAttr` | A/W | Partial | unit, integration | Read-only/common attributes, buffer/type rules, wide entry tests |
| `SQLEndTran` | A | Partial | unit, integration | Environment-wide completion and multi-connection behavior |
| `SQLExecDirect` | A/W | Partial | unit failure, integration, DM | Full state sequencing, empty/invalid text, cancellation |
| `SQLPrepare` | A/W | Partial | integration, DM | Full state sequencing, preparation errors, pre-execution metadata |
| `SQLExecute` | A | Partial | integration | Re-execution states, parameter arrays, data-at-execution |
| `SQLFetch` | A | Partial | integration, DM | Row arrays/status, bound-buffer boundaries, state matrix |
| `SQLFetchScroll` | A | Partial | unit, integration, DM | Only `SQL_FETCH_NEXT` is supported; keep other orientations honest |
| `SQLMoreResults` | A | Partial | unit, integration, DM | Error/result/update-count sequences and state transitions |
| `SQLGetData` | A | Partial | integration, DM | Complete conversion matrix and call-order/chunking edge cases |
| `SQLBindCol` | A | Partial | unit internals, integration | Row arrays, row-wise binding, invalid buffer/type combinations |
| `SQLBindParameter` | A | Partial | unit internals, integration | Input arrays, data-at-execution, supported C/SQL type matrix |
| `SQLNumParams` | A | Partial | integration, DM | Invalid state/output and complex marker parsing |
| `SQLNumResultCols` | A | Partial | unit, integration | State transitions and no-result/update-count cases |
| `SQLRowCount` | A | Partial | unit, integration | Statement-state matrix and all statement classes |
| `SQLDescribeCol` | A/W | Partial | unit, integration | Buffer boundaries, bookmark column, wide edge cases |
| `SQLColAttribute` | A/W | Partial | integration | Complete field identifiers and numeric/string destination rules |
| `SQLDescribeParam` | A | Partial | unit internals, integration | Availability after prepare and complete type metadata |
| `SQLSetStmtAttr` | A/W | Partial | unit, integration | Most non-default values intentionally unsupported; pointer attributes absent |
| `SQLGetStmtAttr` | A/W | Partial | unit | Pointer/status attributes, row number, wide entry tests |
| `SQLCloseCursor` | A | Partial | unit, integration, DM | Complete statement-state matrix |
| `SQLFreeStmt` | A | Partial | unit, DM | All options across statement states and descriptor side effects |
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
| `SQLGetDiagField` | A/W | Partial | unit, integration | Retrieval preserves records and reports the generating return code; complete field matrix remains |
| `SQLError` | A/W | Partial | unit, integration | ODBC 2 sequencing and multi-record consumption |
| `SQLGetInfo` | A/W | Partial | unit, DM | Complete information matrix and capability accuracy |
| `SQLGetFunctions` | A | Partial | unit, DM | Automatically prove advertised functions match usable exports |
| `SQLNativeSql` | A/W | Partial | unit, DM | ODBC escape translation; currently effectively pass-through |
| `SQLSetEnvAttr` | A | Partial | unit, integration, DM | ODBC version is locked after DBC allocation; supported attribute matrix remains |
| `SQLGetEnvAttr` | A | Partial | unit, DM | Supported attribute matrix and buffer/type rules |
| `SQLGetDescField` | A/W | Partial | unit | Descriptor-kind restrictions and complete header/record fields |
| `SQLSetDescField` | A/W | Partial | unit | Consistency checks, descriptor-kind restrictions, pointer fields |
| `SQLCopyDesc` | A | Partial | unit concurrency | Opposite-direction copies are serialized without deadlock; source/target restrictions, consistency checks, state matrix remain |

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
`SQL_ATTR_TXN_ISOLATION`, and iODBC application `SQLWCHAR` negotiation.

Not yet implemented or fully classified: access mode, connection timeout,
current catalog, metadata ID, packet size, connection-dead status, automatic
IPD reporting, asynchronous connection/statement defaults, and platform/Driver
Manager-owned tracing or cursor-library attributes.

### Statement

Stored: query timeout and maximum rows. Default-only behavior is reported for
forward-only cursor type, read-only concurrency, single-row arrays,
column-wise binding, retrieve-data on, bookmarks off, and ODBC async off.

Not yet implemented: row arrays and row status, rows-fetched pointers, row-wise
binding, parameter arrays and status, explicit APD/ARD attachment, maximum
length, metadata ID, no-scan behavior, row number, and genuine asynchronous
ODBC function completion.

### Descriptors

Four implicit descriptor handles and explicit descriptor allocation exist.
The explicit descriptor object supports a useful subset of header and record
fields, but statement attachment, descriptor-kind rules, consistency checks,
and the full field matrix remain partial.

## Logging coverage

The logging backend is suitable as a foundation: logging is off by default,
configuration precedence is tested, file rotation and asynchronous delivery
are tested, structured output is escaped, and connection passwords are covered
by a non-disclosure test. The current instrumentation is narrower than the
ODBC surface, however. It records connection lifecycle and direct/prepared
query outcomes, but does not yet consistently record API operation class,
SQLSTATE, native error, duration, or connection identity for metadata,
attribute, descriptor, transaction, fetch, and diagnostic failures. Logging is
therefore **implemented but partial**, not a substitute for ODBC diagnostics.

## Maintainability snapshot

- `odbc_api.cpp` is 2,430 lines and contains all 73 exported wrappers;
  `odbc_handles.cpp` is 2,376 lines and combines connection, statement,
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
- No local `clang-tidy` or `cppcheck` executable was available for this pass.
  Compiler warnings, sanitizers, focused source inspection, and behavioral
  tests were used instead; CI should add a pinned static-analysis tool later.

## Priority findings

### P0 — correctness and safety

1. **C entry-point exception containment:** audit batch 4 routes all 73 exported
   ODBC symbols through one exception barrier. Unexpected failures return
   `SQL_ERROR`; non-diagnostic calls attach `HY000` when their handle remains
   usable. Allocation-failure injection through real entry points remains.
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
   the complete header/record field matrix remains.
5. **Input validation parity:** ANSI and wide entry points have historically
   differed on null and invalid-length handling. Audit batch 1 begins closing
   this with execution/preparation boundary tests.
6. **Experimental async database facade:** do not expose or advertise the
   simulated callback/future query path as implemented. Either connect it to
   real protocol I/O with cancellation/deadline ownership or remove it after
   the transport layer no longer needs its test scaffolding.

### P1 — conformance and interoperability

1. Complete the connection/statement/descriptor attribute matrices.
2. Prove that `SQLGetFunctions` and `SQLGetInfo` never over-advertise behavior.
3. Exercise every exported wide symbol directly on two-byte and four-byte
   `SQLWCHAR` Driver Manager paths.
4. Add state-machine tests for allocated, connected, prepared, executed,
   fetched, exhausted, closed, disconnected, and freed states.
5. Complete truncation, NULL, overflow, malformed UTF, numeric conversion,
   date/time, binary, and very-large-value matrices.
6. Add failure injection for DNS, connect, authentication, TLS, mid-query
   disconnect, timeout, malformed protocol frames, and allocation failures.

### P2 — observability and maintainability

1. Extend logging to consistently capture operation class, SQLSTATE, native
   error, duration, and connection ID without logging secrets or bound values.
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
