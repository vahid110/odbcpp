# Building and installing ODBCPP

The [command matrix](tools/development/COMMANDS.md) and `CMakePresets.json`
are the current build entry points. The earlier static-linking guide is retained
as [historical documentation](docs/history/build-system-pre-security-20261025.md),
not current deployment instructions.

## Dependencies and profiles

Use CMake 3.21+, CTest, Python 3.9+, Git and a C++20 compiler. Unix presets
use Ninja. Windows presets use Visual Studio 2022 x64 tools. Install the selected
driver manager's development headers/libraries and OpenSSL 3 development files.
Use vendor-supported security updates; an OpenSSL major-version check alone
is not a vulnerability or support assessment.

| Platform | Redshift preset | Crypto selection |
| --- | --- | --- |
| Windows x64 | `windows-redshift` | Pass `-DODBCPP_CRYPTO_ROOT=C:/absolute/OpenSSL3`; bundled shared profile |
| Linux/WSL, unixODBC | `unixodbc-redshift` | System-shared OpenSSL 3 |
| Linux/WSL, iODBC | `linux-iodbc-redshift` | System-shared; set `ODBCPP_IODBC_ROOT` to the UCS4 iODBC prefix |
| macOS, iODBC | `macos-iodbc-redshift` | System-shared; set `ODBCPP_IODBC_ROOT`, and `CMAKE_PREFIX_PATH` as needed |

For PostgreSQL replace `redshift` with `pg` in these explicit profile names.
MySQL and SQL Server product builds are not implemented. Match driver/application
architecture and `SQLWCHAR` width; see [Unicode ABI](README.md#driver-manager-unicode-abi).

```sh
cmake --list-presets
cmake --preset unixodbc-redshift
cmake --build --preset unixodbc-redshift
ctest --preset unixodbc-redshift-offline --output-junit offline.xml
```

Windows example, in a developer shell:

```powershell
cmake --preset windows-redshift -DODBCPP_CRYPTO_ROOT=C:/absolute/OpenSSL3
cmake --build --preset windows-redshift
ctest --preset windows-redshift-offline --output-junit offline.xml
```

Each preset has its own `build-<preset>` directory. Do not share an existing graph
across crypto providers, linkage modes, driver managers or incompatible compilers.
Offline results do not replace integration/live qualification.

## Private runtime installation

After building, install the runtime component into a new application-owned prefix:

```sh
cmake --install build-unixodbc-redshift --prefix /absolute/private-prefix --component ODBCPPDriverRuntime
```

For Windows use `build-windows-redshift` and add `--config Release`. The component
installs the loadable driver and crypto manifest, using CMake's GNU installation
directories. It does not register a driver/DSN, install a driver manager, deploy
all external crypto libraries, or install CA roots. Retain the selected runtime
dependencies and verify TLS using OS trust or an explicit custom CA source.
See [trust-store options](docs/trust-store-design.md#3-options-defaults-and-precedence).

For Windows installer packaging and registration, use the instructions in
[packaging/windows](packaging/windows/README.md); a private CMake runtime install
is a different deployment path. The default install without a component also
exports SDK headers/libraries/CMake targets and is intended for C++ integration.

## Build identity and evidence

Development snapshots are identified by their full Git commit, configuration,
compiler/dependency versions and output hashes. The CMake project version alone
is not a unique identifier for an unreleased binary. Record `git rev-parse HEAD`,
local modifications, `CMakeCache.txt`, the crypto manifest and actual test logs
with packages. Do not label a development package a released or certified build.

For complete local and live test collection, follow the
[command matrix](tools/development/COMMANDS.md). Keep credentials out of source,
public build logs, test reports and issue attachments.
