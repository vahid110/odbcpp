# ODBCPP Build System Configuration

## Static vs Dynamic Linking

### Current Configuration (Default)
- **Core Library**: ✅ **STATIC** (`libodbcpp_core.a`)
- **OpenSSL**: ✅ **STATIC** (default: `OPENSSL_USE_STATIC_LIBS=ON`)
- **System Libraries**: Dynamic (libc++, libSystem - required by OS)

### Build Outputs

#### Static Build (Default)
```bash
cmake -B build
cmake --build build
```

**Dependencies:**
- `libc++.1.dylib` (system C++ runtime)
- `libSystem.B.dylib` (system library)
- **OpenSSL**: Statically linked (no external .dylib dependencies)

**File Size:** ~5.3MB (includes OpenSSL)

#### Dynamic Build (Optional)
```bash
cmake -DOPENSSL_USE_STATIC_LIBS=OFF -B build-dynamic
cmake --build build-dynamic
```

**Dependencies:**
- `libc++.1.dylib` (system C++ runtime)
- `libSystem.B.dylib` (system library)
- `libssl.3.dylib` (OpenSSL SSL)
- `libcrypto.3.dylib` (OpenSSL Crypto)

**File Size:** ~1.3MB (requires OpenSSL installed on target system)

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `OPENSSL_USE_STATIC_LIBS` | `ON` | Link OpenSSL statically for portability |
| `BUILD_EXAMPLES` | `ON` | Build example applications |
| `BUILD_TESTING` | `ON` | Build unit and integration tests |
| `TARGET_DATABASE` | `REDSHIFT` | Target database (REDSHIFT/POSTGRESQL/MYSQL/SQLSERVER) |

## Deployment Considerations

### Static Build (Recommended)
✅ **Advantages:**
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
libodbcpp_core.a (3.0MB)
├── ODBC API Layer
│   ├── odbc_api.cpp.o
│   ├── odbc_handles.cpp.o
│   └── connection_string.cpp.o
├── Database Layer
│   ├── async_database_connection.cpp.o
│   ├── connection_pool.cpp.o
│   ├── database_factory.cpp.o
│   └── postgres/pg_protocol_parser.cpp.o
└── Transport Layer
    ├── socket_transport.cpp.o
    ├── thread_pool_transport.cpp.o
    └── tls_transport.cpp.o
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
cmake -DCMAKE_BUILD_TYPE=Release -DOPENSSL_USE_STATIC_LIBS=ON -B build-release
cmake --build build-release --config Release
```

### Development Build (Dynamic)
```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_USE_STATIC_LIBS=OFF -B build-debug
cmake --build build-debug --config Debug
```

### Distribution Build (Minimal)
```bash
cmake -DCMAKE_BUILD_TYPE=Release -DOPENSSL_USE_STATIC_LIBS=ON -DBUILD_TESTING=OFF -B build-dist
cmake --build build-dist --config Release
```

The build system is optimized for **static linking by default** to ensure maximum portability and ease of deployment.