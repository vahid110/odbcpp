# ODBCPP PostgreSQL x64 beta package

Build from the repository root on x64 Windows with Visual Studio C++ tools,
CMake, .NET SDK and the OpenSSL 3 build dependency:

    pwsh packaging/windows/build.ps1

The build pins WiX 6.0.2, rejects non-x64 PE payloads, stages only runtime files
and notices, and emits an MSI, SHA-256 checksum and per-file dependency inventory.
The generated crypto manifest is validated against the Windows `OPENSSL` +
`BUNDLED_SHARED` profile. Packaging also inspects the finished driver, records
the exact imported OpenSSL DLL basenames, copies only those files from the
controlled dependency root, and binds the resulting evidence to driver and
runtime SHA-256 hashes. Both manifests are installed and hashed in the inventory.
This is a repeatable build recipe with recorded inputs, not a claim of byte-for-byte
MSI reproducibility across compiler/SDK versions. The source revision, tool version
and payload hashes accompany every artifact. Packages are currently unsigned.
The repository has no declared project license; artifacts are for internal beta
validation until release licensing/signing is resolved under G11.

Install the MSI with Windows Installer (elevation required). Configure a User or
System DSN through the 64-bit ODBC Data Source Administrator. Connection testing
uses a transient password; DSNs do not save passwords. Supply credentials through
your application. Only x64 Windows and PostgreSQL are claimed by this beta.
No .NET runtime is needed on the destination machine.

The MSI registers the driver and setup DLL under the 64-bit ODBC registry view
and installs OpenSSL and the MSVC redistributable DLLs beside them. The Microsoft
redistributable files are from the installed Visual Studio Redist directory;
see inventory.json for exact versions, hashes and redistribution terms. OpenSSL
and spdlog notices are included. Keep Windows' Universal CRT up to date.

Install a higher package version to upgrade. Windows Installer removes the old
version inside the transaction and restores it if the upgrade fails. Downgrades
are rejected. To deliberately return to an older version, uninstall first and
install the retained earlier MSI. Close applications using the driver before
upgrading or uninstalling.

Uninstall removes package-owned files and registry values. It never deletes DSNs;
remove selected DSNs explicitly in ODBC Administrator before uninstalling if
wanted. Other drivers and unrelated registry values are retained. Installation
and removal refuse a conflicting or externally repointed registration; restore
that registration deliberately before retrying. Do not delete registry trees as
a workaround. Use verbose MSI logging to diagnose failures:

    msiexec /i odbcpp-postgresql-1.0.0-x64.msi /l*v install.log

CI uses a separate fresh Windows runner for package acceptance, excluding build
paths from PATH, adding hostile same-name PATH decoys, checking actual loaded
runtime module paths and version, rejecting a decoy when an app-local runtime is
missing, connecting through the
installed driver, invoking the setup dialog, upgrading, forcing rollback with a
separate test-only MSI, rejecting downgrade and foreign registration, and
uninstalling while checking DSN/unrelated-registration preservation. The failure
custom action is absent from the beta MSI. SQL Server, Power BI and Excel remain
G8 real-application checks.

This proves exact-import staging and fresh-process app-local loader origin for
the packaged OpenSSL profile. It does not prove coexistence when another module
has already loaded a same-basename crypto provider, symbol isolation, FIPS, or a
qualified provider/linkage row. Those remain separate S2C gates.
