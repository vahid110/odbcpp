# Changelog

All notable changes to the ODBCPP project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

These notes describe development changes, not a published release. For exact
source history, see [main commits](https://github.com/vahid110/odbcpp/commits/main).

### Added
- Operating-system TLS trust stores: Windows CryptoAPI chain policy, macOS Security framework evaluation and Linux/Unix distribution CA bundles, selected by default or with `UseSystemTrustStore=1`; custom `SSLCAFILE`/`SSLCADIR` trust fails closed on missing, empty or malformed input and never falls back. The Windows DSN Security page distinguishes inherited driver defaults from an explicit operating-system trust-store selection.
- Indicator-only and row-wise result bindings, checked row and single-set parameter-binding offsets, and SQL_ATTR_RETRIEVE_DATA suppression with reset/rebind recovery
- Expanded SQLGetInfo/SQLGetInfoW identity, naming, catalog, schema, cursor, and SQLGetData capability reporting
- Explicit descriptor-handle allocation, SQLGetDescField/SQLSetDescField storage, and SQLCopyDesc cloning
- SQLConnectW, SQLDriverConnectW, SQLExecDirectW, SQLPrepareW, SQLGetDiagRecW, and SQLNativeSqlW Unicode entry points
- Strict UTF-8/SQLWCHAR conversion with surrogate-pair and malformed-input validation
- SQL_C_WCHAR result retrieval, chunking, column binding, and prepared-statement parameters
- Wide-character variants of all eight catalog APIs, including Unicode identifier discovery
- SQLDescribeColW, SQLColAttributeW, SQLGetInfoW, SQLGetDiagFieldW, and SQLErrorW compatibility APIs
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
- Expanded unit and integration tests; discover the current inventory with CTest
- Column binding example and documentation
- Development roadmap and changelog

### Changed
- Verified TLS without `SSLCAFILE`/`SSLCADIR` now uses the operating-system trust store instead of the crypto provider's compiled default verify paths
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
- Security tooling now rejects input/output file aliases before destructive cleanup in the MySQL test helpers; original failure evidence is retained.
- Dependency downloads pin the GoogleTest archive digest. CI action references and candidate cache permissions are being hardened as part of the security baseline.
- The declared POSTGRESQL build target no longer depends on missing database-specific converter files
- Numeric and temporal conversion validation now rejects overflow, trailing characters, invalid booleans, and impossible dates
- SQL injection test handling for malicious input
- Prepared-query SQL injection risk from client-side parameter substitution
- Connection synchronization after PostgreSQL query errors
- DSN loading from build directory

## Historical draft entries

[Earlier undated entries](docs/history/changelog-undated-entries.md) are preserved
as historical draft notes, not authenticated release records. No formal release
is asserted by those placeholder dates. Development snapshots are identified by
full commit and build evidence; the CMake version alone is not unique.
