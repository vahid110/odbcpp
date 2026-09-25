# Shared ODBC and database backend boundary

Planning baseline: 2026-09-23, implementation inspected at `4f6de2a`.
Status: required architecture work, not a claim that the boundary is implemented.
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
