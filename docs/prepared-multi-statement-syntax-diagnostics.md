# Prepared multi-statement syntax diagnostics

## Summary

ODBCPP preserves the server's specific syntax-error SQLSTATE `42601` for rejected
prepared multi-statement batches on its PostgreSQL and Redshift products, matching
the diagnostics emitted by native PostgreSQL, Amazon Redshift native ODBC, and
Simba Redshift ODBC drivers.

## Diagnostic contract and native SQLSTATE 42601

In standard SQL (ISO/IEC 9075:2003 Part 2 Foundation, Section 22.1):
- Class `42` designates **Syntax Error or Access Rule Violation**.
- Subclass codes beginning with characters `5` through `9` or `I` through `Z`
  are reserved for implementation-defined subclasses.
- Subclass `000` is the generic unclassified fallback ("Syntax error or access rule violation").

`42601` is PostgreSQL's and Amazon Redshift's implementation-defined subclass
designating query syntax errors (`syntax_error`), including the extended query
protocol rejection:
`ERROR: cannot insert multiple commands into a prepared statement`.

Previously, ODBCPP's `PgBackendProvider::normalize_error_sqlstate` returned
`std::nullopt` for `42601`, causing `query_failure_sqlstate` to collapse it to
the caller-supplied fallback (`42000` `SQLSTATE_SYNTAX_ERROR` or `HY000`
`SQLSTATE_GENERAL_ERROR`). By preserving `42601`:
1. Applications, ORMs, and Driver Managers can distinguish a concrete syntax error
   (`42601`) from general access rule violations or ambiguous errors (`42000`).
2. Diagnostic parity is maintained with vendor ODBC drivers (Amazon native
   2.2.4.0 and Amazon/Simba 1.6.3.1008 both return `42601`).
3. Arbitrary internal or undocumented vendor states are not blindly passed through:
   unmapped vendor-specific states continue to fall back to the caller-supplied
   fallback (`HY000` or `42000`).
4. Driver-internal syntax errors (such as escape syntax parsing in ODBCPP's escape
   parser) remain `42000` (`SQLSTATE_SYNTAX_ERROR`).

### ODBC 2.x diagnostic behavior

Driver Managers map standard ODBC 3.x `42000` to ODBC 2.x `37000` when an application
sets `SQL_ATTR_ODBC_VERSION = SQL_OV_ODBC2`. Because `42601` is a vendor-native
subclass, Driver Managers do not have a built-in mapping for it; applications
running in ODBC 2.x mode receive `42601` as returned by the driver.

## Preparation timing: deferred vs eager auto-IPD paths

The ODBC specification permits drivers to defer statement preparation until
execution or explicit description. ODBCPP does not perform unnecessary server
round-trips during `SQLPrepare` merely to mimic vendor error timing.

| Execution path | `SQLPrepare` behavior | Error timing | Emitted SQLSTATE |
| --- | --- | --- | --- |
| **Default deferred prepare** (`SQL_ATTR_ENABLE_AUTO_IPD = SQL_FALSE`) | Stores SQL text locally; counts parameter markers; 0 network calls | `SQLExecute` or `SQLDescribeParam` | `42601` |
| **Eager prepare / auto-IPD** (`SQL_ATTR_ENABLE_AUTO_IPD = SQL_TRUE`) | Eagerly issues statement description (`Parse` + `Describe`) | `SQLPrepare` | `42601` |
| **Direct execution** (`SQLExecDirect`) | Uses simple query protocol (`Q` frame); multiple commands supported | None (succeeds) | `SQL_SUCCESS` |

## Recovery and subsequent statements

Rejection of a prepared multi-statement batch does not invalidate the connection
or prevent statement handle reuse:
- In autocommit mode, freeing statement resources via `SQLFreeStmt(..., SQL_CLOSE)`
  and parameter bindings via `SQLFreeStmt(..., SQL_RESET_PARAMS)` allows subsequent
  direct execution (`SQLExecDirect`) and prepared parameterized execution
  (`SQLPrepare` + `SQLBindParameter` + `SQLExecute`) to succeed on the same handle.
- In manual-transaction mode, connection recovery after a rejected prepared
  batch via `SQLEndTran(..., SQL_ROLLBACK)` is not currently covered by automated
  tests and remains open debt.

## Integration test assertions and intentional behavior change

Because PostgreSQL and Redshift emit `42601` for all native SQL syntax errors
(such as `SELECT * FROM` or unescaped `{fn ...}` with `SQL_NOSCAN_ON`), preserving
`42601` intentionally changes the diagnostic SQLSTATE for these syntax errors from
the old collapsed fallback `42000` to the specific native SQLSTATE `42601`.
Integration tests in `it_metadata_real.cpp`, `it_native_sql.cpp`, and
`it_prepared_statements_real.cpp` were updated accordingly to assert `42601`.
