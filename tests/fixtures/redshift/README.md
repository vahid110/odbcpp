# Redshift catalog fixture proposal

`catalog_contracts.sql` is a source-reviewed proposal, not an executor or a live-qualified fixture. No SQL was executed to validate it. Manual review and a new bounded admission under the current canonical pilot budget, identity, network, deadline and cleanup controls are required before any DDL or diagnostic SQL. The existing `odbcpp_fixture` schema must already be present; this proposal creates no schema, grant, user, database or cloud resource.

The three exact names use the finite batch suffix `20261003_c01`. Before admission, verify that both table names and the procedure name with **every signature** are absent in the intended database/schema. Any collision blocks this proposal; do not replace or remove the existing object. Execute admitted setup statements individually, stop on the first failure or uncertain outcome, and record confirmed creations. This file has no automatic retry or idempotent replacement. A future executor must preserve the procedure's dollar-quoted body as one statement rather than split at every semicolon.

The empty parent and child tables declare a two-column key in reverse physical column order. Expected PK sequence is `key_b=1`, `key_a=2`; expected FK pairs are `ref_b -> key_b` at sequence 1 and `ref_a -> key_a` at sequence 2. Constraint names are server-generated and must be observed, not hard-coded. Redshift key constraints provide informational metadata and do not establish enforced uniqueness or referential integrity. The procedure has `p_input IN INTEGER` followed by `p_result INOUT INTEGER`, with a no-op body and explicit invoker security. It must never be invoked. The expected parameter-mode mapping for the pinned upstream postprocessor is IN -> SQL_PARAM_INPUT (1), INOUT -> SQL_PARAM_INPUT_OUTPUT (2); actual descriptors and rows remain unqualified.

The next reviewed diagnostics can inspect SQLPrimaryKeys, imported/exported/both-table SQLForeignKeys, SQLProcedures and SQLProcedureColumns against these exact objects. Capture descriptor types, names, key/parameter order, nullable/default/reserved fields and errors; do not substitute a successful empty result for failure. SHOW CONSTRAINTS requires ownership or schema USAGE plus table SELECT. Routine SHOW metadata requires ownership or schema USAGE plus routine EXECUTE. An owner/ordinary-user path is **conditional**: admission must name the creator and diagnostic principal and verify their existing privileges. These objects receive no grants from this file. A privilege-separated denied fixture remains **unprovisioned** and requires its own review; this package grants no privileges and tests no denial behavior.

Teardown statements are commented out in the SQL file and must be reviewed/executed separately. Drop only an object whose creation in this admission was confirmed and whose identity/ownership remains verified. Remove the procedure by its exact `(INTEGER, INTEGER)` input signature, then the child table, then the parent table using RESTRICT. On partial setup failure, select only confirmed creations; an uncertain creation or unexpected dependency requires reconciliation instead of broad cleanup. No DROP SCHEMA, CASCADE, IF EXISTS, or preexisting-object deletion is proposed.

Official syntax references, checked 2026-10-03 (AWS documentation is not an immutable source revision): [CREATE TABLE](https://docs.aws.amazon.com/redshift/latest/dg/r_CREATE_TABLE_NEW.html), [CREATE PROCEDURE](https://docs.aws.amazon.com/redshift/latest/dg/r_CREATE_PROCEDURE.html), [PL/pgSQL no-op](https://docs.aws.amazon.com/redshift/latest/dg/c_PLpgSQL-statements.html), [DROP PROCEDURE](https://docs.aws.amazon.com/redshift/latest/dg/r_DROP_PROCEDURE.html), [DROP TABLE](https://docs.aws.amazon.com/redshift/latest/dg/r_DROP_TABLE.html), [SHOW CONSTRAINTS](https://docs.aws.amazon.com/redshift/latest/dg/r_SHOW_CONSTRAINTS.html), [SHOW PARAMETERS](https://docs.aws.amazon.com/redshift/latest/dg/r_SHOW_PARAMETERS.html).

Immutable behavioral reference: official Amazon Redshift ODBC source commit `56d35297f9bee0cc31c0148581c87ca455639a39`, [SHOW query definitions](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsMetadataAPIHelper.cpp#L186), [procedure parameter discovery](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/src/odbc/rsodbc/rsMetadataServerProxy.cpp#L1118). This does not claim legacy discovery equivalence, broader catalog parity, overload coverage or live qualification.


`primary_key_edges.sql` is a distinct offline two-table proposal for a visible no-key object and a quoted composite key object. The quoted stored table name contains a period, internal double quote, apostrophe, percent and underscore; physical column order differs from declared key order, and the constraint name is explicit. Only the new `primary_key_edges` inventory may use these names; earlier inventories and consumed admissions remain unchanged. SQLPrimaryKeys receives stored identifier bytes, while fixture DDL quotes identifiers and collision/ownership queries escape string literals. No mixed-case policy change is proposed.

A future setup requires existing schema USAGE for the diagnostic principal, verified by a standalone [HAS_SCHEMA_PRIVILEGE](https://docs.aws.amazon.com/redshift/latest/dg/r_HAS_SCHEMA_PRIVILEGE.html) read; this leader-node function must not be combined with table queries. SELECT grants are limited to the two exact tables. Collision refusal, separately recorded creations, uncertain-outcome reconciliation and confirmed-ownership selective RESTRICT teardown remain mandatory. Source-grounded no-key empty rows and quoted lookups are intended future GoogleTest assertions, not observed native results. Missing/denied outcomes remain separate unknowns. No new launcher or paid admission is supplied by this proposal.

The first consumed edge attempt stopped before creation because the native psql privilege output was `t`, while its reviewed helper expected `true`; preserve that failure and do not replay it. The next offline helper uses a standalone searched CASE with exact `schema_usage_allowed`/`schema_usage_blocked` markers, accepts only the trimmed positive marker, and rejects false/NULL/errors/late or ambiguous output. This keeps the same user/schema/USAGE predicate. Native CASE compatibility still needs separate finite admission because the function documentation describes a textual return type; a SQL error blocks before creation.

The future `primary_key_missing_legacy` profile has an empty object inventory and exactly ConnectionTest plus OdbcPrimaryKeyMissingLegacyContract. It validates the existing fixture hash without parsing or dispatching DDL. The fixed absent name `m2_pk_absent_20261004_c01` is checked before and after tests and is never created, granted or dropped, even if it appears. Identity, canonical schema USAGE and activity checks precede tests within the execution deadline. Post-absence and independent activity checks retain a frozen cleanup cutoff and separate evidence. A concurrent transient appearance between checks remains a limitation; any observed appearance invalidates the scope. No live qualification or admission is implied.


### SQLCancelHandle statement compatibility

`RedshiftCancelHandleRealTest.IdleNeedDataRefillNullAndSiblingRemainOwning`
requires `ODBCPP_REDSHIFT_CANCEL_HANDLE_ADMISSION=cancel_handle_happy_v1` plus
fresh reviewed finite admission. One verified TLS login, at most five SQL
exchanges (one prepare, two prepared executions, two fixed SELECTs), 40 seconds
aggregate and five seconds per operation; no fixture objects, DDL or DML.
NeedData cancellation is local before dispatch. The case checks owning sibling
rows, NULL/refill recovery, idle cursor preservation and DBC IM001 refusal.
It does not demonstrate active native server cancellation. That timing-dependent
native scenario remains pending separate bounded qualification; deterministic
unit cases exercise active cancellation/completion races through both APIs.
Windows/other widths/platform execution remains pending the periodic matrix.


### DatabaseMetadataCurrentDbOnly existing-principal scope

`RedshiftMetadataScopeRealTest.DefaultVisibleAndCurrentReconnectKeepCatalogNullSiblingAndEmptySemantics`
requires `ODBCPP_REDSHIFT_METADATA_SCOPE_ADMISSION=metadata_scope_existing_principal_v1`
plus fresh independently reviewed finite admission at the combined checkpoint.
Its source ledger allows three TLS logins and at most nine SQL exchanges:
three SQLTables catalog enumerations, three fixed NULL SELECTs and three
SQLColumns queries with an explicit empty catalog. Aggregate40s, each operation5s,
maximum64 returned catalogs, no fixtures, DDL, DML, shares, grants or resources.
Source/native binary/head and actual selected cases must be bound by admission;
this marker alone grants no execution authority. Unknown completion fences SQL.

The case tests default/TRUE equivalence, FALSE catalog inclusion under existing
visibility, A/W login and catalog calls, typed NULL sibling ownership, explicit
empty-catalog no-match and invalid-option recovery without echoed values. It
uses LEGACY discovery to qualify actual SVV view grammar. Authenticated sharing
flags can legitimately leave FALSE effective-current. This is not proof of
two-database collisions, SHOW fanout, or privileged/denied visibility: actual
cross-database and SHOW cases remain pending applicable existing-principal
inputs and separate bounded case authoring/qualification. No database/share
creation or privilege expansion is implied. Native execution remains pending.


The conditional modern case
`RedshiftMetadataScopeShowRealTest.TwoVisibleDatabasesKeepCommonSchemaIdentityThroughWideFanoutAndSibling`
is separately selected by
`ODBCPP_REDSHIFT_METADATA_SCOPE_SHOW_ADMISSION=metadata_scope_two_visible_contexts_v1`.
Fresh admission must bind existing `ODBCPP_REDSHIFT_METADATA_SCOPE_SECOND_DATABASE`
and `ODBCPP_REDSHIFT_METADATA_SCOPE_COMMON_SCHEMA` names, both visible to the same
ordinary principal. It requires exactly two visible databases (including
odbcpp_pilot) and a schema collision already present in them; unexpected
visibility fails before schema fanout. No fixtures or grants are created.
Explicit controls MAXRESULTS3/MAXROWS128/MAXCELLS4096/MAXCOLUMNS18 are checked,
not enlarged by the case. Admission must also bind adequate finite SQL/name/
metadata/byte budgets. Maximum one TLS login/six SQL exchanges,40s aggregate,
five seconds per operation,64 returned schema rows. It checks owning catalog
identity through actual SHOW discovery/fanout, A/W calls and NULL descriptors.

This authored case is UNRUN. A single available database, absent authenticated
sharing/external capability, or lack of an existing common schema is a concrete
coverage dependency. Do not skip it as passed, provision a fixture automatically,
or substitute the legacy case's success for modern multi-database qualification.

## Signed negative C numeric scale case

`RedshiftNegativeNumericScaleRealTest.DescriptorLossNullOwnershipAndRecovery`
uses `ODBCPP_REDSHIFT_NEGATIVE_NUMERIC_SCALE_ADMISSION=negative_numeric_scale_v1`.
It covers descriptor-controlled bound/GetData conversion, whole-digit refusal
(22003), fractional loss (01S07), NULL/output canaries, unaligned owning APD
input and same-cursor recovery. SQL target scale support is unchanged.

The authored case is not live acceptance. Fresh reviewed finite admission must
bind its exact green source head and native binary. Its bounds are one verified
TLS login, four SQL operations (one fixed SELECT, one Prepare, two Execute),
40 seconds aggregate and at most five seconds per operation; no DDL or DML.
Required controls include MAXSQLBYTES at least455, MAXPARAMETERS at least1,
MAXPARAMETERBYTES and MAXPARAMETERTOTALBYTES each at least11 (the inherited
fixture floor, even though this case sends five bytes), MAXCOLUMNS at least6,
MAXROWS at least5, MAXCELLS at least30 and MAXMETADATAENTRIES at least18.
Other existing admission controls and unknown-completion fences remain required.

## Numeric input target rescaling

`RedshiftNumericTargetRescaleRealTest.PreparedFractionWholeNullAndOwningRefill`
requires `ODBCPP_REDSHIFT_NUMERIC_TARGET_RESCALE_ADMISSION=numeric_target_rescale_v1`.
One verified TLS login, one Prepare and three Execute operations (positive,
negative recovery and NULL), 40 seconds aggregate, five seconds per operation.
A whole-overflow Execute refuses locally with22003 and sends no SQL. No DDL/DML.
Literal12.34/-12.34 echoes distinguish client truncation from server rounding
of12.345/-12.345. Unaligned APD authority, input/result ownership, NULL poison
output/canaries and neighboring integer42 are checked.

Inherited controls require MAXPARAMETERBYTES/MAXPARAMETERTOTALBYTES each at
least11, MAXPARAMETERS1, MAXSQLBYTES at least51, MAXCOLUMNS2, MAXROWS1, MAXCELLS2
and MAXMETADATAENTRIES3 (two columns and one prepared parameter).
Fresh reviewed exact green source/binary and finite admission are mandatory;
authored/compiled/LIST evidence is not actual live acceptance.

## Character input numeric loss diagnostics

`RedshiftCharacterNumericLossRealTest.AnsiWideLossRefusalNullAndPreparedRecovery`
uses `ODBCPP_REDSHIFT_CHARACTER_NUMERIC_LOSS_ADMISSION=character_numeric_loss_v1`.
Known valid NUMERIC/DECIMAL target whole or fractional loss from ANSI/wide
character inputs refuses locally with22001. Both paths repair/rebind and echo
exact12.34/-12.34; NULL, statuses, caller/result ownership and canaries remain.
One verified TLS login, one Prepare and three successful Execute operations,
40 seconds aggregate and five seconds per operation; four local refusal calls
add no SQL exchanges. No DDL/DML. Required controls: MAXSQLBYTES51,
MAXPARAMETERS1, MAXPARAMETERBYTES/MAXPARAMETERTOTALBYTES each11,
MAXCOLUMNS2, MAXROWS1, MAXCELLS2 and MAXMETADATAENTRIES3. Exact source/binary,
reviewed finite admission and cleanup are mandatory. Authored/compiled/LIST
evidence is not actual native acceptance; execution belongs to the periodic batch.

## Fixed numeric SQLGetData completion

`RedshiftFixedNumericCompletionRealTest.FractionWholeNullAndVariableSourceCompletion`
requires
`ODBCPP_REDSHIFT_FIXED_NUMERIC_COMPLETION_ADMISSION=fixed_numeric_completion_v1`.
One verified TLS login and one fixed SELECT; 40 seconds aggregate, at most five
seconds per operation. No Prepare, DDL, DML, new table, recovery query or resource.
The exact query returns five cells in one row: positive/negative DECIMAL(5,3),
VARCHAR(6), NULL decimal and neighboring integer42.

Numeric CHAR/WCHAR fractional prefixes retain01004 and original byte lengths;
a repeated same-column call yieldsSQL_NO_DATA without altering sentinels.
Zero/whole-capacity22003 and missing NULL indicator22002 remain retryable.
Wide ARD authority, unaligned output canaries, source metadata, variable VARCHAR
suffix continuation and neighboring value remain checked.

Inherited minimum controls remain MAXPARAMETERS1,
MAXPARAMETERBYTES/MAXPARAMETERTOTALBYTES each11, even though this SELECT sends
no parameters. Additional controls: MAXSQLBYTES at least191, MAXCOLUMNS5,
MAXROWS1, MAXCELLS5 and MAXMETADATAENTRIES5. Marker-first setup, original
40-second window, known/unknown completion fences and same-principal handle/
session cleanup remain inherited. Actual execution requires reviewed finite
admission bound to exact green source and native binary; authoring, compilation
and LIST do not constitute live acceptance. Execution belongs to the periodic
combined batch.


## C_NUMERIC approximate SQL target range — authored, execution pending

`RedshiftCnumericApproximateRangeRealTest.PreparedFitSubnormalWholeNullAndRecovery`
uses `ODBCPP_REDSHIFT_CNUMERIC_APPROXIMATE_RANGE_ADMISSION=cnumeric_approximate_range_v1`.
It binds unaligned fixed numeric structs with captured APD dimensions to REAL,
FLOAT and DOUBLE, checks fitting values, a positive REAL subnormal, large signed
DOUBLE values, poisoned NULL bypass, positive/negative REAL overflow22003 and
repaired execution on the same prepared statement. Caller bytes/indicators,
parameter status/processed count and an INTEGER neighbor remain observable.

Finite ledger: one ordinary verified TLS login; one fixed parameterized SELECT
Prepare; seven Execute attempts (five successful value/NULL cases and two
expected local refusals). Conservatively reserve eight SQL operations, including
both refusal attempts. No DDL, DML, fixture/resource creation or new principal.
The existing aggregate40-second window and per-operation5-second caps apply;
expiry or unknown completion follows the existing cleanup fence.

Minimum authored floors: SQL text covers the fixed echo statement; three
parameters,102 bytes per parameter,306 aggregate parameter bytes; four columns,
one row,four cells and12 metadata entries. Inherited setup floors remain required.
Current authoring/compile/LIST is not paid admission or native execution. Actual
server subnormal behavior and enforcement remain unproved until the accumulated
CI-first live checkpoint binds this exact source/native image and fresh finite
accounting/one-use controls. No vendor binary parity is claimed.


## Fixed TIME/TIMESTAMP SQLGetData completion — authored, execution pending

`RedshiftFixedTemporalCompletionRealTest.FractionWholeNullWideAndVariableRecovery`
uses `ODBCPP_REDSHIFT_FIXED_TEMPORAL_COMPLETION_ADMISSION=fixed_temporal_completion_v1`.
The fixed echo SELECT returns TIME12:34:56.789, TIMESTAMP2026-10-12 12:34:56.789,
a VARCHAR copy of the TIME text, NULL TIME and INTEGER42. It checks whole-buffer
22003 refusal/retry, first fractional01004 and full byte indicators, repeated
NO_DATA with untouched output/indicator, unaligned ARD WCHAR with width/odd-byte
canaries, NULL22002 recovery, unchanged VARCHAR continuation and full-value
column restart. The first fractional outputs remain exact; no temporal
canonicalization or reference-binary parity is claimed.

Finite ledger: one ordinary verified TLS login, one fixed direct SELECT; no
Prepare, DDL, DML, extra recovery query, objects or new principal. Original40s
aggregate and <=5s operation caps, marker-first setup, sticky unknown-completion
fence, handle/session release and exact-principal cleanup remain inherited.
Minimum controls cover the exact SQL byte length, five columns, one row, five
cells and five metadata entries; inherited parameter floors remain required
even though this SELECT binds no parameters. Native compilation/LIST is not a
live result. Execute with the accumulated CI-first batch only after exact green
head/native binary and fresh reviewed finite accounting/one-use admission.


## APD restricted input pair revalidation — authored, execution pending

`RedshiftApdRestrictedPairRealTest.PreparedDescriptorRefusalNullAndOwningRecovery`
uses `ODBCPP_REDSHIFT_APD_RESTRICTED_PAIR_ADMISSION=apd_restricted_pair_v1`.
The one fixed prepared SELECT echoes INTEGER, DATE and INTEGER42. Valid character
inputs establish literal values; APD DATE-to-INTEGER and NUMERIC-to-DATE mutations
must fail locally with07006 after restoring the descriptor-cleared DataPtr.
Unsupported NUMERIC-to-DATE NULL still rejects, then legal DATE NULL preserves
the output and final character refill recovers on the same statement/session.
Parameter status/processed count, caller bytes/indicators and typed neighbor
remain observable. This covers adapter/shared regression; PostgreSQL execution
is separate evidence and never proves native Redshift behavior.

Finite ledger: one ordinary verified TLS login, one Prepare and seven Execute
attempts (four echoes and three local07006 refusals). Conservatively reserve all
eight SQL operations, including refused attempts. No DDL/DML, objects, new
principal, cancellation/observer or resource changes. Existing aggregate40-second
and per-operation5-second caps, marker-first setup, sticky unknown-completion,
release and exact-principal cleanup remain mandatory.

Minimum authored floors: exact SQL byte length, two parameters,10 parameter
bytes each/12 aggregate, three columns, one row, three cells/nine metadata
entries; inherited setup floors remain. Authored source, compile and LIST do not
prove live execution. Actual Redshift bodies wait for accumulated CI-first
acceptance with exact green source/native binary and fresh reviewed finite
accounting, canonical lock and durable one-use admission. No vendor binary
parity or expanded conversion matrix is claimed.


## Binary application INTEGER input

For `SQL_C_BINARY` bound to `SQL_INTEGER`, this driver accepts exactly four
bytes containing the host object representation of a signed `SQLINTEGER`.
Construct those bytes with `memcpy` from a `SQLINTEGER`; they are not a portable
serialized integer or PostgreSQL wire representation. Unaligned application
buffers are supported. A different nonnegative byte count returns `22003`
before dispatch; negative lengths retain `HY090`. NULL skips payload access.
`SQL_C_DEFAULT` remains ordinary `SQL_C_SLONG` input. Deferred fixed-numeric
input remains `HYC00`; raw SQL binary targets retain their existing byte values.

The primary ODBC length contract requires equality. Pinned official AWS source
uses a host-int copy and accepts at least four bytes, ignoring an oversized
suffix. This driver follows exact equality; this documented source difference
is not a claim of vendor binary parity or release correspondence.

`RedshiftBinaryIntegerInputRealTest.HostIntegerLengthsNullAndOwningRecovery`
requires `ODBCPP_REDSHIFT_BINARY_INTEGER_INPUT_ADMISSION=binary_integer_input_v1`
and fresh independently reviewed finite admission. One TLS login, one prepare,
seven conservative Execute attempts (five typed/NULL echoes, two `22003`
refusals), at most eight SQL exchanges, aggregate40s and per-operation5s.
The fixed SELECT uses one parameter and an INTEGER42 neighbor, with no fixture,
DDL or DML. The case covers signed extrema, unaligned host bytes, canaries,
NULL output preservation, status/processed accounting and same-session recovery.
Normalized parameter floors are11 bytes per/aggregate, two columns, one row,
two cells and six metadata entries. Actual Redshift execution remains queued
for the combined CI-first checkpoint; compile/LIST do not establish a live pass.

The separately authored
`PreparedStatementIntegrationTest.BinaryIntegerHostBytesRejectLengthsNullAndRecover`
uses the existing shared fixture and the same fixed SELECT/scenarios. Private
PostgreSQL execution proves only the shared INTEGER conversion/wire behavior;
it cannot substitute for actual Redshift acceptance.


## Ordinary numeric C-to-temporal input refusal — authored proposal, execution pending

`RedshiftNumericTemporalInputRealTest.PreparedRefusalDefaultNullAndRecovery`
uses `ODBCPP_REDSHIFT_NUMERIC_TEMPORAL_INPUT_ADMISSION=numeric_temporal_input_v1`.
It prepares a DATE echo with an INTEGER neighbor42. CHAR DATE, DEFAULT DATE,
legal DEFAULT DATE NULL and repaired CHAR DATE have literal owning-result
assertions; early SLONG-to-DATE bind and effective APD SLONG-to-DATE ordinary/
NULL inputs must return07006 with exact status/progress and input canaries.

The finite ledger is one TLS login, one Prepare, six Execute attempts (four
echoes and two expected local refusals), conservative SQL upper7,40-second
aggregate and5-second per-operation caps. No DDL/DML or resources. Effective
minima: one parameter10bytes/10total, two columns, one row, two cells and six
metadata entries; MAXSQLBYTES must fit the exact checked-in SELECT.
Compilation/LIST and actual Redshift execution remain separate evidence.
The proposal marker does not grant paid admission or extend any spending,
principal, credentials, network, resource or authorization horizon. Execution
requires the integration owner's combined exact-head CI-first checkpoint and
fresh independently reviewed finite admission.

