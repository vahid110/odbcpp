# Issue #117: Windows/WSL package and live-suite readiness

Readiness only. **No live, cloud or credential operation ran, and this is not a
live acceptance.** Source: `main` `5f7a2ab` (merge of PR #98), plus the two
commits of this branch. One Windows 11 x64 host and its WSL Ubuntu. Raw evidence
(CTest JUnit, `LastTest.log`, GoogleTest XML, discovery JSON, exits, hashes) is
private on that host under `odbcpp-migration/issue117-windows-readiness/`.

## 1. Binaries and package against source and configuration

Six owning builds at `5f7a2ab`, each in its own build tree, with the opt-in
targets each profile admits (`ODBCPP_DM_INTEGRATION_TESTS=ON` everywhere, and on
Redshift the routine, omitted-table, Power BI relationship and package-import
live targets; installed-package live only where its guard allows 2/2):

| Profile | Manager | App/driver SQLWCHAR | Crypto |
|---|---|---|---|
| `windows-pg`, `windows-redshift` | Windows ODBC (`odbc32`) | 2/2 | OpenSSL 3.6.2, bundled shared |
| `unixodbc-pg`, `unixodbc-redshift` | unixODBC 2.3.14 | 2/2 | system OpenSSL 3.5.5 |
| `linux-iodbc-pg`, `linux-iodbc-redshift` | iODBC 3.52.16 | 4/4 | system OpenSSL 3.5.5 |

MSVC 19.44 Release; GCC 15.2 Release; CMake 4.4.4 (Windows) / 4.2.3 (WSL).

**Installed Windows package.** The installed `ODBCPP Redshift Beta (x64)` 1.0.0
records source and packaging revision `54ebedf`. All 14 installed payload files
match its own inventory. Between `54ebedf` and `main`, nothing outside tests, docs
and tooling changed except `cmake/Tests.cmake` and the new
`DmQualifiedIdentity` files, and those touch only test targets. So the driver,
setup and package sources are identical.

The installed driver differs from a rebuild of that source in the **same build
tree** only in its 4-byte link timestamp (header plus debug directory). A build in
another tree also differs in layout: MSVC embeds an absolute source path through
OpenSSL's allocation macros (`__FILE__`, in `sdk/src/security/openssl_tls_client.cpp`),
and there is no `/Brepro`. So Windows driver bytes are not reproducible across
build trees. Bind a package to its build tree and inventory, not to a hash
recomputed elsewhere.

**Fresh packages from `main`.** `packaging/windows/build.ps1` built both MSIs from
the clean tracked source:
- the PostgreSQL and Redshift MSIs;
- `test-inputs.ps1` (x64, product-mismatch, duplicate and unsupported rejection) passed for both graphs;
- `test-products.ps1` (read-only MSI identity, isolation, registry, x64 component and payload inspection) passed.

The new Redshift payload matches the installed one in every runtime file (OpenSSL,
MSVC redistributables), the crypto manifest and both recipe hashes. Only
`odbcpp.dll` and `odbcpp_setup.dll` differ, as explained above.

Host prerequisites the README assumes but this host lacks: PowerShell 7 and a .NET
SDK. Packaging ran under Windows PowerShell 5.1, reusing a retained WiX 6.0.2 tool
folder that runs on the installed .NET 10 runtime. Under 5.1,
`$PSNativeCommandUseErrorActionPreference` has no effect, so step outputs and exits
were checked individually.

## 2. Test inventory (offline discovery, no test bodies)

CTest `--show-only=json-v1` plus `--gtest_list_tests` for every GoogleTest image.
Three plain-main programs (`it_driver_manager`, `it_setup`, `it_package_load`) are
not GoogleTest and must not be listed by execution (see section 5).

| Profile | CTest entries | Unit cases | Direct `it_*` cases | Manager `dm_*` cases |
|---|---:|---:|---:|---:|
| windows-pg | 156 | 2445 | 404 | 373 |
| windows-redshift | 160 | 2445 | 565 | 521 |
| unixodbc-pg | 154 | 2442 | 404 | 373 |
| linux-iodbc-pg | 154 | 2442 | 404 | 373 |
| unixodbc-redshift | 158 | 2442 | 565 | 521 |
| linux-iodbc-redshift | 157 | 2442 | 562 | 520 |

**Against the retained Redshift live inventories** (Windows 541, unixODBC 541,
iODBC 538 tuples, last collected at `a9b1550`), current `main` has, per profile:

- **10 retained cases that cannot run.** `it_redshift_routine_schema_patterns` (2)
  and `it_redshift_columns_omitted_table` (8) are registered but never built by the
  ordinary graph. Fixed by this branch (section 4).
- **34 newer direct cases, outside the retained Redshift scope.** These are the
  same set on all three profiles, in the categories an earlier audit recorded:
  - PostgreSQL-only fixtures: catalog mutation, COPY/BINARY CURSOR, domains, routines, indexes, bytea output, PostgreSQL syntax;
  - connection-free cases: `LoginTimeoutOutputWidth` and the Unicode override harness;
  - two `IAM*` cases bound to a separate principal profile.

  The checkpoint coordinator must record each as unavailable-with-reason or add it
  to scope; none can be silently dropped.
- **No Driver Manager Redshift tuples.** The `dm_it_*` twins (about 520 cases per
  Redshift profile) are absent from every retained Windows/WSL live inventory:
  - On WSL the POSIX live backend supports a manager profile, but no inventory includes one.
  - On Windows the native backend refuses manager profiles (`posix_driver_manager_profile`).
  - macOS ran Redshift through iODBC and unixODBC for PR #98; Windows/WSL have not.

Other explicit coverage boundaries:
- package-live needs 2/2, so it is unavailable on iODBC 4/4;
- the official vendor import comparison is Darwin iODBC 4/4 only;
- mixed application/driver widths (for example application 4 / driver 2), ODBC2
  behaviour and pooling through a manager have no Windows/WSL profile.

## 3. Offline checks at `5f7a2ab`

| Profile | `-offline` CTest | `dm-offline` (4 entries, 8 cases) + validator |
|---|---|---|
| windows-pg | 135/135 | pass, 0 errors |
| windows-redshift | 135/135 | pass, 0 errors |
| unixodbc-pg | 134/134 | pass, 0 errors |
| linux-iodbc-pg | 134/134 | pass, 0 errors |
| unixodbc-redshift | 134/134 | pass, 0 errors |
| linux-iodbc-redshift | 134/134 | pass, 0 errors |

- **Live-suite registrar regression** (`tests/development/live_suite/test_live_suite.py`, WSL): accepted.
- **`python -m unittest discover tools/development`:**
  - Windows: 333 tests, 57 errors, 7 skipped. Every error is a POSIX-owned module needing `fcntl`, `os.O_NOFOLLOW`, `os.geteuid` or `time.clock_gettime`, plus one symlink-privilege error.
  - WSL: 41 failures and errors, all in Windows-owned `test_windows_*` modules (`winreg`, Windows ctypes calls, Windows evidence paths).
  - No module fails on its owning host. The combined command is not a portable single-host gate, as PR #98 recorded; this is unchanged.

## 4. Repair in this branch

`df9de2b` adds `test_registered_test_images` (labels `unit;architecture`). It asks
CTest for its own registration and fails if any entry has no built executable.
On the original graph it failed:
- on `windows-redshift`, naming the two Redshift opt-in tests;
- on `windows-pg` with `ODBCPP_INSTALLED_SETUP_TESTS=ON`, naming `it_setup_registration`.

Running those two Redshift entries under CTest before the fix reported both as
"Not Run" (exit 8). Discovery found the same two unbuilt registrations on
`unixodbc-redshift`, and the WSL before-run repeats the check on both WSL Redshift
profiles.

`2f7dfff` sets `EXCLUDE_FROM_ALL OFF` on those three targets only when their
opt-in option registers them. With the options off (the default), they stay out
of the ordinary graph as before.

| Graph | Check at `df9de2b` (before) | Check at `2f7dfff` | `-offline` at `2f7dfff` | `dm-offline` + validator |
|---|---|---|---|---|
| windows-redshift (live opt-ins) | exit 8: routine, omitted-table | pass | 136/136 | (unchanged graph; see section 3) |
| windows-pg (installed-setup on) | exit 8: `it_setup_registration` | pass | 136/136 | (unchanged graph; see section 3) |
| unixodbc-redshift (live opt-ins) | exit 8: routine, omitted-table | pass | 135/135 | pass, 0 errors |
| linux-iodbc-redshift (live opt-ins) | exit 8: routine, omitted-table | pass | 135/135 | pass, 0 errors |
| unixodbc-pg | not run | pass | 135/135 | pass, 0 errors |
| linux-iodbc-pg | not run | pass | 135/135 | pass, 0 errors |

After the fix, Windows Redshift discovery lists 575 direct cases, which includes
all 541 retained tuples. Nothing is missing and no registration lacks a built
executable.

## 5. Remaining prerequisites for the full live checkpoint

1. **Exact source and CI.** Pick the integrated head (this branch merged, or `main`
   if declined) and get green applicable CI on it. Rebuild every owning profile
   with the opt-in targets enabled.
2. **Complete applicable inventory.** Regenerate the index and the independent
   expected inventory from that head. Include the 10 restored cases and classify
   the 34 newer cases. Then either add Driver Manager Redshift profiles (WSL
   unixODBC and iODBC, Windows once its backend supports one) or record them as
   unrun with reasons. Run `live_checkpoint.py prepare`/`preflight` on each host.
3. **Installed Windows package.** Refreshing the shared installed driver to the
   tested candidate goes through the existing protected `QualificationRunner`
   path that the Windows coordinator owns, with elevation and independent review.
   This branch did not touch it.
4. **Power BI 064 cleanup debt, unresolved.** The receipts show:
   - two relationship fixture tables were created;
   - the final principal-session check returned an error;
   - principal cleanup, fixture cleanup and the final zero check were each refused.

   Removal and zero-state are therefore unverified and need an admitted
   read/cleanup session by the resource owner before any further live use.
5. **Local leftovers.** A per-user DSN `ODBCPP_Setup_<unicode>` from an
   interrupted `it_setup` run predates this work. It points at an uninstalled
   PostgreSQL driver and blocks `it_setup` ("clean fixture"). The owner should
   remove it deliberately.
6. **Host tooling.** Install PowerShell 7 and a .NET SDK on the Windows packaging
   host, or record the 5.1 plus retained-WiX substitution as an accepted
   deviation.
7. **Admission.** Existing human resume, finite admission, verified TLS, network
   and caps apply. This work created none.
