# Handoff: Unicode follow-up 014 (PR #96)

This file is the checked-in continuation point for another machine, such as a
macOS instance. The fuller evidence ledger stays private on the Windows host
(`odbcpp-audits/windows-unicode014-implementation`, which is not in git).
No credentials or connection strings appear here.

## State
- Branch: `codex/unicode-contract-followup014`
- Base: `b5063e1fe0ae1e0a3fc3bf96fe88824411889a2f`
- PR: https://github.com/vahid110/odbcpp/pull/96 (open, not merged; independent review required)
- The PR description is the authoritative change and evidence summary: F1, F2, F3, SQLGetDescRecW and the live fixture warm-up.

### Commits
| Commit | Content |
|---|---|
| `2e8424a` | Regression tests only (before snapshot) |
| `1dfcde3` | F1/F2/F3 implementation |
| `73fbecd` | SQLGetDescRecW full names beyond `SQLSMALLINT` |
| `4f7dce2` | Keep the asserted SQLColAttributeW zero-capacity policy |
| `b1a0ec4` | Four live Redshift GoogleTests |
| `66c777e` | Opt-in live fixture warm-up (`ODBCPP_REDSHIFT_WARMUP_SECONDS`) |
| `cbb6d8e`..`9fe775b` | Driver-manager GoogleTests (`it_driver_manager_unicode`) |
| `a1d9639` | Offline audit tests: wide `SQL_USER_NAME`, combining-sequence chunks, ANSI DSN echo |
| `4d523d3` | Driver-manager test: buffer sized from the connection string, plus the ANSI Latin-1 keyword case |
| `31e5410` | Owner decisions: U+FFFD in posted diagnostics, and SQLColAttributeW zero-capacity 01004 |
| `508ace3` | Independent review R1/R2 (test code only): Unicode decoding of driver-manager overrides; one deadline for every warm-up phase |
| `b56afb3` | Follow-up review, remaining R2 item (test code only): warm-up readiness judged after the attempt's cleanup |

### Verified so far
- **Full offline presets, 0 failures:**
  - Windows ODBC 2/2: 122/122 CTest
  - WSL unixODBC 2/2: 120/120
  - WSL iODBC 4/4: 120/120
  - macOS iODBC 4/4 (REDSHIFT, `macos-iodbc-redshift-offline`, at `a1d9639`): 119 passed, 1 skipped, 0 failed of 120; GoogleTest 2388 cases, 0 failures, 2 skips (`test_odbc_trace_reload`: the macOS loader keeps the driver image resident). All 8 `NativeChainEvaluationTest` cases ran and passed.
  - macOS unixODBC 2/2 (REDSHIFT, `unixodbc-redshift-offline`, at `a1d9639`): the same counts and skips.
  - Both macOS presets again at `31e5410`, after the owner decisions: CTest 119 passed, 1 skipped, 0 failed; GoogleTest 2389 cases, 0 failures, the same 2 skips.
- **Live, Redshift Serverless `odbcpp-redshift-pilot` (eu-north-1), verified TLS:**
  - 10 cases each on Windows, unixODBC and iODBC: 10/10 at `66c777e`.
  - macOS iODBC 4/4 and macOS unixODBC 2/2, at `a1d9639`: 10/10 each. Warm-up: ready after 1 attempt in 328-649 ms, so no cold start.
- **Live Redshift through a real driver manager (macOS, REDSHIFT driver registered in private ini files, at `87f3dd6`):** `it_driver_manager_unicode` passed 8/8 through unixODBC 2.3.14. Through iODBC 3.52.16, 6 passed and 2 were skipped as iODBC defects.
  - iODBC drops every driver-connect warning record (01S00 and 01004) and misreports truncated driver-connect output lengths. A probe confirmed both on macOS.
  - Through unixODBC, an ANSI Latin-1 unknown keyword keeps its 01S00 readable via ANSI and wide calls.
  - At `b1a0ec4`, Windows first passed 8/10, and the 2 failures passed on a re-run.
- **PR CI on macOS** (GitHub-hosted `macos-latest`, `primary.yml` job `macos-iodbc-ucs4`), passed at `66c777e`:
  - Configuration: product **POSTGRESQL** with iODBC and 4-byte SQLWCHAR.
  - Results: unit CTest 121/121, then PostgreSQL integration 12/12 against a disposable local server, including `it_driver_manager` through a registered iODBC DSN.
- **Full CI checkpoint** (`ci.yml` dispatched by hand, run 40024334082) at `2b53ff6`: every job green, including sanitizers, `windows-live` and both macOS `iodbc-unicode` variants. `windows-live` ran `it_driver_manager_unicode` 8/8 through the HKLM-registered PostgreSQL driver.
- **Windows and WSL at `aa949ec` (after the owner decisions), one Windows 11 host plus WSL Ubuntu:**
  - Full offline presets, 0 failures:
    - `windows-redshift-offline`: CTest 122/122; GoogleTest 2392 passed, 6 skipped (macOS-only trust anchors).
    - `unixodbc-redshift-offline` and `linux-iodbc-redshift-offline`: CTest 120/120 each; GoogleTest 2387 passed, 8 skipped (`NativeChainEvaluationTest`, macOS-only) on each.
    - The `a1d9639` and `31e5410` cases passed on all three, including `AnsiDsnEchoReplacesInvalidBytesSoWideDiagnosticsSucceed` (Windows registry branch), `EveryPostingPathReplacesInvalidUtf8SoWideReadsSucceed`, `SQLErrorW_MalformedPostedTextIsReplacedAndLegacyRecordsDrain`, `SQLGetDiagRec_MalformedPostedTextIsReplacedForWideAndAnsiReaders` and `ColumnAttributeNullCharacterPreservesValidationAndNumericDomains`.
  - `it_driver_manager_unicode` against a disposable PostgreSQL 18 through WSL managers: unixODBC 8/8; iODBC 6 passed, 2 skipped (the documented iODBC driver-connect defects).
  - Live Redshift Serverless through installed managers, REDSHIFT driver built at `aa949ec`:
    - Windows Driver Manager: 8/8. Without admin rights the PR-built driver can't be added to HKLM `ODBCINST.INI`, so a temporary per-user DSN (HKCU `ODBC.INI`, removed afterwards) names the driver DLL and the manager loads it from the registry. The HKLM `DRIVER={...}` lookup is covered by `windows-live` with PostgreSQL.
    - WSL unixODBC 2.3.14 (Ubuntu `libodbc2` 2.3.14-1, private ini registration): 8/8.
    - WSL iODBC 3.52.16 (private ini registration): 6 passed, 2 skipped (the same iODBC defects).
  - The same runs at `e1b7665` (before `31e5410`) had the same counts; the Windows offline GoogleTest total was 2391 and WSL 2386 each, one fewer because of the new `31e5410` test.
- **Windows Driver Manager behaviour:** it keeps driver-connect warnings. `AnsiLatin1KeywordWarningStateStaysReadable` passes, so the unknown-keyword 01S00 reaches the application through both `SQLGetDiagRec` and `SQLGetDiagRecW`, and `WideDriverConnectTruncatedOutputReportsFullLength` passes, so 01004 and the full output length arrive. The F1 ANSI-echo route therefore hides no SQLSTATE on Windows.
- **macOS at `508ace3` (independent-review revision R1/R2), the same macOS 15.3.2 arm64 host, Apple Clang:**
  - **Full offline presets, 0 failures:** `macos-iodbc-redshift-offline` and `unixodbc-redshift-offline`, each 121 CTest (120 passed, 1 skipped: `test_odbc_trace_reload`, macOS loader). GoogleTest 2395 cases with 2 skips. All 6 `RedshiftWarmUpTest` cases ran and passed.
  - **R1 before and after**, disposable PostgreSQL 17 with role `café`, `UID=café`, the same driver in every run:
    - iODBC before: 6 failed with 22018, 2 skipped. After: 7 passed, 2 skipped, both with the non-ASCII and the ASCII override.
    - unixODBC before: 7 failed with 22018, 1 passed. After: 9/9 with both.
  - **Live Redshift through the managers,** with `SSLCAFILE` under a directory named `café`: iODBC 7 passed, 2 skipped; unixODBC 9/9.
    - `ConnectionTest` and `UnicodeRoundTrip` with warm-up passed on both builds: ready after 1 attempt in 415-451 ms.
- **Windows and WSL for the review revisions:**
  - At `508ace3`, R1 before and after on WSL, with a disposable PostgreSQL 18.6, role `café` and `UID=café`, the same driver in every run:
    - unixODBC before: 7 of 8 failed with 22018. After: 9/9, with both the non-ASCII and the ASCII override.
    - iODBC before: 6 of 8 failed, 2 skipped. After: 7 passed, 2 skipped.
  - At `508ace3`, live through the Windows Driver Manager with `SSLCAFILE` under `café`: 9/9.
  - At exact head `b56afb3`, full offline presets, 0 failures:
    - `windows-redshift-offline`: CTest 123/123, GoogleTest 2401 passed, 6 skipped.
    - `unixodbc-redshift-offline` and `linux-iodbc-redshift-offline`: CTest 121/121 each, GoogleTest 2396 passed, 8 skipped (macOS-only) on each.
    - All 9 `RedshiftWarmUpTest` cases ran on each.
  - At `b56afb3`, a live Windows warm-up check: 2/2, ready after 1 attempt.
  - A mutation check on WSL: with the old verdict-before-cleanup restored in a scratch header, `ReadinessIsJudgedAfterCleanupAgainstTheOriginalDeadline` fails.
- **macOS at exact head `b56afb3`:**
  - Both REDSHIFT offline presets: 121 CTest (120 passed, 1 skipped: `test_odbc_trace_reload`); GoogleTest 2398 cases, 0 failures, 2 skips. All 9 `RedshiftWarmUpTest` cases ran.
  - The 10 direct live cases: 10/10 on iODBC 4/4 and on unixODBC 2/2, warm-up ready after 1 attempt each. This is the first run of the 10 cases after `31e5410`. Windows and WSL last ran them at `66c777e`.
- **PR checks at `b56afb3`:** all passed (`macos-iodbc-ucs4`, the PR Windows and Ubuntu builds, CodeQL).
- **Not run anywhere yet:**
  - provisioned Redshift;
  - the full Linux and Windows suites in PR CI. The PR Windows and Ubuntu jobs build only and run no tests; the full suites run only in the manually dispatched checkpoint.

### Owner decisions (decided 2026-10-24, implemented in `31e5410`)
- **F1, ANSI encoding policy:** every diagnostic posting path stores valid UTF-8, with each invalid byte replaced by U+FFFD. Wide diagnostic readers therefore always succeed, and ANSI readers receive the replacement as UTF-8 bytes. The statement diagnostic header is not a record and keeps the old unconvertible-text behaviour. `AnsiDsnEchoReplacesInvalidBytesSoWideDiagnosticsSucceed` and `EveryPostingPathReplacesInvalidUtf8SoWideReadsSucceed` assert it.
- **SQLColAttributeW zero capacity:** a non-NULL zero-capacity buffer with non-empty text returns 01004, like ANSI `SQLColAttribute` and `SQLDescribeColW`. The #63 assertion was updated.
- **Unchanged, because A and W already agree:** `SQLGetInfo(W)` and `SQLGetDescRec(W)` keep zero-capacity `SQL_SUCCESS`. Changing them would be a separate decision.

## Continuing on macOS
iODBC with 4-byte SQLWCHAR uses the presets `macos-iodbc-redshift` and `macos-iodbc-redshift-offline`.

```sh
git fetch origin && git checkout codex/unicode-contract-followup014
export ODBCPP_IODBC_ROOT=/path/to/iodbc        # include/ and lib/libiodbc.dylib
python3 tools/development/check_changed_gtest_control_flow.py \
  --base b5063e1fe0ae1e0a3fc3bf96fe88824411889a2f
cmake --preset macos-iodbc-redshift
cmake --build --preset macos-iodbc-redshift
ctest --preset macos-iodbc-redshift-offline --output-junit ctest-junit.xml
```

Keep the toolchain identity, the actual exits, the CTest JUnit and the GoogleTest XML together. The Windows offline run had 6 skips (macOS-only trust anchors), and those tests should run on macOS.

### Live cases on macOS
- **Run as the same 10 cases:**
  - `RedshiftRealTest.ConnectionTest`, `ErrorHandling`, `UnicodeRoundTrip`
  - `WideDiagnosticsCarryServerUnicodeErrorText`
  - `LegacySqlErrorWDrainsServerErrorThenNoData`
  - `WideZeroCapacityAndDescriptorUnicodeColumnNames`
  - `WideDriverConnectZeroCapacityOutputReportsTruncation`
  - `RedshiftUnicodeAliasMetadataRealTest.DirectAndPreparedUnicodeAliasNamesAndRecovery`
  - `RedshiftUnicodeChunkRealTest.PreparedWideChunksBoundTruncationAndNull`
  - `RedshiftDeferredInputRealTest.ChunkedUnicodeOwnsPiecesAndReturnsServerValue`
- **Run them one at a time:** one process per case with `--gtest_filter=<case>` and `--gtest_output=xml:...`. On Linux, the checked-in CTest live registrar (`tests/development/live_suite`) wraps this.
- **Environment:**
  - `ODBCPP_REDSHIFT_TEST_CONNECTION`: `SERVER`, `PORT=5439`, `DATABASE=odbcpp_pilot`, `UID`, `PWD`, `SSL=1`, `SSLCAFILE` (a CA bundle that includes the Amazon roots).
  - `ODBCPP_REDSHIFT_UNICODE_ALIAS_METADATA_ADMISSION=unicode-alias-metadata-v1`
  - `ODBCPP_REDSHIFT_UNICODE_CHUNK_ADMISSION=unicode-chunk-result-v1`
  - `ODBCPP_REDSHIFT_WARMUP_SECONDS=120` (optional)
- **Credentials:** temporary database credentials came from `aws redshift-serverless get-credentials --workgroup-name odbcpp-redshift-pilot --db-name odbcpp_pilot`. Keep them in private (0600) files only. CTest `LastTest.log` records test environments, so it must stay private.
- **Network:** the workgroup's security group must allow the macOS host's IP on port 5439.

## Gaps toward a production-grade driver (priority order)

**Release blockers**
1. **macOS REDSHIFT: done locally at `a1d9639`.** The offline suites (iODBC 4/4 and unixODBC 2/2) and the 10 live cases on both ran on one macOS 15.3.2 arm64 host. Still open: macOS REDSHIFT is not in PR CI, and in PR CI Linux and Windows build without running tests; their suites have run only on one local host.
2. **Installed driver-manager paths: done locally for Redshift on Windows, WSL unixODBC/iODBC and macOS unixODBC/iODBC** (see above). Still open:
   - the Windows run used a per-user DSN naming the PR-built DLL, not an HKLM-installed driver; an MSI-installed REDSHIFT driver has not run live;
   - live Redshift through a manager is not in CI (CI covers it with PostgreSQL only);
   - the iODBC 3.52.16 defects (dropped driver-connect warnings, misreported truncated lengths, unconverted non-BMP counts between mixed widths) are worked around in tests and not reported upstream.
3. **Provisioned Redshift and live PostgreSQL.** This round ran on Serverless only.
4. **Independent review of PR #96.** The owner decisions are made and implemented (`31e5410`).

**Correctness and robustness**

5. **Real client applications:** Power BI, Excel, Tableau, DBeaver, pyodbc, `isql`. They exercise W calls, catalog functions, zero-capacity length queries and legacy `SQLError` in ways the tests don't.
6. **Fuzzing:** the protocol parser (ErrorResponse/NoticeResponse, DataRow, RowDescription) and the connection-string parser. This round found invalid bytes passing straight through `decode_error_fields`.
7. **Sanitizers and soak tests:** ASan/UBSan beyond unit tests, extended to live and driver-manager runs, plus long connect/disconnect and statement cycles with memory and handle counts checked.
8. **Concurrency:** multiple threads on separate connections, and `SQLCancel` from another thread, under ThreadSanitizer.
9. **Network failure:** server restarts, dropped connections in the middle of results, TLS errors, and timeouts at every stage. The first-pass Windows `HYT00`/`08001` came from either a cold start or first-login user creation, and is still unexplained.

**Coverage already listed as debt**

10. **Authentication breadth:** IAM-authenticated SQL, token refresh and reconnect, federation and SSO.
11. **Other parity gaps:** SQLGetDescFieldW name paths, and the full Redshift parity inventory (`REDSHIFT_PARITY_INVENTORY.md`).
12. **Wider live coverage:** run the whole live suite on every profile, not just these 10 cases. Treat a missing endpoint as a failure.

**Release engineering**

13. **Release artifacts:** signed installers and packages, reproducible builds with attestation, and upgrade/uninstall tests.
14. **Performance baselines:** fetch throughput and large result sets compared with the AWS Redshift ODBC driver.

**Known live limits**
- Names over 32767 bytes can't be created on Redshift (identifiers max out at 127 bytes), and non-UTF-8 server text can't be produced with `client_encoding=UTF8`. Both are covered offline only.
- The warm-up hasn't been shown to rescue a real cold start.
