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

## Runtime type evidence and proposed finite inventory

Source audit at8550fe7 distinguishes actual endpoint evidence from shared offline
conversion tests. `DataTypes` only checks ASCII/integer text and a float substring;
it selects NOW() without reading the timestamp. It is not an exact type-fidelity
gate, and its presence alone is not evidence of an admitted live execution.

| Family | Actual Redshift evidence | Missing endpoint proof |
|---|---|---|
| Integer | Ordered1–5 as text, prepared42 as SQL_C_SLONG, IAM7 as SQL_C_LONG | Bigint extremes and checked narrowing diagnostics/output preservation |
| Decimal/floating point | No exact decimal or typed float result recorded | Precision38, descriptor scale, exact numeric magnitude/sign, overflow/truncation |
| Unicode | No multibyte/supplementary result recorded | Narrow/wide retrieval, prepared input, byte versus SQLWCHAR-unit lengths |
| Temporal | None; selected NOW() is not retrieved | Fixed leap date/fractional timestamp, temporal structs and lost-field warnings |
| Binary | None | Redshift VARBYTE OID/wire representation first; PostgreSQL bytea tests are insufficient |
| NULL | Prepared varchar NULL and IAM integer NULL | Decimal/temporal NULLs, bound columns, output preservation and empty-string distinction |

Existing converter tests cover numeric structures/exponents/checked narrowing,
UTF-8/wide conversion, leap dates/fractions, malformed-input preservation and
normalized binary copying (`tests/unit/test_redshift_data_conversion.cpp`).
Native decoder tests exercise PostgreSQL-family OIDs/modifiers and bytea escapes
(`tests/unit/test_native_types.cpp`); neither suite proves Redshift wire fidelity.

The next five GTests are implemented and compile in the real-target executable;
they still require their own reviewed Redshift admission:

1. `IntegerBoundariesAndNarrowing`: bigint min/max through SQL_C_SBIGINT;
  32768 narrowed to SQL_C_SSHORT must report22003 and preserve sentinel output.
2. `ExactDecimalAndNull`: DECIMAL(5,2) ±123.45 with explicit descriptor scale2,
   magnitude bytes39 30 and signs1/0; a38-digit value and a typed NULL. Avoid
   accidental scale0 retrieval, which would test truncation instead of fidelity.
3. `UnicodeRoundTrip`: Grüße plus a supplementary character through narrow/wide
   retrieval and prepared input; compare exact UTF-8 and compute wide length
   using the compiled SQLWCHAR width.
4. `TemporalExactAndNull`: fixed2024-02-29 date and timestamp
  2024-02-29 12:34:56.123456, fraction123456000 nanoseconds; test fractional-time
   loss separately with01S07. Timezone cases require another explicit inventory.
5. `TypedNullAndOutputPreservation`: integer/decimal/varchar/timestamp NULLs,
   SQL_NULL_DATA and preserved sentinel buffers, distinct from empty text.

These are executable test expectations, not newly qualified Redshift behavior.
A local PostgreSQL surrogate validates syntax and ODBC test mechanics only; its
results cannot satisfy Redshift identity or runtime-fidelity qualification. No binary
case is admitted until the VARBYTE decoding contract is audited. No schema writes,
new authentication methods or additional paid window are authorized here.

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

## G1 review and remaining assessment deliverables

Independent source/evidence review at8550fe7 accepted that001/005/009 collectively
cover the selected real-endpoint G1 behaviors. This is cross-artifact evidence,
not one combined live acceptance on8550fe7. Before formal closure, retain a
manifest binding each case to its tested source/binary, profile and private result;
no paid rerun solely to consolidate evidence is needed. The8550fe7 platform CI
remains a separate gate.

Remaining finite assessment work: freeze the selected type/conversion dispositions,
complete the advertised API/capability claim matrix, record deployment/auth/
platform/application beta proposal and RS1–RS7 boundaries, and size M3/RP1
checkpoints with dependencies. Exhaustive official parity remains a later phase;
this proposal must neither claim it nor silently approve exceptions.

## Stopping point

Stop this bounded phase when G1 evidence is reviewed, the compatibility matrices
and beta proposal are recorded, and M3/RP1 work is sized into finite checkpoints.
M3 application/platform/type/catalog gates, RP1 full parity and medium-term RS9
live CI remain distinct deliverables. Advanced S3 transfer requires its own
approved design. Existing spending authority, protected credentials, cumulative
reservations, bounded execution and independent cleanup remain in force; no
additional paid attempt is authorized by this assessment.
