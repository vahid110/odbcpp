# Internal session baseline

This test-only runner qualifies a narrow `IDatabaseConnection` baseline. Its
translation unit compiles against the staged SDK contract include tree alone.
The synthetic executable has no concrete backend, ODBC, crypto or test-framework
dependency. The mandatory PostgreSQL integration adapter supplies its own session
factory, resolved settings and SQL; the runner contains no database SQL.

Fixtures supply a direct query and a typed prepared query returning one row with
three canonical text cells: a scalar, SQL NULL and an engaged empty string. They
also supply a direct query producing a recoverable server error in autocommit.
Column names and native diagnostics are not compared across backends. Every
execution receives a fresh finite absolute deadline. Connection establishment
uses the caller's `ConnectionSettings` timeout and policy.

Checks cover exact Idle/Reusable outcomes and agreeing passive observations,
normalized column families, row/schema width, typed parameter results, server
error classification and operation, subsequent successful recovery, disconnect,
and owning results/diagnostics retained after physical session destruction.
Early failures disconnect through RAII. Exceptions become a fixed `exception`
check ID; reports never contain SQL, settings, secrets or backend messages.

The standalone synthetic tests exercise success and rejected connect state,
malformed rows, missing normalized types, contradictory root errors, incorrect
prepared values, falsely reusable errors, failed recovery, failed disconnect,
and invalid deadline policy. They check cleanup on early rejection.

This is **not** the S4 backend-author conformance kit. It does not qualify
provider/composition behavior, optional facets, transactions, pooling or reuse
admission, complete malformed-result validation, ODBC or a public stable SDK.
Other suites retain responsibility for those contracts. Future backend adapters
must supply their own SQL and authenticated configuration rather than adding
backend conditionals to the runner.
