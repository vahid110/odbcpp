# Redshift M2 compatibility assessment

Working assessment based on6901734, 2026-10-03. Its full platform CI37082777348
is green. M2 is not closed; official feature/behavior inventories remain partial.
This document proposes the next finite proof inventory, not an approved beta
scope, feature exception or full Redshift compatibility claim.

## Real endpoint evidence

| Area | Observed result | Practical limit |
|---|---|---|
| Server identity and verified TLS | Identity checks passed in separate bounded windows | One Stockholm Serverless workgroup, local macOS test host |
| Database credentials | Connection, rows, prepared scalar/NULL and error recovery passed in001 | Selected scalar cases, not complete type/transaction coverage |
| Table/column discovery | Fixture SQLTables/SQLColumns passed in005, including18-field shape, integer/varchar dimensions, ordinals and escaped/no-match patterns | Ordinary local fixture only; other catalogs/types/privileges remain unqualified |
| Externally supplied IAM credentials |009 completion passed identity plus IAMPrincipalScalar/IAMInvalidPassword; cleanup independently verified | Fixed Serverless role/principal; no native SDK credential discovery, renewal, server expiry or federation claim |

The first009 launch never started SQL because its exclusive log filenames
collided with recovery evidence. Its original result is preserved separately
from the explicitly reviewed completion using the original deadline/reservation.
See [live evidence](REDSHIFT_LIVE_TEST_PLAN.md). Full CI and live qualification
are separate facts; no passed PostgreSQL job qualifies Redshift behavior.

## Current implementation boundary

- Compiled Redshift composition selects the PostgreSQL-family provider with an
  explicit immutable Redshift catalog profile. Shared ODBC/SDK workflows remain
  shared; no detection from port, DSN label or server-version substring is used
  to choose that profile.
- Only SQLColumns has a Redshift-specific SVV_COLUMNS builder. SQLTables and
  other catalog requests retain existing PostgreSQL-family builders. Source
  reuse is not proof of Redshift compatibility; each advertised API needs a
  dedicated ordinary-user fixture and diagnostic evidence.
- The external IAM launcher obtains temporary credentials and passes ordinary
  UID/PWD into the driver. That proof does not implement a driver IAM provider.
- SHOW discovery selection/normalization and metadata-ID mode remain outside
  the proved profile. Shared/external/cross-database catalogs, fetch modes and
  official-driver option/behavior parity remain tracked in RS1–RS7, not waived.

## First concrete profile defects to resolve

The bounded static-profile correction selects Redshift policy only through the
existing explicit immutable profile: numeric/decimal precision38 and scale0–37,
identifier length127, and no index CREATE/DROP or schema-in-index support.
Provider isolation and direct ODBC SQLGetInfo/SQLGetTypeInfo regression tests
cover these claims, including preservation of PostgreSQL precision1000 and its
version-dependent negative scale. No paid query is needed to exercise these
static reporting paths; this does not qualify runtime numeric conversions.

Other inherited char/varchar10485760 and bytea/text1GiB rows, quoted identifier
case, integrity and transaction reporting remain separate audits. Each field
needs its ODBC contract and AWS source checked before changing it. Do not silently
advertise all catalog APIs merely because a catalog facet exists. Ordinary-user
catalog execution and full type fidelity remain separate proofs.
Primary references: AWS documents [unsupported indexes](https://docs.aws.amazon.com/redshift/latest/dg/c_unsupported-postgresql-features.html),
[numeric precision38/scale37](https://docs.aws.amazon.com/redshift/latest/dg/r_Numeric_types201.html),
[character declaration/conversion limits](https://docs.aws.amazon.com/redshift/latest/dg/r_Character_types.html)
and [identifier length127 bytes](https://docs.aws.amazon.com/en_en/redshift/latest/dg/r_names.html).
Character expression and declaration limits must be distinguished before choosing
the reported ODBC value; do not replace one unqualified constant with another.

| Catalog API | Current dependency | Evidence / next disposition |
|---|---|---|
| Tables | information_schema/current-database and constant enumeration rows | Local fixture passed; enumeration/cross-database coverage pending |
| Columns | Explicit Redshift SVV_COLUMNS builder | Local integer/varchar fixture passed; broader types/objects/permissions pending |
| PrimaryKeys | PostgreSQL-shaped information-schema key/constraint joins | Ordinary-user Redshift execution and informational-constraint semantics pending |
| ForeignKeys | pg_constraint/attributes/arrays and LATERAL ordinal expansion | PostgreSQL-specific dependencies; no Redshift proof |
| Statistics | PostgreSQL indexes/index-property helpers/LATERAL | Redshift has no indexes; define truthful result/capability contract |
| Procedures | pg_proc.prokind/arguments/obj_description | Catalog/version dependencies unaudited |
| ProcedureColumns | pg_proc/LATERAL/OIDs/domain/type helpers | Routine/type normalization unqualified |
| SpecialColumns | PostgreSQL unique-index identity/domain helpers; selected scopes empty | Identity and empty-result behavior unqualified |

The dispatcher is `core/database/postgres/pg_catalog_query.cpp:749`; existing
builder unit tests in `tests/unit/test_catalog_queries.cpp` prove SQL construction,
not execution on Redshift. SQLGetFunctions currently advertises all eight when
the backend has a catalog facet. A bounded correction must reconcile the claim
with each request's supported implementation without treating a pending parity
item as a user-approved omission.

## Next finite proof packages

| Package | Result required | Stop condition / planning effort |
|---|---|---|
| M2-TYPES | Replace loose string smoke checks with exact scalar and metadata assertions for numeric boundaries, NULLs, Unicode, temporal and binary values selected for beta | Offline case inventory/build first; then one explicitly admitted finite live batch. Initial assessment/test design0.5–1 engineering day, with failures separately triaged |
| M2-CATALOGS | Map SQLGetFunctions/GetInfo/GetTypeInfo claims and all catalog builders to Redshift test evidence; identify unsupported or unqualified claims | Produce API-to-fixture matrix and prioritized defects; no blind PostgreSQL catalog porting. Assessment0.5–1 day; implementation sized from evidence |
| M2-PROFILE | Record selected deployment/auth/type/metadata surface, source-review coverage and remaining RS1–RS7 gaps; estimate bounded M3/RP1 checkpoints | Proposal ready for scope acceptance; no silent omission or approved exception. Assessment0.5 day after the matrices |

These are engineering planning estimates, not elapsed-time percentages or a
promise to finish the beta in that time. ROADMAP's original M2 estimate is3–5
days and M3 is8–12 days; reassess implementation effort when the matrices expose
concrete defects. Exhaustive RP1 estimates require a fuller source inventory.
Do not hide that unknown work inside M2's baseline estimate.

## Stopping point

Stop this bounded phase when G1 evidence is reviewed, the compatibility matrices
and beta proposal are recorded, and M3/RP1 work is sized into finite checkpoints.
M3 application/platform/type/catalog gates, RP1 full parity and medium-term RS9
live CI remain distinct deliverables. Advanced S3 transfer requires its own
approved design. Existing spending authority, protected credentials, cumulative
reservations, bounded execution and independent cleanup remain in force; no
additional paid attempt is authorized by this assessment.
