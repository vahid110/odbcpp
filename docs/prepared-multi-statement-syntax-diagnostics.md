# Prepared multi-statement syntax diagnostics

## Summary

ODBCPP preserves the server's specific syntax-error SQLSTATE `42601` for rejected
prepared multi-statement batches on its PostgreSQL and Redshift products, matching
the diagnostics emitted by native PostgreSQL, Amazon Redshift native ODBC, and
Simba Redshift ODBC drivers.

## Diagnostic contract and standard SQLSTATE 42601

In ISO/IEC 9075 (SQL:1999 and SQL:2003 Part 2 Foundation, Section 22.1):
- Class `42` designates **Syntax Error or Access Rule Violation**.
- Subclass `601` explicitly designates **Syntax Error**.
- Subclass `000` is the generic unclassified fallback ("Syntax error or access rule violation").

Both PostgreSQL and Amazon Redshift emit SQLSTATE `42601` for query syntax errors,
including the extended query protocol rejection:
`ERROR: cannot insert multiple commands into a prepared statement`.

Previously, ODBCPP's `PgBackendProvider::normalize_error_sqlstate` did not map
`42601`, causing `query_failure_sqlstate` to collapse it to the generic caller
fallback `42000` (`SQLSTATE_SYNTAX_ERROR`). By preserving `42601`:
1. Applications, ORMs, and Driver Managers can distinguish a concrete syntax error
   (`42601`) from general access rule violations or ambiguous errors (`42000`).
2. Diagnostic parity is maintained with vendor ODBC drivers (Amazon native
   2.2.4.0 and Amazon/Simba 1.6.3.1008 both return `42601`).
3. Arbitrary internal or undocumented vendor states are not blindly passed through:
   unmapped vendor-specific states continue to fall back to `42000`.

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
or prevent statement reuse:
- In autocommit mode, subsequent single statements (e.g. `SELECT 11` or prepared
  parameterized `SELECT CAST(? AS INTEGER)`) execute and fetch successfully on
  the same handle.
- In manual-transaction mode, the failure transitions the session to a failed
  transaction state; issuing `SQLEndTran(..., SQL_ROLLBACK)` rolls back the failed
  state and restores the connection to idle for subsequent operations.

## Integration test assertions and intentional behavior change

Because PostgreSQL and Redshift emit `42601` for all native SQL syntax errors
(such as `SELECT * FROM` or unescaped `{fn ...}` with `SQL_NOSCAN_ON`), preserving
`42601` intentionally changes the diagnostic SQLSTATE for these syntax errors from
the old collapsed fallback `42000` to the specific standard SQLSTATE `42601`.
Integration tests in `it_metadata_real.cpp`, `it_native_sql.cpp`, and
`it_prepared_statements_real.cpp` were updated accordingly to assert `42601`.


