# Release assessment and bounded scope

Assessment date: 2026-09-23. Code baseline: `4f6de2a`.
Status: PostgreSQL-first planning baseline; product selections below remain open.
Active implementation scope is PostgreSQL and Redshift only. RDS, Aurora and
Athena are not included. SDK architecture is mandatory during PostgreSQL
consolidation (G9a), exercised by Redshift (G9b); only SDK productization is
deferred (G12). The mandatory PostgreSQL handoff is PG-BETA in ROADMAP.md.
No implementation or external Redshift validation was performed for this assessment.

## Assessment: what the evidence establishes

| Area | Evidence at baseline | Release implication |
|---|---|---|
| PostgreSQL implementation | [Audit](ODBC_CONFORMANCE_AUDIT.md), 360 recorded batches, 76 exported symbols; 11 verified and 38 partial operations | Substantial functional foundation, not a conformance percentage or release certification |
| Validation | [Latest CI](https://github.com/vahid110/odbcpp/actions/runs/35865452744), [workflow](.github/workflows/ci.yml) | Linux PostgreSQL/transport runs, Linux sanitizer units, macOS iODBC UTF-16/UCS-4 and Windows units are green; Windows live integration is missing |
| Redshift evidence | [Factory](core/database/database_factory.cpp), [build selection](CMakeLists.txt), [test fixture](tests/integration/it_redshift_real.cpp) | Redshift selects the PostgreSQL parser. The `RedshiftProd` DSN is configured to PostgreSQL in CI; names do not prove a Redshift connection |
| Reuse seams | [Database interface](core/database/i_database_connection.h), [parser interface](core/database/i_protocol_parser.h), [transport interface](core/transport/i_transport.h) | Useful starting points, but protocol/auth messages are PostgreSQL-shaped; portability to unrelated protocols remains unproven |
| ODBC/backend coupling | [Handle implementation](odbc/odbc_handles.cpp): direct parser construction, marker scanning, PostgreSQL OIDs and catalog SQL | Backend creation and semantic mapping need a narrow seam before claiming a reusable SDK |
| Other databases | [MySQL parser](core/database/mysql/mysql_protocol_parser.h) throws not-implemented errors | Placeholder, not a working second protocol |
| Distribution | [Install exports](cmake/Install.cmake), [installation scripts](install/) | Packaging primitives exist; clean-machine usability is not established |
| Evidence quality | Integration fixtures can skip when connections fail | Release evidence must identify endpoint, executed tests, allowed skips, and environment; green alone is insufficient |

Recent work found real bound-length bugs and strengthened failure-path coverage.
However, many later batches added successful permutations without closing an
operation or product milestone. The new stop rule is the finite gates below.
There is no reliable overall completion percentage or validated backlog velocity.

AWS documents material differences between PostgreSQL and Redshift; sharing a
parser does not establish SQL/catalog/type compatibility. Its ODBC documentation
also distinguishes standard credentials from IAM and federation. The pilot must
verify the selected method rather than infer it from PostgreSQL success.
Sources: [database differences](https://docs.aws.amazon.com/redshift/latest/dg/c_redshift-and-postgres-sql.html),
[authentication methods](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-authentication-ssl.html).

## Product decisions and provisional profile

| Decision | Provisional planning assumption | Consequence if changed |
|---|---|---|
| D1: first application/workflow | Driver Manager C++ smoke client plus one user-selected application for PostgreSQL first, then Redshift; latter not yet selected | Application trace determines required APIs. A C++ smoke test alone cannot close application acceptance |
| D2: Redshift deployment/access | One user-supplied provisioned or Serverless endpoint; type/version, network access, test schema and permissions recorded | No live compatibility claim until available. Do not provision or spend on cloud resources implicitly |
| D3: authentication | Database credentials over certificate- and hostname-verified TLS for initial pilot | If IAM/SSO is mandatory, it becomes a gate before beta and requires a fresh estimate; do not ship an unusable credential-only beta |
| D4: supported systems | Existing Linux and macOS configurations; Windows beta only with live Driver Manager evidence | If the selected application is Windows-only, Windows live validation becomes critical path; otherwise label Windows experimental until validated |
| D5: workload | Forward-only, one row/parameter set at a time; declared scalar types and bounded result sizes | Arrays, streaming, cancellation, or larger workloads required by D1 must be promoted explicitly before freezing scope |

These assumptions allow planning, not a silent product decision. G0 is closed separately for each backend; PostgreSQL decisions are required
first. Redshift endpoint/authentication decisions do not block the PostgreSQL
checkpoint. The user has been asked for the first application
and required authentication. Endpoint details must be supplied through the
normal secure configuration path; never put credentials in this document.

## Initial supported surface

- Connect/disconnect, verified TLS, chosen credential method, deadlines and diagnostics.
- Direct/prepared execution, single input parameter sets, forward fetch,
  bound scalar columns, chunked character/binary retrieval, explicit NULLs.
- Core integers, exact/approximate numeric, character/Unicode, boolean,
  date/time/timestamp, and binary values where verified for each backend.
  Redshift mappings and precision limits require G6 evidence; do not copy
  PostgreSQL-only types or OID assumptions into its support table.
- Commit/rollback/autocommit for backend-supported workflows, truthful
  capability reports, and table/column discovery required by the first application.
- Additional exported APIs remain documented as supported/tested, safely
  unsupported, or experimental per backend. An export is not a promise of full
  ODBC conformance; advertised support must agree with behavior.

No blanket claim of complete ODBC, full Redshift feature parity, or public SDK
compatibility. Unsupported features must fail predictably and must not be
advertised as supported. Required workflows cannot be narrowed silently.

## Finite release gates

All gates are **OPEN** at planning baseline, meaning release evidence is not
assembled or complete, not that all behavior is broken. Existing tests count;
do not reimplement or duplicate them. Each gate closes with a commit/run link,
configuration, cases executed, result, and explicit residual limitations.
The implementation owner maintains evidence; the product owner settles scope.
Architecture acceptance is defined in [BACKEND_BOUNDARY.md](BACKEND_BOUNDARY.md).
G9a is required during PostgreSQL consolidation, not postponed until Redshift
or SDK packaging. Its four work items must be estimated in M0 before M1 dates
are committed.

| ID | Classification / milestone | Acceptance evidence |
|---|---|---|
| G0 | Required verification / M0 | Record D1–D5, exact platform/server/app versions, scalar type list, workload limits and applicable API list. Map each of the 38 partial audit rows to a gate or a deferred item; no unclassified release promise. Freeze a named test manifest |
| G1 | Evidence integrity M1; Redshift pilot M2 | Release tests fail on absent endpoint/authentication rather than skip. Confirm actual server identity; Redshift results cannot be satisfied by PostgreSQL. On real Redshift: verified TLS login, `SELECT 1`, prepared scalar+NULL, fetch, one table/column metadata call, invalid SQL and clean disconnect |
| G2 | Required verification / M1 | Review existing manifest for supported conversions: for each conversion family, valid/boundary, NULL/empty, truncation or range error, malformed input where relevant, recovery, and width/alignment-sensitive cases. Exact SQLSTATE, return value, and output preservation. Add only missing contract evidence; no full Cartesian product |
| G3 | Required verification / M1 | Supported state flows: allocated→connected→prepared→executed→fetched/exhausted→closed→disconnected; direct execution, reprepare, multiple results/errors, invalid sequence, failed free and descriptor attachment. Commit/rollback and one failed-transaction recovery scenario on each claimed backend |
| G4 | Safety/reliability gate / M1 | Inventory existing injections first; cover DNS/refused connect, bad credentials, TLS certificate/hostname rejection, login/query timeout, mid-query disconnect, malformed protocol and allocation failure at a representative boundary. No crash/deadlock, bounded completion, correct diagnostics, and safe reuse or clear connection retirement. TLS success and secret-free logging included |
| G5 | Required verification / M1 | Supported attributes, descriptors and capability claims agree; known optional features are explicitly rejected. Existing complete PostgreSQL, both iODBC widths and sanitizer gates pass with reviewed skips. Windows live ODBC tests required for a Windows support claim |
| G6 | Backend compatibility gate / M3 | Real Redshift matrix for frozen types and metadata: scalar/binary/Unicode round trips, exact numeric precision, timestamp behavior, NULL/truncation/errors; catalog queries under the chosen ordinary-user permissions. Record each PostgreSQL difference and implementation or explicit limitation; test every advertised catalog API or remove its unsupported claim |
| G7 | Authentication gate / M3 | Chosen Redshift method succeeds and rejects invalid/expired credentials as applicable, uses verified TLS and redacts secrets. Reconnect works; if credential renewal is promised, expiry/refresh behavior is exercised. IAM/SSO cannot be accepted from docs alone |
| G8 | Application beta gate / M1 and M3 | On PostgreSQL at M1 and Redshift at M3, the named application installs/configures the driver, connects, discovers its schema, runs parameterized and ordinary queries, fetches NULL/Unicode/numeric/temporal data, handles an error, and reconnects. Exercise writes/transactions if in its workflow. Record application, OS, driver-manager versions and known limits |
| G9a | Architecture gate / M1 | Complete A1–A4 and PostgreSQL acceptance in [BACKEND_BOUNDARY.md](BACKEND_BOUNDARY.md): backend creation, SQL dialect, native types/catalogs and capabilities separated from common ODBC behavior; ownership/error/deadline contract documented; fake-backend contract cases and PostgreSQL regressions pass. Required before PG-BETA |
| G9b | Reuse gate / M3 | Redshift implements and refines the G9a contract with observed differences; no duplicate shared ODBC wrappers. Common contract tests and live acceptance pass for both backends. Evidence and limitations are recorded under [G9b acceptance](BACKEND_BOUNDARY.md#g9b--redshift-reuse-acceptance-required-before-redshift-beta) |
| G10 | Reliability/performance gate / M4 | Execute the frozen workload below; establish time/memory/throughput baselines and timeout behavior. No observed leak trend, corruption, crash, deadlock, or silently wrong result. Compare like-for-like against a pinned vendor-driver baseline for triage, not a promised speedup |
| G11 | Basic beta delivery M1; full delivery M4 | Clean install/configure/uninstall on each claimed OS; exact dependency versions and license inventory, TLS/auth and limitations guide, diagnostics troubleshooting, versioned artifacts/checksums, rollback instructions. Re-run application acceptance on packaged artifacts; all applicable G0–G10 remain green |
| G12 | Deferred SDK preview gate | Build/install the library and an out-of-tree sample backend without editing shared ODBC wrappers. Document ownership, errors, deadlines, capabilities, types and extension points. Run reusable backend contract tests against PostgreSQL, Redshift and a synthetic backend; mark public API unstable. A fake backend proves wiring only, not a new database protocol |

### Bounded workload for G10 (proposal to freeze in G0)

Use a fixed recorded dataset: 100,000 mixed scalar rows, 1 KiB average payload,
one 1 MiB text/binary value, 1,000 connect/execute/fetch/close cycles and a
four-hour mixed-workload run with eight independent connections. Record RSS at
fixed intervals, throughput, p50/p95 latency, failures and environment versions.
Separate application-buffer memory from retained driver memory. The current
materialized result design must have a documented supported size envelope;
chunked SQLGetData alone is not evidence of network streaming.

After warmup, investigate a sustained final-hour RSS rise above 10% or 10 MiB
(whichever is larger), or a greater-than-20% median performance regression across
three comparable runs. These are investigation triggers, not proof of a leak or
acceptable production performance. Any confirmed unbounded growth or incorrect
result blocks release. Thresholds and dataset sizes are proposed acceptance
budgets, not measured capabilities. Broader analytics workloads require D5 revision.

The [M0 working inventory](PG_BETA_CHECKLIST.md) classifies each of the 38
partial operations, proposes a named regression manifest and sizes architecture.
G0 remains open pending product selections and case-level evidence review.

## Allocation of the existing partial audit rows

This groups every partial row; G0 must split each row's promised behavior from
its residual backlog with test references. It does not promote a row to Verified.

| Audit operations | Primary release gates | Residual scope candidate |
|---|---|---|
| AllocHandle, FreeHandle | G3/G4/G5 | Optional pooling tokens: T1 |
| Connect, DriverConnect, Disconnect | G1/G4/G7 | Interactive prompts: T5; async ODBC: T1 |
| Set/GetConnectAttr, Set/GetStmtAttr | G5 | Optional pooling/platform attributes, scroll/arrays/async: T1 |
| EndTran | G3/G4/G6 | Unclaimed transaction modes: T1 |
| ExecDirect, Prepare, Execute, Fetch, FetchScroll, MoreResults | G3/G4/G8 | Arrays/streamed input/scrolling and cancellation: T1/T2 |
| GetData, BindCol, BindParameter | G2/G6/G8/G10 | Additional conversion combinations and types: T3/T4 |
| NumParams, NumResultCols, DescribeCol, ColAttribute, DescribeParam | G3/G4/G6/G8 | Unused origin/precision metadata fields: T4 |
| Tables, Columns, PrimaryKeys, ForeignKeys, Statistics, Procedures, ProcedureColumns, SpecialColumns | G6/G8 | Unneeded backend catalog extensions/statistics: T4; do not defer ordinary-user access for promised metadata |
| GetDiagField | G4/G5 | Array-specific row/column diagnostics: T1 |
| GetDescField, GetDescRec, SetDescField, SetDescRec, CopyDesc | G3/G5 | Bookmarks/interval/array combinations outside profile: T1/T3 |

## Investigation limits and change control

- Start a lead with a hypothesis, affected gate, severity and reproducer plan.
  Spend at most four engineering hours on initial triage, then at most two
  working days on a bounded investigation before an explicit review.
- At the limit, record evidence, reproduction status, likely impact, estimate,
  next step and disposition. Choose: fix, split into a smaller task, defer with
  a trigger, or propose a support-scope change. A timeout is not permission to
  classify a defect as safe.
- Crash, corruption, incorrect supported results, credential exposure, or an
  unbounded hang in supported behavior remains a blocker even if expensive.
  Unknown impact on such behavior remains unresolved/blocking until assessed.
- Spend no more than 20% of a milestone's implementation budget on optional
  exploratory coverage. Existing incident/security evidence overrides the cap,
  but requires a revised estimate. Never stop investigating a serious defect
  merely because the percentage has been reached.
- Reserve 30% schedule contingency. At half the reserve consumed, review the
  forecast; at exhaustion, replan before starting more discretionary work.
  A feature addition is estimated separately, not hidden inside contingency.
- Every implementation batch names its gate, intended evidence and timebox.
  Newly discovered optional cases enter the backlog, not the current milestone
  automatically. Report gates closed, blockers, effort/reserve used, dependencies
  and forecast changes. Batch count and test count are not progress measures.

## Deferred work and revisit triggers

| ID | TODO / boundary | Revisit trigger |
|---|---|---|
| T1 | Arrays, row-wise binding, scroll/positioned updates, bookmarks, genuine async ODBC, optional pooling attributes | Named application requires it or a separately scheduled throughput milestone |
| T2 | SQLCancel and data-at-execution APIs; currently not part of the exported surface | Application needs cancellation/streamed input. Verify bounded deadlines/cleanup now; if required, promote and estimate before beta |
| T3 | Exhaustive conversion cross-product, intervals and backend-specific types beyond frozen scalar list | Concrete required column/query, interoperability defect, or proven missing safety boundary |
| T4 | Extra catalog precision/origin fields, statistics accuracy, restricted-user scenarios beyond the selected permission model | Promised catalog behavior or selected application fails; security/visibility defects in supported paths are immediate blockers |
| T5 | Windows DSN GUI, polished MSI and distribution-specific installers | Product distribution requires them; documented clean installation remains G11 |
| T6 | Additional IAM/SSO providers, browser auth, automatic credential refresh beyond selected method | User-selected enterprise authentication or credential lifecycle requires it; chosen auth never deferred |
| T7 | Native handling for Redshift SUPER/spatial/sketch types and other extensions | Selected workload uses them. Only offer a text fallback if its metadata and representation are verified on real Redshift |
| T8 | Binary wire-format acceleration, prepared cache, pooling tuning, transport micro-optimizations | G10 measurements identify a bottleneck; no unmeasured speedup target |
| T9 | Public SDK stability/versioning promises and actual MySQL/TDS implementations | Internal preview G12 passes and an unrelated backend is selected with funded scope |
| T10 | Broad file splitting, universal plugin registry, broad static-analysis cleanup | A release defect/change needs broader cleanup; focused A1–A4/G9a extraction is mandatory in PostgreSQL M1, with G9b refinement in Redshift M3 |
| T11 | Extra PostgreSQL versions/features beyond frozen validation baseline | Customer deployment needs them or a shared/Redshift defect reproduces there |

A deferred item records owner, rationale, known risk, revisit trigger and evidence
when instantiated as work. It may not hide a serious supported-path defect.

## Completion and scheduling

The milestone sequence, base effort and buffered estimates are in
[ROADMAP.md](ROADMAP.md). PostgreSQL consolidation and the PG-BETA checkpoint precede Redshift
implementation. Preparatory access work may happen earlier. Work waiting on a
Redshift endpoint must be reported as waiting,
not replaced indefinitely by new PostgreSQL edge-case permutations.

The scheduler is paused and remains paused. On an explicitly authorized resume,
use PostgreSQL G0 and PG-BETA first and stop at the agreed milestone. No automation is changed by
this assessment. The new plan supersedes the old unbounded batch-selection policy.
