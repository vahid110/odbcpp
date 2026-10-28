# ODBC 2 compatibility boundary (#118 Units B/C)

This batch adds tests and documents the existing adapter. It does not turn
ODBCPP into a native ODBC 2 driver or change product behavior. The application
links its real Driver Manager, never `odbcpp_core`. ODBC 3 results are not
ODBC 2 qualification. Redshift, mixed SQLWCHAR widths, pooling and Windows
installed-package checkpoint support remain separate work.

## Existing coverage and new scenarios

| Boundary | Existing coverage | New coverage |
| --- | --- | --- |
| Environment selection | Direct attribute tests accept SQL_OV_ODBC2; usual manager suites select ODBC 3 | Explicit SQL_OV_ODBC2 and SQLAllocEnv implicit selection, verified on the actual handle |
| No driver loaded | ODBC 3 offline manager lifecycle/identity tests | Legacy ENV/DBC allocation/free, login-timeout aliases, disconnected statement/capability errors, consumed and cleared SQLError, diagnostic bridge |
| Driver loaded, no connection | Export inventory; direct internal diagnostic tests | Exact candidate's modern replacement exports, retained SQLError/W, absence of legacy allocation/attribute/transaction exports |
| Connected manager/driver | ODBC 3 public integration suites | Both ODBC 2 environment paths: legacy statement options, guarded numeric output, bind/fetch/close/drop, autocommit, real commit/rollback, capability arrays and SQLSTATE diagnostics |
| Fake backend | Direct tests can inject malformed backend state; existing manager tests cannot | No new fake framework or product switch. The added pre-driver cases close genuine database-free gaps; backend faults remain direct-layer coverage |

The sequence is pre-driver collection, exact driver export check, connected
loopback PostgreSQL cases, then independent diff/result review. Keep failures
from each stage; do not interpret a disconnected capability error as a driver's
capability report.

## Native exports and Driver Manager mappings

The candidate exports SQLAllocHandle/SQLFreeHandle, environment and connection/
statement attribute APIs, SQLEndTran and SQLGetDiagRec. It also retains SQLError,
SQLErrorW and SQLFreeStmt. It does not export SQLAllocEnv/Connect/Stmt,
SQLFreeEnv/Connect, Get/SetConnectOption, Get/SetStmtOption or SQLTransact.
The manager therefore supplies those legacy entry points and maps them to
modern driver calls. SQLFreeStmt(SQL_DROP) is also a manager mapping boundary.
[Microsoft's mapping table](https://learn.microsoft.com/en-us/sql/odbc/reference/appendixes/mapping-deprecated-functions)
explains these replacements; merely finding a symbol in the manager is not
proof that it works with this driver.

SQL_API_ALL_FUNCTIONS is a 100-element SQLUSMALLINT boolean array, not the
ODBC 3 bitmap. Tests guard both ends and compare relevant entries with single
queries. SQLGetFunctions itself can be answered by the manager even before a
connection, so it is not a valid disconnected-error stimulus.
[SQLGetFunctions contract](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetfunctions-function).

SQL_OV_ODBC2 requires the older SQLSTATE mappings, including HY010 to S1010
and HY092 to S1092. Legacy diagnostic consumption and clearing are separate
from modern record retrieval.
[SQLSTATE mappings](https://learn.microsoft.com/en-us/sql/odbc/reference/develop-app/sqlstate-mappings),
[diagnostic retrieval](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdiagrec-function).

## Known manager defect, narrowly identified

Initial native evidence `odbc2-compatibility132/predriver002` shows Linux
x64 iODBC 3.52.16, SQL_DM_VER `03.52.1623.0502`, application SQLWCHAR=4,
ODBC environment=2, returning SQL_ERROR when SQLGetDiagRec reads a cleared,
never-connected DBC. No driver was loaded. Its
[herr.c fallback](https://github.com/openlink/iODBC/blob/v3.52.16/iodbc/herr.c#L1096)
returns this error when neither driver diagnostic entry point exists.

Only manager SHA256
`3451bc5bafb224754414cacda207e6693680051077515e86b5163980db31d7aa`
is eligible for that exact expectation. Configure snapshots the matching file;
runtime verifies the actual provider path and bytes, version, platform/ABI,
ODBC 2 environment and absence of a loaded driver. XML marks the defect.
Unknown images retain SQL_NO_DATA. This records an incompatibility; it does
not claim conformance or reuse the ODBC 3 expectation table.

## Running and interpreting results

Build `test_odbc2_compatibility` using the platform's explicit manager/SQLWCHAR
preset. It builds the candidate driver as a dependency. Ordinary CTest includes
`dm_odbc2_predriver` and `dm_odbc2_exports` as separate processes under `unit`.
No registration, database or privileged operation is needed for those cases.

For the connected cases, configure POSTGRESQL with
`-DODBCPP_ODBC2_CONNECTED_TESTS=ON`. Provision an isolated loopback database and
role both named `odbc2_132`, then supply `ODBCPP_ODBC2_TEST_PORT` to CTest.
The tests connect only to 127.0.0.1 at that explicit port, without ambient DSNs,
password inputs or TLS (local synthetic fixture only). They use temporary tables,
finite ODBC timeouts and the CTest process timeout. The fixture owner must stop
its server and retain logs after collection, including when assertions fail.
Run `ctest --test-dir BUILD -R '^dm_odbc2_' --output-on-failure` (add
`-C Release` for Windows). Keep JUnit, GoogleTest XML, driver API traces,
source/configuration and hashes of the test, driver and observed manager together.

Native result table and supported/unsupported details will be completed after
collection. No connected pass is currently claimed. ANSI entry points with
matching 2/2 Windows/unixODBC and 4/4 iODBC profiles are this batch's boundary;
Unicode conversion, catalog/date-code legacy semantics, extended fetch,
parameter arrays and connection-level statement options are not qualified here.
