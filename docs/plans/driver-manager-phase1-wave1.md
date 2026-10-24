# Driver-manager test layer: Phase 1, wave 1 results

Part of [driver-manager-test-layer.md](driver-manager-test-layer.md). Wave 1 runs six PostgreSQL integration suites through a real Driver Manager: `it_prepared_statements_real`, `it_bind_col_real`, `it_native_sql`, `it_handle_lifecycle`, `it_diagnostics` and `it_get_info`. It doesn't change any product code or any assertion.

## What changed

Each suite now builds twice from the same source:

- **Direct build** (unchanged target name): links `odbcpp::core`, behaves as before, and remains the diagnostic baseline.
- **Manager build** (`dm_<suite>`): links only the platform manager and GoogleTest, and defines `ODBCPP_TEST_VIA_DRIVER_MANAGER`. Because it never links `odbcpp_core`, any driver internal left in the suite fails to link instead of quietly bypassing the manager.

Two source changes are needed for the manager build. In both, the direct build keeps its existing behaviour.

**1. `SQLConnect` gets a DSN name.** Fixtures used to pass the whole configured connection string (default `DSN=RedshiftProd`) as `SQLConnect`'s ServerName. A manager treats ServerName as a DSN name.
- `odbcpp::test::configured_server_name()` returns the configured string unchanged in direct builds.
- In manager builds it returns `NAME` from `DSN=NAME[;]`.
- `SQLDriverConnect` call sites are unchanged.

**2. Internals are replaced or compiled out:**

| what | direct build | manager build |
|---|---|---|
| Compiled-product check (`DatabaseFactory`) | `odbcpp::test::compiled_redshift_product()` calls the core, as before | the same function reads `ODBCPP_TEST_REDSHIFT_PRODUCT`, set from `TARGET_DATABASE` |
| UTF-8/SQLWCHAR conversion | `odbcpp::test::text` aliases the driver's `rs::odbc` converters | a strict equivalent in the test, checked at 2 and 4 bytes |
| Driver headers | `odbc/odbc_api.h`, `odbc/odbc_types.h` | the manager's `<sql.h>`/`<sqlext.h>` (`tests/test_dm_odbc.h`) |
| `DiagnosticsIntegrationTest`: 9 cases on a statement whose connection was never opened (built with `make_statement`) | runs | compiled out: no public API can create that state, and a manager returns 08003 first |
| `GetInfoFixtureProfile`: 1 case checking the fixture's `DatabaseType` mapping | runs | compiled out |
| Two `it_handle_lifecycle` backend health cases using `ODBCBackendTestAccess` | run | compiled out |
| `it_bind_col_real` Redshift timeout blocker: verified-TLS DSN check | runs | fails with an explicit reason if reached. It runs only for Redshift, which wave 1 doesn't cover |

Correction to the Phase 0 inventory: the 9 `DiagnosticsIntegrationTest` cases can't be migrated by replacing `make_statement`, because the state they test is unreachable through public calls. They stay on the direct layer. `it_diagnostics` contributes 1 manager case.

The Phase 0 inventory reads source text and counts both sides of each `#ifdef`, so it still reports the direct-build internals for these files. The authoritative proof that the manager build contains no internals is that `dm_*` links without `odbcpp_core`.

### Ownership

| component | owner file | reason | allowed dependencies | proof |
|---|---|---|---|---|
| Manager-side ODBC declarations | `tests/test_dm_odbc.h` | test-only; the application side of the manager ABI | manager `<sql.h>`/`<sqlext.h>` | `dm_*` links without `odbcpp_core` |
| Configured DSN name for `SQLConnect` | `tests/test_connection_config.h` | test fixture configuration | std, ODBC types | direct suites unchanged: 310/310 pass |
| Compiled-product check | `tests/test_product_profile.h` | test fixture; direct builds still ask the core | direct: `core/database/database_factory.h`; manager: none | as above |
| Test-side text conversion | `tests/test_text.h` | the manager build can't link `rs::odbc` | direct: `odbc/unicode.h`; manager: std | round-trip and invalid-input checks at 2- and 4-byte SQLWCHAR |
| Manager targets and opt-in registration | `cmake/Tests.cmake`, `CMakeLists.txt` | test build graph | manager library, GoogleTest | configure, build, CTest listing |

Nothing moved into `sdk/`, and no product or driver file changed.

### Registration

The `dm_it_*` targets always build whenever a manager library is configured, so the three-platform PR builds compile them.

CTest registers them only with `-DODBCPP_DM_INTEGRATION_TESTS=ON`, under the labels `integration;dm-integration`. They're opt-in because:
- `primary.yml` runs `ctest -L integration` on macOS iODBC for every PR;
- the manager deviations below aren't yet asserted as expectations; that's Phase 3;
- registering them by default would turn every full run red on known manager behaviour.

To run them, configure with `-DODBCPP_DM_INTEGRATION_TESTS=ON` and register the driver privately:
- **iODBC:** `ODBCINI` and `ODBCINSTINI` are full paths.
- **unixODBC:** `ODBCSYSINI` is the directory, and `ODBCINSTINI` is a file name relative to it (`odbcinst.ini`). A full path there is joined to `ODBCSYSINI`, and every connect fails. This defect in my first fixture skipped 290 cases.

The fixtures skip, rather than fail, when they can't connect, so a run with skips means the setup is wrong.

## Runs

- **Environment:** macOS 15.3.2 arm64, Homebrew libiodbc 3.52.16 and unixODBC 2.3.14, a disposable loopback PostgreSQL 17.11 (port 57432, SSL off, `RedshiftProd` DSN as in CI), `TARGET_DATABASE=POSTGRESQL`.
- **Method:** each manager case ran in its own process, so a manager crash couldn't hide later cases.
- **Traces:** the iODBC application-side trace plus the driver's own API trace (`ODBCPP_TRACE`), which records what the manager called and what the driver returned.
- **Evidence:** raw output, XML, traces and macOS crash reports are kept in the private evidence directory `~/odbcpp-audits/dm-test-layer/`.

Final runs are on `10d664c` with a clean worktree and `-DODBCPP_DM_INTEGRATION_TESTS=ON`; an earlier collection on the same source gave the same buckets.

| profile | direct (`it_*`) | manager (`dm_it_*`) |
|---|---|---|
| iODBC 3.52.16, 4/4 (`macos-iodbc-pg`) | 310 pass, 0 fail, 0 skip | 298 cases: 100 pass, 192 fail, 6 crash, 0 skip |
| unixODBC 2.3.14, 2/2 (`unixodbc-pg`) | 310 pass, 0 fail, 0 skip | 298 cases: 281 pass, 17 fail, 0 crash, 0 skip |

One `it_handle_lifecycle` case crashed in the final iODBC run but failed in the earlier one (6 crashes against 5). That matches bucket J, whose crashes depend on memory layout.

With the option off (the default), CTest lists 133 tests and no `dm_*`.

Windows ODBC and Linux runs aren't part of this wave.

## Failure classification

Each failing case is counted once, by its first failure, which usually masks later assertions in the same case. "Origin" says where the observed result came from:
- **manager:** the manager answered, or changed what the driver returned. The driver trace proves which.
- **driver:** the driver's own answer.

No driver defect was confirmed in this wave. K, at first an open question, is decided by the ODBC reference in the driver's favour.

| id | profile | cases | observed | origin and evidence | kind |
|---|---|---:|---|---|---|
| A | iODBC | 174 | ANSI `SQLGetDiagRec` (and `SQLNativeSql`) with a NULL output buffer and length 0 returns `SQL_SUCCESS_WITH_INFO`, and the application's SQLSTATE stays empty | **Manager.** iODBC passes the driver's `SQLGetDiagRecW`/`SQLNativeSqlW` a non-null scratch buffer with length 0. The driver correctly reports truncation (`SQL_SUCCESS_WITH_INFO`, state 01004 or the real state), and iODBC then doesn't copy the SQLSTATE back. The test helpers use exactly this NULL-buffer pattern, so this one behaviour masks most of the suite. | manager defect |
| A2 | unixODBC | 13 | `SQLGetDiagRec` with NULL message and length 0 returns `SQL_SUCCESS_WITH_INFO` | **Manager.** unixODBC serves these records from its own store: records it raised itself, or driver records it already fetched with full 513-unit buffers (driver trace). | manager defect |
| B | iODBC | 2 | SQL with an embedded NUL and an explicit length runs (`SQLExecDirect`) or is translated (`SQLNativeSql`) instead of failing | **Manager.** iODBC converts the ANSI text to Unicode and passes `SQL_NTS` instead of the length (driver trace: `TextLength="SQL_NTS"`), so the driver sees only the text before the NUL. | manager defect |
| C | iODBC | 5 | `S1010`, `S1003`, `S1093`, `S1105` instead of `HY010`, `HY003`, `07009`, `HY105` | **Manager.** iODBC raises these itself without calling the driver, using ODBC 2.x SQLSTATEs although the application set `SQL_OV_ODBC3`. | manager defect |
| D | iODBC | 4 | `SQLGetData(..., SQL_ARD_TYPE, ...)` returns `SQL_ERROR` | **Manager.** iODBC rejects `SQL_ARD_TYPE` without calling the driver. | manager defect |
| E | iODBC | 3 | Bound `SQL_C_WCHAR` length indicator is 8 instead of 24, and related binary/wide `SQLGetData` lengths differ | **Manager.** Calling the built driver directly (dlopen, no manager), with and without iODBC's `IODBC_ATTR_APP_WCHAR_TYPE`=UCS4 (attribute 1061), returns 24. The application gets 8 through iODBC with the same bound pointers, so iODBC rewrites the indicator. One case was probed; the other two are probably the same family. | manager defect |
| F | both | 1 + 1 | `SQL_ODBC_VER` is `03.52.0000` (iODBC) or `03.52` (unixODBC), not `03.80` | **Manager, by design:** the manager answers with its own version. | expectation is direct-only |
| G | iODBC | 1 | After `SQLFreeHandle(SQL_HANDLE_DBC)` on a connected connection returns `SQL_ERROR`, the handle is invalid (`SQL_INVALID_HANDLE`), and teardown aborts | **Manager.** The driver gets no call. iODBC frees the connection while reporting the error, and ASan reports a double free in iODBC's `SQLFreeHandle`. | manager defect |
| H | iODBC | 1 | `SQLGetDiagField(SQL_HANDLE_STMT, 0, SQL_DIAG_ROW_COUNT)` returns `SQL_INVALID_HANDLE`, then the process segfaults at exit | **Manager.** iODBC calls the driver with `HandleType=SQL_HANDLE_DBC` and the statement handle, and the driver correctly rejects it. The exit crash didn't reproduce under ASan; its cause is undetermined. | manager defect; crash undetermined |
| I | iODBC | 3 | Driver-connect output lengths are truncated (3 instead of 16) and the 01004/01S00 warning records are missing; with zero output capacity, iODBC crashes inside its own `SQLDriverConnectW` | **Manager**, matching the PR #96 iODBC findings. The driver returns the full lengths and records. | manager defect |
| J | iODBC | 2 | Segfault in the test body after manager calls | Didn't reproduce under ASan; there the first failure is A. The crash cause is undetermined and not attributed. | undetermined |
| K | iODBC | 1 | `SQLNativeSql(..., NULL output, BufferLength -7)` returns `HY090` | **Manager:** iODBC rejects the call without calling the driver. The ODBC reference raises HY090 only when "*BufferLength* was less than 0 and the argument *OutStatementText* was not a null pointer", so the driver's success is the spec answer, and the direct expectation stays. | manager defect |
| L | iODBC | 1 | ANSI `SQLNativeSql` of UTF-8 text returns `42000` | Probably iODBC's ANSI-to-Unicode conversion under the C locale. Not confirmed: the driver trace doesn't record the text. | probable manager |
| M | unixODBC | 1 | `SQLGetData` before execute returns `HY010`, not `24000` | **Manager, per the ODBC state tables** (the statement is still in states S1 to S3). | profile expectation |
| N | unixODBC | 1 | `SQL_API_ODBC3_ALL_FUNCTIONS` also reports 10 ODBC 2.x function IDs (1, 2, 3, 14, 15, 22, 23, 42, 46, 50) | **Manager:** unixODBC maps those ODBC 2.x functions onto the driver's ODBC 3 functions. | profile expectation |
| O | unixODBC | 1 | After a failed re-`SQLPrepare`, `SQLExecute` of the earlier prepared statement fails | **Manager, per the state tables:** a failed prepare returns the statement to the allocated state. The driver isn't called. The driver keeps the old statement when called directly. | expectation is direct-only |

Totals: iODBC's buckets sum to 198 failing or crashing cases, and unixODBC's to 17. The bucket list for each case is reproducible from the evidence directory.

## Not done in this wave

- **Windows ODBC and Linux unixODBC/iODBC runs.** The PR builds compile `dm_*` on all three platforms, but the suites ran only on macOS.
- **unixODBC manager tracing.** Homebrew unixODBC 2.3.14 wrote no trace file even with a minimal private `odbcinst.ini`. The driver-side trace was used instead.
- **Wave 2:** `it_metadata_real` and `it_redshift_real`, which needs live Redshift admission.
- **Asserting deviations per profile.** That's Phase 3: the expectation table, with the evidence above as its source.
