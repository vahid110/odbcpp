# Windows native DSN configuration

## Windows GUI refresh — authorized 2026-10-19

The human authorized an isolated Windows DSN GUI package following the source
audit at `5bf2582d9954bec47a1e3a6aad3c3dfa480225d7`. This releases this package
only; unrelated product/AUTH work and paused timers retain their existing state.
Implementation and local qualification use the managed `windows-dsn-gui` worktree.
This work does not change cloud/live authority or claim Windows Redshift packaging
acceptance. Integration and required platform/CI qualification remain separate.

The observable result is a polished native dialog using the existing Windows SDK:
correct compiled-product identity, supported advanced settings, preserved unknown
DSN attributes, draft connection tests using the effective saved/edited settings,
responsive tests with a requested diagnostic login timeout, and useful
credential-safe diagnostics. Keep passwords
transient and preserve cancel, scope isolation and installer rollback behavior.
Expose only authentication methods available in the Windows driver product.

| Component | Physical owner | Reuse and allowed dependencies | Required proof |
|---|---|---|---|
| Product identity/defaults and supported neutral options | `sdk/include/odbcpp/database/backend_provider.h`, `core/database/postgres/pg_backend_provider.*`, `product/compiled_backend.cpp` | Reuse existing SDK contracts; backend owns applicability and composition selects the product. No Windows/ODBC dependency in SDK/backend. | Provider tests, SDK first-include and architecture checks |
| DSN fields, keyword mapping, validation, quoting and draft attributes | `odbc/setup_model.h` | ODBC-specific model consumes neutral contracts; registry/DSN syntax is not SDK functionality. | GoogleTests for applicability, omission, quoting and persistence/test parity |
| Installer scope, persistence and rollback | `odbc/setup/setup.cpp` | Windows installer APIs plus the setup model. | Native installer tests with isolated fixtures |
| Layout, accessibility, theme/DPI handling and background connection testing | `odbc/setup/setup_dialog.*`, `odbc/setup/setup.rc`, `odbc/setup/resource.h` | Windows APIs and ODBC connection/diagnostic APIs; no concrete backend includes. | Native dialog interactions and captures at multiple scales |
| Local automation and privileged acceptance admission | `tools/development/test_windows_dsn_gui.ps1`, `tools/development/windows_local_process.cs`, `tools/ci/windows-administrator.ps1`, `tools/ci/windows-package.ps1` | Windows-only test process orchestration belongs in tools, outside product/SDK runtime contracts. Local checks build/test current-user hosts; machine acceptance requires an already elevated disposable CI session. | Embedded execution-level inspection, actual local runner, process containment tests and fail-fast admission checks |

The human additionally requested an automated local workflow without UAC prompts.
Local qualification must not launch Windows ODBC Administrator or machine
installers. Use current-user (`asInvoker`) test hosts; keep installation and System
DSN acceptance on an already elevated disposable CI runner. Do not disable UAC or
add a persistent privileged service/task to avoid consent prompts.

Before integration, independent review must inspect actual foundation placement,
the exact candidate and retained qualification evidence. Existing Windows beta
support remains PostgreSQL x64 until a Redshift package is separately qualified.
This section records authorized scope; implementation/qualification results are
recorded with the candidate rather than inferred from this plan.

### Local candidate qualification — 2026-10-19

Source candidate `bf8d97768fa708eb40cf5a849c5e9fd4bd90f393` is based on
`5bf2582d9954bec47a1e3a6aad3c3dfa480225d7`, on the isolated
`codex/windows-dsn-gui` branch. Independent review approved the actual ownership,
implementation and preserved successor fixes. No integration or push is claimed.

Both PostgreSQL and Redshift Windows x64 Release configurations compiled the
actual core, driver DLL, setup DLL and owning tests with MSVC 19.44.35229 and
OpenSSL 3.6.2 `BUNDLED_SHARED` / ODBC SQLWCHAR2. Setup uses `/W4 /WX`.
Each profile passed all 23 provider, 17 setup-model and six actual native-dialog
GoogleTests, with no failures, disabled cases or skips. The six applicable
architecture checks passed in each profile. PostgreSQL additionally compiled all
24 currently listed SDK contract headers as first includes, passed the SDK
consumer and compiled the real installer acceptance executable (`it_setup`).
CTest totals were 10/10 PostgreSQL and 9/9 Redshift entries passed.

Native captures of all four sections were inspected, including selection of the
last section. The native tests check layout bounds on every selection, transient
credentials, responsive draft testing, trust/inheritance, cancel and deferred
close. Final setup imports were checked against system ODBC32 exports to verify
that testing calls the Driver Manager. Raw GoogleTest/CTest results, compiler
logs/tlogs, original failed attempts, binary hashes, captures and a compact
artifact register remain in the owned `build-windows-dsn-*` graphs and
`build-gui-evidence/qualification.json`. Owned gate processes were reaped.

The initial UI visibility/paint race, strict DSN-mode conversion warning,
intermediate layout compile failure and capture-discovered navigation overflow
remain recorded. These are corrected successor results, not erased failures.
No registry, service, private PostgreSQL, cloud or live session was started.

Remaining acceptance: actual installer/User-System persistence and real-login
runtime; clean MSI/package acceptance; macOS and Ubuntu/Linux builds; hosted final
CI; and broader monitor/DPI, high-contrast and screen-reader validation. Local
native UI testing does not establish these results or Windows Redshift packaging
support. The requested login timeout still has the DNS limitation described below.

### Automated local checks without UAC

From the managed checkout, run:

```powershell
./tools/development/test_windows_dsn_gui.ps1
# Or use the separately configured Redshift graph:
./tools/development/test_windows_dsn_gui.ps1 -BuildDirectory build-windows-dsn-redshift
```

The build graph must already be configured for this exact checkout. The runner
builds the owning targets, checks the exact three-suite inventory and each test
host's embedded `asInvoker`/`uiAccess=false` manifest, then executes the existing
GoogleTests. Helpers use `CreateProcess` without shell elevation. Finite build and
test timeouts contain owned descendants in a Windows job; GoogleTest selection
environment variables are cleared and restored to prevent partial/empty runs.
Each run retains stdout/stderr, executable hashes, manifests, raw GoogleTest
and CTest XML, native captures and a result record in a unique `gui-local-*`
directory inside that graph. No installer, driver registration, ODBC Administrator,
live server, scheduled task or persistent privileged service is involved.

Windows ODBC Administrator's actual installed manifest requests
`highestAvailable`; local test executables request `asInvoker`. This explains a
possible UAC path, but does not identify the source of an earlier unseen prompt.
The machine acceptance scripts `tools/ci/windows-administrator.ps1` and
`tools/ci/windows-package.ps1` now check the current administrator token before
doing work. A standard token receives a clear error instead of an elevation
request. Run those acceptance jobs in the existing already elevated disposable
CI environment; their pending acceptance scope remains unchanged.

The final runner passed all 46 owning GoogleTests under a standard token with
PowerShell 7.6.5/PostgreSQL and Windows PowerShell 5.1/Redshift. Both runs injected
hostile inherited filter/repeat/sharding/list/flag-file settings, verified their
restoration and verified zero active owned processes. Focused Windows PowerShell
5.1 tests additionally cover quoting, nonzero exit with retained output,
simultaneous output drains, timeout, a lingering descendant holding output and
launch failure. Both privileged entry points rejected a standard token before
launch/mutation. `build-gui-evidence/uac-automation.json` binds this evidence to
the source files and existing GUI candidate. Original fixture-inheritance,
exception-serialization and empty-environment failures remain retained alongside
their passing successors. Elevated installer/Administrator runtime remains pending.

The Windows driver reads native registry configuration. The setup component
described below creates entries through the installer API without saving
passwords. The packaged beta remains PostgreSQL x64.

## Resolution rules

1. A User DSN under `HKCU\SOFTWARE\ODBC\ODBC.INI\<name>` takes precedence over
   a same-named System DSN under `HKLM\SOFTWARE\ODBC\ODBC.INI\<name>`.
2. An existing User DSN is a complete selection: missing fields do not inherit
   from the same-named System DSN. Empty, malformed or inaccessible entries never
   cause fallback to another native DSN or INI source.
3. System DSNs and driver registration use the process architecture's registry
   view. The initial supported Windows artifact is x64. Windows shares User DSNs
   across architecture views, so a User DSN still needs a compatible driver.
4. The same hive's `ODBC Data Sources` value identifies the registered driver
   name. Driver defaults are read from
   `HKLM\SOFTWARE\ODBC\ODBCINST.INI\<driver name>`. A DLL path is not treated as
   a registry subkey; absent registration metadata uses the configured product
   registration, then the existing generic ODBCPP compatibility default.
5. Effective attributes follow driver defaults < selected DSN < explicit
   connection string. HOST/SERVER, DB/DATABASE, USER/UID and PASSWORD/PWD aliases
   obey that precedence. The primary spelling wins if both are in one layer.
   Explicit empty values are preserved. SQLConnect's explicit user/password
   arguments retain their existing final precedence.

Registry strings must be valid UTF-16, terminated REG_SZ with no embedded NUL;
REG_DWORD values become decimal text (including installation UsageCount metadata).
Other registry data types, malformed strings and denied reads fail clearly without
silently choosing another connection. REG_EXPAND_SZ environment expansion is not
supported. Attribute values are not trimmed, unbraced or logged by the reader.

Explicit ODBCINI/ODBCSYSINI/ODBCINSTINI files remain a legacy/test fallback when
no native entry exists. Windows no longer discovers implicit current-directory
or Windows-directory INI files. Normal native DSNs do not depend on any INI file.
FileDSN support is not added by this change.

## Service identities and evidence

User DSNs belong to the executing Windows identity, not the interactive user who
configured them. Configure a System DSN accessible to the service identity for
SQL Server/MSDASQL. Database credentials and file/certificate permissions must
also be available to that identity. The driver does not impersonate another user.

Windows CI reads registry-only DSNs for all integration tests. It installs a
conflicting 32-bit System DSN to check x64 view selection. A scheduled LocalSystem
process runs the real Windows Driver Manager acceptance executable with nonexistent
INI paths while the runner user's same-named User DSN has unusable settings.
This verifies service-identity visibility, not SQL Server/OPENQUERY application
acceptance; that remains G8. The test runs in the disposable hosted runner and
removes its scheduled task and temporary User DSN afterward.

Windows-only unit cases isolate HKCU/HKLM through process-local registry overrides
and cover User/System precedence, Unicode, empty/missing attributes, invalid names,
malformed registry data, DWORD values and denied-access recovery. They do not
replace the machine's actual ODBC configuration.

Registry layout and architecture behavior follow Microsoft's
[registry entries for data sources](https://learn.microsoft.com/en-us/sql/odbc/reference/install/registry-entries-for-data-sources)
and [32-bit/64-bit ODBC administration guidance](https://learn.microsoft.com/en-us/troubleshoot/sql/connect/odbc-tool-displays-32-bit-64-bit).

## Native connection-string acceptance (W2)

The Windows Driver Manager suite checks 11 successful connections and four
credential rejection/recovery sequences across SQLDriverConnectA/W. It verifies
actual database/user identity, DSN overrides against deliberately wrong defaults,
case-normalized keys, braced semicolons and escaped braces in SCRAM passwords,
Unicode passwords on W, explicit input lengths and completed-output lengths.
Empty passwords override stored credentials and must fail authentication; a valid
retry on the same handle must clear old diagnostics. Failure messages must not
contain the rejected password. Test connection strings are never printed.

The same executable runs under the runner identity and LocalSystem. Dedicated
roles and DSNs exist only in the disposable CI fixture. Direct parser tests cover
first-occurrence duplicate handling and empty credentials because Windows DM can
rewrite duplicates before the driver sees them. This does not introduce a second
Windows parser or promise arbitrary non-ASCII ANSI code-page conversion.

## Administrator setup component (W3)

`odbcpp_setup.dll` exports ConfigDSN/ConfigDSNW and is registered as the driver's
Setup library. It uses native C++/Win32 controls, without C++/CLI, .NET or a GUI
framework runtime. Connection, Security, Advanced and Diagnostics sections group
the supported settings. Only database-password authentication is exposed today;
new methods require backend support, validation and tests before appearing here.
The dialog code is separate from persistence; the portable ODBC model owns keyword
mapping, validation and connection-string quoting. Compiled-product identity and
defaults come from the SDK provider, and its neutral optional-policy support
controls which backend-specific settings appear. The GUI does not select a
backend from an endpoint, port or display-name string, and does not override the
product's current defaults. Future settings extend the model and persistence
allowlist rather than introduce a second connection parser.

Add/configure/remove use the Windows installer API and its User/System config
mode. A supplied DSN name cannot be renamed in the dialog. Duplicate adds, orphan
sections, invalid settings and foreign-driver edits/removals are rejected.
Partial edits retain omitted fields and unrecognized existing settings. Save
writes the basic fields and explicit supported overrides, removes legacy
PWD/PASSWORD, and never persists the test password. Absent advanced values inherit
driver defaults; an explicit empty application name remains distinct from absence.
Clearing an override removes its owned setting and any supported timeout alias.
Cancel and connection testing do not save.

Connection settings include application identity; Security includes transient
test credentials, verified TLS, and optional CA file/directory browsing. Advanced
settings include timeouts, TCP keepalive, logging and applicable backend metadata
policies. Native file/folder pickers use the existing Windows SDK. New GUI code
does not require a .NET, browser or external GUI framework runtime.

Testing runs on a worker owning a snapshot of the draft and its ODBC handles.
The DSN-less W connection includes retained safe settings and explicit edits,
while excluding DSN/driver/file-routing keys and legacy credentials. The selected
compiled driver remains authoritative. A five-second login attribute deliberately
overrides the configured login timeout for this diagnostic operation; this is not
a test of the application's configured timeout behavior. Synchronous DNS
resolution in the existing transport can exceed this requested timeout; this GUI
package does not establish a hard wall-clock deadline or introduce cancellation.
The UI remains
responsive and disables edits/Save until completion. Closing during a test waits
for it to finish rather than claiming cancellation. Diagnostic details include
elapsed time, SQLSTATE, native code and fixed actionable explanations. Raw
driver/server message text and test passwords are excluded.

Common controls v6 are activated within the setup DLL's context. DPI awareness is
scoped to the calling thread and restored on exit; process-wide host settings are
not changed. Standard native controls retain keyboard and system accessibility
behavior. The dialog uses native system appearance, without a custom dark theme.

Validation completes before writes. A failed write attempts to restore the
previous owned values (or remove a newly created DSN); this is best-effort recovery,
not a transactional registry guarantee under persistent access failure or concurrent
external edits. Installation/upgrade rollback belongs to W4. System DSN changes
require suitable Windows permissions; this component does not elevate itself.

Hosted Windows CI invokes the real installer API and automates the native sections to check
masking, invalid input, failed/successful test login, cancel, save, Unicode/ANSI
entry points and User/System/foreign-driver deletion isolation. Dialog captures
are retained with the Windows evidence. W4 must still validate the packaged setup
component from ODBC Administrator on a clean Windows environment.

`test_setup_dialog` additionally exercises the actual modal dialog with bounded
injected connection operations, without registry changes or network access. It
checks draft delivery, worker responsiveness, inheritance reset, deferred close
and all four pages. Setting `ODBCPP_GUI_CAPTURE_DIR` captures the native pages for
visual review. These offline tests do not replace installer, TLS, real-server or
clean-package acceptance.

The setup entry-point contract follows Microsoft's
[ConfigDSN documentation](https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/configdsn-function).
