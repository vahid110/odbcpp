# ODBCPP - Multi-Database ODBC Driver Framework

A modern C++20 framework for building database-specific ODBC drivers with pluggable protocol support.

## Features

- **Single Database Per Build**: Each build targets one specific database for optimal size and performance
- **Pluggable Architecture**: Easy to add new database protocols
- **Modern C++20**: Clean, type-safe interfaces
- **Secure Transport**: Built-in TLS/SSL support via OpenSSL
- **Cross-Platform**: macOS, Linux, Windows support
- **Comprehensive Testing**: Unit and integration tests included

## Supported Databases

| Database | Status | Protocol |
|----------|--------|----------|
| **Redshift** | ✅ Ready | PostgreSQL Wire Protocol |
| **PostgreSQL** | ✅ Ready | PostgreSQL Wire Protocol |
| **MySQL** | 🚧 Planned | MySQL Protocol |
| **SQL Server** | 🚧 Planned | TDS Protocol |

## Quick Start

### Prerequisites

- CMake 3.20+
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- OpenSSL 3.0+

### Build ODBC Driver

```bash
# Build both static library and ODBC driver
cmake -B build
cmake --build build

# Install as system ODBC driver (optional)
cd build
sudo ../install/install-driver.sh
```

### Build for Specific Database

```bash
# Redshift (default)
cmake -DTARGET_DATABASE=REDSHIFT -B build-redshift
cmake --build build-redshift

# PostgreSQL
cmake -DTARGET_DATABASE=POSTGRESQL -B build-postgresql
cmake --build build-postgresql
```

### Usage

#### Standard ODBC (Recommended)
```cpp
#include <sql.h>
#include <sqlext.h>

int main() {
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt;
    
    // Allocate handles
    SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    
    // Connect using DSN or connection string
    SQLConnect(hdbc, 
        (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS,
        (SQLCHAR*)"username", SQL_NTS,
        (SQLCHAR*)"password", SQL_NTS);
    
    // Execute query
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT version()", SQL_NTS);
    
    // Fetch results
    char version[512];
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        SQLGetData(hstmt, 1, SQL_C_CHAR, version, sizeof(version), nullptr);
        printf("Version: %s\n", version);
    }
    
    // Cleanup
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    
    return 0;
}
```

#### Direct Library Usage
```cpp
#include "core/database/database_factory.h"

using namespace rs::core::database;

int main() {
    // Create connection for compiled database
    auto conn = DatabaseFactory::create_connection();
    
    // Configure connection
    ConnectionSettings settings;
    settings.host = "your-redshift-cluster.amazonaws.com";
    settings.port = 5439;
    settings.user = "username";
    settings.password = "password";
    settings.database = "dev";
    settings.use_ssl = true;
    
    // Connect and query
    conn->connect(settings);
    
    auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
    auto result = conn->execute_query("SELECT version()", deadline);
    
    std::cout << "Version: " << result.rows[0][0] << std::endl;
    
    return 0;
}
```

## Build Options

### Database Selection

| Option | Description |
|--------|-------------|
| `-DTARGET_DATABASE=REDSHIFT` | Build Redshift ODBC driver |
| `-DTARGET_DATABASE=POSTGRESQL` | Build PostgreSQL ODBC driver |
| `-DTARGET_DATABASE=MYSQL` | Build MySQL ODBC driver (future) |
| `-DTARGET_DATABASE=SQLSERVER` | Build SQL Server ODBC driver (future) |

### Additional Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_EXAMPLES` | ON | Build example applications |
| `BUILD_TESTING` | ON | Build unit and integration tests |
| `OPENSSL_USE_STATIC_LIBS` | OFF | Link OpenSSL statically |

### CMake Presets

```bash
# List available presets
cmake --list-presets

# Use preset
cmake --preset redshift
cmake --build --preset redshift
```

## Examples

The `examples/` directory contains:

- **query_example**: Basic connection and query execution
- **pg_handshake_example**: Connection establishment and server parameters
- **extended_query_example**: Prepared statements and transactions
- **multi_database_example**: Shows database-specific build behavior

Run examples:
```bash
# After building
./build-redshift/examples/query_example your-host.com 5439 mydb user pass
```

## Testing

```bash
# Run all tests
cmake --build build-redshift
ctest --test-dir build-redshift

# Run specific test categories
ctest --test-dir build-redshift -L unit
ctest --test-dir build-redshift -L integration
```

## Architecture

```
core/
├── database/           # Database abstraction layer
│   ├── i_database_connection.h    # Generic connection interface
│   ├── i_protocol_parser.h        # Protocol parser interface
│   ├── database_factory.{h,cpp}   # Factory for database connections
│   ├── generic_database_connection.{h,cpp}  # Generic implementation
│   └── postgres/       # PostgreSQL/Redshift protocol implementation
│       ├── pg_protocol_parser.{h,cpp}
│       └── pg_messages.h
├── transport/          # Network transport layer
│   ├── i_transport.h   # Transport interface
│   ├── socket_transport.{h,cpp}   # TCP sockets
│   └── tls_transport.{h,cpp}      # TLS/SSL transport
└── util/              # Utilities
    ├── deadline.h     # Timeout handling
    ├── errors.h       # Error types
    └── platform.h     # Platform abstractions
```

## Adding New Database Support

1. **Create protocol parser**:
   ```bash
   mkdir -p core/database/mysql
   # Implement mysql_protocol_parser.{h,cpp}
   ```

2. **Implement IProtocolParser interface**:
   ```cpp
   class MySQLProtocolParser : public IProtocolParser {
       // Implement all virtual methods
   };
   ```

3. **Update factory** (automatic via conditional compilation)

4. **Build**:
   ```bash
   cmake -DTARGET_DATABASE=MYSQL -B build-mysql
   ```

## Contributing

1. Fork the repository
2. Create feature branch: `git checkout -b feature/mysql-support`
3. Make changes and add tests
4. Ensure all tests pass: `ctest --test-dir build`
5. Submit pull request

## License

[Your License Here]

## Support

- **Issues**: [GitHub Issues](https://github.com/your-repo/issues)
- **Documentation**: See `examples/` directory
- **Architecture**: See `docs/` directory (if available)