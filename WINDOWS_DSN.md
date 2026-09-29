# Windows native DSN configuration

The x64 PostgreSQL driver reads native registry configuration. The minimal ODBC
Administrator setup GUI and installer are subsequent W3/W4 work; this reader does
not create entries or save credentials.

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
