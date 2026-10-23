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

### Verified so far
- **Full offline presets, 0 failures:**
  - Windows ODBC 2/2: 122/122 CTest
  - WSL unixODBC 2/2: 120/120
  - WSL iODBC 4/4: 120/120
  - macOS iODBC 4/4 (REDSHIFT, `macos-iodbc-redshift-offline`, at `a1d9639`): 119 passed, 1 skipped, 0 failed of 120; GoogleTest 2388 cases, 0 failures, 2 skips (`test_odbc_trace_reload`: the macOS loader keeps the driver image resident). All 8 `NativeChainEvaluationTest` cases ran and passed.
  - macOS unixODBC 2/2 (REDSHIFT, `unixodbc-redshift-offline`, at `a1d9639`): the same counts and skips.
- **Live, Redshift Serverless `odbcpp-redshift-pilot` (eu-north-1), verified TLS:**
  - 10 cases each on Windows, unixODBC and iODBC: 10/10 at `66c777e`.
  - macOS iODBC 4/4 and macOS unixODBC 2/2, at `a1d9639`: 10/10 each. Warm-up: ready after 1 attempt in 328-649 ms, so no cold start.
  - At `b1a0ec4`, Windows first passed 8/10, and the 2 failures passed on a re-run.
- **PR CI on macOS** (GitHub-hosted `macos-latest`, `primary.yml` job `macos-iodbc-ucs4`), passed at `66c777e`:
  - Configuration: product **POSTGRESQL** with iODBC and 4-byte SQLWCHAR.
  - Results: unit CTest 121/121, then PostgreSQL integration 12/12 against a disposable local server, including `it_driver_manager` through a registered iODBC DSN.
- **Not run anywhere yet:**
  - installed driver-manager paths on Windows and unixODBC;
  - provisioned Redshift;
  - the full Linux and Windows suites in CI. The PR Windows and Ubuntu jobs build only and run no tests.

### Open owner decisions
- SQLColAttributeW zero-capacity non-NULL buffer. The existing assertion at `tests/unit/test_backend_contract.cpp` (from #63) requires `SQL_SUCCESS`; the Microsoft truncation rule points to `01004`.
- SQLGetDescRec(W) zero-capacity, left unchanged because A and W currently behave the same.
- F1's second route: the driver echoes ANSI input bytes (for example a Latin-1 DSN in `DSN '...' not found`) into its own messages without `replace_invalid_utf8`. `SQLGetDiagRecW` then returns `SQL_ERROR` with SQLSTATE and native error written. `AnsiDsnEchoKeepsRawBytesAndWideDiagnosticsKeepStateAndNative` asserts today's behavior. Fixing it means deciding the driver's ANSI encoding policy, together with the unixODBC ANSI-connect finding in the PR.

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
2. **Installed driver-manager paths on Windows and unixODBC.** macOS CI already runs `it_driver_manager` through iODBC with PostgreSQL. Live Redshift runs so far link the driver directly. The driver manager changes Unicode conversion, how errors reach the application, and how connect works.
3. **Provisioned Redshift and live PostgreSQL.** This round ran on Serverless only.
4. **Independent review of PR #96,** plus the owner decision on SQLColAttributeW zero-capacity.

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
