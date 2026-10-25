# ODBCPP Build System Configuration

> **Current versus planned:** the implementation currently links through
> OpenSSL CMake targets selected by a strict build-time provider/linkage
> profile. Artifact inspection and AWS-LC qualification remain open. The full contract is
> [CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md). Examples below describe
> the OpenSSL build and must not be read as AWS-LC,
> linkage, packaging or FIPS support claims.

## Static vs Dynamic Linking

### Current Configuration (Default)
- **ODBC Driver**: ✅ **SHARED** (`libodbcpp.dylib` - for ODBC Driver Manager)
- **Core Library**: ✅ **STATIC** (`libodbcpp_core.a` - for examples/tests)
- **OpenSSL request**: system-shared by default on Linux/macOS and bundled-shared
  on Windows; resulting linkage is not yet artifact-verified
- **System Libraries**: Dynamic (libc++, libSystem - required by OS)

### Build Outputs

#### Default Build
```bash
cmake -B build
cmake --build build
```

**Produces:**
- `libodbcpp.dylib` (5.3MB) - **ODBC Driver** for system integration
- `libodbcpp_core.a` (455KB) - Static library for development

**Intended ODBC Driver Dependencies (inspect the built artifact):**
- `libc++.1.dylib` (system C++ runtime)
- `libSystem.B.dylib` (system library)
- **OpenSSL**: selected by the requested profile; binary inspection remains a
  separate qualification step

#### Dynamic OpenSSL Build (Optional)
```bash
cmake -DODBCPP_CRYPTO_PROVIDER=OPENSSL \
      -DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED -B build-dynamic
cmake --build build-dynamic
```

**Additional Dependencies:**
- `libssl.3.dylib` (OpenSSL SSL)
- `libcrypto.3.dylib` (OpenSSL Crypto)

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ODBCPP_CRYPTO_PROVIDER` | `OPENSSL` | Provider selected for the artifact; AWS-LC is rejected until qualified |
| `ODBCPP_CRYPTO_LINKAGE` | platform default | `SYSTEM_SHARED`, `BUNDLED_SHARED`, or `BUNDLED_STATIC` |
| `ODBCPP_CRYPTO_ROOT` | empty | Existing absolute provider prefix required for bundled profiles |
| `BUILD_EXAMPLES` | `ON` | Build example applications |
| `BUILD_TESTING` | `ON` | Build unit and integration tests |
| `TARGET_DATABASE` | `REDSHIFT` | Implemented product (`REDSHIFT` or `POSTGRESQL`); reserved future values fail configuration |

**Note**: ODBC drivers are always built as shared libraries (.dylib/.so/.dll) as required by the ODBC specification.

## Deployment Considerations

### Legacy Static-Preference Build
**Intended advantages after artifact verification:**
- Self-contained executable
- No OpenSSL version dependencies on target system
- Easier deployment and distribution
- Works on systems without OpenSSL development packages

⚠️ **Considerations:**
- Larger executable size (~5.3MB vs ~1.3MB)
- OpenSSL version fixed at build time

### Dynamic Build
✅ **Advantages:**
- Smaller executable size
- Can use system OpenSSL updates
- Shared OpenSSL libraries across applications

⚠️ **Requirements:**
- Target system must have compatible OpenSSL installed
- Version compatibility management required

## Library Structure

```
libodbcpp.dylib (5.3MB) - ODBC Driver
├── ODBC API Layer
│   ├── SQLConnect, SQLExecDirect, SQLFetch
│   ├── Handle management (ENV/DBC/STMT)
│   └── DSN and connection string parsing
├── Database Layer
│   ├── PostgreSQL wire protocol
│   ├── Async connection management
│   └── Connection pooling
└── Transport Layer
    ├── TCP sockets with TLS
    └── Thread pool for async I/O

libodbcpp_core.a (455KB) - Development Library
└── Same components as shared library
    (for linking examples and tests)
```

## Cross-Platform Support

### macOS (Current)
- **Static OpenSSL**: `/opt/homebrew/opt/openssl@3/lib/libssl.a`
- **ODBC Headers**: `/opt/homebrew/include`
- **Architecture**: arm64 (Apple Silicon)

### Linux (Planned)
- **Static OpenSSL**: System package or custom build
- **ODBC Headers**: `/usr/include` (unixodbc-dev)
- **Architecture**: x86_64, arm64

### Windows (Planned)
- **Static OpenSSL**: vcpkg or custom build
- **ODBC Headers**: Windows SDK
- **Additional Libraries**: `ws2_32`, `crypt32`

## Recommended Build Commands

### Production Build (Static)
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED -B build-release
cmake --build build-release --config Release
```

### Development Build (Dynamic)
```bash
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED -B build-debug
cmake --build build-debug --config Debug
```

### Distribution Build (Minimal)
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DODBCPP_CRYPTO_LINKAGE=BUNDLED_STATIC \
      -DODBCPP_CRYPTO_ROOT=/absolute/provider/prefix \
      -DBUILD_TESTING=OFF -B build-dist
cmake --build build-dist --config Release
```

### ODBC Driver Installation
```bash
# Build the driver
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build

# Install system-wide (requires sudo)
cd build
sudo ../install/install-driver.sh

# Test installation
isql -v YourDSNName username password
```

The build system produces both a **shared ODBC driver** for system integration
and a **static library** for development. The legacy default asks CMake to prefer
static OpenSSL; only artifact inspection can establish the actual linkage.
