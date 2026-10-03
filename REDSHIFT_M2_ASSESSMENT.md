# Redshift M2 compatibility assessment

Bounded assessment at8614ce0, 2026-10-03. Static reporting CI37084444992 and
exact-test CI37087097081 are green across required gates. Full M2/beta scope is
not closed; official feature/behavior inventories remain partial.
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

## Corrected static profile and remaining defects

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

## Complete provider capability grouping

Source audit at8614ce0 covers every field of `BackendCapabilities`, not every
possible SQLGetInfo identifier or official-driver option. SQLGetInfo string and
numeric mappings are in `odbc/odbc_api.cpp:327` and `:1519`; provider values are
in `core/database/postgres/pg_capabilities.cpp`. A returned flag is a claim, not
proof of server behavior.

| Provider field group | Advertised Redshift values | Evidence/disposition |
|---|---|---|
| dbms_name | Amazon Redshift | Explicit composition; independent live identity evidence |
| identifier_quote, catalog_separator/term, schema/table/procedure_term, pattern_escape | quote, dot, database/schema/table/procedure, backslash | Inherited lexical conventions;005 selected escaped patterns only |
| max_identifier_length, identifier_case, quoted_identifier_case |127, lower, sensitive |127 corrected/tested; configured case semantics unqualified |
| catalog_at_start, catalog_names | At start; names supported | Current-database fixture does not prove cross-database names |
| column_aliases, describe_parameters | Both true | Selected aliases/prepared scalar only; describe-parameter fidelity pending |
| order_by_expressions/requires_select, correlation_names, group_by | True/false, any, unrelated | Ordered rows proved; expression/grouping breadth pending |
| null_collation, concat_null_yields_null, non_nullable_columns | High, true, true | NULL retrieval does not qualify ordering/concatenation/non-null constraints |
| read_only, integrity, like_escape, outer_joins, procedures | False, true, true, true, true | Escaped discovery partly proved; write/integrity/join/routine semantics pending |
| create_index, drop_index | Both false | Corrected static reporting, direct ODBC regression |
| insert_literals/searched, select_into | All true | Inherited; no general ODBC write qualification |
| sql92_entry, union_distinct/all | All true | UNION DISTINCT in001; broader conformance unqualified |
| schema_in_dml/procedures/table_definitions/index_definitions/privileges | All true except indexes | Corrected index bit tested; other usage masks pending |

Transaction claims are separate: `pg_transactions.cpp:6` advertises transactions,
transactional DDL, default READ_COMMITTED and all four ODBC isolation bits.
SQLGetInfo additionally reports cursor preservation across commit/rollback.
These are inherited and unqualified on Redshift. Command aliases must not be
mistaken for distinct effective isolation semantics. Test effective isolation,
rollback/write/DDL behavior and result lifetime before changing or qualifying
these claims.

SQLGetFunctions lists shared handle/attribute/descriptor, connection, execution,
binding/fetch, diagnostics, transaction and information entry points in
`odbc_api.cpp:458`. Catalog support is gated facet-wide at`:1866`, so all eight
catalog APIs in the matrix above advertise support despite differing builder
compatibility. A complete release-level API/option inventory is still required;
this finite provider matrix does not stand in for RS4/RS5 source parity.

## Advertised API-to-evidence matrix

The49 entries in `is_supported_function()` at8614ce0 are partitioned below.
This covers our current function advertisement, not official-driver exports,
all attributes/options, or every return/state branch. Wide/legacy mapping parity
and the upstream RS4/RS5 inventory stay separate work. A supported entry does not
promise every optional mode; forward-only fetch is the current shared profile.

| Group / all advertised members | Redshift evidence disposition |
|---|---|
| SQLAllocHandle, SQLFreeHandle, SQLFreeStmt, SQLCloseCursor | Selected successful lifecycle in existing cases; complete state/error matrix pending |
| SQLGetEnvAttr, SQLSetEnvAttr, SQLGetStmtAttr, SQLSetStmtAttr | ODBC version/query timeout setup used; getter/attribute breadth pending |
| SQLConnect, SQLDriverConnect, SQLDisconnect, SQLGetConnectAttr, SQLSetConnectAttr | DriverConnect/disconnect and timeout setup proved; SQLConnect/getter/other attributes unqualified |
| SQLCopyDesc, SQLGetDescField, SQLGetDescRec, SQLSetDescField, SQLSetDescRec | Shared offline contracts; no Redshift descriptor workflow proof yet |
| SQLColAttribute, SQLDescribeCol, SQLNumResultCols | Metadata005 proves18 result fields via NumResultCols; DescribeCol/attributes await exact type cases |
| SQLGetInfo, SQLGetFunctions, SQLGetTypeInfo | Static direct-ODBC/profile regressions; field-specific server interpretation pending |
| SQLNativeSql | Shared translation tests; translated execution/Redshift dialect breadth pending |
| SQLBindParameter, SQLDescribeParam, SQLNumParams | Selected integer prepared binding proved; describe/count APIs and other parameter types pending |
| SQLPrepare, SQLExecute, SQLExecDirect, SQLMoreResults, SQLRowCount | Selected direct/prepared execution proved; multiple results and rowcount contracts pending |
| SQLBindCol, SQLFetch, SQLFetchScroll, SQLGetData | Forward fetch/selected GetData proved; BindCol, scroll modes and broader conversions pending |
| SQLError, SQLGetDiagField, SQLGetDiagRec | Selected SQLGetDiagRec error recovery/invalid credentials; legacy/error-chain/field breadth pending |
| SQLEndTran | Shared transaction contract only; Redshift transaction qualification pending |
| SQLTables, SQLColumns, SQLPrimaryKeys, SQLForeignKeys, SQLStatistics, SQLProcedures, SQLProcedureColumns, SQLSpecialColumns | Request-specific dispositions in the eight-catalog matrix; only local Tables/Columns proved |

## Candidate beta profile and checkpoint dependencies

This is a proposal, not a frozen beta or user-approved feature exception:

- Deployment: existing Serverless workgroup, verified TLS, bounded ordinary-user
  local schema/table workflows first. Provisioned/shared/external/cross-database
  applicability stays explicit future evidence, not assumed equivalence.
- Authentication: database credentials as the baseline; external fixed-role
  temporary credentials have selected proof. Native discovery/renewal/federation
  remain RS2 work and cannot be advertised from the external-launcher result.
- Types: integer, selected exact decimal, Unicode text, date/time/timestamp and
  typed NULL contracts must pass the finite cases. VARBYTE and broader type/zone
  behavior need their own source/fixture review before a support decision.
- Platforms/applications: Linux/unixODBC, Windows DM and macOS/iODBC Unicode
  targets remain planned live acceptance gates, plus selected Power BI/Excel
  workflows. Current macOS endpoint evidence and build/platform CI are separate.
- Metadata: local Tables/Columns have a bounded baseline; remaining advertised
  catalogs and transaction/profile claims are blockers to a truthful frozen
  surface. SHOW/shared/fetch/legacy/official-option breadth remains RS1–RS7.

The following checkpoint estimates are engineering effort after the selected
fixture/profile is ready, not calendar elapsed time, paid runtime or guarantees:

| Checkpoint | Deliverable / stop | Initial estimate / dependency |
|---|---|---|
| M3-T1 | One reviewed exact five-case Redshift batch; triage failures into evidenced fixes |0.5–1 day qualification/triage; fixes sized separately. Test code is prepared; paid admission remains separate |
| M3-C1 | Resolve Statistics/SpecialColumns index contracts and diagnose remaining ordinary-user key/routine catalogs |1–2 days source/contract diagnostics, then2–4 days selected repairs; depends on actual query evidence and fixture permissions |
| M3-P1 | Audit remaining type/identifier/integrity/isolation claims and freeze truthful supported profile |1–2 days; depends on C1/type evidence and effective server settings |
| M3-A1 | Native selected auth decision/provider work and renewal/cancellation evidence |1 day finite design/inventory, implementation estimate after provider scope; external token injection alone does not satisfy it |
| M3-G1 | Selected platform/application acceptance and packaging evidence |2–3 days preparation/first runs, plus external application/runner availability; broad liveOS migration belongs to RS9 |
| RP1-I1 | Pinned official feature/options/behavior inventory with applicability and differential-test design |2–4 days bounded inventory review; stop with prioritized count/unknowns and separate implementation estimates, not exhaustive parity closure |
| RS9-I1 | Shared remote admission/cleanup design and first live CI platform plan |1–2 days design/source review; implementation sized separately, no new runner/IAM/spend authority implied |

The original M3 estimate8–12 days remains provisional and cannot include unknown
native auth, exhaustive RP1 or all RS9 work by assumption. Re-estimate the selected
beta after C1/P1/A1 decisions; these checkpoints are not additive promises or
approved omissions. RS1–RS7 remain scheduled parity obligations; RS8 requires its
own later approved design. User scope acceptance is needed before calling this
candidate a frozen beta, but no question or new authority is required to record
the completed bounded assessment now.

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
not one combined live acceptance on8550fe7. A protected supplemental
`g1-evidence-manifest-20261003.json` now binds001/005/009 request/result raw hashes,
reviewed executable hashes, profile, case counts and cleanup outcomes. Recorded
source revisions are owner build provenance, not reproducible-build attestations;
no paid rerun solely to consolidate evidence is needed. The8550fe7 and8614ce0 platform CI
passes remain separate evidence from those earlier live artifacts.

The bounded assessment now records selected type/conversion dispositions,
all49 advertised APIs and provider capability groups, the deployment/auth/
platform/application proposal, RS1–RS7 boundaries, and sized finite M3/RP1
checkpoints with dependencies. Independent stopping review on2026-10-03 accepted this bounded assessment
checkpoint. Full M2, frozen beta, parity and new paid admission remain open. Exhaustive official parity remains a later phase;
this proposal must neither claim it nor silently approve exceptions.

## Stopping point

The agreed bounded assessment stopping point is reached: G1 behavior evidence
was reviewed, compatibility matrices and the beta proposal are recorded, and
M3/RP1 work is sized into finite checkpoints. This assessment originally marked
a pause boundary; the owner subsequently authorized continuous unattended work
and explicitly prohibited timer pauses at intermediate checkpoints. Record each
review and select the next finite authorized scope while keeping the timer active.
M3 application/platform/type/catalog gates, RP1 full parity and medium-term RS9
live CI remain distinct deliverables. Advanced S3 transfer requires its own
approved design. Existing spending authority, protected credentials, cumulative
reservations, bounded execution and independent cleanup remain in force; no
additional paid attempt is authorized by this assessment.

## Resumed M3-T1 / M3-C1 bounded phase

The owner explicitly resumed implementation after the assessment checkpoint.
The next exact-type proof is identity followed by the five existing GTests in
8614ce0, using one separately reviewed finite010 request. The offline successor
preserves completev7 history, retained liability, original24h horizon and60s
cleanup margin. It neither migrates protected state implicitly nor exchanges
IAM credentials. Live Redshift type qualification remains pending.

Initial catalog source review found that the pinned official driver
[SQLStatistics common path](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rscatalog.cpp#L1268)
returns an empty13-field result. The explicit Redshift builder now follows that
bounded empty-result contract without PostgreSQL index catalogs; PostgreSQL's
builder remains isolated. Tests cover field order/cast families and invariant
output for valid filters/uniqueness. This is offline query evidence, not live
column-description parity: VARCHAR widths, lengths and nullability still need
runtime evidence. Shared API rejection of SQL_ENSURE remains a recorded parity
gap; this repair covers SQL_QUICK only.

SpecialColumns requires separate contracts: sampled modern SHOW path for upstream ROWVER is empty,
while BEST_ROWID has metadata processing that needs further source assessment
and an ordinary-user diagnostic. No blanket PostgreSQL catalog port or binary
assertion is enabled. VARBYTE OID/wire evidence is under separate review.
This resumed phase stops at reviewed exact-type evidence or an explicit live
qualification blocker, plus the initial catalog contract deliverable.

The separate pinned-source VARBYTE audit identifies OID6551 and unprefixed hex
for text-format binary retrieval. Current runtime lacks that OID mapping and
uses PostgreSQL binary decoding/parameter hints. Binary tests and conversion
repairs require a separate finite package; the existing five-case type window
excludes VARBYTE. SpecialColumns legacy upstream paths still use PostgreSQL
xmin/oid queries, so modern SHOW behavior does not prove compatibility across
all upstream modes. BEST_ROWID requires explicit source/fixture qualification.

### M3-T1 / M3-C1 checkpoint: connectivity blocker

Codebd6f220 CI37104759411 passed all required gates. One separately reviewed010
request was durably reserved after fresh controls passed; stale host ingress
was replaced with the current exact host /32 using existing scoped permissions.
Both independent cleanup connections then timed out before SQL execution was
evidenced. No driver cases ran. This is endpoint connectivity/cleanup
verification trouble, not an observed Redshift type or authentication failure.

Independent stopping review accepted the phase's explicit qualification blocker
plus initial Statistics contract deliverable. The five exact type cases remain
unqualified on Redshift. Protected010/result/history and34.68 compute/84.68 total
conservative bounds remain retained; they are not actual bills or a balance.
Any further SQL requires a separately reviewed finite cleanup-only recovery
following connectivity investigation. No replay, automatic retry or zero-session
inference is permitted. Runtime Statistics metadata and SQL_ENSURE, SpecialColumns,
VARBYTE and broader parity remain open. The owner subsequently directed continuous unattended work through checkpoints.
The15-minute timer remains active; this blocked live gate does not stop
connectivity investigation, reviewed recovery preparation or other authorized
offline implementation. Intermediate checkpoints are review records, not
scheduler stopping conditions.

### Connectivity investigation and finite recovery preparation

Read-only AWS inspection found the existing workgroup available and publicly
accessible, its exact host ingress intact, and VPC internet route/gateway active.
One bounded TCP handshake subsequently succeeded without application bytes,
credentials or SQL. The earlier timeout's precise cause is not established;
this is current reachability evidence, not proof of cleanup or a type test.

The minimal offline010 cleanup successor reuses the reviewed recovery lifecycle
with trusted fixed policy constants. It freezes completev8 and failed010 result
bytes intov9, adds1200s metering and60s cleanup, retains original24h and100/150
caps, and permits exactly one cleanup call with no driver/authentication tests,
fixture changes or automatic retry.239 offline tests pass, including source
mutation, crash/persistence, history, no-replay and cleanup-only composition.
Independent review and separate fresh root admission remain required before SQL.

### Result-only VARBYTE and startup-phase diagnostics

The bounded result decoder now recognizes OID6551 only in the explicit immutable
Redshift profile. Text-format values use strict unprefixed hex and become owned
binary cells; NULL, empty, malformed values and additional result sets retain
their existing ODBC distinctions. The family is SQL_LONGVARBINARY with unknown
size, octet length and display size. No maximum width, TYPE_NAME or SQLGetTypeInfo
claim is added. PostgreSQL OID17 bytea and unknown-OID fallback are unchanged.
Scripted wire/profile tests and ODBC chunk/error tests are offline evidence;
VARBYTE remains unqualified against a live Redshift endpoint.

Prepared VARBYTE encoding remains unqualified and unchanged. The parameter
metadata resolver rejects OID6551 rather than issuing PostgreSQL type-discovery
SQL. This is not a pre-execution guard: a combined prepared exchange can send
Execute before response metadata is rejected. A regression test records that
timing; this package establishes no prepared VARBYTE support.

The separately admitted010 cleanup recovery also timed out before SQL was
evidenced. Its v9 uncertainty and added liability remain retained, and further
paid qualification/recovery is blocked pending concrete startup diagnosis.
A synthetic localhost fixture exercises the exact launcher/libpq invocation:
verified TLS and ReadyForQuery permit SET/query, while a post-TLS startup stall
reproduces the connection timeout with zero SQL packets. This isolates a possible
failure phase, not the cause at the real endpoint. No timeout increase, paid
probe, replay, or inference of zero remote activity follows from this fixture.

The pinned SpecialColumns audit distinguishes `show_discovery >= 4` from the
legacy path. Modern ROWVER returns a local empty eight-field result; modern
BEST_ROWID discovers declared primary keys through SHOW and returns SESSION
scope. Legacy code instead uses xmin/oid and PostgreSQL index catalogs. Current
BEST_ROWID SQL therefore needs a separate Redshift contract; copying it or
returning blanket empty rows would not establish parity. The next smallest
offline candidate is an explicit modern ROWVER eight-field typed-empty builder,
with all valid scope/nullable combinations and PostgreSQL isolation. Its SELECT
round trip would remain distinct from upstream's local result construction.
BEST_ROWID mode selection, key ordering, SHOW permissions, dimensions and name
semantics remain open, requiring ordinary-user fixture evidence.

The explicit Redshift ROWVER query candidate now has eight typed fields and an
empty result across all six valid scope/nullable combinations. Focused catalog
tests pass and independent source review found no blocker. PostgreSQL dispatch
and BEST_ROWID stay unchanged. This is the modern typed-empty contract only:
server-version negotiation, legacy xmin/oid, upstream's local no-I/O construction
and live descriptor widths/result behavior are not established. Full batch gates
and a future separately scoped live descriptor/empty-fetch case remain required.

Two exact future GoogleTests now check ROWVER's eight and SQL_QUICK Statistics'
thirteen descriptor names/type families, repeated empty fetch and cursor closure.
ROWVER covers six scope/nullable combinations; Statistics covers ALL/UNIQUE.
Independent review and a verified-TLS PostgreSQL surrogate passed both cases;
this proves test mechanics only. Neither case is enabled in a paid launcher,
and no live Redshift catalog, descriptor width or SQL_ENSURE qualification follows.

### Ordinary-user key/routine catalog audit

All four remaining key/routine APIs still dispatch to inherited PostgreSQL SQL.
PrimaryKeys' information-schema path remains unqualified rather than proved
broken. ForeignKeys uses array containment, paired unnest/ordinality and LATERAL;
routine builders use array expansion, comment helpers and pg_proc fields that
AWS instead documents in [PG_PROC_INFO](https://docs.aws.amazon.com/redshift/latest/dg/r_PG_PROC_INFO.html).
AWS's [unsupported-function list](https://docs.aws.amazon.com/redshift/latest/dg/c_unsupported-postgresql-functions.html)
documents the array/comment dependencies; occasional successful execution is
not support evidence. Exact runtime errors remain unobserved.

The pinned modern upstream discovers constraints with SHOW, and procedures plus
functions with signature-specific SHOW PARAMETERS. Its field families/counts
also differ: PK six/FK fourteen fields; Procedures eight with three NULL VARCHAR
reserved count fields; ProcedureColumns nineteen with INTEGER data-type fields.
Legacy upstream has separate pg_index/pg_proc_info paths. No source path should
be copied without mode, ordinary-user visibility and error-propagation contracts.

The next smallest diagnostic inventory needs a separately reviewed composite
PK/FK fixture whose key order differs from table order, imported/exported/both
requests, one IN/INOUT routine without invocation, exact/quoted/empty/omitted
names, missing objects and a separately prepared permission-denied object.
SHOW constraints require ownership or schema USAGE plus table SELECT;
routine discovery requires ownership or schema USAGE plus EXECUTE. These
[constraint](https://docs.aws.amazon.com/redshift/latest/dg/r_SHOW_CONSTRAINTS.html)
and [routine](https://docs.aws.amazon.com/redshift/latest/dg/r_SHOW_PROCEDURES.html)
visibility requirements need fixture evidence; errors must not become empty rows.
Overloads/functions, ordering, descriptor widths, datashares and broader parity
remain separate packages. No new fixture, grant, query or runtime port is enabled
by this audit.

The source-reviewed [fixture proposal](tests/fixtures/redshift/README.md) now
defines two empty reverse-order composite-key tables and a never-invoked IN/INOUT
procedure inside the existing schema. It creates no grants or cloud resources;
collision refusal, confirmed-creation tracking and selective exact teardown
remain prerequisites to any future admission. Ordinary-user access is conditional
on naming the creator/diagnostic principal and verifying existing privileges.

Two future key-catalog GoogleTests check PK six/FK fourteen descriptor families,
table/schema identity, declared key order, non-NULL sequence/rule/deferrability
values and observed constraint names. FK covers imported/exported/both requests.
On catalog failure, they capture diagnostics and check one scalar recovery on
the same connection without replaying the catalog. Independent review caught
and resolved a stale-output false positive for a NULL rule. A local verified-TLS
PostgreSQL fixture passed both cases and the prior two empty-catalog regressions;
this proves fixture/test mechanics only, not Redshift compatibility. Native TEXT
fields normalize to ODBC VARCHAR in that surrogate, so a native-field difference
alone does not prove a returned ODBC-family mismatch. No fixture, grant, paid
inventory or live qualification is enabled. Permission-denied/quoted/empty/omitted
fixtures remain future packages.

Two future routine GoogleTests now describe the pinned modern upstream contract
for the proposed never-invoked IN/INOUT procedure. Procedures checks eight fields,
the bare procedure name, three NULL VARCHAR reserved counts and procedure kind.
ProcedureColumns checks nineteen fields, exact parameter names/modes, INTEGER
data-type fields, dimensions, copied ordinals and NULL/default behavior. Both
filter actual schema/name after pattern discovery, bound fixture traversal to 64
rows and require exact cardinality; this is a fixture bound, not a driver limit.
NULL checks verify every byte of the output buffer remains untouched. Descriptor
widths and procedure remarks are recorded as observations rather than invented
expectations: upstream's remarks construction remains ambiguous.

Independent review accepted the test mechanics. The strict modern expectations
intentionally fail against a local TLS PostgreSQL surrogate: inherited reserved
counts are numeric/non-NULL, three parameter descriptor fields are SMALLINT,
integer type names are `integer`, decimal digits are zero and parameter remarks
are NULL. Those differences are diagnostic evidence about inherited behavior,
not observed Redshift failures or live qualification. Prior key and empty-catalog
surrogate regressions still pass. No procedure is invoked, fixture activated,
grant added or paid launcher inventory enabled by these tests.

### Modern SHOW execution boundary

Pinned upstream modern catalog discovery uses dependent commands, including
schema/table enumeration and signature-specific routine parameters. The current
pure catalog facet returns one SQL string and cannot represent that workflow.
A future execution facet must run under the session lease with one absolute
deadline, bounded discovery, owning results and original failure propagation.
It must negotiate `show_discovery` explicitly, preserve ordinary-user visibility
and distinguish exact identifiers from LIKE patterns. Missing or unsupported
capabilities must not silently select inherited PostgreSQL SQL or fabricate empty
success. That execution wiring remains a separate reviewed package.

The first offline candidate is a pure exact-table primary-key command-plan value
and SHOW-result normalizer. It owns database/schema/table names without rendering
or executing SQL, validates the six named input columns and builds six normalized
ODBC fields. It retains source row order, NULL constraint names and failure
diagnostics; malformed identities, duplicate keys/columns, inconsistent constraint
names and noncontiguous key sequences reject the whole result. Text widths remain
unknown. Pinned source bindings establish text and signed-smallint consumption,
not native SHOW OIDs/widths: accepted known normalized text and integer families
are an offline input contract only. No adapter, default catalog, broad discovery,
session capability selection or live compatibility is enabled by this candidate.
Independent review found no functional blocker and corrected a pinned-source
line reference. Nine focused tests cover accepted text/integer families, owned
output, reversed key order, nullable names, malformed input/native failures and
the signed sequence boundary. Local PostgreSQL/Redshift unit, Unicode integration,
TLS and sanitizer gates pass; these are offline regression evidence only.


## Provisioned live proof and CI alignment — 2026-10-03

The user authorized one persistent provisioned RG pilot and resumed unattended development. The existing reviewed Redshift test executable passed the five exact-type GoogleTests plus ConnectionTest on that cluster (6/6), followed by a separately scoped batch containing ConnectionTest, ConfiguredFixtureMetadata, RowVersionEmptyDescriptorContract and StatisticsQuickEmptyDescriptorContract (4/4). Both batches verified exact admin identity and zero other owned sessions before and after execution over verified TLS. Private admissions/results bind executable/source/accounting hashes. These are local provisioned live results, not a new platform CI run, a reproducible build attestation, or complete parity.

All feature assertions reside in `tests/integration/it_redshift_real.cpp`, including descriptor names/type families, exact integer/decimal/Unicode/temporal values, NULL output preservation and configured fixture metadata. Python performs orchestration and safety checks (credentials, finite inventory, accounting, deadlines, identity and cleanup); its offline safety tests do not replace C++ feature coverage. The planned multi-OS live CI must run these same GoogleTests and publish exact executed-case/artifact manifests, treating absent endpoints and incomplete inventory as failures. No temporary Python feature suite is a release oracle. Composite key/routine fixtures, prepared VARBYTE, native IAM renewal, data sharing and wider parity remain separate unqualified scopes.


## Pure foreign-key contract foundation — 2026-10-03

The next unwired helper owns an exact imported/exported/both table plan and validates a fourteen-field normalized SHOW FK result before producing owned ODBC rows. Named constraints group within each foreign table, so sequences may restart across different constraints or tables. Required exact identities, contiguous unique signed-smallint sequences, consistent referenced identities/names/rules and unique key pairs are enforced. Nullable PK names and source row order remain intact; NULL rule/deferrability defaults3/3/7 follow pinned source. Both-direction filtering follows validation of every source row, a deliberately conservative candidate policy. Native/deferred errors and successful session snapshots remain intact; malformed/partial result data cannot become success. Unknown native widths/OIDs are not inferred.

Nine focused GoogleTests cover directions, accepted normalized families, descriptor shape, ownership, groups, source order, defaults, malformed excluded groups and preserved failures. Standalone and root CMake focused tests passed; independent review found no functional blocker and source ranges/test-copy warnings were corrected. The helper has no runtime caller or SQL renderer and does not qualify native SHOW, ordinary-user visibility or broader FK parity. Full required batch gates and hosted CI remain required before acceptance.

Combined candidate validation passed: PostgreSQL67/67unit plus full integration/verifiedTLS; UTF16/UCS4 unit/integration/verifiedTLS; sanitizer unit; Redshift67/67unit/build;241offline runner safety tests; exact absent-endpoint rejection. Hosted Windows/package and pinned live MySQL remain CI gates, not local passes. No additional paid SQL in this code batch.


## Provisioned key/routine diagnostic — 2026-10-03

One separately reviewed batch on e8d451d created the exact three proposed empty fixture objects, granted only SELECT on the two tables and EXECUTE on the exact procedure signature to the existing diagnostic user, and ran the five checked-in GoogleTests. ConnectionTest passed; four catalog cases failed. PrimaryKeys returned no rows for the composite key. ForeignKeys and ProcedureColumns returned syntax errors at inherited CROSS JOIN LATERAL unnest; Procedures returned an unavailable procedures.proargmodes column error. These are observed Redshift failures, superseding source-only hypotheses for this fixture. No procedure was invoked and assertions were not weakened.

Fresh identity/control/artifact checks, durable consumption, individual DDL/grant outcomes and one shared execution deadline bound the batch. Confirmed ownership/signature checks preceded exact procedure, child and parent teardown; independent exact-principal session/query checks verified no remaining owned activity. All three objects were removed. Private request/result and GoogleTest artifacts retain the original failures. The new injected fixture lifecycle helper has no launcher, credentials, cloud calls or budget authority; its offline safety tests support future CI orchestration and never replace feature GoogleTests. Pure PK/FK normalizers remain unwired. Modern capability negotiation and a session execution facet are the next bounded implementation target; full catalog parity remains open.


## Modern catalog execution prerequisite

The optional session-owned ICatalogExecution boundary returns owning normalized catalog results under the original absolute deadline. SessionLease invokes it exclusively, invalidates cache scopes before every attempt and preserves native errors, final snapshots and terminal retirement/exception behavior. Missing or unsupported facets never route to a SQL fallback. No physical backend or ODBC adapter is wired; PostgreSQL and Redshift still expose no executor. The SDK/internal-header manifests explicitly include the new contract.

Pinned upstream SHOW PRIMARY KEYS uses the fixed template `SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;` and three separately bound database/schema/table values. Its default VARCHAR hint is OID1043; an explicitly configured unspecified hint is OID0. Our existing Text parameter remains OID25, so it must not be treated as equivalent. Two protocol GoogleTests prove the existing OID0 path rewrites only markers, keeps identifier values separate (including quote/period/wildcard bytes) and emits unnamed framing with the extra statement Describe used by this driver. They do not prove Redshift acceptance or native metadata. Future execution must gate authenticated show_discovery ParameterStatus, qualify this exchange and normalize the exact result before any adapter wiring.

Independent design/code review accepted this narrow prerequisite. Lease facet tests cover exact request/deadline forwarding, cache invalidation, missing-facet passive states, owning native errors/results, exceptions and retirement; provider-isolation tests explicitly confirm that execution remains disabled. No new paid SQL, authentication scope or catalog parity claim accompanies this package.


## Native SHOW PK proof candidate

ModernPrimaryKeyShowUnspecifiedContract is a future GoogleTest using the actual immutable Redshift session, verified TLS, authenticated bounded show_discovery>=4 and one fixed parameterized SHOW with the three OID0 identifier hints. The existing production resolver may perform its bounded pg_type read for unknown parameter OIDs using the same original deadline; no override suppresses that behavior. Raw failures remain failures. The test normalizes the owning result and checks the composite fixture rows after disconnect. No configured endpoint run or paid request has been admitted for it.

Source inspection exposed a deterministic preparation mismatch: GenericDatabaseConnection retains prepared parameter descriptions, while the pure PK normalizer rejected every nonempty parameter-description list. The bounded repair accepts only an absent list or exactly three known text-family descriptions, rejects unknown/nontext/wrong-count metadata and keeps input descriptions out of catalog output. Focused owning/snapshot and negative tests cover that change; this is not native server metadata proof. The independently reviewed orchestration profile creates/grants/drops only the exact parent table and runs only ConnectionTest plus the new SHOW proof, never the failed inherited catalog inventory, child table or procedure. No launcher admission or runtime catalog wiring is enabled by these candidates.

Candidate validation passed:259 offline orchestration tests, PostgreSQL/Redshift67 unit targets, full PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer unit, future SHOW test registration/build and absent-endpoint rejection. The additional MySQL unsigned INT metadata-to-binary-row regression passed all36 session cases under warning-as-error compilation across PostgreSQL, Redshift, both Unicode profiles and sanitizer builds. These are offline regression/fixture mechanics results, not a configured native SHOW run.


## Native SHOW proof and backend execution candidate

Exact-head 9c0f894 platform CI passed every required gate. Separately admitted
provisioned-modern-pk-001 passed ConnectionTest and
ModernPrimaryKeyShowUnspecifiedContract (2/2) in 6.324 seconds. The exact parent
fixture was removed after ownership checks; independent owned session/query
checks verified cleanup. Independent review accepted this fixed real-session
SHOW exchange and normalized composite-key assertions. OID0/extra Describe are
bound source and wire-test evidence plus the accepted exchange, not a captured
wire trace. Resolver-query occurrence and native widths remain unqualified.
The original inherited catalog failures remain retained separately.

The next backend-only candidate exposes ICatalogExecution only for the explicit
immutable Redshift profile and accepts exact PrimaryKeys requests. It validates
explicit database/schema/table identifiers and authenticated bounded
show_discovery>=4 before I/O, uses one fixed SHOW with three Unspecified inputs
and the original caller deadline, then owns normalized output and native errors.
Unsupported requests/capability never select inherited PostgreSQL SQL. PostgreSQL
execution facet remains absent. ODBC routing and its autocommit/open-cursor
semantics are unchanged and need a separate reviewed package; this candidate is
not a live SQLPrimaryKeys qualification or broader catalog parity claim.


Independent source review accepted the backend-only candidate. Eight executor
GoogleTests cover explicit profile isolation, missing/malformed/old capability,
invalid exact identifiers and unsupported requests without SQL, unchanged
parameters/deadline, owning snapshots, native errors and malformed partial
results without fallback. The future ModernPrimaryKeyExecutionContract reuses
the exact verified-TLS fixture/output assertions of the direct native proof,
but calls the actual execution facet. It is a distinct registered GoogleTest;
no paid inventory or configured run is enabled. Local batch checks passed: eight
executor, sixteen catalog and seventy-nine lease focused cases; PostgreSQL and
Redshift 68 unit targets; full PostgreSQL/UTF16/UCS4 integration and verified
TLS; sanitizer unit; 259 offline orchestration tests; native test registration
and absent-endpoint rejection. Hosted CI owns Windows/package and pinned MySQL
gates. No new paid SQL accompanies this backend-only package.
