# Driver-manager test layer: live Redshift on macOS

Part of [driver-manager-test-layer.md](driver-manager-test-layer.md). This is the first live Redshift run of the three macOS builds: direct (in-process driver), iODBC 3.52.16 and unixODBC 2.3.14. Each failure is sorted into wiring, test or fixture, manager deviation, product, or open. The fixes below are made in source but have not yet been re-run live.

## Run `dm-live-macos-9c79c86-a`

- **Source:** `9c79c86`, exact-head CI green (runs 40136432563 and 40136432583).
- **Resource:** provisioned cluster `odbcpp-redshift-provisioned-pilot` (rg.large, eu-north-1), verified TLS. The cluster was paused before and after the run (verified). Fixture and principal cleanup completed with nothing left over.
- **Network:** the Mac's public IP had changed. Its single 5439 `/32` rule was moved to the new address for the run and removed afterwards. The stale rule for the old address was not restored.
- **Harness:** `tools/development/live_checkpoint.py run`, one CTest invocation, 946 s.
- **Evidence:** private, at `~/.local/state/odbcpp/redshift-pilot/dm-live-macos-9c79c86-a/` (CTest XML, console and full logs, fixture and SQL events).

| build | cases | pass | fail |
|---|---|---|---|
| direct | 524 | 512 | 12 |
| iODBC | 487 | 338 | 149 |
| unixODBC | 488 | 451 | 37 |
| **total** | **1,499** | **1,301** | **198** |

No case skipped or timed out. The generic suites skip when the connection fails, so zero skips also means every suite reached the server through its profile.

## Classification

Probes behind the D buckets ran on the disposable loopback PostgreSQL fixture used in waves 1 and 2. They measure behaviour that the manager produces itself, so the backend doesn't matter.

| bucket | kind | direct | iODBC | unixODBC | cause |
|---|---|---|---|---|---|
| W1 | wiring | 10 | | | The profile of `it_redshift_columns_omitted_table` and `it_redshift_routine_schema_patterns` sets only `ODBCPP_REDSHIFT_TEST_CONNECTION`. These suites connect with `ODBCPP_TEST_DSN`, so they fall back to `DSN=RedshiftProd`, which has no file behind it. The TLS-profile check then finds no `SSL`/`SSLCAFILE`. |
| W2 | wiring | | 145 | | iODBC passes a braced `DRIVER={path}` value to `dlopen` with its braces, then raises IM003. The test sees SQLSTATE `00000` because it reads the record with no message buffer. Every `it_redshift_real` case through iODBC failed this way, so the iODBC rows for D1–D4 are masked. |
| B1 | boundary | | 2 | 2 | The shared timeout fixture (`QueryTimeoutUsesTransportDeadline`, `PreparedMetadataRefreshesAfterTimeoutReconnect`) refuses Redshift in manager builds by design: its verified-TLS check needs the driver's own DSN parser. |
| T1 | test oracle | 1 | 1 | | `ReportsSupportedTypeInformation` expects 17 Redshift rows. The catalog now has 20: `super` (ab4a2a7) and `geometry`/`geography` (0dfa6c2). None of the three has a literal prefix, suffix or create parameters. unixODBC never reaches the assertion (`FailedCallForgetsOpenCursor`). |
| T2 | test | | 1 | 1 | `MissingSchemaUsesOdbcSqlstate`: the cursor-close step for a failed `SQLMoreResults` ran on the Redshift branch too. There, the batch fails at `SQLExecDirect`, so no cursor exists. |
| T3 | test | | | 1 | `DescribesPreparedResultsBeforeExecution` (Redshift branch): a known bucket W deviation (unixODBC writes -1 to the row count on error) that this branch did not apply. |
| D1 | manager | | (masked) | 27 | On an unconnected handle the manager answers `SQL_ATTR_LOGIN_TIMEOUT` itself: unixODBC 15 (`SQL_LOGIN_TIMEOUT_DEFAULT`), iODBC 0 with 8 bytes written and length 0 (bucket J). The direct driver reports 30. This is live bucket R1; the re-run refined the model (see [R1](#live-deviations-r1r17)). |
| D2 | manager | | (masked) | 1 | `NegativeTests` passes a connection string as `SQLConnect` ServerName. The manager reads it as a DSN name and refuses it: unixODBC HY090 (longer than `SQL_MAX_DSN_LENGTH`), iODBC IM002. This is live bucket R2. |
| D3 | manager | | (masked) | 1 | `SQLGetDiagRecW` through a manager reports the record's UTF-8 byte count as its length in characters. Probe: 62 from both managers, 56 direct. The text itself is complete. This is live bucket R3. |
| D4 | manager | | (masked) | 1 | `SQLErrorW` with a NULL buffer and length -1 consumes the record. unixODBC returns `SQL_SUCCESS`. iODBC returns `SQL_ERROR` as the driver does, but the record is still gone. This is live bucket R4. |
| P1 | manager (R11) | | | 2 | Static-cursor `SQL_ATTR_ROW_NUMBER` after `SQL_NO_DATA`, or before any fetch: the driver returned `SQL_SUCCESS` with 0, and unixODBC raised 24000 itself. Recorded as R11; resolved as a product change in issue #120 (driver now raises 24000/HY109 itself; the 0-vs-24000 clause of R11 is dropped, the failed-fetch interception clause is retained as `RowNumberUnfetchableIntercepted`) ([P1](#p1-static-cursor-row_number-off-a-row)). |
| O1 | test oracle | 1 | | 1 | `SuperResultsPreserveServerJsonText` expected the text `null` for `JSON_PARSE('null')`. The driver correctly returned `SQL_NULL_DATA`, because the server makes a top-level JSON null SQL NULL. See [O1](#o1-super-json-null). |

All 198 failures are assigned to a bucket. The case-by-case list is in the private evidence (`classified.json`).

## Fixes made

| bucket | change | verified offline |
|---|---|---|
| W1 | The inventory adds `ODBCPP_TEST_DSN` to the two suites' profile. The full connection string is a valid ServerName for the in-process driver. | The case passes the TLS check and fails only at connect, against `example.invalid`. |
| W2 | `redshift_fixture_factory.build_profile_connection` emits `DRIVER=<path>` bare and refuses a driver path containing `;`, `{`, `}` or `=`. In `it_redshift_real`, the seven duplicated serializer loops are now one `connection_text()`, which leaves `DRIVER` bare through iODBC (live bucket R5, `BracedDriverValueLoadedVerbatim`). | iODBC and unixODBC both load the driver from the bare path and reach the network. Through iODBC, a path-loaded driver round-trips wide text as UCS4 with no registration. |
| B1 | The inventory records both cases through a manager as `MANAGER_BUILD_BOUNDARY`. | The inventory now has 1,495 cases and 104 boundaries. |
| T1 | `main` fixed this independently: 20 rows, with SUPER and EWKB rows that have no literal and no length parameter. The merge keeps main's version. | Passed live in the re-run. |
| T2 | The cursor-close step moves into the PostgreSQL branch. | As T1. |
| T3 | Uses `untouched_row_count(93)`. | As T1. |
| O1 | Column 4 expects `SQL_NULL_DATA`. `main` made the same change independently; the doc correction is this branch's. | Server check (below); passed live in the re-run. |
| D1–D4 | New entries R1–R4 in `tests/test_dm_expectations.h`. `RedshiftLoginTimeoutOptionsRealTest` records the login timeout the application sets on an unconnected handle (fixture forwards) and expects the manager's answer while disconnected. D4 asserts the manager's result and the drained record, then stops the case (`dm_case_stopped_by`). | Expected values come from the probes and the unixODBC live result. The iODBC rows need the re-run. |

## Needs a decision

### P1: static-cursor ROW_NUMBER off a row (resolved: driver raises 24000/HY109)

- **Spec:** `SQLGetStmtAttr` says two different things here:
  - The attribute description says: "If the number of the current row cannot be determined or there is no current row, the driver returns 0."
  - The diagnostics table lists 24000 when the attribute is `SQL_ATTR_ROW_NUMBER` and the cursor is not open, or is positioned before the start or after the end of the result set, and HY109 when the row had been deleted or could not be fetched.
- **Driver:** The driver follows the diagnostics table for forward-only cursors (`MetadataIntegrationTest.ReportsCurrentRowNumberOnlyWhilePositioned`). For static cursors it followed the attribute description, through a deliberate carve-out added with static scrolling (`8ee2cfc`, `odbc/odbc_handles.cpp` `SQL_ATTR_ROW_NUMBER`). Unit tests in `tests/unit/test_backend_contract.cpp` asserted the 0, and so did `RedshiftRecentFeaturesRealTest`.
- **unixODBC:** It applies the diagnostics table itself and returned 24000 after `SQL_NO_DATA`, before the call reached the driver.
- **Done in this branch:** the driver was unchanged, and the unixODBC behaviour was recorded as live bucket R11 (`RowNumberOffRowRefused`). The two live `row_number(…,0)` checks expected the manager's 24000 there.
  - The authorization for this branch covered test infrastructure only. Changing the driver would also have meant reversing a deliberate design that 10 static-cursor unit assertions in `tests/unit/test_backend_contract.cpp` checked, so it wasn't made here.
- **Resolution (issue #120, product change):** the carve-out is removed. `SQLGetStmtAttr(SQL_ATTR_ROW_NUMBER)` now returns 24000 when the cursor is not open on a result set or is positioned before the start or after the end, HY109 when the requested row could not be fetched (an HY111 bookmark failure or an all-slots bulk fetch failure), the positioned row number when positioned, and 0 only for a sparse bulk bookmark rowset whose current row cannot be determined. Each of the 10 unit assertion sites was classified before the change: 7 became 24000, 1 became HY109, 2 (successful bulk bookmark fetches) kept 0. The two live checks in `RedshiftRecentFeaturesRealTest` now expect the driver's own 24000 (after `SQL_NO_DATA`) and HY109 (after a stale-bookmark refusal); their actual live execution remains pending. The old 0-vs-24000 R11 deviation is retired, but its failed-fetch interception clause stays real: unixODBC answers 24000 itself for the HY109 case too, so that clause is retained as `RowNumberUnfetchableIntercepted` in `tests/test_dm_expectations.h` and the live helper maps the HY109 expectation to 24000 through that manager.

### O1: SUPER JSON null (resolved: test oracle)

- **Server check, 2026-10-25:** one read-only `psql` query on the cluster over verified TLS (`sslmode=verify-full`). `JSON_PARSE('null') IS NULL` is true. `JSON_SERIALIZE` of it is the 4 bytes `null`, and `JSON_TYPEOF` is `null`.
- **Driver:** returned `SQL_NULL_DATA`, which is correct. The test's `text_cell` returns an empty string for a NULL cell, and that was the `""` in the failure.
- **Fix:** The oracle now expects `SQL_NULL_DATA` for column 4. `docs/redshift-special-types.md` is corrected. A nested JSON null stays in the text, which the array column already checks.

## Re-run `dm-live-macos-c6d6793-a`

- **Source and resource:** source `c6d6793`, after `main` was merged in, with exact-head CI green (runs 40218714493 and 40218714501). The same cluster, paused before and after.
- **Inventory:** 1,495 cases, 104 boundaries.
- **Time:** 1,740 s, against 946 s for the first run.
- **Result:** 85 failed.

| build | pass | fail | first-run failures fixed |
|---|---|---|---|
| direct | 522 | 2 | 12 of 12 |
| iODBC | 410 | 75 | 72 of 149 |
| unixODBC | 478 | 8 | 28 of 37 |

- **iODBC `it_redshift_real` (75):** these cases ran for the first time, because W2 had hidden them.
  - Most are wave-1 deviations that this suite didn't apply:
    - the NULL-buffer `SQLGetDiagRec` call (A), behind about 40 of them through `get_error()`, `observed(…, state)` and direct checks;
    - ANSI text widened per byte (L), 9;
    - `SQL_ARD_TYPE` rejected (D), 6;
    - driver-connect outputs (I);
    - pre-connect attributes written as 8 bytes (J).
  - Four are new iODBC deviations (R6, R9 and R10 below, and the zero-capacity connect length).
  - Seven fail on a repeated `SQLFetch` after the end of an empty Redshift catalog result. That doesn't reproduce on PostgreSQL. The fix only adds the diagnostic to those assertions, so the next run shows the refusing component and its SQLSTATE.
- **Timeouts (direct 2, unixODBC 1, iODBC 2):** `RedshiftMaterializedEnvelopeRealTest`'s 1,000-row and 8 KiB materializations got HYT00. With the run taking nearly twice as long overall, this is classified as execution environment for now. It needs another look if it repeats.
- **unixODBC (other than the timeout):**
  - P1, 2 cases;
  - the D1 model was incomplete (see R1);
  - three new deviations (R3 truncation, R7, R8);
  - the ReadOnly case, which failed because of the leftover schema below.
- **Cleanup incomplete:**
  - The iODBC ReadOnly case failed partway (bucket A) and left its owned schema `odbcpp_readonly_2026101500000001`, with table `payload`. The harness reported this as residual and doesn't DROP automatically.
  - After the run, the cluster was resumed once. The exact identity was checked against the harness record: schema OID 1398783, table OID 1398784, owner OID 101, one column, one row. The table and schema were dropped with RESTRICT, the schema count was then 0, and the cluster was paused again.

## Third run `dm-live-macos-7d678b9-a`

- **Source and time:** source `7d678b9`, CI green (runs 40227092963 and 40227092940); 1,288 s.
- **Result:** 22 of 1,495 failed. Cleanup completed with nothing left over, and the cluster was paused and verified.

| build | pass | fail |
|---|---|---|
| direct | 523 | 1 |
| iODBC | 468 | 17 |
| unixODBC | 482 | 4 |

- **Timeouts:** the only direct failure, and one in each manager build, is `DirectThousandFourKiBExactCountsAndOwningRecovery` with HYT00. It pulls a ~4 MB result within the 5-second statement budget. It passed in the first run (946 s) and timed out in the slower second and third runs, so it is classified as this connection's throughput, not the code.
- **Other failures:** the cases now run further, so failures that earlier ones had hidden became visible. Each is fixed in the source after this run:
  - R12 (9 iODBC catalog cases);
  - R3 truncation of the copied text (both managers);
  - bucket L ANSI statement text (2 iODBC cases);
  - bucket E bound-wide length (1);
  - R13 (1), R14 (1), R15 (1) and R16 (1).

## Fourth run `dm-live-macos-1a677ec-a`

- **Source and time:** source `1a677ec`, CI green (runs 40236356829 and 40236356838); 1,156 s.
- **Result:** 3 of 1,495 failed. Cleanup completed with nothing left over, and the cluster was paused and verified. The HYT00 case passed in all three builds this time.
- **Failures (all fixed in source after this run):**
  - **`WideDiagnosticsCarryServerUnicodeErrorText`, both managers:** the complete read showed that the manager's wide text is the UTF-8 bytes widened one by one (R3, corrected).
  - **`WideZeroCapacityAndDescriptorUnicodeColumnNames`, iODBC:** narrow name lengths are counted in characters (R17).

## Fifth run `dm-live-macos-4010fe8-a`: all pass

- **Source and time:** source `4010fe8`, CI green (runs 40240011211 and 40240011242); 1,108 s.
- **Result:** **1,495 of 1,495 passed.** No case was skipped. Cleanup completed with nothing left over, and the cluster was paused and verified.

| build | pass | fail |
|---|---|---|
| direct (in-process) | 524 | 0 |
| iODBC 3.52.16 | 485 | 0 |
| unixODBC 2.3.14 | 486 | 0 |

- **Early stops:** 36 manager cases asserted a recorded deviation and then ended, because the rest of the case was unreachable through that manager. Each records `dm_case_stopped_by` in its XML. By bucket: D 11, L 9, O 5, B 2, G 1, R3 2, R4 2, R9 1, R10 1, R15 1, R16 1.
- **Not run:** 104 boundary instances stay unrun and are recorded in the inventory's `applicability.json`.
- **Evidence:** private, at `~/.local/state/odbcpp/redshift-pilot/dm-live-macos-4010fe8-a/`.

## Live deviations R1–R17

All were measured on the loopback PostgreSQL fixture unless marked live.

| bucket | manager | behaviour |
|---|---|---|
| R1 | both | `SQL_ATTR_LOGIN_TIMEOUT` is not the driver's value on an unconnected handle. |
| | iODBC | Answers itself until the first connect attempt loads the driver, even a failed one: the application's value or 0, 8 bytes written, length 0. After that the driver answers. |
| | unixODBC | Answers whenever the handle is not connected: the last value the application set, even one the driver refused while connected, or 15. |
| R2 | both | A connection string passed as `SQLConnect` ServerName is refused: unixODBC HY090, iODBC IM002. |
| R3 | both | `SQLGetDiagRecW` returns the record's UTF-8 bytes widened one by one: one unit per byte, so a non-ASCII message comes back garbled, and its length and every capacity are in bytes. A buffer that fits the true text reports truncation. Confirmed on the fixture by comparing unit by unit. |
| R4 | both | `SQLErrorW` with a NULL buffer and length -1 consumes the record. unixODBC returns SQL_SUCCESS; iODBC returns SQL_ERROR. |
| R5 | iODBC | A braced `DRIVER={path}` value is passed to dlopen with its braces. |
| R6 | iODBC | A failed `SQLDriverConnect(W)` writes 1 to StringLength2 and leaves the output buffer alone. |
| R7 | unixODBC | A wide connection string loses everything after a character outside the BMP, so the manager raises IM002. |
| R8 | unixODBC | `SQLGetFunctions` for the individual ODBC 3.8 ID 1022 raises HY095. The ODBC 3 bitmap carries the driver's answer. |
| R9 | iODBC | `SQLPutData(NULL, 0)` is refused with HY009. The ODBC reference allows a NULL pointer with length 0. |
| R10 | iODBC | `SQLDescribeCol` on column 0 is refused with S1002 (07009) while bookmarks are on (live). |
| R11 | unixODBC | Static-cursor `SQL_ATTR_ROW_NUMBER` after `SQL_NO_DATA`, or after a failed fetch before any row, raises 24000 from the manager. The driver reports 0 (P1). *Resolved by the issue #120 product change: the driver now raises 24000/HY109 itself. The 0-vs-24000 clause is retired; the failed-fetch interception clause remains, because unixODBC still answers 24000 for the driver's HY109, and is retained as `RowNumberUnfetchableIntercepted`.* |
| R12 | iODBC | A second `SQLFetch` after `SQL_NO_DATA` on a Redshift catalog result is refused with HY010 (live). The ODBC state table allows it and the driver returns `SQL_NO_DATA`. It didn't reproduce on the PostgreSQL fixture. |
| R13 | iODBC | `SQLFetchScroll` with an unknown orientation raises HY092. The ODBC 3 state is HY106, which is what the driver returns. |
| R14 | iODBC | Every connect re-applies the login timeout stored before the driver was first loaded, over a later value and the keyword. |
| R15 | unixODBC | A login timeout the driver refused while connected is replayed to the driver before the next connect. The driver then ignores an invalid `LOGINTIMEOUT` keyword, as it does for any application value. |
| R16 | unixODBC | `SQLCancelHandle` on an idle statement with an open cursor closes the cursor (the ODBC 2.x meaning): the next `SQLFetch` raises HY010. |
| R17 | iODBC | ANSI `SQLDescribeCol` reports a non-ASCII column name's length in characters, not UTF-8 bytes (live and fixture: 2 for a 5-byte name). |

Bucket L also applies in more places: ANSI statement text with UTF-8 identifiers or values. Where that text is incidental to the case, the fixture sends it through the wide API instead (`exec_utf8`, `utf8_direct`). Where ANSI input is the point of the case, the case asserts the widened result and stops.

## State at handoff

- **macOS:** all three builds pass the complete applicable inventory live (fifth run).
- **Repeated iODBC catalog fetch:** diagnosed as R12.
- **HYT00:** passed in the fourth and fifth runs. It depends on the link's throughput.
- **Still open:**
  - P1, a product question;
  - Windows verification: see [driver-manager-windows-handoff.md](driver-manager-windows-handoff.md);
  - Redshift `dm_*` through the Windows manager, which `windows_live_backend.py` doesn't support yet.
