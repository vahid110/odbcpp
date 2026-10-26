# Ordinary Driver Manager offline coverage

Ordinary PR CI builds the existing full PostgreSQL graph on Windows native ODBC
2/2, Ubuntu unixODBC 2/2 and macOS iODBC 4/4. It then executes the `dm-offline`
CTest label. This adds no database connection, registry fixture, installed driver,
cloud operation or permission flow. Existing macOS connected tests keep their
separate configuration and execution.

## Cases and limits

| CTest entry | GoogleTest bodies | What actually executes |
| --- | --- | --- |
| `dm_offline_identity` | Five `DriverManagerIdentity` cases | Qualification predicates, byte comparison, actual manager ENV/DBC/version observation and clean ENV diagnostic read |
| `dm_offline_direct_login_timeout` | `ConnectionAttributeIntegrationTest.LoginTimeoutOutputWidth` | Direct public API ENV/DBC allocation, timeout width/canary and handle cleanup |
| `dm_offline_manager_login_timeout` | The same source and case | Actual manager API calls before any driver connection; preserves exact known-profile width expectations |
| `dm_offline_unicode_conversion` | `DriverManagerUnicodeHarnessTest.OverrideTextDecodesCompleteScalarsIntoManagerUnits` | Fixture conversion unit coverage, including complete scalars; this case does not call the manager |

The two login-timeout entries compile the same existing test source; the manager
image links the manager instead of `odbcpp_core`. They compare pre-connect API
behavior, not database execution. The complete shared direct/manager suites and
full native/live checkpoints remain required for their applicable acceptance.

The identity case reports actual `SQL_DM_VER`, loaded API provider, pointer width,
application SQLWCHAR width, configured driver SQLWCHAR width and observed ODBC
application environment. It records only fixed set/unset configuration provenance,
never environment values or connection strings. The evidence validator hashes the
observed provider file. A configured driver width is not a runtime driver ABI
measurement: this subset never loads the driver through a connection.

Observed identity is separate from permission to apply a known deviation. The
existing version/profile rules and qualified unixODBC path/hash/byte comparison
remain unchanged. A newly observed provider does not acquire a waiver.

## Running and retaining evidence

Build the existing default ALL graph for the selected product/profile first. With
that source and its images frozen, run (PowerShell uses `$LASTEXITCODE` instead of
`$?` and Windows multi-config builds add `-C Release`):

```sh
ctest --test-dir BUILD -L '^dm-offline$' --no-tests=error --output-on-failure --output-junit dm-offline/ctest.xml
result=$?
printf '%s\n' "$result" > BUILD/dm-offline/ctest-exit.txt
python3 -B tools/development/check_dm_offline_evidence.py --build BUILD --ctest-exit-file BUILD/dm-offline/ctest-exit.txt
```

For Windows, add `--config Release` to the validator. CI uses fresh build folders.
Before a local rerun, preserve the preceding `dm-offline` directory outside the
build tree; the validator refuses to overwrite its previous evidence report.
CTest controls processes, selection and 30-second per-entry timeouts. Empty or
skipped GoogleTest execution fails CTest, and the verifier additionally requires
exactly four CTest entries and eight completed GoogleTest cases, no missing,
duplicate, failed, skipped or disabled cases, and an actual CTest exit of zero.

The retained directory contains four GoogleTest XML files, CTest JUnit, actual exit
and source/config/image/provider bindings. CI also retains CMakeCache and CTest
logs. These are current-file bindings: callers must keep source/images frozen;
they are not a historical execution attestation or complete platform acceptance.
Original failures must be archived before any repair/rerun.

## Recommendation disposition

Shared execution sources, Unicode/buffer/indicator boundaries, diagnostics and
connected lifecycle scenarios already exist in the Phase 1/3 suites; this subset
does not duplicate or weaken them. Windows/WSL PostgreSQL comparison evidence is
recorded in [Windows results](driver-manager-windows-results.md), with macOS live
observations in [the macOS report](driver-manager-live-macos.md). Those reports
retain fixture defects, manager behavior and unresolved product questions as
separate classifications.

Remaining independently scoped work includes a complete ODBC2/pooling matrix,
full mixed driver2/application4 qualification, fake-backend scenarios through a
manager, and Windows generic Redshift DM checkpoint support. None is established
by this offline subset. They remain explicit follow-ups to the
[Driver Manager plan](driver-manager-test-layer.md), alongside full native/live
qualification and unchanged product assertions.
