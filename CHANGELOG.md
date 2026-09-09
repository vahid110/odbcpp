# Changelog

All notable changes to the ODBCPP project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- SQLConnectW, SQLDriverConnectW, SQLExecDirectW, SQLPrepareW, SQLGetDiagRecW, and SQLNativeSqlW Unicode entry points
- Strict UTF-8/SQLWCHAR conversion with surrogate-pair and malformed-input validation
- SQL_C_WCHAR result retrieval, chunking, column binding, and prepared-statement parameters
- Wide-character variants of all eight catalog APIs, including Unicode identifier discovery
- Unicode unit, PostgreSQL integration, and unixODBC driver-manager coverage
- SQLMoreResults traversal of ordered PostgreSQL multi-statement results and SQL_ATTR_MAX_ROWS enforcement
- SQLFetchScroll support for forward-only SQL_FETCH_NEXT cursors
- Forward-only cursor, read-only concurrency, and single-row binding attribute defaults
- SQLDriverConnect support for DSN-less driver-manager connections
- SQLGetFunctions support for individual, ODBC 2.x array, and ODBC 3.x bitmap queries
- SQLCloseCursor and SQLFreeStmt lifecycle operations
- SQLGetTypeInfo metadata for the supported PostgreSQL type set
- SQLNumParams with quote- and comment-aware ODBC parameter marker counting
- SQLGetEnvAttr and validated ODBC environment version selection
- SQLNativeSql pass-through for PostgreSQL-native SQL text
- SQLTables catalog discovery with ODBC patterns and table-type filtering
- SQLColumns catalog discovery with PostgreSQL type and nullability mapping
- SQLPrimaryKeys discovery with ordered PostgreSQL key columns
- SQLForeignKeys discovery with ODBC update, delete, and deferrability mappings
- SQLStatistics discovery for PostgreSQL indexes, uniqueness, ordering, and predicates
- SQLProcedures discovery for PostgreSQL functions and procedures
- SQLProcedureColumns discovery for PostgreSQL routine parameters and return values
- SQLSpecialColumns best-row identifiers backed by PostgreSQL primary keys
- SQL_ATTR_AUTOCOMMIT and connection-level SQLEndTran commit/rollback support
- SQL_ATTR_TXN_ISOLATION with PostgreSQL session-level isolation configuration
- unixODBC end-to-end coverage that dynamically loads the shared driver
- SQLGetInfo reporting for transaction capability, isolation, and cursor behavior
- PostgreSQL bytea decoding for SQL_C_BINARY and SQL_C_DEFAULT results
- Chunked SQLGetData retrieval for binary values
- SQL_C_BINARY parameter encoding for PostgreSQL bytea prepared statements
- SQLSetConnectAttr/SQLGetConnectAttr support for SQL_ATTR_LOGIN_TIMEOUT
- SQLSetStmtAttr/SQLGetStmtAttr support for SQL_ATTR_QUERY_TIMEOUT
- PostgreSQL SCRAM-SHA-256 authentication with server-signature verification
- PostgreSQL 17 SCRAM-SHA-256 integration coverage across transport modes
- Chunked SQLGetData retrieval for long character values across repeated calls
- Shared text-result conversion for small integers, integers, big integers, floating-point values, booleans, dates, times, and timestamps
- Metadata-driven SQL_C_DEFAULT handling for SQLGetData and bound columns
- Null-aware result cells that preserve PostgreSQL NULL separately from empty strings
- SQLSTATE 22002 reporting when a fetched NULL has no indicator variable
- PostgreSQL RowDescription and ParameterDescription decoding with ODBC type mapping
- SQLRowCount support backed by PostgreSQL CommandComplete row counts
- Native PostgreSQL extended-query messages (Parse, Bind, Describe, Execute, Sync)
- Typed and SQL NULL-aware parameter values from SQLBindParameter
- Linux AddressSanitizer and UndefinedBehaviorSanitizer CI coverage
- Complete ODBC descriptor API suite (SQLBindCol, SQLDescribeParam)
- Enhanced SQLFetch with automatic bound column population
- Comprehensive test coverage (19 unit and integration tests)
- Column binding example and documentation
- Development roadmap and changelog

### Changed
- Installation now includes the shared ODBC driver library
- ODBC login and query timeouts now drive transport deadlines and report HYT01/HYT00
- Timed-out queries close their connection to prevent reuse of an unsynchronized protocol stream
- SQLGetData now reports the remaining length before each chunk and returns SQL_NO_DATA after exhaustion
- Continuous integration now builds the native POSTGRESQL target on Linux and Windows
- Result conversions use PostgreSQL column metadata instead of assuming SQL_VARCHAR
- SQLFetch and SQLGetData now propagate protocol-level NULL indicators without modifying application buffers
- Asynchronous query extraction now preserves complete QueryResult metadata
- Updated README with current implementation status
- Enhanced fetch mechanism to support bound columns
- Improved test matrix with descriptor API coverage

### Fixed
- The declared POSTGRESQL build target no longer depends on missing database-specific converter files
- Numeric and temporal conversion validation now rejects overflow, trailing characters, invalid booleans, and impossible dates
- SQL injection test handling for malicious input
- Prepared-query SQL injection risk from client-side parameter substitution
- Connection synchronization after PostgreSQL query errors
- DSN loading from build directory

## [0.9.0] - 2024-12-XX

### Added
- Complete 4 ODBC descriptor implementations (IRD, APD, ARD, IPD)
- SQLBindCol API for column binding
- SQLDescribeParam API for parameter metadata
- Descriptor field access framework
- 23 core ODBC API functions implemented
- Comprehensive prepared statement support
- Real database integration with Redshift
- Parameter substitution with type detection
- 17 comprehensive tests (10 unit + 7 integration)

### Changed
- Enhanced SQLFetch to auto-populate bound columns
- Improved parameter binding with data type conversion
- Updated build system for comprehensive test coverage

### Fixed
- Prepared statement execution with real database
- Parameter substitution for ODBC-style placeholders
- Connection string parsing and DSN loading

## [0.8.0] - 2024-12-XX

### Added
- Prepared statement support (SQLPrepare, SQLExecute, SQLBindParameter)
- APD (Application Parameter Descriptor) implementation
- Parameter binding with comprehensive data type support
- Prepared statement test matrix with real database validation
- SQL injection prevention through parameter binding

### Changed
- Enhanced PostgreSQL protocol parser for prepared statements
- Improved parameter substitution mechanism
- Updated test suite with prepared statement coverage

### Fixed
- Parameter binding for multiple data types
- Prepared statement reuse and execution
- Error handling for invalid parameters

## [0.7.0] - 2024-12-XX

### Added
- Result set metadata APIs (SQLNumResultCols, SQLDescribeCol, SQLColAttribute)
- IRD (Implementation Row Descriptor) implementation
- Data type conversion framework
- Comprehensive metadata test coverage
- Cross-platform build system improvements

### Changed
- Enhanced ODBC handle management
- Improved error handling and diagnostics
- Updated documentation with metadata examples

### Fixed
- Column metadata extraction and conversion
- Data type mapping between SQL and C types
- Memory management in result processing

## [0.6.0] - 2024-12-XX

### Added
- Core ODBC API framework (17 essential functions)
- Handle management (SQLAllocHandle, SQLFreeHandle)
- Connection management (SQLConnect, SQLDisconnect)
- Statement execution (SQLExecDirect, SQLFetch, SQLGetData)
- Error handling (SQLGetDiagRec)
- Driver information (SQLGetInfo, SQLSetEnvAttr)

### Changed
- Established ODBC-compliant architecture
- Implemented handle registry system
- Added comprehensive error handling

### Fixed
- Handle validation and lifecycle management
- Connection string parsing
- Basic query execution workflow

## [0.5.0] - 2024-12-XX

### Added
- PostgreSQL wire protocol implementation
- Redshift database support
- TLS/SSL connection support
- Basic connection pooling
- Async database operations

### Changed
- Modular database protocol architecture
- Enhanced transport layer with TLS support
- Improved connection management

### Fixed
- Network protocol handling
- SSL/TLS handshake implementation
- Connection stability and error recovery

## [0.4.0] - 2024-12-XX

### Added
- Database factory pattern
- Generic database connection interface
- Transport layer abstraction
- Socket and TLS transport implementations
- Deadline-based timeout handling

### Changed
- Pluggable database architecture
- Improved error handling with Result types
- Enhanced async operation support

### Fixed
- Memory management in database operations
- Thread safety in connection handling
- Resource cleanup and lifecycle management

## [0.3.0] - 2024-12-XX

### Added
- Core database abstraction layer
- Protocol parser interface
- Basic PostgreSQL message handling
- Authentication support (cleartext, MD5)
- Query execution framework

### Changed
- Established database protocol abstraction
- Improved message parsing architecture
- Enhanced authentication mechanisms

### Fixed
- Protocol message parsing
- Authentication flow handling
- Query result processing

## [0.2.0] - 2024-12-XX

### Added
- CMake build system
- Cross-platform support (macOS, Linux, Windows)
- OpenSSL integration
- Basic project structure
- Example applications

### Changed
- Modular build configuration
- Platform-specific optimizations
- Dependency management

### Fixed
- Build system compatibility
- Library linking issues
- Platform-specific compilation

## [0.1.0] - 2024-12-XX

### Added
- Initial project structure
- Basic C++20 framework
- Core utility classes
- Error handling foundation
- Platform abstraction layer

### Changed
- Established coding standards
- Set up development environment
- Created initial architecture

---

## Legend

- **Added** for new features
- **Changed** for changes in existing functionality  
- **Deprecated** for soon-to-be removed features
- **Removed** for now removed features
- **Fixed** for any bug fixes
- **Security** for vulnerability fixes
