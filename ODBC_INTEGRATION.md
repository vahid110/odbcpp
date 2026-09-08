# ODBC Integration Guide

## Standard ODBC Configuration Files

### `odbcinst.ini` - Driver Registration
**Purpose**: Registers ODBC drivers with the system ODBC Driver Manager
**Locations**:
- **System**: `/etc/odbcinst.ini` or `/usr/local/etc/odbcinst.ini`
- **User**: `~/.odbcinst.ini`

**Required for**: System-wide driver recognition by ODBC applications

### `odbc.ini` - Data Source Names (DSNs)
**Purpose**: Defines named data sources that applications can connect to
**Locations**:
- **System**: `/etc/odbc.ini` or `/usr/local/etc/odbc.ini`
- **User**: `~/.odbc.ini`

**Required for**: Standard DSN-based connections

## Current ODBCPP Integration Status

### ✅ What Works Now
- **Custom DSN files**: `odbcpp.dsn` format
- **Connection strings**: Direct parameter format
- **Standard locations**: Checks `/etc/odbc.ini`, `~/.odbc.ini`
- **Shared driver**: CMake builds and installs `libodbcpp`
- **ODBC Driver Manager**: unixODBC loading and query execution are covered in CI
- **ODBC API**: DSN-based SQLConnect and DSN-less SQLDriverConnect support

### ⚠️ Missing for Full ODBC Compliance
- **Installer-managed registration**: Registration still uses an `odbcinst.ini` entry
- **Catalog discovery**: Table, column, and primary-key discovery work; foreign-key, index, and procedure APIs remain incomplete

## Integration Paths

### Path 1: Standalone Driver
```bash
# Direct executable usage
./redshift_test "SERVER=host;DATABASE=db;UID=user;PWD=pass"
```

**Pros**: Self-contained, no system dependencies
**Cons**: Not discoverable by standard ODBC tools

### Path 2: ODBC Driver Manager Integration
```bash
# 1. Build and install the shared driver
cmake -DTARGET_DATABASE=POSTGRESQL -B build-postgresql
cmake --build build-postgresql
sudo cmake --install build-postgresql

# 2. Register driver
sudo odbcinst -i -d -f install/odbcinst.ini.template

# 3. Create DSN
odbcinst -i -s -f install/odbc.ini.example
```

**Pros**: Works with all ODBC applications (Excel, Tableau, etc.)
**Cons**: Requires system installation and configuration

### Path 3: Hybrid Approach (Recommended)
- **Standalone executable** for direct usage
- **Shared library option** for ODBC integration
- **Automatic DSN detection** from standard locations

## Implementation Status

### Current DSN Search Order
1. `/etc/odbc.ini` (system-wide)
2. `/usr/local/etc/odbc.ini` (Homebrew)
3. `~/.odbc.ini` (user-specific)
4. `odbcpp.dsn` (local file)
5. `~/odbcpp.dsn` (user local file)

### Connection Methods Supported
```cpp
// Method 1: Connection string
SQLConnect(hdbc, "SERVER=host;PORT=5439;DATABASE=dev;UID=user;PWD=pass", SQL_NTS, NULL, 0, NULL, 0);

// Method 2: DSN name
SQLConnect(hdbc, "DSN=RedshiftProd", SQL_NTS, NULL, 0, NULL, 0);

// Method 3: DSN with overrides
SQLConnect(hdbc, "DSN=RedshiftProd", SQL_NTS, "different_user", SQL_NTS, "different_pass", SQL_NTS);
```

## Next Steps for Full ODBC Integration

### 1. Shared Library Build
```cmake
option(BUILD_SHARED_LIBS "Build shared library for ODBC Driver Manager" OFF)
```

### 2. Driver Manager Entry Points
```cpp
// Required ODBC Driver Manager functions
SQLRETURN SQLGetInfo(SQLHDBC hdbc, SQLUSMALLINT fInfoType, SQLPOINTER rgbInfoValue, SQLSMALLINT cbInfoValueMax, SQLSMALLINT *pcbInfoValue);
SQLRETURN SQLGetFunctions(SQLHDBC hdbc, SQLUSMALLINT fFunction, SQLUSMALLINT *pfExists);
SQLRETURN SQLGetTypeInfo(SQLHSTMT hstmt, SQLSMALLINT fSqlType);
```

### 3. Installation Scripts
```bash
#!/bin/bash
# install-odbcpp.sh
sudo cp libodbcpp.so /usr/local/lib/
sudo odbcinst -i -d -f odbcinst.ini.template
echo "ODBCPP driver installed successfully"
```

## Application Compatibility

### Works Now
- **Direct integration**: Applications linking to our library
- **unixODBC SQLConnect**: Applications connecting through registered DSNs
- **Custom tools**: Using our connection string format
- **Manual DSN**: Applications reading standard odbc.ini files

### Requires Full Integration
- **Excel**: Needs driver in odbcinst.ini
- **Tableau**: Needs ODBC Driver Manager registration
- **Power BI**: Needs Windows ODBC registry entries
- **Catalog-driven clients**: Need broader metadata APIs

## Recommendation

**Current approach is optimal for:**
- Embedded applications
- Custom database tools
- Containerized deployments
- Development and testing

**Full ODBC integration needed for:**
- Enterprise BI tools
- Standard ODBC applications
- System-wide deployment
- End-user installations
