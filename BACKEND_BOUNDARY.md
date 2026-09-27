# Shared ODBC and database backend boundary

Planning baseline: 2026-09-23, implementation inspected at `4f6de2a`.
Status: architecture acceptance remains open; incremental progress is recorded below.
Scope: PostgreSQL first, Redshift second. No other backend is added here.

## Requirement and ownership

The shared framework must become reusable during PostgreSQL consolidation.
Redshift must exercise that boundary, not trigger a wholesale extraction after
both drivers are finished. Public SDK packaging and stability guarantees come later.

| Shared framework owns | Database backend owns |
|---|---|
| ODBC entry points, handles, lifetime and call serialization | Connection creation, protocol session, authentication exchange |
| Descriptor storage, binding buffers and alignment rules | Native type IDs, backend type metadata and value representations |
| ODBC return codes, diagnostic record lifecycle and validation | Server error interpretation and backend-specific capability values |
| Common conversion rules, NULL and truncation handling | SQL dialect, parameter-marker syntax and native parameter encoding |
| Common catalog argument validation and ODBC output shape | Catalog SQL, type discovery, visibility and backend-specific metadata |
| Deadline propagation, transport primitives and logging infrastructure | Backend session/deadline recovery decisions and connection retirement |
| Contract-test harness and shared regression suites | Live backend fixtures and backend-specific expected results |

Common text conversions may be shared where their semantics agree. PostgreSQL
wire encodings (for example, bytea representations) are not universal ODBC rules.
Transport reuse does not require every future backend to use PostgreSQL framing.

## Observed coupling and bounded extraction work

| ID | Current evidence | Required PostgreSQL-stage outcome |
|---|---|---|
| A1 | `ODBCConnection::connect` in [odbc_handles.cpp](odbc/odbc_handles.cpp) directly constructs `PgProtocolParser`; [DatabaseFactory](core/database/database_factory.cpp) separately constructs connections | One selected backend creation route; preserve transport options, ownership, TLS settings and deadline behavior. No direct PostgreSQL parser construction in shared ODBC orchestration |
| A2 | Statement preparation calls `PgProtocolParser::parameter_marker_count` | Route SQL/marker processing through the backend boundary; preserve existing PostgreSQL quoting, escaping and marker tests |
| A3 | `postgres_type_info`, OID/domain lookups and catalog queries are embedded in `odbc_handles.cpp` | Backend owns native type interpretation and catalog construction. Shared ODBC code consumes the required type/capability/catalog contract without interpreting PostgreSQL OIDs or issuing PostgreSQL catalog SQL itself |
| A4 | [QueryResult](core/database/query_result.h) exposes native IDs/type modifiers; [IProtocolParser](core/database/i_protocol_parser.h) exposes PostgreSQL-shaped messages/authentication | Document native versus common fields; keep protocol details below the backend boundary. Reuse/evolve [IDatabaseConnection](core/database/i_database_connection.h) and [QueryParameter](core/database/query_parameter.h) where sufficient instead of adding parallel abstractions |

These are the initial four architecture work items, not four automatic large
commits. Inventory exact call sites and size them in M0. Split implementation
into reviewable changes. Moving files alone does not close a work item; callers
must use the boundary and tests must exercise it. A live Redshift endpoint is
not required to implement and verify the PostgreSQL boundary.

## G9a — PostgreSQL architecture acceptance (required before PG-BETA)

- A1–A4 are mapped to concrete code and their PostgreSQL-stage outcomes are met.
- The documented contract specifies ownership/lifetimes, native versus normalized
  metadata, NULL representation, error/SQLSTATE handling, capabilities, deadlines,
  and whether an error permits reuse or requires retiring the connection.
- Shared ODBC orchestration no longer selects a PostgreSQL parser directly,
  interprets PostgreSQL type IDs, or constructs PostgreSQL catalog queries.
  Those operations reside behind the selected backend. Protocol libraries may
  remain PostgreSQL-specific internally and be shared with Redshift later.
- A small fake backend exercises backend selection, successful result/NULL
  delivery, an error, and an unsupported capability through the shared layer.
  It must not duplicate a real protocol or become a new product backend.
- Existing PostgreSQL behavior is preserved by the full PostgreSQL, Driver
  Manager width and sanitizer gates; relevant Windows checks remain green.
- Review the remaining dependencies explicitly. No known leak of database
  semantics across the required boundary is silently deferred to SDK packaging.

## G9b — Redshift reuse acceptance (required before Redshift beta)

- Select PostgreSQL and Redshift backends through the same contract; keep shared
  ODBC wrappers, lifetime rules, descriptors and diagnostic machinery in one place.
- Add observed Redshift differences to its backend or to demonstrated shared
  PostgreSQL-family protocol components, rather than scattering backend branches
  through shared ODBC workflows.
- Run common contract tests against both backends plus real Redshift acceptance.
  Refine only contract gaps demonstrated by this work; preserve PostgreSQL gates.
- Record which components are shared and which differ. This proves reuse within
  the PostgreSQL family, not arbitrary-protocol portability.

## What remains later

G12 covers independent SDK consumption, out-of-tree examples, extension tutorials,
package/version compatibility, and an explicit API stability policy. It does not
own A1–A4: those cannot be postponed under a label such as “SDK work.”

No generic plugin registry, ABI guarantee, additional protocol implementation,
or universal authentication framework is required for G9a. Apply the investigation
limits in [RELEASE_PLAN.md](RELEASE_PLAN.md). If a necessary extraction exceeds
its budget, report the impact and re-estimate the milestone; do not silently
waive architecture acceptance or hide the extra work in contingency.

## Implementation progress — 2026-09-25

A1's construction change routes `ODBCConnection::connect` through
`DatabaseFactory`, transferring its configured transport. Existing no-argument
and explicit-type factory entry points remain available. Tests cover selected
startup/query, authentication error/timeout, refused TLS and transport destruction
on unsupported selection. Transport options and deadline logic remain in their
existing paths and are covered by the full regression gates.

The marker-counting part of A2 now uses `IDatabaseConnection` and the selected
parser's lexical rules, removing the PostgreSQL parser include from shared ODBC
handles. A test parser with different quoting semantics proves dispatch is not
hardwired to PostgreSQL. This adds an internal virtual contract; no stable public
C++ ABI is promised, and downstream implementations must provide the new method.
The MySQL placeholder remains explicitly unimplemented.

A2 is not closed: shared SQL escape translation still needs backend ownership.
A3/A4 and fake-backend acceptance through the complete shared ODBC layer remain
open. Removing the parser include does not establish complete SDK reuse.

## Implementation progress — 2026-09-27

A2's remaining escape translator now lives in
[pg_sql_dialect.cpp](core/database/postgres/pg_sql_dialect.cpp). SQLNativeSql A/W,
direct execution and preparation obtain translation through the selected backend.
The backend owns syntax/lexical decisions; the ODBC layer keeps SQLSTATE mapping,
input/output validation, truncation and NOSCAN behavior. The result contract is
[sql_translation.h](core/database/sql_translation.h); backend/parser implementations
must implement pure `translate_sql`, in addition to marker counting. The MySQL
placeholder explicitly returns Unsupported and remains outside release scope.

UTF-8 scalar decoding/counting is shared in [utf8.h](core/util/utf8.h), so the
PostgreSQL dialect does not include ODBC headers. The ODBC Unicode adapter uses
the same validator, preserving UTF-16/UCS-4 and malformed-input behavior.

Tests exercise selected-backend translation, syntax/datetime/unsupported errors,
recovery and a fake parser with different translation rules. Real PostgreSQL
A/W direct/prepare errors preserve a previous prepared statement and permit
reuse. Existing native-SQL output/truncation, Unicode, quoting, nested comments,
NOSCAN and Driver Manager tests remain required. A3/A4 and complete fake-backend
ODBC acceptance remain open; A2 closure is recorded with the batch validation.

## A3 scalar-metadata batch — 2026-09-27

The selected backend now interprets native type IDs, widths and modifiers through
`describe_type`, returning the ODBC-independent families and fields in
[native_type_info.h](core/database/native_type_info.h). PostgreSQL interpretation
lives in [pg_type_info.cpp](core/database/postgres/pg_type_info.cpp). Shared ODBC
column and parameter metadata maps those normalized families to ODBC constants;
it no longer contains the `postgres_type_info` native-ID switch. The connection
contract delegates to its selected parser, and a differing mock mapping is tested.

Known/unknown classification for parameter discovery also comes from the backend.
Existing domain lookup, cache semantics, bound parameter precision overrides,
SQLGetTypeInfo definitions, descriptor type names and catalog SQL remain for
subsequent A3 work. Therefore A3 and overall G9a remain open. Native IDs in
QueryResult are still opaque backend metadata awaiting the remaining extraction.

Tests cover scalar mappings, numeric precision/negative scale, temporal widths,
character limits, unknown-type fallbacks, and metadata agreement before/after
execution including NULL output preservation. Temporal width calculation uses
unsigned wide arithmetic to avoid signed overflow on extreme metadata modifiers;
this does not promise support for out-of-range temporal precision.
