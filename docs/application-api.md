# Application API and diagnostics

Applications connect through ODBC using their platform driver manager. Include
its `sql.h` and `sqlext.h`, link its ODBC library, and load the ODBCPP driver built
for the selected database, architecture and Unicode ABI. PostgreSQL and Redshift
are separate product configurations; a function being exported does not mean
all its options or optional ODBC features are implemented.

## Function reference and supported subset

The [ODBC function reference](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/odbc-function-summary)
links each function's argument types, input/output meanings, return values,
SQLSTATEs and sequencing rules. Use it together with ODBCPP's
[API coverage and known limitations](../ODBC_CONFORMANCE_AUDIT.md), rather than
assuming the entire ODBC standard is supported. Query `SQLGetFunctions` for
function availability and `SQLGetInfo` for the connected driver's capabilities.
The audit records partial implementations and remaining test gaps; it is not a
conformance certificate.

| Operation | ODBC calls | ODBCPP-specific guidance |
| --- | --- | --- |
| Allocate and connect | `SQLAllocHandle`, `SQLSetEnvAttr`, `SQLConnect`, `SQLDriverConnect` | Set ODBC version before allocating the connection. `SQLConnect` receives a DSN name; `SQLDriverConnect` receives a connection string. |
| Execute and bind | `SQLPrepare`, `SQLBindParameter`, `SQLExecute`, `SQLExecDirect` | Bind data values instead of interpolating them into SQL. Check the audit for parameter-array and data-at-execution limitations. |
| Fetch and convert | `SQLFetch`, `SQLFetchScroll`, `SQLBindCol`, `SQLGetData`, `SQLMoreResults` | Respect buffer capacities, returned lengths and `SQL_NULL_DATA`. Forward-only cursor support does not imply arbitrary scrolling. |
| Inspect metadata | `SQLNumResultCols`, `SQLDescribeCol`, `SQLColAttribute`, catalog functions | Backend capabilities and metadata semantics differ; see the audit and Redshift type guide. |
| Control transactions | `SQLSetConnectAttr`, `SQLEndTran` | Check actual supported isolation/autocommit settings; a timeout can retire the connection. |
| Release resources | `SQLCloseCursor`, `SQLFreeStmt`, `SQLDisconnect`, `SQLFreeHandle` | Close/free only handles successfully allocated; preserve errors before subsequent calls replace diagnostics. |

Each function uses the lengths/units specified by its reference. Do not assume
all wide-call lengths are bytes or all are characters. Never pass a UTF-16
buffer to a UCS4 interface by casting it. Use matching-width application and
driver headers or the documented driver-manager conversion path; bound data has
additional constraints in the [Unicode ABI guide](../README.md#driver-manager-unicode-abi).
Redshift `SUPER`, spatial and binary representations are documented in
[the special-type guide](redshift-special-types.md).

## Return values and error handling

Check every ODBC return code. `SQL_SUCCESS_WITH_INFO` can contain warnings such
as truncation; it is not a reason to ignore diagnostics. `SQL_NO_DATA` normally
terminates a fetch or diagnostic-record iteration. On `SQL_ERROR`, collect
`SQLGetDiagRec` records from the relevant handle, beginning at record 1, before
another operation replaces them. Do not use output values from a failed call
unless that call's contract explicitly defines them. An invalid handle is not a
usable diagnostic handle. See Microsoft's
[diagnostic usage reference](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/using-sqlgetdiagrec-and-sqlgetdiagfield).

Keep SQLSTATE and native code when diagnosing a problem. Treat diagnostic text,
SQL and returned data as potentially private; redact them before public reports.
A login/query timeout or protocol failure may make the session unusable. Do not
blindly replay a transaction merely because reconnecting succeeds.

## Connection configuration and security

Connection keys and precedence are documented in
[ODBC configuration](../README.md#odbc-configuration), including host, port,
database, transport, deadlines and resource limits. Normal precedence is
connection string, DSN, driver registration, then defaults. Supply database
credentials through the application's protected login path; avoid placing them
in command-line arguments, source, issue reports or logs.

TLS is enabled when `SSL` is omitted. Use the endpoint hostname and OS trust,
or configure `SSLCAFILE`/`SSLCADIR` according to the
[trust-source rules](trust-store-design.md#3-options-defaults-and-precedence).
Do not disable certificate/hostname verification to make a connection succeed.
Use [driver logging and trace settings](ODBC_TRACE.md) for diagnosis; sensitive
trace options require care because diagnostic text can contain SQL/data.

## C++ SDK and internal APIs

C++ extension contracts live under `sdk/include/odbcpp`; their owning types,
inputs, lifecycle and dependency rules are described in
[SDK_ARCHITECTURE.md](../SDK_ARCHITECTURE.md), with implementation status in
[SDK_PRODUCT_PLAN.md](../SDK_PRODUCT_PLAN.md). These development contracts do
not promise a stable released SDK ABI. Historical `core/` include paths and
backend factories are not the recommended application interface.

`BackendResult<T>` carries an owning backend success or failure. Check success
before accessing its value; on failure inspect `backend_error()` and its session
state/disposition. `BackendError::safe_summary()` is the fixed public summary;
its detailed message may contain private context. A `Reusable` disposition
means protocol readiness for the current owner, not a reset for another pooled
borrower. See the [pooling boundary](../SDK_ARCHITECTURE.md#reuse-pooling-and-caches).
