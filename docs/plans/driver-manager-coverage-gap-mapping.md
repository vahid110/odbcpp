# Driver Manager coverage: gap mapping and implementation sequence

Follow-up to PR #98, tracked as issue #118. This is the analysis deliverable the
issue asks for first: a case/profile mapping of existing coverage against genuine
gaps, plus a bounded implementation sequence. It changes no product or test
behavior, records no new cloud operations, and expands no feature authority.

Baseline: `main` `2cc55803d3b97a0ee48c3e9b2c1ba0ddbeac0cdd` (post PR #98, #99,
#125, #126). Coverage facts below refer to that head; #117 evidence is cited as
in-flight and is not restated as completed.

## 1. What exists today

### 1.1 Direct layer (diagnostic baseline)

- 3,132 GoogleTest case definitions are classified in
  `docs/plans/driver-manager-test-inventory.csv` (generator
  `tools/development/dm_test_inventory.py`): 302 public-only, 101 fake-backend,
  871 needs-internals, 1,858 no-odbc-entry.
- The direct layer remains the diagnostic baseline and is not replaced by any
  manager run.

### 1.2 Manager layer: profiles actually executed

| Platform | Manager (observed identity) | App/driver SQLWCHAR | Executed evidence |
|---|---|---|---|
| Windows x64 | odbc32, `SQL_DM_VER=03.81.26100.0000` | 2/2 | dm-offline 4 entries / 8 cases at PR #98 head `7277f4c`; seven shared suites through the manager: 373 cases collected, 346 PASS / 27 FAIL, all classified W1–W10 in `tests/test_dm_expectations.h` (`docs/plans/driver-manager-windows-results.md`). PR #126 added manager host checks and `dm_it_*` image execution through the Windows DM. |
| Windows x64 | as above | 2/2 | #117 in flight (separate owner): 487 prepared DM live cases / 53 exclusions, fresh 137 offline PASS. The owner reported a network timeout before CTest on 2026-10-28: its current public IP no longer matches the pilot SG rule. This is a blocked planned run, not active execution or a pass; see [the owner receipt](https://github.com/vahid110/odbcpp/issues/117). |
| Ubuntu | unixODBC 2.3.14 (qualified receipt) | 2/2 | dm-offline CI at `7277f4c`; complete native/live checkpoint through unixODBC pending (#117). |
| macOS | iODBC 3.52.16 | 4/4 | dm-offline CI; first live run `dm-live-macos-9c79c86-a`: 487 cases collected, 338 PASS / 149 FAIL (mostly W2-masked braced `DRIVER=`); the fifth checkpoint at `4010fe8` then passed all 485 iODBC cases. Combined historical checkpoint: 1,495 PASS / 0 FAIL / 0 SKIP (524 direct, 485 iODBC, 486 unixODBC), with 36 manager early stops and 104 unrun boundary instances explicitly retained in `driver-manager-live-macos.md`. Current-source #117 qualification remains separate and pending. |
| macOS | unixODBC 2.3.14 (qualified receipt) | 2/2 | First live run: 488 cases collected, 451 PASS / 37 FAIL; deviation buckets D1–D4 and R1–R17 recorded in `tests/test_dm_expectations.h`; the fifth checkpoint at `4010fe8` passed all 486 unixODBC cases. These historical passes do not establish current-source #117 acceptance. |

### 1.3 Manager-only images (never link `odbcpp_core`)

- `test_dm_identity` (5 cases: 4 no-odbc-entry, 1 public-only), the `it_driver_manager`
  smoke program, `it_driver_manager_unicode` (9 cases), `it_redshift_powerbi_relationship`
  (2), `it_redshift_package_live` (3), `it_redshift_package_import_live` (3),
  `it_redshift_official_import_live` (3, Darwin iODBC 4/4 only), `it_crypto_profile_live` (3),
  `it_redshift_max_length_reference` (1).
- Shared-suite manager targets (`dm_it_*`, compiled from the direct sources with
  `ODBCPP_TEST_VIA_DRIVER_MANAGER=1`): `it_prepared_statements_real`,
  `it_bind_col_real`, `it_native_sql`, `it_handle_lifecycle`, `it_diagnostics`,
  `it_get_info`, `it_metadata_real`, plus `it_redshift_real` in Redshift builds.
  Registered as `dm-integration` when `ODBCPP_DM_INTEGRATION_TESTS=ON`.

## 2. SQLWCHAR width matrix

Width mechanisms, verified against the local headers and `odbc/odbc_types.h`:

- Windows: `SQLWCHAR` is `unsigned short` (2 bytes) in the native ODBC headers.
- unixODBC headers: `WCHAR` is `unsigned short` unless `SQL_WCHART_CONVERT` is
  defined (then `wchar_t`). Ordinary unixODBC builds are therefore 2-byte.
- iODBC headers on macOS/Linux: `SQLWCHAR` is `wchar_t` (4 bytes) in C++ builds.
- The driver's width is header-determined; `odbc_types.h` static-asserts against
  `ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE`. `ODBCPP_EXPECT_DM_SQLWCHAR_SIZE` is the
  application-side ABI check for manager-linked tests.
- iODBC negotiates app/driver widths at the driver boundary via
  `IODBC_ATTR_APP_WCHAR_TYPE` / `IODBC_ATTR_DRIVER_UNICODE_TYPE`
  (`odbc/odbc_types.h`). Its headers translate wide string arguments, but not
  bound `SQL_C_WCHAR` buffers, across ABI widths (comment in `odbc_types.h`).

Qualified pairs today: Windows 2/2, Ubuntu unixODBC 2/2, macOS unixODBC 2/2,
macOS iODBC 4/4. **No recorded mixed-pair (2/4 or 4/2) qualification results
exist.** A mixed build path does exist but has no recorded execution: the
`iodbc-unicode` workflow_dispatch job in `.github/workflows/ci.yml` builds the
driver against either unixodbc (2-byte) or libiodbc (4-byte) headers while the
manager-linked tests compile against libiodbc headers at 4 bytes, registers the
driver via `odbcinst.ini` `DriverUnicodeType`, and runs against a disposable
PostgreSQL server. It is dispatch-only, its results are not recorded in any
plan document, and it never builds a 2-byte application. The mixed-width guard
`kManagerConvertsNonBmpCharacterCounts` in `it_driver_manager_unicode.cpp` is
written for that driver-2/app-4 pairing and has never produced recorded results.
The reverse pairing — 4-byte driver under a 2-byte application — has no
representation on macOS/Linux: iODBC headers outside WIN32 are 4-byte only and
unixODBC requires a matched driver ABI, so that boundary must be recorded as
explicitly unsupported rather than qualified.

## 3. ODBC 2 compatibility surface

Current state:

- The driver accepts `SQL_OV_ODBC2`, `SQL_OV_ODBC3` and (where declared)
  `SQL_OV_ODBC3_80` for `SQL_ATTR_ODBC_VERSION` (`odbc/odbc_api.cpp`); a direct
  set/get matrix test exists (`EnvironmentVersionGetSetMatrix` in
  `tests/unit/test_attribute_apis.cpp`).
- Legacy diagnostics `SQLError`/`SQLErrorW` are implemented and exported
  (`tests/test_driver_exports.h`); tested direct and through managers
  (`LegacySqlErrorWDrainsRecordsThenReturnsNoData`, live buckets R4/D4).
- The Windows manager's `SQLGetFunctions` augmentation, including the ODBC2
  bitmap, is observed and asserted (bucket W3).
- Legacy ODBC 2 aliases — `SQLSetConnectOption`, `SQLGetConnectOption`,
  `SQLSetStmtOption`, `SQLGetStmtOption`, `SQLTransact`, `SQLAllocConnect`,
  `SQLAllocStmt`, `SQLFreeConnect`, `SQLParamOptions` — are **not exported by
  the driver**. For ODBC2-declared applications the managers remap these to the
  attribute/tran/handle entry points. Whether and where SQLSTATE class remapping
  (HYxxx → S1xxx) happens for ODBC2 applications has never been recorded.

No checked-in definition of the supported ODBC 2 surface exists, and no ODBC2
application profile has ever been executed through any manager.

## 4. Database-free / fake-backend state

- The dm-offline subset (identity, login-timeout direct + manager, fixture
  conversion — 8 GoogleTest cases) runs in ordinary PR CI on all three platforms
  without a database.
- Phase 2 of the plan (`ODBCPP_TEST_BACKEND` connection-string keyword making the
  in-process fake reachable through a manager) is **not implemented anywhere**
  (no occurrences in the tree). The 101 fake-backend cases (93 touching fake
  state) remain direct-only.
- The issue requires an assessment of remaining database-free gaps and allows
  only meaningful shared scenarios — explicitly no generic framework to inflate
  counts.

## 5. Windows generic Redshift checkpoint through the DM

- The targets exist and build on Windows Redshift: `dm_it_redshift_real`
  (manager-linked `it_redshift_real`, 157 case definitions) and the seven
  `dm_*` suites; the Windows PR job built them at `7277f4c`.
- #117 owns the current Windows DM live checkpoint (487 prepared cases, runner
  and registration exercised; the latest owner receipt reports a network timeout before CTest). #118 must not
  duplicate or replace it: the remaining item-1 work is the *distinct* generic
  Redshift inventory execution through the DM with retained manager identity,
  filling only runner gaps #117 leaves, and reporting it separately from
  direct-driver Redshift results.

## 6. Gap table

| ID | Item | Concrete missing evidence | First step | Blocked by |
|---|---|---|---|---|
| G5.1 | 1 | Windows DM execution of the applicable existing Redshift inventory, reported separately from direct-driver results | Record #117 outcome; fill runner gaps | #117 resource release |
| G2.1 | 2 | Recorded mixed-pair qualification; the driver-2/app-4 build path exists (dispatch-only `iodbc-unicode` CI job) but has no recorded results; app-2/driver-4 must be recorded as explicitly unsupported | Reproduce the existing path locally with recorded identities, then qualify | none |
| G2.2 | 2 | Mixed-width buffer/indicator/diagnostic boundary cases (truncation, non-BMP counts, unit-vs-byte lengths, bound-buffer limits) | Extend `it_driver_manager_unicode` and `test_dm_expectations.h` entries | G2.1 |
| G2.3 | 2 | Per-profile expectations for mixed pairs; the existing `kManagerConvertsNonBmpCharacterCounts` skip converted to an asserted deviation | Assert the known iODBC non-BMP count behavior with evidence | G2.2 |
| G3.1 | 3 | Checked-in definition of the supported ODBC 2 surface (native support vs manager remapping vs unsupported) | Write the surface definition doc | none |
| G3.2 | 3 | ODBC2-declared application profiles through Windows DM, unixODBC and iODBC: SQLSTATE remapping, `SQLGetFunctions(SQL_API_ODBC2_ALL_FUNCTIONS)`, legacy aliases arriving as attribute calls, `SQLError` semantics | Add ODBC2 profile cases; offline where pre/post-connect, loopback PostgreSQL for connected paths | G3.1 |
| G4.1 | 4 | Assessment of which manager-level behaviors current coverage (dm-offline + loopback suites) cannot exercise, and whether a test-only fake closes a real hole | Analysis only; propose specific scenarios or conclude none | none |
| G4.2 | 4 | Conditional: bounded, compile-gated, deterministic test backend | Only if G4.1 finds a real uncovered scenario | G4.1 |

## 7. Bounded implementation sequence

Each unit is independently reviewable and preserves valid assertions. Units
A–F use local source, offline tests or a disposable loopback database and need
no cloud admission. Unit G requires separately coordinated Redshift live
authority and must reuse or follow #117 without duplicating its resource ownership.
Platform rule from `AGENTS.md`: every product PR carries
one actual macOS, Ubuntu and Windows build; full cross-platform tests stay
checkpoint-scope.

- **Unit A (this document):** case/profile mapping and sequence. Stop condition:
  independent review of the facts against main `2cc5580`.
- **Unit B:** ODBC 2 surface definition + driver-side qualification (G3.1,
  driver half of G3.2 where no manager is needed: version matrix, `SQLError`,
  recorded remapping behavior). Source + existing offline tests only.
- **Unit C:** ODBC2 application profiles through the three managers (G3.2).
  Requires manager identity receipts (unixODBC receipt, observed Windows/iODBC
  versions). Offline where possible; loopback PostgreSQL for connected paths.
- **Unit D:** mixed-width qualification (G2.1/G2.2/G2.3): activate the existing
  `iodbc-unicode` build/registration path locally on macOS — 2-byte driver
  (unixodbc headers) registered under the 4-byte iODBC manager — record actual
  manager/version/width identities, run the offline subset and the connected
  Unicode cases against a disposable local PostgreSQL, convert the non-BMP
  count skip into an asserted deviation, and record app-2/driver-4 as an
  explicitly unsupported boundary.
- **Unit F:** fake-backend gap assessment (G4.1); analysis only.
- **Unit G:** Windows generic Redshift DM checkpoint (G5.1), sequenced after
  #117 closes or releases its resources; reuses #117's validated runner and
  registration, and reports DM results separately from direct-driver results.

Recommended order: Units B/C and D are independent local source/test lanes;
F can run anytime as analysis; G waits for #117. No pooling work (#101/#102) is
in scope.

## 8. Boundaries

- Does not re-do, claim or block #117; does not claim full live certification
  for PR #98; starts no cloud operations; enables no permissive expectations for
  unknown managers; adds no generic test framework.
- Preserves the accepted shared direct/manager scenarios and every explicit
  profile-specific expectation currently in `tests/test_dm_expectations.h`.
