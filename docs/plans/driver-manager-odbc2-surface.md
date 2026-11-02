# ODBC 2 compatibility surface: native support, Driver Manager remapping, and unsupported boundaries

Deliverable for **Unit B** of issue #118 (G3.1 and driver-side G3.2), sequenced in
`docs/plans/driver-manager-coverage-gap-mapping.md`.

This document defines the supported ODBC 2 compatibility surface for ODBCPP. It
establishes the exact architectural boundaries between what the driver natively
exports and handles, what is delegated to Driver Manager remapping, and what
remains explicitly unsupported.

## 1. Architectural context

ODBCPP is architected as an ODBC 3.8 / Core conformance driver. Per `ARCHITECTURE.md`,
provider-neutral contracts and lifecycle management live in the SDK, while ODBC handle
management, attribute routing, and diagnostic records live in the adapter layer (`odbc/`).

In accordance with the ODBC 3.x specification (Microsoft ODBC Programmer's Reference,
Appendix G: "Driver Guidelines for Backward Compatibility"):
- ODBC 3.x drivers are **not** required to export deprecated ODBC 2.x entry points
  such as `SQLAllocEnv`, `SQLAllocConnect`, `SQLAllocStmt`, `SQLFreeEnv`, `SQLFreeConnect`,
  `SQLSetConnectOption`, `SQLGetConnectOption`, `SQLSetStmtOption`, `SQLGetStmtOption`,
  or `SQLTransact`.
- Driver Managers (Windows `odbc32`, `unixODBC`, and `iODBC`) provide backward
  compatibility by exposing the legacy 2.x entry points to ODBC 2.x applications and
  mapping them to modern 3.x driver entry points.
- The driver directly supports `SQL_OV_ODBC2` as a valid environment version and
  retains native implementations of legacy diagnostic functions `SQLError` and `SQLErrorW`.

## 2. Native driver export inventory

### 2.1 Exported functions

The driver shared library (`libodbcpp.dylib`, `libodbcpp.so`, or `odbcpp.dll`) exports:
1. **Core & Modern 3.x entry points**:
   - `SQLAllocHandle`, `SQLFreeHandle`
   - `SQLSetConnectAttr`, `SQLGetConnectAttr` (and wide variants `SQLSetConnectAttrW`, `SQLGetConnectAttrW`)
   - `SQLSetStmtAttr`, `SQLGetStmtAttr` (and wide variants `SQLSetStmtAttrW`, `SQLGetStmtAttrW`)
   - `SQLSetEnvAttr`, `SQLGetEnvAttr`
   - `SQLEndTran`
   - `SQLGetDiagRec`, `SQLGetDiagField` (and wide variants `SQLGetDiagRecW`, `SQLGetDiagFieldW`)
   - Standard execution, metadata, descriptor, and cursor APIs (`SQLExecDirect`, `SQLPrepare`, `SQLExecute`, `SQLFetch`, `SQLFetchScroll`, `SQLGetData`, `SQLBindCol`, `SQLBindParameter`, `SQLColAttribute`, `SQLDescribeCol`, `SQLRowCount`, `SQLGetInfo`, `SQLGetFunctions`, etc.).
2. **Retained legacy diagnostic functions**:
   - `SQLError`, `SQLErrorW`: Exported directly for legacy caller compatibility.
3. **Retained legacy statement function**:
   - `SQLFreeStmt`: Exported directly with options `SQL_CLOSE`, `SQL_UNBIND`, and `SQL_RESET_PARAMS`.

### 2.2 Intentionally unexported legacy entry points

The following deprecated ODBC 2.x functions are **intentionally not exported** by the driver library:
- `SQLAllocEnv`
- `SQLAllocConnect`
- `SQLAllocStmt`
- `SQLFreeEnv`
- `SQLFreeConnect`
- `SQLSetConnectOption`
- `SQLGetConnectOption`
- `SQLSetStmtOption`
- `SQLGetStmtOption`
- `SQLTransact`
- `SQLParamOptions`

By not exporting these symbols, the driver prevents dual-path confusion and relies on the Driver
Manager to validate arguments and map calls into modern handle and attribute APIs.

This omission is verified by unit test `DriverCapabilitiesTest.DoesNotExportDeprecatedOdbc2EntryPoints`
using direct dynamic library inspection (`dlsym` / `GetProcAddress`).

## 3. Driver Manager remapping boundary

When an ODBC 2.x application links against or calls a Driver Manager, the Driver Manager
remaps legacy calls to the driver's modern entry points as follows:

| Legacy ODBC 2.x entry point | Mapped modern driver call | Parameter conversion details |
|---|---|---|
| `SQLAllocEnv(&henv)` | `SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv)` | Driver Manager allocates environment and sets internal version state |
| `SQLAllocConnect(henv, &hdbc)` | `SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc)` | Directly allocates connection handle |
| `SQLAllocStmt(hdbc, &hstmt)` | `SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt)` | Directly allocates statement handle |
| `SQLFreeEnv(henv)` | `SQLFreeHandle(SQL_HANDLE_ENV, henv)` | Frees environment handle |
| `SQLFreeConnect(hdbc)` | `SQLFreeHandle(SQL_HANDLE_DBC, hdbc)` | Frees connection handle |
| `SQLFreeStmt(hstmt, SQL_DROP)` | `SQLFreeHandle(SQL_HANDLE_STMT, hstmt)` | Driver Manager maps `SQL_DROP` to `SQLFreeHandle`; other options pass to driver `SQLFreeStmt` |
| `SQLTransact(henv, hdbc, fType)` | `SQLEndTran(HandleType, Handle, fType)` | `HandleType` is `SQL_HANDLE_DBC` if `hdbc != SQL_NULL_HDBC`, else `SQL_HANDLE_ENV` |
| `SQLSetConnectOption(hdbc, fOption, vParam)` | `SQLSetConnectAttr(hdbc, fOption, vParam, StringLength)` | Option IDs map to connection attribute IDs (e.g. `SQL_LOGIN_TIMEOUT`, `SQL_AUTOCOMMIT`) |
| `SQLGetConnectOption(hdbc, fOption, pvParam)` | `SQLGetConnectAttr(hdbc, fOption, pvParam, BufferLength, StringLengthPtr)` | Option IDs map to connection attribute IDs |
| `SQLSetStmtOption(hstmt, fOption, vParam)` | `SQLSetStmtAttr(hstmt, fOption, vParam, StringLength)` | Option IDs map to statement attribute IDs (e.g. `SQL_QUERY_TIMEOUT`, `SQL_MAX_ROWS`) |
| `SQLGetStmtOption(hstmt, fOption, pvParam)` | `SQLGetStmtAttr(hstmt, fOption, pvParam, BufferLength, StringLengthPtr)` | Option IDs map to statement attribute IDs |
| `SQLParamOptions(hstmt, crow, pirow)` | `SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMSET_SIZE, ...)` + `SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, ...)` | Driver Manager maps `crow` to `SQL_ATTR_PARAMSET_SIZE` and `pirow` to `SQL_ATTR_PARAMS_PROCESSED_PTR` |

## 4. Environment version selection (`SQL_ATTR_ODBC_VERSION`)

### 4.1 Supported versions

The driver accepts the following values for `SQL_ATTR_ODBC_VERSION`:
- `SQL_OV_ODBC2`: Declares the environment as operating with ODBC 2.x behavior.
- `SQL_OV_ODBC3`: Declares the environment as operating with ODBC 3.x behavior.
- `SQL_OV_ODBC3_80`: Declares the environment as operating with ODBC 3.8 behavior (where supported by platform headers).

### 4.2 Sequence and validation rules

1. **Rejection of unsupported versions**:
   Passing any value other than the supported versions (e.g. `0`, `999`, `-1`) returns `SQL_ERROR`
   with SQLSTATE `HY024` (`SQLSTATE_INVALID_ATTRIBUTE_VALUE`).
2. **Immutability after connection allocation**:
   Once a connection handle has been allocated on an environment via `SQLAllocHandle(SQL_HANDLE_DBC, henv, ...)`,
   calling `SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, ...)` returns `SQL_ERROR` with SQLSTATE `HY010`
   (`SQLSTATE_FUNCTION_SEQUENCE_ERROR`). The ODBC version must remain stable for the lifetime of all child handles.

## 5. SQLSTATE diagnostics and remapping

### 5.1 Driver-native diagnostic emission

The driver internally generates standard ODBC 3.x / ISO 9075 SQLSTATE strings (5 characters):
- `HY000` (General error)
- `HY010` (Function sequence error)
- `HY024` (Invalid attribute value)
- `HY092` (Invalid attribute/option identifier)
- `01004` (Data truncated)
- `08003` (Connection not open)

The driver does not internally translate SQLSTATEs based on `SQL_OV_ODBC2`; it emits modern
ODBC 3.x SQLSTATEs directly. When mediated by a Driver Manager, the Driver Manager performs
SQLSTATE translation from ODBC 3.x classes (such as `HYxxx`) to legacy ODBC 2.x classes (such as `S1xxx`)
when returning diagnostics to an ODBC 2.x application. Direct driver callers operating under `SQL_OV_ODBC2`
receive modern 3.x SQLSTATEs; native driver-side translation under `SQL_OV_ODBC2` is driver debt tracked
for Unit C qualification.

Standard Driver Manager SQLSTATE remappings (per Microsoft ODBC Appendix Y):
- `HY000` -> `S1000` (General error)
- `HY010` -> `S1010` (Function sequence error)
- `HY024` -> `S1009` (Invalid argument value)
- `HY092` -> `S1092` (Option type out of range)
- `01004` -> `01004` (Data truncated)
- `08003` -> `08003` (Connection not open)

### 5.2 Legacy `SQLError` and `SQLErrorW` semantics

The driver's native implementation of `SQLError` / `SQLErrorW`:
- Accepts `henv`, `hdbc`, and `hstmt`.
- Handle hierarchy routing:
  - If `hstmt != SQL_NULL_HSTMT`, reads and drains statement diagnostic records.
  - If `hstmt == SQL_NULL_HSTMT` and `hdbc != SQL_NULL_HDBC`, reads and drains connection diagnostic records.
  - If `hstmt == SQL_NULL_HSTMT` and `hdbc == SQL_NULL_HDBC` and `henv != SQL_NULL_HENV`, reads and drains environment diagnostic records.
- Records are drained sequentially on successive calls; returns `SQL_NO_DATA` when all records have been consumed.
- Parameter bounds: allows `Sqlstate`, `NativeError`, `MessageText`, and `TextLength` to be `nullptr` independently without crashing.

## 6. Implementation gaps and explicit boundaries

1. **Dual-export driver library**: The driver library does not provide exported stubs for legacy 2.x function aliases.
   Applications calling legacy 2.x aliases must link through and be mediated by a Driver Manager.
2. **`SQLParamOptions` symbol export**: The driver library does not directly export `SQLParamOptions`.
   However, per Microsoft's mapping rules, the Driver Manager translates `SQLParamOptions` to `SQL_ATTR_PARAMSET_SIZE`
   and `SQL_ATTR_PARAMS_PROCESSED_PTR`, which ODBCPP supports. End-to-end DM-mediated verification of
   `SQLParamOptions` is tracked as an acceptance test in Unit C.
3. **Version-dependent catalog datetime data types (genuine gap)**:
   Per Microsoft ODBC Appendix G rules, an ODBC 3 driver operating with `SQL_ATTR_ODBC_VERSION = SQL_OV_ODBC2`
   must return legacy datetime type codes (`SQL_DATE`, `SQL_TIME`, `SQL_TIMESTAMP`) in the `DATA_TYPE` column of
   catalog result sets (such as `SQLColumns` and `SQLGetTypeInfo`), whereas under `SQL_OV_ODBC3` it returns modern
   codes (`SQL_TYPE_DATE`, `SQL_TYPE_TIME`, `SQL_TYPE_TIMESTAMP`).
   While the driver accepts `SQL_OV_ODBC2` on the environment handle, dynamic version-dependent catalog row value
   remapping is not yet implemented natively in catalog queries. Because Driver Managers do not inspect or repair
   catalog result row contents, this remains an explicit implementation and test gap tracked for Unit C follow-up.
