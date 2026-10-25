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
| D1 | manager | | (masked) | 27 | On an unconnected handle the manager answers `SQL_ATTR_LOGIN_TIMEOUT` itself. It returns the value the application set, or its own default: unixODBC 15 (`SQL_LOGIN_TIMEOUT_DEFAULT`), iODBC 0 with 8 bytes written and length 0 (bucket J). A value the driver took from a `LOGINTIMEOUT=` keyword isn't visible after disconnect. The direct driver reports 30. This is live bucket R1. |
| D2 | manager | | (masked) | 1 | `NegativeTests` passes a connection string as `SQLConnect` ServerName. The manager reads it as a DSN name and refuses it: unixODBC HY090 (longer than `SQL_MAX_DSN_LENGTH`), iODBC IM002. This is live bucket R2. |
| D3 | manager | | (masked) | 1 | `SQLGetDiagRecW` through a manager reports the record's UTF-8 byte count as its length in characters. Probe: 62 from both managers, 56 direct. The text itself is complete. This is live bucket R3. |
| D4 | manager | | (masked) | 1 | `SQLErrorW` with a NULL buffer and length -1 consumes the record. unixODBC returns `SQL_SUCCESS`. iODBC returns `SQL_ERROR` as the driver does, but the record is still gone. This is live bucket R4. |
| P1 | product | | | 2 | Static-cursor `SQL_ATTR_ROW_NUMBER` after `SQL_NO_DATA`, or before any fetch: the driver returns `SQL_SUCCESS` with 0, and unixODBC raises 24000 itself. See [proposal P1](#p1-static-cursor-row_number-off-a-row). |
| O1 | test oracle | 1 | | 1 | `SuperResultsPreserveServerJsonText` expected the text `null` for `JSON_PARSE('null')`. The driver correctly returned `SQL_NULL_DATA`, because the server makes a top-level JSON null SQL NULL. See [O1](#o1-super-json-null). |

All 198 failures are assigned to a bucket. The case-by-case list is in the private evidence (`classified.json`).

## Fixes made

| bucket | change | verified offline |
|---|---|---|
| W1 | The inventory adds `ODBCPP_TEST_DSN` to the two suites' profile. The full connection string is a valid ServerName for the in-process driver. | The case passes the TLS check and fails only at connect, against `example.invalid`. |
| W2 | `redshift_fixture_factory.build_profile_connection` emits `DRIVER=<path>` bare and refuses a driver path containing `;`, `{`, `}` or `=`. In `it_redshift_real`, the seven duplicated serializer loops are now one `connection_text()`, which leaves `DRIVER` bare through iODBC (live bucket R5, `BracedDriverValueLoadedVerbatim`). | iODBC and unixODBC both load the driver from the bare path and reach the network. Through iODBC, a path-loaded driver round-trips wide text as UCS4 with no registration. |
| B1 | The inventory records both cases through a manager as `MANAGER_BUILD_BOUNDARY`. | The inventory now has 1,495 cases and 104 boundaries. |
| T1 | The oracle has 20 rows in DATA_TYPE order, with an `object_type` flag (no literal syntax, no create parameters). | Compiles. The PostgreSQL rows are unchanged, and `it_metadata_real` passes on PostgreSQL direct and through both managers. |
| T2 | The cursor-close step moves into the PostgreSQL branch. | As T1. |
| T3 | Uses `untouched_row_count(93)`. | As T1. |
| O1 | Column 4 expects `SQL_NULL_DATA`. | Server check (below). |
| D1–D4 | New entries R1–R4 in `tests/test_dm_expectations.h`. `RedshiftLoginTimeoutOptionsRealTest` records the login timeout the application sets on an unconnected handle (fixture forwards) and expects the manager's answer while disconnected. D4 asserts the manager's result and the drained record, then stops the case (`dm_case_stopped_by`). | Expected values come from the probes and the unixODBC live result. The iODBC rows need the re-run. |

## Needs a decision

### P1: static-cursor ROW_NUMBER off a row

- **Spec:** `SQLGetStmtAttr` says two different things here:
  - The attribute description says: "If the number of the current row cannot be determined or there is no current row, the driver returns 0."
  - The diagnostics table lists 24000 when the attribute is `SQL_ATTR_ROW_NUMBER` and the cursor is not open, or is positioned before the start or after the end of the result set.
- **Driver:** The driver follows the diagnostics table for forward-only cursors (`MetadataIntegrationTest.ReportsCurrentRowNumberOnlyWhilePositioned`). For static cursors it follows the attribute description, through a deliberate carve-out added with static scrolling (`8ee2cfc`, `odbc/odbc_handles.cpp` `SQL_ATTR_ROW_NUMBER`). Unit tests in `tests/unit/test_backend_contract.cpp` assert the 0, and so does `RedshiftRecentFeaturesRealTest`.
- **unixODBC:** It applies the diagnostics table itself and returns 24000 after `SQL_NO_DATA`, before the call reaches the driver.
- **Recommendation:** Raise 24000 for static cursors too. That gives one rule for both cursor types, matches the diagnostics table and matches what an application already sees through unixODBC. This is a product change:
  - the static-cursor unit tests change first, as failing-before evidence;
  - the carve-out goes;
  - the two live `row_number(…,0)` checks change to 24000.
- **Alternative:** Keep 0, and record the unixODBC behaviour as a manager deviation instead.
- **Waiting for your decision:** nothing is changed yet.

### O1: SUPER JSON null (resolved: test oracle)

- **Server check, 2026-10-25:** one read-only `psql` query on the cluster over verified TLS (`sslmode=verify-full`). `JSON_PARSE('null') IS NULL` is true. `JSON_SERIALIZE` of it is the 4 bytes `null`, and `JSON_TYPEOF` is `null`.
- **Driver:** returned `SQL_NULL_DATA`, which is correct. The test's `text_cell` returns an empty string for a NULL cell, and that was the `""` in the failure.
- **Fix:** The oracle now expects `SQL_NULL_DATA` for column 4. `docs/redshift-special-types.md` is corrected. A nested JSON null stays in the text, which the array column already checks.

## Next run

Same three builds after the fixes, against the new inventory:
- 1,495 cases, with the B1 boundaries excluded.
- Expected to fail: P1 (2 unixODBC cases) until it is decided.
- iODBC `it_redshift_real` runs for the first time.
