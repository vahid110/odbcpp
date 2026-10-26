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

CTest registers the complete connected suites with `-DODBCPP_DM_INTEGRATION_TESTS=ON`, under `integration;dm-integration`. They remain opt-in because they need a configured database and an applicable manager profile. Known deviations now have explicit, profile-bound expectations in `tests/test_dm_expectations.h`; unknown profiles retain the direct expectations.

The database-free `dm-offline` subset always registers when the manager targets exist. Ordinary Windows, Ubuntu and macOS PR CI executes it and retains exact-case GoogleTest XML, CTest JUnit and observed provider/profile evidence. See [the offline subset and coverage limits](driver-manager-offline-ci.md). It does not enable or replace the complete connected suites.

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

At the original wave-1 revision, the option-off inventory contained 133 tests and no `dm_*`. Current builds additionally register the bounded offline subset described above.

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

| P | iODBC | 1 | ANSI `SQLGetInfo(SQL_DRIVER_NAME)` reports length 4 for a truncated 5-byte buffer, and 84 for a NULL buffer, where the answer is 21 characters | **Manager.** iODBC calls the driver's `SQLGetInfoW`, which correctly reports truncation and 84 bytes (21 four-byte characters). iODBC then hands the ANSI application the copied length, or the wide byte count, instead of 21. The application's own wide calls get the correct 84. | manager defect |
| Q | unixODBC | 1 | After a driver-connect warning, record 1 is `01S00` and record 2 is `01004`; the driver posted them in the opposite order | **Manager.** The driver trace shows unixODBC fetching `01004` then `01S00` through `SQLError` during the connect. The application then reads them in reverse. | manager defect |
| R | iODBC | 1 | `SQLGetFunctions(..., NULL)` returns `SQL_SUCCESS` instead of `HY009` | **Manager.** iODBC answers without calling the driver. | manager defect |

Totals: iODBC's buckets sum to 198 failing or crashing cases, and unixODBC's to 17. The bucket list for each case is reproducible from the evidence directory. Buckets P, Q and R were hidden behind bucket A or A2 in this first collection and became visible in Phase 3 step 1 (below).

## Phase 3 step 1: expectation table and NULL-buffer diagnostics

Plan principles 3, 4 and 6 call for expected manager results kept as data, known deviations asserted rather than skipped, and each expectation recording who raised the SQLSTATE. `tests/test_dm_expectations.h` is that table.

The default for every profile is "same as direct". A manager that behaves differently fails the test, and the table is updated with evidence. A manager's own version (`SQL_DM_VER`, read on an unconnected handle) selects its entries. The origin of a record comes from its message prefix: `[iODBC][Driver Manager]`, `[unixODBC][Driver Manager]` or `[Microsoft][ODBC Driver Manager]`. This driver's messages carry no vendor prefix.

The first entry covers ANSI `SQLGetDiagRec` with a NULL message buffer and length 0 (buckets A and A2), the pattern behind most first failures. A direct probe on both managers gave:

| manager (`SQL_DM_VER`) | driver-raised record | manager-held record |
|---|---|---|
| iODBC `03.52.1623.0502` | `SQL_SUCCESS_WITH_INFO`, SQLSTATE buffer not written | same as direct |
| unixODBC `03.52.0002.0003` | same as direct | `SQL_SUCCESS_WITH_INFO`, SQLSTATE written |

"Manager-held" means a record the manager raised, or, for unixODBC, a driver record posted by `SQLConnect` or `SQLDriverConnect`. unixODBC copies those into its own store during the connect call. The fixture connect forwards record them (state plus message, read non-destructively), so a later, different record on the same connection isn't mistaken for one.

`odbcpp::test::diag_rec` applies the entry:
- **Direct builds:** it is `::SQLGetDiagRec` itself, so nothing changes.
- **Manager builds,** for the NULL-buffer call only: it asserts the result and SQLSTATE against the table, then reads the record again with a real buffer and returns that. The case therefore goes on to check the record instead of stopping at the first diagnostic.

It is reached through a fixture-member `SQLGetDiagRec` forward in `it_prepared_statements_real` and `it_bind_col_real`, the same forwarding pattern those fixtures already use for timeout budgets, and directly from the three `diagnostic_state` helpers. No test body changed.

Results on the same fixture as above; the direct suites are unchanged (310 of 310 pass on both profiles):

| profile | manager before | manager after |
|---|---|---|
| iODBC 3.52.16 | 100 pass, 192 fail, 6 crash | 269 pass, 24–25 fail, 4–5 crash, 0 skip (one bucket J case crashes in some runs) |
| unixODBC 2.3.14 | 281 pass, 17 fail | 292 pass, 6 fail, 0 skip |

Of the 174 iODBC cases whose first failure was bucket A, 169 now pass entirely. Four now fail on a deeper deviation (D, P, R and O), and one, the `SQLNativeSql` case, is still A. The remaining failures, by bucket:

| profile | cases | buckets |
|---|---:|---|
| iODBC | 29 | A 1 (`SQLNativeSql` with a NULL output, which doesn't use the diagnostic helper), B 2, C 5, D 5 (one more `SQL_ARD_TYPE` case surfaced), E 3, F 1, G 1, H 1, I 3, J 2, K 1, L 1, O 1, P 1, R 1 |
| unixODBC | 6 | F 1, M 1, N 1, O 2 (one more state-table case, `ReplacesPreparedStatementsAndProtectsOpenCursors`: after the driver's `24000`, unixODBC answers the next call with `HY010` without calling the driver), Q 1 |

No driver defect surfaced in the previously hidden assertions.

## Phase 3 for wave 1: every deviation asserted

Every remaining deviation is now an entry in `tests/test_dm_expectations.h`, keyed by the manager's exact `SQL_DM_VER`. Each site asserts it, so wave 1 passes on both managers while still checking what each manager does. Any manager without an entry, such as Windows ODBC or another iODBC or unixODBC version, defaults to "same as direct", so a new or changed behaviour fails visibly.

Results on the same fixture. Each manager case ran in its own process:

| profile | direct | manager |
|---|---|---|
| iODBC 3.52.16 | 311 pass (one new case), 0 skip | 299 pass, 0 fail, 0 skip |
| unixODBC 2.3.14 | 311 pass, 0 skip | 299 pass, 0 fail, 0 skip |

### How a site asserts a deviation

The site helpers return the direct answer in direct builds and for any manager without an entry:
- `dm::value(id, direct, deviated)`: the expected value at the site.
- `dm::text(id, direct)`: the manager's own value, kept in the table entry.
- `dm::deviates(id)`: whether this manager has the entry.

Where a deviation makes the rest of a case unreachable through that manager, the case asserts the deviating result, then calls `dm::stops_case(id)`. That records `dm_case_stopped_by=<bucket>` in the case's XML and ends the case. The case is not skipped: it passes, having asserted the manager's behaviour.

Two helpers inside `odbcpp::test::diag_rec` handle general patterns, after asserting the deviation:
- The NULL-buffer diagnostic.
- iODBC's ODBC 2.x SQLSTATEs on records it raises (`S1010`, `S1003`, `S1090`, `S1093`, `S1105`, `S1001`). These map to ODBC 3.x through the ODBC reference's SQLSTATE mapping table, so the test's own SQLSTATE check still applies.

### Cases that end early, with the reason in XML

- **iODBC, 11 cases:**
  - D, 5 cases: `SQL_ARD_TYPE` is rejected, and the rest needs the ARD-typed value.
  - B, 2 cases: an explicit length is replaced by `SQL_NTS`.
  - L, 2 cases: the ANSI statement is widened byte by byte.
  - O, 1 case: the state table after a failed re-prepare.
  - G, 1 case: iODBC frees the connection while refusing to free it, so any further call would read freed memory.
- **unixODBC, 2 cases:** O, the state table after a failed re-prepare, including one where the open cursor is forgotten.

### Root causes found in this phase

- **The crashes (bucket J) and the `it_diagnostics` exit crash.** iODBC 3.52.16 answers `SQL_ATTR_LOGIN_TIMEOUT` before connect itself. It writes **8 bytes** into the application's 4-byte `SQLUINTEGER`; the attribute is 32-bit.
  - A probe with sentinel-filled buffers showed the exact width. The faulting addresses (`0x100000000` and similar) are pointers whose low 32 bits were zeroed. AddressSanitizer missed it because it doesn't instrument libiodbc's own stores.
  - The fixtures' timeout-budget helpers read this attribute into a stack variable before every connect.
  - They now read it through zeroed 8-byte storage (`odbcpp::test::get_login_timeout`) and take the declared 4 bytes, which stays correct for the driver's 4-byte write.
  - A new case, `ConnectionAttributeIntegrationTest.LoginTimeoutOutputWidth`, asserts the width on every build: 4 bytes direct, 8 under the iODBC entry.
  - With that, every former crash runs to completion except G, whose iODBC double free is asserted by ending the case early.
- **Bucket L confirmed and widened.** The driver receives the application's buffers and target types unchanged. But the stored data is the Latin-1 reading of the UTF-8 statement literal: iODBC widens the ANSI statement byte by byte in the C locale. `GetDataRetrievesWideTextInWholeCodePoints` and `CharacterToBinaryReturnsRawUtf8Bytes` therefore move from E to L, and assert exactly that stored value (`dm::latin1_widened`). E keeps only the bound indicator rewrite, which the dlopen probe showed.
- **New buckets:**
  - **S (iODBC):** `SQLNumResultCols` passes the driver iODBC's own variable, and copies its 0 to the application even when the driver fails.
  - **T (unixODBC):** unixODBC answers `SQL_XOPEN_CLI_YEAR` itself: "1995" for the ANSI call and "1994" for the wide call.
  - **R:** also on unixODBC, which accepts the NULL output and clears the diagnostics, where iODBC leaves the previous `HY095`.
  - **K:** iODBC raises `S1001` (mapped to `HY001`), not `HY090`.

No driver defect was found in wave 1.

## Wave 2

### `it_metadata_real` (PostgreSQL, run)

`dm_it_metadata_real` links only the manager. What stays direct-only, or changes for manager builds:
- **Direct-only case:** `PreparedMetadataRefreshesAfterLocalSchemaMutationAndReset`, which needs the backend session view.
- **Redshift sizing reconnect:** only reachable on Redshift. Direct builds check the driver's own parser round-trip and pass the string to `SQLConnect`. Manager builds append `MAXVARCHARSIZE=0` and use `SQLDriverConnect`.
- **Redshift timeout blocker:** it fails with a stated reason if reached in a manager build, as in `it_bind_col_real`.

Its deviations are asserted from the table like wave 1's. New entries, each attributed from the driver trace:

| id | manager | observed | origin |
|---|---|---|---|
| V | both | after `SQLMoreResults` fails, the next statement is refused with `24000` | Manager state table: the cursor is still open after a failed `SQLMoreResults`, although the driver ended the batch. The case asserts the refusal, closes the cursor with `SQLFreeStmt(SQL_CLOSE)` and continues unchanged. |
| O (widened) | unixODBC | after any call on an open cursor fails with the driver's `24000`, unixODBC forgets the cursor and refuses `SQLFetch`/`SQLCloseCursor` itself | Manager. Seen in `SQLStatistics`, `SQLGetTypeInfoW`, `SQLProcedures`, `SQLProcedureColumns` and `SQLSpecialColumns`, and in wave 1's re-prepare. |
| W | unixODBC | its own `SQLRowCount` refusal (`HY010`) writes -1 into the output | Manager; the driver isn't called. |
| X | unixODBC | the IRD of a statement whose cursor is closed is refused with `HY007` | Manager; the driver isn't called. |
| U | iODBC | `SQLSpecialColumns(..., SQL_NULLABLE)` returns the non-nullable index | Manager: the driver receives `Nullable=SQL_NO_NULLS` for every call. |
| P (widened) | iODBC | ANSI `SQLColAttribute` truncated name and type-name lengths are the copied length | Manager. |
| S (widened) | iODBC | `SQLNumResultCols(stmt, NULL)` succeeds | Manager: the driver gets iODBC's own variable, so it never sees the NULL. |
| Y | both | after the driver closes a session internally on a query timeout, `SQLConnect` is refused with `08002` | Manager: it still has the connection open. The case asserts the refusal and ends. |
| C, L, H | iODBC | `S1002` (mapped to `07009`); widened ANSI catalog names; statement header fields | As in wave 1. |

| profile | direct | manager |
|---|---|---|
| iODBC 3.52.16 | 75/75 pass | 74/74 pass |
| unixODBC 2.3.14 | 75/75 pass | 74/74 pass |

### `it_redshift_real` (live Redshift, built, not run)

`dm_it_redshift_real` builds and links against iODBC and unixODBC in the `macos-iodbc-redshift` and `unixodbc-redshift` configurations. It hasn't been run: it needs a live Redshift session, which the human schedules.

What manager builds use in place of the driver internals:
- The fixtures read their configured connection string through `tests/test_connection_string.h`. Direct builds call the driver's parser through it. Manager builds use a copy of the same rules: trimmed upper-cased keys, braced values with `}}`, first occurrence kept, and errors for an embedded NUL or an unterminated brace.
- The SDK's `Clock`, `Deadline` and `ResultCell`/`ResultRow`/`ResultRows` are aliases of `std` types, so local aliases with the same definitions replace them.
- The SDK's default `ResultLimits`/`InputLimits` come from `tests/test_sdk_limits.h`. Direct builds pin every field to the SDK with `static_assert`, so the copies can't drift.

Ten cases stay direct-only:
- three primary-key-mode cases that resolve the DSN through the driver;
- five that open a backend session;
- one that observes raw backend metadata;
- the `SQLCancelHandle` case, on iODBC only, since iODBC 3.52 neither declares nor exports it.

That leaves 147 manager cases on iODBC and 148 on unixODBC, against 157 direct.

### Totals after wave 2 (PostgreSQL fixture, `2ddbe2d`)

| profile | direct, 7 suites | manager, 7 suites |
|---|---|---|
| iODBC 3.52.16 | 386/386 pass | 373/373 pass |
| unixODBC 2.3.14 | 386/386 pass | 373/373 pass |

## Not done in this wave

- **Windows ODBC and Linux unixODBC/iODBC runs.** The PR builds compile `dm_*` on all three platforms, but the suites ran only on macOS.
- **unixODBC manager tracing.** Homebrew unixODBC 2.3.14 wrote no trace file even with a minimal private `odbcinst.ini`. The driver-side trace was used instead.
- **The live `dm_it_redshift_real` run,** which waits for a scheduled Redshift session.
- **Windows ODBC expectations.** Windows has no entries yet. Its first run will show where it differs from the direct driver.
