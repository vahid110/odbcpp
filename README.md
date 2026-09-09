# ODBCPP - Multi-Database ODBC Driver Framework

A modern C++20 framework for building database-specific ODBC drivers with pluggable protocol support.

## Features

- **ODBC API Foundation**: 73 entry points (47 non-wide, 26 Unicode) with diagnostics and descriptor storage
- **Unicode SQL and Data**: Strict UTF-8/SQLWCHAR conversion for wide connections, execution, preparation, diagnostics, native SQL, results, and parameters
- **ODBC Descriptors**: IRD, APD, ARD, and IPD storage plus explicit descriptor handles and field APIs
- **Prepared Statements**: Typed/null-aware PostgreSQL Parse/Bind/Describe/Execute workflow
- **Column Binding**: SQLBindCol with automatic data population during fetch
- **Null-Aware Results**: PostgreSQL NULL values remain distinct from empty strings through SQLFetch and SQLGetData
- **Typed Results**: PostgreSQL metadata drives SQL_C_DEFAULT and validated C-type conversions
- **Large Values**: SQLGetData supports repeated, chunked retrieval of character and binary data
- **Multiple Results**: PostgreSQL multi-statement results remain distinct through SQLMoreResults
- **Binary Results**: PostgreSQL bytea maps to SQL_VARBINARY/SQL_C_BINARY with hex and escape decoding
- **Binary Parameters**: SQL_C_BINARY parameters round-trip through native PostgreSQL bytea bindings
- **Single Database Per Build**: Each build targets one specific database for optimal size and performance
- **Pluggable Architecture**: Easy to add new database protocols
- **Modern C++20**: Clean, type-safe interfaces
- **Secure Transport**: Built-in TLS/SSL support via OpenSSL
- **Configurable Driver Logging**: Asynchronous rotating files, stderr, or syslog with text/JSON output and connection/query timing
- **Modern Authentication**: PostgreSQL cleartext, MD5, and SCRAM-SHA-256 password authentication
- **Cross-Platform**: macOS, Linux, Windows support
- **Cross-Platform Testing**: Unit, PostgreSQL integration, unixODBC and mixed-width iODBC driver-manager, sanitizer, and Windows CI coverage

## Implementation Status

The entries below describe implemented surface area, not a claim of complete
ODBC conformance. See the [conformance and hardening audit](ODBC_CONFORMANCE_AUDIT.md)
for attribute, state, diagnostic, negative-test, and maintainability gaps.

### ODBC API Coverage
| Component | Status | Functions |
|-----------|--------|----------|
| **Core APIs** | Implemented; under audit | SQLAllocHandle, SQLFreeHandle, SQLConnect, SQLDriverConnect, SQLDisconnect |
| **Timeout Attributes** | Partial | SQL_ATTR_LOGIN_TIMEOUT and SQL_ATTR_QUERY_TIMEOUT set/get APIs |
| **Transactions** | Partial | SQL_ATTR_AUTOCOMMIT, SQL_ATTR_TXN_ISOLATION, SQLEndTran, capability reporting |
| **Statement Execution** | Partial | SQLExecDirect, forward fetch, SQLGetData, SQLRowCount, SQLMoreResults, cursor cleanup |
| **Prepared Statements** | Partial | SQLPrepare, SQLExecute, input SQLBindParameter, SQLNumParams |
| **Unicode APIs** | Implemented for current surface; parity audit active | 26 `W` entry points plus SQL_C_WCHAR results, binding, and parameters |
| **Column Binding** | Scalar rows only | SQLBindCol with automatic data population; row arrays remain pending |
| **Metadata** | Implemented subset | Result metadata, type info, and 8 ANSI/wide catalog operations |
| **Parameter Metadata** | Partial | SQLDescribeParam is populated after server execution |
| **Diagnostics** | Partial | Multi-record SQLGetDiagRec/Field and legacy SQLError |
| **Driver Info** | Implemented subset | SQLGetInfo, SQLGetFunctions, SQLNativeSql, environment attributes |

### ODBC Descriptors
| Descriptor | Status | Purpose |
|------------|--------|----------|
| **IRD** | Storage implemented; under audit | Implementation Row Descriptor - result column metadata |
| **APD** | Storage implemented; under audit | Application Parameter Descriptor - parameter binding |
| **ARD** | Storage implemented; under audit | Application Row Descriptor - column binding |
| **IPD** | Storage implemented; under audit | Implementation Parameter Descriptor - parameter metadata |

## Supported Databases

| Database | Status | Protocol |
|----------|--------|----------|
| **Redshift** | Compatibility target; revalidation deferred | PostgreSQL Wire Protocol |
| **PostgreSQL** | Primary target; integration validated | PostgreSQL Wire Protocol |
| **MySQL** | 🚧 Planned | MySQL Protocol |
| **SQL Server** | 🚧 Planned | TDS Protocol |

## Quick Start

### Prerequisites

- CMake 3.20+
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- OpenSSL 3.0+
- unixODBC or iODBC (for system integration)

### Build ODBC Driver

```bash
# Build database-specific driver (recommended)
./build-database.sh redshift

# Or build manually
cmake -DTARGET_DATABASE=REDSHIFT -B build-redshift
cmake --build build-redshift
```

### ODBC Configuration Setup

```bash
# Setup test environment with local ODBC configuration
source ./setup-test-env.sh

# Verify configuration
echo $ODBCINI    # Should point to ./odbc.ini
echo $ODBCSYSINI # Should point to current directory
```

### Build for Specific Database

```bash
# Redshift ODBC driver
./build-database.sh redshift

# PostgreSQL ODBC driver  
./build-database.sh postgresql

# Minimal build (no examples/tests)
./build-database.sh minimal

# Debug build
./build-database.sh redshift --debug
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

#### Column Binding (Advanced)
```cpp
#include <sql.h>
#include <sqlext.h>

int main() {
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt;
    
    // Setup handles
    SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS, nullptr, 0, nullptr, 0);
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    
    // Execute query
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT name, age, salary FROM employees", SQL_NTS);
    
    // Bind columns to variables
    char name[256];
    SQLINTEGER age;
    SQLDOUBLE salary;
    SQLLEN name_len, age_len, salary_len;
    
    SQLBindCol(hstmt, 1, SQL_C_CHAR, name, sizeof(name), &name_len);
    SQLBindCol(hstmt, 2, SQL_C_SLONG, &age, 0, &age_len);
    SQLBindCol(hstmt, 3, SQL_C_DOUBLE, &salary, 0, &salary_len);
    
    // Fetch automatically populates bound variables
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        printf("Employee: %s, Age: %d, Salary: %.2f\n", name, age, salary);
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
| `ODBC_DRIVER_MANAGER_FLAVOR` | AUTO | Driver manager for the external integration test: `AUTO`, `UNIXODBC`, or `IODBC` |
| `ODBC_DRIVER_MANAGER_INCLUDE_DIR` | — | Application-side ODBC headers for mixed-ABI driver-manager tests |
| `ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE` | — | Fail the build unless the driver headers define a 2- or 4-byte `SQLWCHAR` as expected |
| `ODBCPP_EXPECT_DM_SQLWCHAR_SIZE` | — | Fail the external test build unless its application headers have the expected width |

### Driver logging

Logging is disabled by default. Configure it in the driver section of
`odbcinst.ini`, a DSN, or a connection string. The precedence is the same as
transport settings: connection string, DSN, driver section, then defaults.

```ini
[ODBCPP PostgreSQL]
LogLevel=Info
LogSink=File
LogFile=/var/log/odbcpp/driver.log
LogMaxSize=10485760
LogMaxFiles=5
LogAsync=true
LogFormat=Json
LogQueries=false
```

`LogLevel` accepts `Off`, `Error`, `Warn`, `Info`, `Debug`, or `Trace`.
`LogSink` accepts a comma-separated combination of `File`, `Stderr`, and
`Syslog` (syslog is available on POSIX systems). File logs rotate at
`LogMaxSize` bytes and retain `LogMaxFiles` archives. Every record includes a
connection ID; connection and query completion records include elapsed time,
row counts, and affected-row counts.

SQL text is never logged unless both `LogLevel=Debug|Trace` and
`LogQueries=true` are set. Parameter values and passwords are never logged.
JSON output escapes control characters and is suitable for log processors.

### Driver-manager Unicode ABI

`SQLWCHAR` is not the same size in every ODBC environment. Windows and a
default unixODBC build use a two-byte UTF-16 code unit. iODBC on Unix uses
native `wchar_t`, which is normally four-byte UCS-4 on macOS and Linux.
unixODBC can also be built in its optional four-byte mode.

ODBCPP advertises its compiled representation through iODBC's
`SQL_ATTR_DRIVER_UNICODE_TYPE` extension and handles
`SQL_ATTR_APP_WCHAR_TYPE`. An iODBC application can set
`SQL_ATTR_APP_UNICODE_TYPE`; iODBC then converts between its application
representation and the driver's representation. Register the normal
two-byte ODBCPP build with this fallback setting:

```cpp
SQLSetEnvAttr(environment, SQL_ATTR_APP_UNICODE_TYPE,
              reinterpret_cast<SQLPOINTER>(SQL_DM_CP_UCS4),
              SQL_IS_UINTEGER);
```

These constants are provided by iODBC's `iodbcext.h`. Set the attribute on
the environment before allocating connections. The registration setting is
still useful as a driver-wide fallback:

```ini
[ODBCPP PostgreSQL]
Driver=/path/to/libodbcpp.dylib
DriverUnicodeType=UTF16
```

This lets a four-byte iODBC application use the regular UTF-16 driver build.
For a native iODBC/UCS-4 build, compile against the iODBC headers, set
`ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=4`, and register
`DriverUnicodeType=UCS4`. The CI suite validates the mixed case directly: a
four-byte iODBC application loads the two-byte driver and round-trips Unicode,
including a supplementary-plane character, through PostgreSQL.

### Build Scripts

**build-database.sh** - Database-specific builds:
```bash
# Show all options
./build-database.sh --help

# Build specific database drivers
./build-database.sh redshift     # Redshift-only driver
./build-database.sh postgresql   # PostgreSQL-only driver
./build-database.sh minimal      # Minimal Redshift (no examples/tests)

# Build options
./build-database.sh redshift --debug  # Debug build
./build-database.sh redshift --clean  # Clean first
```

**setup-test-env.sh** - ODBC environment setup:
```bash
# Source to set environment variables
source ./setup-test-env.sh

# Or run to check configuration
./setup-test-env.sh
```

### CMake Presets (Alternative)

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

## ODBC Configuration

### Local Configuration (Recommended for Development)

The project includes local ODBC configuration files for development and testing:

```bash
# Setup local ODBC environment
source ./setup-test-env.sh

# This sets:
# ODBCINI=./odbc.ini          # Data source definitions
# ODBCSYSINI=./               # Driver definitions (odbcinst.ini)
```

### Configuration Files

**odbc.ini** - Data Source Names (DSNs):
```ini
[RedshiftTest]
Driver = ODBCPP
Description = Redshift Test Database
Server = your-cluster.redshift.amazonaws.com
Port = 5439
Database = dev
SSL = true
TransportMode = Auto
DeadlineModel = Strict

[PostgreSQLTest] 
Driver = ODBCPP
Description = PostgreSQL Test Database
Server = localhost
Port = 5432
Database = testdb
SSL = false
TransportMode = Sync
DeadlineModel = SocketTimeout
```

**odbcinst.ini** - Driver Definitions:
```ini
[ODBCPP]
Description = ODBCPP Multi-Database Driver
Driver64 = /path/to/build-redshift/libodbcpp.dylib
Setup64 = /path/to/build-redshift/libodbcpp.dylib
FileUsage = 1
TransportMode = Auto
AsyncMaxInflight = 64
AsyncQueueDepth = 256
AsyncEngine = Auto
DeadlineModel = Strict
```

Transport settings use the following precedence, from highest to lowest:
connection string, DSN, driver section in `odbcinst.ini`, built-in defaults.
With `DeadlineModel=Strict`, `TransportMode=Auto` selects the platform-native
reactor: epoll on Linux and IOCP on Windows. `TransportMode=Async` selects the
same path explicitly, and `AsyncEngine=Epoll|IOCP` can require a particular
engine. Native asynchronous mode supports TLS, including PostgreSQL's in-band
SSL upgrade. `Auto` falls back to the synchronous transport for
`DeadlineModel=SocketTimeout` and on platforms without a native engine.

### System Installation (Optional)

```bash
# Install driver system-wide
sudo cp build-redshift/libodbcpp.dylib /usr/local/lib/
sudo odbcinst -i -d -f odbcinst.ini

# Add DSNs system-wide
sudo odbcinst -i -s -f odbc.ini
```

## Testing

### Test Coverage
- **Total Test Executables**: 31 (22 unit + 9 integration)
- **CI Coverage**: Linux, Windows, sanitizers, and mixed-width iODBC Unicode
- **Current Focus**: Transport, PostgreSQL protocol, core ODBC behavior, Unicode,
  descriptors, prepared statements, column binding, diagnostics, and logging;
  the audit matrix records untested and partial behavior

### Running Tests

```bash
# Setup test environment first
source ./setup-test-env.sh

# Build and run all tests
./build-database.sh redshift
ctest --test-dir build-redshift

# Run specific test categories
ctest --test-dir build-redshift -L unit         # 22 unit test executables
ctest --test-dir build-redshift -L integration  # 9 integration executables

# Test specific functionality
ctest --test-dir build-redshift -R prepared_statements
ctest --test-dir build-redshift -R descriptor_apis
ctest --test-dir build-redshift -R bind_col
```

### Test Requirements

- **Unit Tests**: No external dependencies
- **Integration Tests**: Require valid DSN configuration in odbc.ini
- **Database Access**: Integration tests need actual Redshift/PostgreSQL connection
- **PostgreSQL Authentication**: The protocol layer supports cleartext, MD5, and
  SCRAM-SHA-256 password authentication. CI validates SCRAM-SHA-256 against
  PostgreSQL 17.

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
