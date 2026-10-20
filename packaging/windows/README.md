# ODBCPP PostgreSQL and Redshift x64 beta packages

Build from the repository root on x64 Windows with Visual Studio C++ tools,
CMake, .NET SDK and the OpenSSL 3 build dependency:

    pwsh packaging/windows/build.ps1

The default command retains the PostgreSQL build directory and existing upgrade
identity. The configured `TARGET_DATABASE` selects the product; an optional
`-TargetDatabase` must agree with that graph. For a Redshift graph, use:

    pwsh packaging/windows/build.ps1 -BuildDirectory build-windows-redshift -TargetDatabase REDSHIFT -OpenSslRoot 'C:/absolute/OpenSSL3'

Output defaults to `<BuildDirectory>/package`. The packages are named
`odbcpp-postgresql-<version>-x64.msi` and `odbcpp-redshift-<version>-x64.msi`.
They have separate upgrade/component identities, driver registrations
(`ODBCPP PostgreSQL` / `ODBCPP Redshift`) and install directories under
`Program Files/ODBCPP`. Installing or upgrading one must preserve the other.
The existing PostgreSQL identities are unchanged. MySQL and mismatched graphs
are rejected before staging. Keep the configured tracked source clean and immutable;
untracked qualification XML/log reports do not block packaging. The inventory
records the tracked checkout's Git revision, build-cache hash, separate packaging
revision and actual recipe hashes. Payload hashes identify the runtime images;
packaging an older artifact does not become a new compilation result. The
tracked-source guard alone does not prove which source compiled a cached image;
retain its actual source/image/build/CI evidence separately.

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
your application. Packages target x64 Windows; PostgreSQL and Redshift retain
their separate backend qualification and application acceptance requirements.
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

The PostgreSQL CI uses a separate fresh Windows runner for package acceptance, excluding build
paths from PATH, adding hostile same-name PATH decoys, checking actual loaded
runtime module paths and version, rejecting a decoy when an app-local runtime is
missing, connecting through the
installed driver, invoking the setup dialog, upgrading, forcing rollback with a
separate test-only MSI, rejecting downgrade and foreign registration, and
uninstalling while checking DSN/unrelated-registration preservation. The failure
custom action is absent from the beta MSI. SQL Server, Power BI and Excel remain
G8 real-application checks.

Run `test-inputs.ps1` against each configured product graph for input rejection.
Run `test-products.ps1 -PostgreSqlPackage <pg.msi> -RedshiftPackage <rs.msi>`
to inspect actual MSI identities, x64 components, registration searches and
payload inventories without installation. `-PostgreSqlBaseline <old-pg.msi>`
additionally compares the original PostgreSQL component/directory/registry
tables. This is package inspection, not installation, connection, Power BI or
Access certification. Native coexistence/upgrade/uninstall and real Redshift
application results must be recorded separately.

On an approved elevated local x64 Windows session with both product registrations
and install folders initially absent, `test-product-lifecycle.ps1` installs both
packages, loads their installed runtimes through the existing `/MT` native loader,
tests Redshift rollback/upgrade/downgrade rejection while preserving PostgreSQL,
then removes both owned installations. Supply the 1.0.0 packages, Redshift 1.0.1
upgrade and intentional 1.0.2 rollback fixture, `it_package_load.exe` and a new
evidence directory. It refuses existing installations, holds the input MSIs,
bounds MSI operations and loader children, retains verbose logs, and performs
owned cleanup after failures. It creates only temporary owned User DSNs for the
setup tests and makes no database connections. A
timed-out still-active installer requires reconciliation before another mutation.

Build the installed-setup test program for each product without rebuilding the
runtime (example for Redshift; repeat with POSTGRESQL and another build directory):

    cmake -S tests/windows/installed_setup -B build-installed-setup-redshift -G "Visual Studio 17 2022" -A x64 -DTARGET_DATABASE=REDSHIFT
    cmake --build build-installed-setup-redshift --config Release --parallel 2

Pass both configured directories as `-PostgreSqlSetupBuild` and
`-RedshiftSetupBuild` to the lifecycle script. CTest runs all three GoogleTest
cases per product through the actual installed setup DLL: ANSI/Unicode User DSN
creation, configuration/removal, Unicode storage, password non-persistence and
wrong-product/duplicate refusal. Both groups are collected before assessment;
each uses absence-checked process-specific names and owner-checked cleanup.
There is no database or GUI dialog call. The same source is available in a full
product graph as `it_setup_registration`, default-off from registration until
`ODBCPP_INSTALLED_SETUP_TESTS=ON`. The small entry point compiles the adapter test
only; it does not establish a new product build or replace product regression.

This proves exact-import staging and fresh-process app-local loader origin for
the packaged OpenSSL profile. It does not prove coexistence when another module
has already loaded a same-basename crypto provider, symbol isolation, FIPS, or a
qualified provider/linkage row. Those remain separate S2C gates.
