# Driver-manager test layer: Windows verification

This continues [the Windows handoff](driver-manager-windows-handoff.md) for
PR #98. The initial Windows source is `442eecc`, the merge of handoff `f74c2f9`
with main `3dc4ee7`. Existing migration and Power BI evidence is retained.

## Profile and original collection

Both POSTGRESQL and REDSHIFT default ALL builds passed with MSVC 19.44 x64,
Release, OpenSSL 3.6.2 BUNDLED_SHARED, driver/manager SQLWCHAR 2/2 and
`ODBCPP_DM_INTEGRATION_TESTS=ON`. This includes both `it_redshift_real` and
`dm_it_redshift_real`, previously uncompiled by the Windows PR job.
Both unit CTest collections passed 130/130. The seven database suites used a
new loopback PostgreSQL 18.6 fixture, a per-user DSN and the PR-built DLL.

The Windows manager reports `SQL_DM_VER=03.81.26100.0000`, under SQL_OV_ODBC3.
Its System32 `odbc32.dll` SHA256 is
`8a120c65049e31b26cf1608e9d9e3253f386d29539a508b345e30390542b88d7`.
The observed active ANSI code page is 1252. Results apply to this profile;
other manager versions still require the direct expectations.

| Initial collection | Cases | Pass | Fail | Skip |
|---|---:|---:|---:|---:|
| Seven direct suites | 386 | 386 | 0 | 0 |
| Seven Windows manager suites | 373 | 346 | 27 | 0 |

All suites were collected before repair. The diagnostic DSN was retired and
the isolated server stopped; historical PostgreSQL data and installed Redshift
driver were unchanged. Private raw evidence is under
`%LOCALAPPDATA%/odbcpp-migration/dm-windows098/pg-collection01/`: CTest and
GoogleTest XML, per-suite driver API traces, console logs, source/image hashes,
actual exits and registry cleanup. Diagnostic probes are in its parent folder.
The original failures remain failures, including two caught SEH exceptions.

## Classification and repairs

Six failures were Unicode fixture defects: bind-col binary/wide retrieval,
native-SQL Unicode escape, and three metadata catalog cases. The fixture sent
UTF-8 bytes to Windows ANSI APIs. The manager interpreted those bytes as ACP1252
and called the driver's wide APIs. A controlled comparison on the same DLL
proved raw ANSI `AéZ` became six bytes, while properly encoded ANSI and wide
input both returned the original four UTF-8 bytes. The supplementary character
likewise round-tripped through wide input. This is not a driver conversion defect.

`tests/test_text.h` now provides strict UTF-8-to-application-ANSI fixture input
conversion on Windows manager builds, refusing replacement/best-fit characters.
ANSI cases retain their ANSI calls. The wide-output supplementary-character
case uses wide fixture setup. Direct/POSIX input behavior and original Unicode
output assertions remain unchanged. `probe-unicode098-02.json` and its trace
retain the comparison, including the native-SQL 42000 versus successful escape.

The remaining 21 failing cases fall into the following measured manager or
fixture behaviors. Entries in `tests/test_dm_expectations.h` use the exact
manager version above; they do not relax the direct-driver expectations.

| Bucket | Observation and proof | Treatment |
|---|---|---|
| W1 | Manager-owned SQL_ODBC_VER is `03.80.0000`, with wide length 20 bytes. | Assert the exact string and length. |
| W2 | Truncated/null ANSI SQLGetInfo and SQLColAttribute retain the wide required byte length. Driver trace shows SQLGetInfoW/SQLColAttributeW returning those lengths; full-buffer ANSI column attributes return the narrow length. | Assert exact lengths, truncation state, content and sentinels. |
| W3 | Manager caches/augments SQLGetFunctions. Full arrays in `diagnostic_probe098.json` show its additional IDs; ODBC2 bitmap alone omits 24 and 73. A NULL output is accepted and clears diagnostics. | Keep the complete 4000-bit/100-entry and single-call assertions with exact expectations. |
| W4 | ANSI/wide SQLDriverConnect adds a trailing semicolon, increasing full required lengths by one. | Compute the normalized expected string, retaining content, terminator, warning and truncation assertions. |
| W5 | Successful SQLDisconnect frees manager statement/descriptor handles. Two fixture cases reused those freed pointers and caught SEH before any subsequent driver entry. | Retain direct tombstone assertions; record the unreachable manager probe, clear stale handles, and verify reconnect plus fresh child allocation. |
| W6 | SQLBindCol column 0 with SQL_C_CHAR is refused with manager-origin 07006, before driver entry. | Assert 07006 and manager origin; retain direct 07009 and its message check. |
| W7 | SQLGetData before execution, and SQLExecute after failed prepare, are refused by the manager with HY010. | Apply the existing state-table expectations; preserve the explicit unreachable-case XML property. |
| W8 | After failed SQLMoreResults the manager still has an open cursor and rejects another execute with 24000. | Assert that refusal, close the cursor, then retain the recovery assertions. |
| W9 | After the driver's internal timeout close, the manager refuses reconnect with 08002. | Assert the known boundary and record it; direct reconnect/metadata assertions stay intact. |
| W10 | SQLColAttribute rejects a negative text-buffer length with HY090 even for numeric fields; output sentinels remain unchanged. | Assert refusal and sentinels, then repeat with valid ignored length and retain all numeric-value checks. |

`probe-states-dm.json` and `probe-states-direct02.json`, with corresponding API
traces, compare W2/W7/W8/W9/W10 against the same driver directly. The first direct
probe could not load the crypto dependency; its original error is retained.
The successful retry explicitly added the qualified OpenSSL directory to that
process's DLL search. The manager timeout probe's final handle cleanup calls
were refused after its internal-close state mismatch; its process exited and
the isolated PostgreSQL server was stopped. This is not recorded as successful
API cleanup. Other probe readers and the diagnostic DSN were retired.

Microsoft documents the [bookmark-type 07006 validation](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindcol-function)
and [statement/descriptor release at disconnect](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldisconnect-function).
These support W6 and the W5 fixture correction; actual traces establish which
component handled each measured call.

## Ownership and remaining qualification

Performance follow-up requested by the human: the cold PostgreSQL and Redshift
CMake configurations took 360.9 and 367.8 seconds, respectively, with substantial
CPU use before compilation. An isolated reproduction of the suspected dependency
security-patch regex/configure operations completed in about 0.3 seconds. The
actual cause is not established; retain the configure logs and investigate it
separately rather than attributing the delay to that patch.

The changes are test infrastructure only. `tests/test_text.h` owns application
fixture encoding, reusing the strict conversion pattern from
`tests/integration/it_driver_manager_unicode.cpp`; it depends on the existing
test Unicode helper and Windows conversion APIs. `tests/test_dm_expectations.h`
owns measured manager expectations, depending only on ODBC declarations and
GoogleTest. Owning integration fixtures keep scenario setup and assertions.
No SDK/backend/adapter contract or product code changed in this repair.

The literal `python -m unittest discover tools/development` command is not a
portable all-host gate: this directory mixes POSIX controls (`fcntl`, clock/file
primitives) with Windows controls (`winreg`, Windows paths and ctypes). Both
original full discovery failures are retained. Every failing module passed
unchanged on its owning host: 166 POSIX cases and 99 Windows cases, then the
remaining native image-dependent smoke passed with the freshly built image
(five tests, including intentional CTest failure/continuation and cleanup).

At repair `5665f6d`, both Windows default ALL rebuilds passed and both unit
collections passed 130/130. The complete PostgreSQL rerun passed all 386 direct
and 373 manager cases, with no skips. All 14 executable hashes match their
receipts; the driver DLL stayed unchanged, the owned DSN was retired and the
isolated PostgreSQL server stopped. Independent result review accepted these
local results. All 15 manager executable/project pairs import ODBC32 and link
without `odbcpp_core` or an ODBCPP DLL dependency.

The matching Linux/GCC 15 unixODBC 2.3.14 default ALL build and 129 unit tests
passed. Its full PostgreSQL collection exposed an additional W5 fixture defect:
the lifecycle suite crashed in `libodbc.so.2` while using a child handle after
successful disconnect. All 13 lifecycle cases passed in separate processes,
but GDB reproduced the crash in the complete suite. This allocator-sensitive
result is not proof that freed pointers are valid. The trace, original CTest
crash, isolated-case collection and debugger stack are retained in private
`wsl-pg-collection02`, `wsl-lifecycle-isolated02` and `wsl-lifecycle-gdb03` evidence.
At `54ebedf`, the complete follow-up passed on both Windows and WSL: each
collected all 386 direct and 373 manager cases with no failures or skips.
All three default ALL rebuilds passed, and independent review verified every
suite image and both driver images against the `pg-collection03` and
`wsl-pg-collection03` receipts. Both owned DSNs were retired and the isolated
server stopped. The unit results at `5665f6d` apply to `54ebedf`: that delta
changes only the W5 table entry and this document, with no unit-test consumer.

The native direct Redshift checkpoint remains outstanding; no live pass is
inferred from compilation or PostgreSQL execution. Windows Redshift DM live is
still the unsupported follow-up described in the handoff. Remaining cross-platform
regression for the repaired fixtures is required before final acceptance.

## Review corrections after `54ebedf`

The `54ebedf` evidence remains immutable. The successor corrects three issues
identified by the independent Mac review, with fresh local qualification pending:

- unixODBC's `03.52.0002.0003` string aliases multiple patch releases. Known
  expectations now require an explicit reviewed 2.3.14 library receipt, its
  configure-time SHA256 and canonical path, the matching loaded API provider's
  bytes, and the qualified ODBC 3, pointer and driver/manager SQLWCHAR profile.
  Without matching evidence, both table and null-buffer diagnostic selection
  retain direct expectations. Windows and iODBC retain their distinct version
  matches with explicit ABI/environment guards.
- An expected ANSI escape error no longer returns from the whole native-SQL
  case: independent wide translation and ASCII quote execution assertions run.
  Both stored binary length and wide translation length are checked before
  constructing a string or span over a fixed buffer.
- Driver-only tombstone probes never reuse a manager-owned child pointer after
  disconnect, including on an unqualified manager. Direct assertions and the
  positive reconnect/fresh-child checks remain.

`tests/test_dm_identity.h` owns the test qualification predicate and data types;
`tests/support/dm_identity.cpp` owns loaded-provider discovery and bounded byte
comparison. `cmake/DmQualifiedIdentity.cmake` validates the reviewed receipt and
creates the build-local manager snapshot through `DmQualifiedIdentity.h.in`;
[driver-manager-unixodbc-receipt.md](driver-manager-unixodbc-receipt.md) shows
how to produce the receipt. `cmake/Tests.cmake` connects that helper to manager test targets. These are test
infrastructure, not reusable product/SDK services: their purpose is selecting
measured test expectations. Permitted dependencies are standard C++, POSIX
loader APIs, the selected ODBC manager and GoogleTest; no driver core or crypto
dependency is added. The executable and reference snapshot are trusted build
inputs, not protection against a hostile writer modifying a running build.

`tests/driver_manager/test_dm_identity.cpp` adds five offline GoogleTests for
positive and negative identity/profile selection, actual provider selection and
byte comparison. Nine offline CMake receipt checks passed, including mismatched
hash, version and profile rejection and missing-receipt default behavior. The
generated Phase 0 CSV/counts now match the current 3,132 source case definitions;
these lexical counts do not substitute for executable discovery or live results.
