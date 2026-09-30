# Connectivity SDK product plan

Decision date: 2026-09-30.
Status: approved planning direction; implementation has not started.

## Product intent

ODBCPP will be developed as a database-connectivity SDK, with ODBC as its first
mature client API. PostgreSQL is the first production reference backend. A
bounded MySQL 8 implementation will prove that the SDK boundary works for an
unrelated wire protocol. Redshift remains the next product backend and will
prove specialization within the PostgreSQL protocol family when endpoint access
is restored.

ADBC is a planned second client API. This milestone must avoid architecture
that prevents Arrow columnar batches, but it does not add an Arrow dependency,
implement the ADBC ABI, or claim ADBC compatibility.

The product layers are:

1. **Client API adapters:** ODBC now; ADBC later.
2. **Shared connectivity SDK:** session and statement orchestration, ownership,
   deadlines, cancellation contract, normalized metadata and values,
   diagnostics, capabilities, and reusable contract tests.
3. **Backend implementations:** PostgreSQL and MySQL as sibling reference
   backends. Redshift specializes demonstrated PostgreSQL-family components
   without placing Redshift branches in shared client-API code.
4. **Transport and security primitives:** sockets, TLS, timeouts and logging,
   reused only where their contracts agree.

The shared layer is not an ODBC wrapper around a PostgreSQL implementation.
ODBC handles and constants stay in the ODBC adapter; native packet formats,
authentication exchanges, type identifiers and catalog SQL stay in backends.

## MS1: SDK foundation and MySQL proof

This is a bounded architecture-validation milestone, not a MySQL beta. It may
proceed while the Redshift pilot is blocked on user account access. PostgreSQL
regression gates remain mandatory. Redshift resumes at M2 as soon as usable
access returns; MS1 does not silently replace or waive Redshift acceptance.

| Package | Scope | Base estimate | Exit evidence |
|---|---|---:|---|
| S1 | Freeze SDK boundary, dependency direction, ownership/error/deadline rules, internal versioning policy and extension lifecycle; inventory current PostgreSQL dependencies; pin the MySQL 8 test version and initial `caching_sha2_password`-over-TLS profile | 2–4 days | Contract document and dependency audit reviewed; no behavior change |
| S2 | Evolve the smallest necessary interfaces and reusable conformance harness; keep PostgreSQL behavior and ABI-facing ODBC paths green | 4–6 days | PostgreSQL plus synthetic-backend contract tests pass; no PostgreSQL/native ODBC concepts cross the documented boundary |
| S3 | MySQL 8 protocol vertical slice: verified TLS, one password method appropriate to the test server, connect/disconnect, direct and prepared execution, scalar/NULL fetch, parameters, transactions, essential table/column metadata, server errors and recovery | 10–16 days | Unit edge cases and live container tests pass through the same shared ODBC orchestration used by PostgreSQL |
| S4 | Backend author test kit, MySQL/PostgreSQL comparison review, extension guide and one minimal out-of-tree sample backend | 4–6 days | A clean consumer can build the sample without editing shared ODBC wrappers; limitations and unstable interfaces are explicit |

Total: **20–32 base engineering days**, plus **6–10 days of 30% contingency**,
for a buffered working range of **26–42 engineering days**. These are focused
engineering days, not elapsed calendar promises. Review the estimate after S1
and after the first live MySQL handshake; do not consume contingency on added
features.

### Required proof

- PostgreSQL and MySQL are selected through the same backend contract and use
  the same ODBC handle, descriptor, diagnostic and conversion orchestration.
- A live MySQL 8 container gate covers successful TLS connection, direct query,
  prepared parameters, NULL and representative scalar results, commit/rollback,
  essential metadata, invalid credentials, invalid SQL, truncation/range
  behavior, recovery and clean disconnect.
- Parser tests cover fragmented/coalesced packets, malformed lengths, sequence
  errors, unknown capability/type values and bounded failure without crashes or
  hangs.
- PostgreSQL, iODBC UTF-16/UCS-4, Windows and sanitizer gates remain green for
  every implementation batch. MySQL gains focused unit and live Linux gates
  before its proof is accepted.
- Backend capabilities are explicit. Unsupported behavior fails predictably and
  is not advertised.
- The result contract remains representation-neutral: the current row path is
  supported, and a future columnar-batch path can be added without exposing
  ODBC types to backend implementations.

### Explicit non-goals

- A production or generally available MySQL driver.
- MariaDB compatibility claims; add a separate compatibility matrix later.
- Every MySQL authentication plugin, compression mode, replication command,
  administrative API, native type or server version.
- Runtime-loadable plugins, a frozen C++ ABI, semantic-version compatibility
  guarantees, or broad source compatibility for third parties.
- Arrow integration, an ADBC driver, Flight SQL, or ADBC packaging.
- SQL Server/TDS, Athena, Aurora or another backend.
- Deferring a serious PostgreSQL regression or a supported-path safety defect
  in order to complete the proof.

## Gates and stop rules

G12 closes when S1–S4 and the required proof above pass. It establishes a
credible internal SDK and a real unrelated-protocol reference backend. It does
not establish a public stable SDK or MySQL beta.

G13 is the later public SDK preview: versioned installable SDK artifacts, API
stability policy, compatibility testing, complete reference documentation,
licensing, support lifecycle and an externally consumable sample. Commercial
availability and stable ABI promises require a separate product decision.

Each implementation batch must close one named S1–S4 contract gap and include
happy-path and relevant failure evidence. Initial investigation is limited by
the budgets in `RELEASE_PLAN.md`. Optional protocol breadth goes to a TODO with
a trigger. At S3 completion, stop MySQL feature work and perform the SDK boundary
review before considering any MySQL beta plan.

## Sequencing with existing releases

- PostgreSQL W1–W4 and G9a are complete. PG-BETA remains formally open because
  G8 real-application acceptance and final release evidence are outstanding.
- SQL Server/OPENQUERY, Power BI Desktop and Excel acceptance remain recorded;
  the user has deferred those host-dependent runs while other work proceeds.
- The Redshift pilot implementation and non-live CI gate are ready. Live M2 is
  waiting on restored AWS/Redshift access and cannot be simulated by PostgreSQL.
- MS1 is the active bounded engineering milestone during that wait.
- When Redshift access returns, finish the current coherent MS1 batch, preserve
  its evidence, and run M2 before expanding MySQL scope. Re-estimate whether to
  finish the remaining MS1 packages or proceed directly to Redshift M3 based on
  the observed SDK boundary and product priority.

## Product validation after the technical proof

After G12, validate demand before funding a broad SDK surface. Seek design
partners among database and analytics vendors and test the value of commercial
SDK licensing, OEM distribution, paid driver development, compatibility
certification and enterprise support. Product discovery does not change the
technical release claims or authorize public distribution.
