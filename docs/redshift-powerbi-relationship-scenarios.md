# Optional single-key Power BI relationship qualification

This is a qualification fixture and two native ODBC tests, not a product feature
or a new resource executor. Source authorship and offline controls establish no
Redshift, driver or Power BI pass. Until the coordinator integrates the closed
stage hook, its readiness is **NOT_EXECUTABLE_UNTIL_ROOT_INTEGRATION**.

The existing composite-key fixture and its native assertions remain unchanged.
For the Import-model experiment here, use one integer column on each side.
Microsoft describes a model relationship as column-to-column, with a specialized
multi-column DirectQuery exception; that exception is outside this experiment.
See [model relationships](https://learn.microsoft.com/en-us/power-bi/transform-model/desktop-relationships-understand)
and [star-schema guidance](https://learn.microsoft.com/en-us/power-bi/guidance/star-schema).
Do not reinterpret a missing composite relationship in this Import model as a
failure of the existing native composite PK/FK metadata contract.

## Fixed optional fixture

The existing database is `odbcpp_pilot`, existing shared schema is
`odbcpp_fixture`, owner is `odbcpp_pilot_admin`, and the only grant recipient is
the already admitted `odbcpp_pilot_test`. This package creates no schema,
principal, password, infrastructure or access rule.

| Table | Columns and declared keys | Fixed rows |
| --- | --- | --- |
| `pbi_parent_single_20261022_q007` | `parent_id INTEGER NOT NULL` PK; `parent_label VARCHAR(16) NOT NULL` | `(1, North)`, `(2, South)` |
| `pbi_fact_single_20261022_q007` | `fact_id INTEGER NOT NULL` PK; `parent_id INTEGER NOT NULL` FK to parent; `amount INTEGER NOT NULL` | `(101,1,10)`, `(102,1,20)`, `(103,2,7)`, `(104,2,13)`, `(105,2,5)` |

Named constraints are `pbi_parent_single_q007_pk`, `pbi_fact_single_q007_pk` and
`pbi_fact_single_q007_fk`. Redshift's declared PK/FK metadata is not evidence that
Redshift enforces those constraints. The fixture separately checks exact row
custody, and the native tests assert values and metadata.

| Parent filter | Expected fact rows | Expected sum of amount |
| --- | ---: | ---: |
| None / cleared | 5 | 55 |
| North | 2 | 30 |
| South | 3 | 25 |

## Closed integration contract

`redshift_powerbi_relationship.py` is a pure fixed SQL planner/custody reducer.
No import or function executes SQL, reads credentials, resumes a resource or
starts a process. Its only stage arguments are the durable state and a member
of this ordered inventory:

| Kind | SQL upper bound before existing wrapper | Preconditions / completion |
| --- | ---: | --- |
| `inspect` | 3 | Both exact names absent, existing schema and distinct existing principals observed |
| `create_parent` | 9 | Verified absence; fixed CREATE, rows and SELECT grant; actual CREATE_ACK and held OID/owner/schema/shape/rows |
| `create_fact` | 10 | Parent remains held with exact shape/rows; same create custody for fact |
| `verify` | 8 | Both objects still match their held OIDs, owners, columns and rows |
| `drop_fact` | 7 | Readers stopped, principal/query cleanup independently verified by lifecycle; exact held identity, shape, rows, no relation locks; RESTRICT; DROP_ACK and original OID absent |
| `drop_parent` | 7 | Fact absent or acknowledged dropped; equivalent exact cleanup; shared schema still has its original identity |

One successful lifecycle consumes six helper calls, six logins and at most 62
SQL statements including the existing three-statement wrapper per helper. A
counter that combines statements and logins must therefore charge **68**. The
existing full native plan (36 helpers / 392 inclusive) cannot absorb this optional
fixture within its current 40-helper / 450-inclusive guards, especially with
network helpers. Do not raise those guards or silently add these stages. The
coordinator must separately admit an app/relationship execution partition using
the minimum required fixtures, while preserving the complete union of all
applicable cases/profiles for the checkpoint. The prior app estimate of 33
combined helpers / 234 inclusive plus this fixture would be 39 / 302; that is a
planning bound, not reuse of an earlier admission or proof that all later helper
calls fit. Account actual counters and network helpers before fresh admission.

A failed or interrupted create/drop leaves its intent unknown; no blind retry,
discovered-object adoption, CASCADE or automatic unknown-custody cleanup is
available. Even an exact preexisting table is a collision. CREATE_ACK follows
successful CREATE/INSERT/GRANT in the canonical error-stopping psql invocation;
missing/incomplete output is not an acknowledgement. An ambiguous failure can
therefore leave a retained object requiring separate verified reconciliation.

The coordinator must add a narrowly reconstructed stage branch to canonical
`Operations._closed_stage`, rather than passing arbitrary SQL through it:

1. Build the exact stage with `stage_for(state, kind)`. Its public keys are
   `stage`, `keys`, `role`, `query`, `statements`; `keys` is the tuple
   `('POWERBI_SINGLE_KEY',)`, role is always `admin` and stage names start with
   `powerbi_relationship_`. Reject any changed field by exact reconstruction.
2. Persist `begin(state, stage)` **before** one actual dispatch. The reducer
   retains the preceding state and exact stage, validates both at completion,
   and records CREATE_UNKNOWN/DROP_UNKNOWN before a mutation can happen.
3. Use the existing native psql input/error-stop, identity, private-custody and
   process controls. Keep its separately validated `RSD1` wrapper frame out of
   the bounded `RSPBI1` payload. Only native exit zero plus exact frames may be
   passed to `complete(intent, payload)`, then persist the resulting state.
   On exception/nonzero/unknown completion retain the original intent, report
   failure and do not claim fixture cleanup complete.
4. Use the original absolute execution/cleanup deadlines, existing statement
   timeout ceiling (15,000 ms), remaining-time checks, helper/statement/login
   accounting, readers-known and principal/query-zero gates. The planner grants
   no additional time or quota. Count the optional six helpers and 62 statements
   together with every existing helper; refuse admission if the total does not
   fit existing bounds. Repeated verify calls also consume actual counters.
5. Retain the shared schema. Cleanup is exact reverse dependency order using
   held OIDs, with RESTRICT and independent named/original-OID absence. Never
   erase the unknown state to force cleanup success.

After actual custody is established, stage the existing private DSN with verified
TLS, exact endpoint/driver image, original limits and test principal. There is no
new secret store or arbitrary input parser in this package. Export
`ODBCPP_TEST_DSN`, `ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER` (canonical pinned
driver path), `ODBCPP_REDSHIFT_POWERBI_PARENT_OID` and
`ODBCPP_REDSHIFT_POWERBI_FACT_OID` (observed decimal OIDs), and the explicit
`ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP=1` marker. OIDs are published only from
successful custody completion, never guessed or inferred from names.

## Native ODBC evidence and inventory

The optional executable links the actual platform driver manager, not core.
It uses the existing owning environment helper, observes a unique exact loaded
driver path after connecting, verifies database/principal and both actual OIDs,
and bounds every case by one original 40-second window with at most five seconds
for an individual operation. Final process/principal/input cleanup remains the
canonical lifecycle's responsibility. The external runtime must retain its
source/image hash pins; a path match alone is not a cryptographic image pin.

* `PowerBiRelationshipNative.ColumnsAndSingleKeyMetadata` asserts SQLTables,
  SQLColumns (identity, order, integer/varchar types, non-nullability), both
  SQLPrimaryKeys results and the single SQLForeignKeys row including KEY_SEQ=1
  and exact named PK/FK constraints.
* `PowerBiRelationshipNative.RowsFilterAndReset` fetches every exact parent/fact
  value and checks SQL join counts/sums before filtering, for each parent, and
  after clearing the predicate. This is a SQL baseline, not a Power BI filter
  propagation assertion.

Absent opt-in marker is a reported SKIP in an ordinary nonadmitted local run.
Malformed/present-empty marker, missing DSN/OIDs/image, setup refusal or wrong
result is a failure. A newly initiated applicable full live checkpoint must add
**both cases** to its complete inventory, stage all inputs and require no skips.
Use each platform's owning DM/driver width pair and actual inventories; ANSI
calls here do not establish wide-call behavior. Do not substitute a selected
relationship run for the complete applicable existing suite.

## Actual Power BI procedure and separate results

Run only after fresh finite admission, actual fixture readiness and native
baseline, using the already qualified ODBC source and bounded application owner.
Record the actual Power BI version, source/driver image, mode, input/profile
bindings and raw artifacts. Do not claim any step from metadata or screenshots
of a different attempt.

1. Import both tables through Navigator. Confirm exact columns and row counts.
   Before editing relationships, inspect Model view and record whether source
   relationship discovery created the expected active single-key relationship.
   An absent/incorrect automatic relationship remains a distinct failed or
   limited discovery result; later manual success must not overwrite it.
2. In a separate manual-model result, create parent `parent_id` (one) to fact
   `parent_id` (many), active, single filter direction from parent to fact.
   Confirm the one/many markers, active line and direction. Preserve any dialog
   refusal as failure rather than changing cardinality, direction or data.
3. Add a parent-label slicer and a fact count plus sum-of-amount visual. First
   clear all filters: expect 5 and 55. Select North: expect 2 and 30. Select
   South: expect 3 and 25. Clear: expect 5 and 55. Capture actual visual values,
   selected parent and relationship properties for each result.
4. Refresh with North selected; expect the same relationship, 2 and 30. Clear
   and confirm 5 and 55. Save a new owned PBIX, close the app, reopen that exact
   file, inspect the relationship and repeat North/South/clear. Refresh again
   and repeat. Save/reopen and refresh success are separate observations; a SQL
   join or native reconnect cannot stand in for them.
5. Exit through the bounded owner and retain raw driver/DM traces and artifacts.
   Only after readers are known stopped and principal/query cleanup succeeds
   may the lifecycle retire exact fixtures/inputs and pause the resource.

Do not weaken MAXRESULTS, SQL/TLS/type assertions or native composite metadata
tests to make the model work. Any app-specific profile change needs its own
explicit reviewed finite binding. These scenarios establish bounded observations,
not full Power BI certification or automatic-discovery support by inference.

## Physical ownership

| Component | Exact owner | Reuse and dependencies / proof |
| --- | --- | --- |
| Fixed qualification SQL and fixture custody | `tools/development/redshift_powerbi_relationship.py` | External test lifecycle only; standard-library pure functions; canonical Operations must reconstruct it; offline custody tests do not prove SQL runtime compatibility |
| Inert safety controls | `tools/development/test_redshift_powerbi_relationship.py` | Synthetic frames/state only; imports the pure fixture planner; no feature-result parsing |
| Driver metadata/value assertions | `tests/integration/it_redshift_powerbi_relationship.cpp` | Test-only adapter-facing exported ODBC contracts; existing `tests/test_live_package_inputs.h`, owning platform DM and GoogleTest; no production dependency or DDL |
| Human-visible app procedure | `docs/redshift-powerbi-relationship-scenarios.md` | Separates SQL baseline, discovery, manual modeling and persistence; Microsoft primary documentation above |

No reusable production contract, transport primitive, shared session lifecycle or
backend protocol is introduced. These fixed qualification-only names, rows,
psql frames and app expectations belong outside `sdk/`; moving them into the SDK
would expose a test fixture as production API. Independent review must verify
this placement and the coordinator's later integration before execution.
