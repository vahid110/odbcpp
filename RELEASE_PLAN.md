# Release assessment and bounded scope

Assessment date: 2026-09-23. Code baseline: `4f6de2a`.
Status: PostgreSQL technical baseline established; SDK/MySQL proof approved
2026-09-30 while Redshift live access is unavailable.
Exact versions, access and detailed workflows remain open.
Active implementation scope is PostgreSQL, Redshift and the bounded MySQL 8
proof defined in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md). RDS, Aurora, Athena,
MariaDB and SQL Server/TDS are not included. SDK architecture is mandatory
during PostgreSQL consolidation (G9a), proven across an unrelated protocol by
MySQL (G12), and refined through Redshift (G9b). Public SDK productization is
deferred (G13). PG-BETA remains open in ROADMAP.md; host-dependent G8 evidence
is deferred by explicit product decision, not waived.
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
| D1: application/workflow | User-requested targets: SQL Server linked server including OPENQUERY on Windows; Power BI Desktop on Windows; Excel on macOS. PostgreSQL first, then Redshift. Exact versions/workflows pending | Three real-application tracks replace the one-application assumption. Each needs independent evidence; see the acceptance matrix below |
| D2: Redshift deployment/access | One user-supplied provisioned or Serverless endpoint; type/version, network access, test schema and permissions recorded | No live compatibility claim until available. Do not provision or spend on cloud resources implicitly |
| D3: authentication | Database credentials over certificate- and hostname-verified TLS for initial pilot | If IAM/SSO is mandatory, it becomes a gate before beta and requires a fresh estimate; do not ship an unusable credential-only beta |
| D4: supported systems | Linux/macOS driver gates plus Windows live validation required for linked-server and Power BI targets | Windows units alone cannot close these targets. Pin Windows, SQL Server/provider, Power BI, macOS/Excel and process/driver architectures |
| D5: workload | Forward-only, one row/parameter set at a time; declared scalar types and bounded result sizes | Arrays, streaming, cancellation, or larger workloads required by D1 must be promoted explicitly before freezing scope |
| D6: SDK product | ODBC is the first mature client adapter; PostgreSQL and MySQL are sibling reference backends; Redshift is a PostgreSQL-family specialization. ADBC is a future adapter whose columnar needs constrain the boundary now | G12 must prove an unrelated protocol without implementing ADBC or promising a stable public ABI; G13 owns public SDK productization |
| D7: pooling/cache policy | Driver Manager pooling compatibility and safe physical-session reuse are correctness requirements. An SDK-managed pool is optional/internal. Metadata/type and prepared caches require explicit scope/invalidation; general result caching is excluded | S1 freezes lifecycle contracts; G12 proves correctness for PostgreSQL/MySQL; G10 measurements gate new caching, sizing and performance tuning |
| D8: security architecture | Verified transport is the default for credential authentication; backend sessions enforce method/channel policy, resource budgets, secret lifecycles, borrower isolation and typed logging. Native extensions are trusted in-process code | S2 begins with the blockers in [SECURITY_MODEL.md](SECURITY_MODEL.md); G12 requires hostile-input, fuzz, isolation, supply-chain and hardening evidence; G13 adds independent review, signing and response policy |

These assumptions allow planning, not a silent product decision. G0 is closed separately for each backend; PostgreSQL decisions are required
first. Redshift endpoint/authentication decisions do not block the PostgreSQL
checkpoint. Application families are now named; exact versions, available test
machines and detailed workflows still need recording. Endpoint details must be supplied through the
normal secure configuration path; never put credentials in this document.

## Application acceptance targets — added 2026-09-26

These are requested compatibility targets, not current support claims. SQL Server
is a consumer of the PostgreSQL/Redshift driver here, not a new database backend.
OPENQUERY is the pass-through query path of a linked server, not a fourth app.
All three tracks need explicit evidence before claiming the requested application
profile complete. Infrastructure waits do not silently waive acceptance.

| Track | Proposed bounded first workflow | Execution environment / Actions strategy |
|---|---|---|
| SQL Server linked server + OPENQUERY | Register driver/System DSN; connect through MSDASQL; metadata discovery; OPENQUERY scalar/NULL/Unicode/numeric/temporal results; simple four-part-name SELECT separately; invalid-query diagnostic and recovery | Windows SQL Server plus matching driver/provider architecture. Scriptable SQL assertions; prototype GitHub-hosted Windows setup or use a dedicated Windows self-hosted runner. Linux SQL Server containers cannot substitute for this non-SQL-Server linked-server path |
| Power BI Desktop | Generic ODBC connector in Import mode; schema navigation, native query, load typed data, simple filter/transform, save/reopen and refresh; invalid credentials/query and recovery | Windows desktop with pinned Power BI version. Hosted CI can test underlying ODBC behavior; actual Desktop acceptance initially uses a recorded manual run, then a dedicated interactive test machine if reliable automation is established |
| Excel on macOS | Install/register our driver, use the supported database/ODBC import path for the pinned Excel version, load the same fixture to a sheet, verify types/NULL/Unicode, save/reopen/refresh and error recovery | Licensed/activated Excel on a Mac, with matching Excel/driver architecture. Existing iODBC width tests remain CI prerequisites but do not prove Excel compatibility. Record a real-app run; consider a dedicated Mac runner after automation feasibility is established |

The proposed first workflows are read-oriented. Linked-server writes, distributed
transactions/MSDTC, broad remote joins, Power BI DirectQuery, custom connectors,
service/gateway refresh, incremental refresh and Excel writeback are separate
scope decisions. Do not promise these from successful imports. If a requested
workflow needs one, promote and estimate it before freezing G0. OPENQUERY itself
does not accept variables; do not use it to claim bound-parameter coverage.

Each backend/app record must identify exact app/OS/driver-manager/provider/server
versions, architecture, installed driver artifact, authentication, fixture and
expected values, executed actions, results and limitations. Use our ODBC driver
explicitly, not the application's native PostgreSQL/Redshift connector. Repeat
with a real Redshift endpoint at M3; PostgreSQL success is not Redshift evidence.

GitHub Actions supports self-hosted machines with installed software. Using one
for these tracks is an execution design recommendation, not evidence that GUI
automation already works. Keep fast hosted driver gates on each batch; run the
application suite on a release candidate and after relevant metadata, dialect,
type or packaging changes. Manual app evidence is acceptable initially; missing
apps must be reported as untested, not a passing or silently skipped gate.

### Hosted Windows approach — agreed 2026-09-29

Use standard GitHub-hosted Windows x64 runners first. No paid custom images or
self-hosted machines are required for the initial Windows PostgreSQL/ODBC gate.
The Windows Server 2025 runner already supplies PostgreSQL 17 binaries; create a
disposable cluster per job rather than downloading a database installer. Build
and exercise the driver through Windows' odbc32 Driver Manager as well as the
in-process integration suite. Reject skipped live integration cases.

Next, prototype SQL Server Developer installation and scripted MSDASQL/System
DSN/OPENQUERY assertions on a standard hosted Windows runner. This is a separate
application gate, not evidence supplied by the PostgreSQL integration job.
Use the native registry System DSN reader from W1. Registry-only and LocalSystem
visibility checks are part of Windows CI; actual SQL Server service-account
permissions and application behavior still require this G8 track. Record installation/runtime costs and keep the first workload
read-only. SQL Server acceptance has not yet run.

Power BI Desktop remains a separate feasibility task: verify installation,
interactive-session availability and a real generic-ODBC import/refresh before
choosing hosted automation or a manual desktop run. Do not provision paid hosts
or claim Desktop compatibility from API-only tests. Excel acceptance is unchanged.

References: [Windows runner image inventory](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-Readme.md),
[MSDASQL linked-server configuration](https://learn.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/sp-addlinkedserver-transact-sql),
[Power BI Desktop requirements](https://learn.microsoft.com/en-us/power-bi/fundamentals/desktop-get-the-desktop).

### Windows delivery work packages — approved 2026-09-29

These are required PostgreSQL beta work, not deferred SDK or Redshift work.
Retain the sequence: A4/G9a closure, native Windows configuration and basic
packaging, then G8 application acceptance on packaged artifacts, then Redshift.
The hosted Windows API gate is already green at `b5f9d0b`; it does not close G11.

| ID | Gate / acceptance | Provisional base engineering days |
|---|---|---:|
| W1 | G5/G11: read native User/System DSNs and driver defaults in the matching registry view; explicit connection attributes override DSN values; define same-name User/System precedence. Test absent/malformed entries, Unicode, denied access and SQL Server service-account visibility. No dependency on CI INI files for native DSNs | 1–2 |
| W2 | G5: preserve one parser; test DSN-less and DSN-plus-overrides through native Windows DM in A/W paths, braces/semicolons, empty values, normalization, credential handling and failure recovery. Retain direct parser tests where DM rewrites input | 0.5–1 |
| W3 | G11: Windows ODBC Administrator setup component with add/configure/remove, minimal validated connection fields, masked credentials, connection test and cancel without persistence. Test successful edits, bad inputs, failed login and deletion isolation. No persisted plaintext passwords by default | 2–3 |
| W4 | G11: reproducible x64 beta installer/uninstaller registers driver and setup component, deploys runtime dependencies, handles upgrade/rollback and architecture mismatch; clean-machine install/configure/connect/uninstall evidence. Remove only owned registration and explicitly selected DSNs | 1–2 |

W1/W2 precede SQL Server/Power BI acceptance. W3/W4 precede final packaged G8
acceptance and PG-BETA; scripted application prototyping may run earlier.
Initial Windows scope is x64; x86/ARM64 delivery requires separate sizing.
ODBC Administrator DSN configuration is included; SQLDriverConnect interactive
login prompts remain deferred unless a selected application requires them.

Windows subtotal: **4.5–8 base engineering days**, plus **1.35–2.4 days (30%)**
contingency. These are planning estimates, not elapsed-time promises. Replace
the Windows portion of the old unsized delivery allowance with these packages;
do not add both allowances or absorb this scope into contingency. The total M1
forecast remains provisional until the three application tracks are sized. Review
after W1 and before W3; bounded investigation rules still apply.

**Estimate impact:** M1's 14–23 base days included only one application and
2–3 days for application/delivery. That is now a pre-expansion baseline, not a
validated forecast for all three tracks. Size Windows live setup and each app
workflow separately once versions/access are known; do not absorb added scope
into the 30% contingency. Shared backend work can proceed meanwhile.

Official sources reviewed 2026-09-26:

- [OPENQUERY](https://learn.microsoft.com/en-us/sql/t-sql/functions/openquery-transact-sql?view=sql-server-ver17)
  defines linked-server pass-through and its argument limitations.
- [Linked-server provider setup](https://learn.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/sp-addlinkedserver-transact-sql?view=sql-server-ver17)
  documents MSDASQL for ODBC; pin SQL Server patch level and record provider setup.
- [SQL Server Linux limitations](https://learn.microsoft.com/en-us/sql/linux/sql-server-linux-editions-and-components-2025?view=sql-server-ver17)
  exclude linked servers to non-SQL-Server sources.
- [Power Query ODBC connector](https://learn.microsoft.com/en-us/power-query/connectors/odbc)
  documents Import; [Power BI Desktop requirements](https://learn.microsoft.com/en-us/power-bi/fundamentals/desktop-get-the-desktop)
  specify Windows.
- [Excel for Mac ODBC](https://support.microsoft.com/en-us/excel/odbc-drivers-that-are-compatible-with-excel-for-mac)
  describes third-party ODBC drivers; it does not certify this driver or imply
  Windows Power Query connector parity on macOS.
- [GitHub self-hosted runners](https://docs.github.com/en/actions/concepts/runners/self-hosted-runners)
  support custom software environments.

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

## Redshift feature and parity backlog

The user's eight Redshift requirements, priorities, source baseline, evidence
matrices and exception policy are recorded in
[REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md). M2 inventories all official
driver features/settings and explicit observable behaviors; M3 remains a scoped
beta. RS1–RS7 coverage continues in separately estimated RP1 parity batches;
exceptions require user approval. RS8 transparent S3 bulk transfer requires a
later design discussion and approval. This does not change active S2/S2C/MS1.
The existing M3 range excludes comprehensive parity and advanced S3 work; revise
the forecast at M2 rather than spending contingency on this additional scope.

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
| G8 | Application beta gate / M1 and M3 | On PostgreSQL at M1 and Redshift at M3, each requested application track above installs/configures the driver, connects, discovers its schema, runs parameterized and ordinary queries, fetches NULL/Unicode/numeric/temporal data, handles an error, and reconnects. Exercise writes/transactions if in its workflow. Record application, OS, driver-manager versions and known limits |
| G9a | Architecture gate / M1 | Complete A1–A4 and PostgreSQL acceptance in [BACKEND_BOUNDARY.md](BACKEND_BOUNDARY.md): backend creation, SQL dialect, native types/catalogs and capabilities separated from common ODBC behavior; ownership/error/deadline contract documented; fake-backend contract cases and PostgreSQL regressions pass. Required before PG-BETA |
| G9b | Reuse gate / M3 | Redshift implements and refines the G9a contract with observed differences; no duplicate shared ODBC wrappers. Common contract tests and live acceptance pass for both backends. Evidence and limitations are recorded under [G9b acceptance](BACKEND_BOUNDARY.md#g9b--redshift-reuse-acceptance-required-before-redshift-beta) |
| G10 | Reliability/performance gate / M4 | Execute the frozen workload below; establish time/memory/throughput baselines and timeout behavior. No observed leak trend, corruption, crash, deadlock, or silently wrong result. Compare like-for-like against a pinned vendor-driver baseline for triage, not a promised speedup |
| G11 | Basic beta delivery M1; full delivery M4 | Clean install/configure/uninstall on each claimed OS; exact dependency versions and license inventory, TLS/auth and limitations guide, diagnostics troubleshooting, versioned artifacts/checksums, rollback instructions. Re-run application acceptance on packaged artifacts; all applicable G0–G10 remain green |
| G12 | SDK/MySQL proof gate / MS1 | Complete S1–S4 and the engineering quality bar in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md): implement [SDK_ARCHITECTURE.md](SDK_ARCHITECTURE.md), pass [SECURITY_MODEL.md](SECURITY_MODEL.md), preserve PostgreSQL behavior, prove safe reset/reuse/retirement and cache invalidation for PostgreSQL/MySQL, pass reusable contract and forbidden-dependency checks, run the real MySQL 8 slice, and pass the S4 clean-room exercise with zero shared ODBC workflow edits. Publish the test kit, extension guide and evidence. Undocumented internal knowledge blocks closure. Mark interfaces unstable; this is not a MySQL beta |
| G13 | Deferred public SDK preview gate | Build/install a versioned SDK for external consumers; define API/ABI compatibility and support lifecycle; complete licensing, reference documentation and compatibility testing. ADBC implementation, Arrow integration and commercial availability require separately approved scope |
| G14 | Redshift parity expansion / RP1 | Close RS1–RS7 feature/configuration and observable-behavior inventories in [REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md), including source coverage, differential/live evidence and user-approved exceptions. A scoped M3 beta does not imply G14 closure. RS8 advanced S3 modes need separate approval |

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
| T5 | SQLDriverConnect interactive login prompts, cosmetic installer polish and extra distribution-specific packages | Selected workflow needs them. Native Windows DSNs, minimal ODBC Administrator GUI and a usable x64 beta installer are required W1–W4/G11, not deferred |
| T6 | Additional IAM/SSO providers, browser auth, automatic credential refresh beyond selected method; planned Redshift RS2 parity breadth | M2 inventory/estimate, then RP1 batches in REDSHIFT_FEATURE_PLAN.md; promote required methods to G7 before beta. Chosen auth never deferred; omission from parity requires user approval |
| T7 | Native handling for Redshift SUPER/spatial/sketch types and other extensions | Selected workload uses them. Only offer a text fallback if its metadata and representation are verified on real Redshift |
| T8 | Binary wire-format acceleration, new prepared-cache behavior, public/advanced pool policy, sizing/eviction tuning and transport micro-optimizations | G10 measurements identify a bottleneck; S1/G12 lifecycle correctness is mandatory, but no unmeasured speedup target is promoted |
| T9 | Public SDK stability/versioning promises, full MySQL beta breadth and TDS | Bounded G12 proof passes and public SDK/MySQL product scope is selected with funded requirements |
| T10 | Broad file splitting, universal plugin registry, broad static-analysis cleanup | A release defect/change needs broader cleanup; focused A1–A4/G9a extraction is mandatory in PostgreSQL M1, with G9b refinement in Redshift M3 |
| T11 | Extra PostgreSQL versions/features beyond frozen validation baseline | Customer deployment needs them or a shared/Redshift defect reproduces there |
| T12 | FIPS implementation and per-driver/platform qualification; preserve architectural flexibility during S2/S2C reviews only, per CRYPTO_PROVIDER_PLAN.md | Concrete customer/deployment requirement or explicit reprioritization; separately estimate module selection, approved-operation policy, integration evidence and compliance review before activation |
| T13 | Integration owner: Windows bundled-shared same-basename preload isolation; current harness detects foreign binding after load, but production does not prevent it. Qualifying this path is blocked | Before qualifying/releasing the Windows crypto profile or closing any gate requiring it; separately select and estimate one prevention strategy, then run live/package acceptance. S2 noncrypto migration may proceed; no qualification waiver |
| T14 | Integration owner: explicit finite OpenSSL row signoff, including supported host compatibility and static provenance scope; see S2C_MATRIX_REVIEW.md | Before closing S2C/G12 requirements for the matrix. Missing mandatory evidence remains a blocker; broader public distribution requirements stay in G13 |
| T15 | Redshift RS1–RS7 sharing, auth breadth, fetch modes, behavior/feature parity, SHOW metadata and legacy compatibility | M2 scope/inventory checkpoint; required beta cases go to M3, remainder to RP1/G14. Integration owner tracks evidence; user approves exceptions. See REDSHIFT_FEATURE_PLAN.md |
| T16 | Redshift RS8 explicit/automatic S3 COPY/UNLOAD and staged ODBC transfer modes | Separate user design/scope approval after baseline assessment; estimate semantics, security, cleanup and cost before implementation |

A deferred item records owner, rationale, known risk, revisit trigger and evidence
when instantiated as work. It may not hide a serious supported-path defect.

## Completion and scheduling

The milestone sequence, base effort and buffered estimates are in
[ROADMAP.md](ROADMAP.md). PostgreSQL technical consolidation, G9a and W1–W4 are
complete, while PG-BETA remains open on G8 application evidence. The user
explicitly deferred those host-dependent runs and approved bounded MS1 SDK/MySQL
work on 2026-09-30. The real Redshift pilot is waiting on restored account
access; it must be reported as waiting and resumes before any expansion of the
MySQL proof into a product driver.

The scheduler is paused and remains paused. On an explicitly authorized resume,
use the currently approved milestone and stop at its gate. No automation is
changed by this assessment. The new plan supersedes the old unbounded
batch-selection policy.


## Architecture checkpoint — batch 12

A1–A4 implementation, the final dependency review and independent fake-backend
ODBC acceptance are complete. Confirm batch 12's full regression evidence and
exact-revision CI (including Windows live tests) to close G9a. The next work is
W1–W4 under the existing estimates and checkpoints above, then G8 application
acceptance before PG-BETA. This does not claim PostgreSQL beta, real Redshift
compatibility or a packaged public SDK. No additional A4 feature work is planned.


## W1 implementation checkpoint — batch 13

Native Windows User/System DSNs, architecture-view selection, driver defaults,
Unicode/malformed/denied-read handling and explicit attribute/alias precedence are
implemented. See WINDOWS_DSN.md for source-selection rules and supported registry
data types. Windows CI is the acceptance gate for native reading, registry-only
live integration, conflicting registry views and LocalSystem visibility. CI 36570790247 passed all five jobs at d1e8d0b, closing W1.
W2 native Driver Manager A/W connection-string coverage follows. SQL Server-specific application acceptance remains G8;
this does not complete W2, W3 or W4 or require the user to supply hosts now.

W1 stayed within its bounded reader/configuration scope. Retain the existing
W2–W4 estimates; review delivery effort again before W3 as planned.

## W2 implementation checkpoint — batch 14

The existing parser remains shared. Native Windows DM acceptance now covers A/W
DSN-less and DSN-plus-override connections, normalized keys, braced delimiters and
escaped closing braces in real passwords, explicit lengths, completed-string
lengths, explicit empty password rejection, diagnostic credential handling and
same-handle recovery. A Unicode password additionally checks W-path conversion
and UTF-16 length units. Direct parser tests retain first-duplicate and empty-value
semantics where the DM rewrites input. Exact-revision Windows CI closes this gate.

Next is the planned pre-W3 effort review, then the minimal Administrator setup GUI
and x64 delivery. Retain W3's 2–3 and W4's 1–2 base-day estimates plus the existing
30% contingency until implementation evidence changes them. W2 introduces no new
parser, packaging dependency or application-host requirement. G8 remains separate.

## Pre-W3 review and W3 implementation checkpoint — batch 15

W2 closed at 5ed113a (CI 36572088585, all five jobs green). The pre-W3 review kept
scope within the existing 2–3 base-day allowance: native setup DLL, add/configure/
remove, validated connection fields, password masking, test login and cancel.
Connection/Authentication tabs and separate UI/model/persistence code accommodate
future settings and authentication methods; unsupported methods are not exposed.
No .NET or third-party GUI runtime is introduced. The W4 1–2 base-day estimate and
30% contingency remain provisional until clean-machine packaging evidence.

W3 implementation uses real installer APIs and hosted native dialog automation.
Exact-revision Windows CI and rendered-dialog review are required before closure.
W4 must install/register the setup component and verify the packaged Administrator
workflow, dependency deployment, upgrades and uninstall. This does not close G8 or
require application hosts yet. See WINDOWS_DSN.md for password and write-recovery
boundaries. Persistent registry write failures have best-effort recovery; no
transactional registry or concurrent-editor guarantee is claimed.

## W4 implementation checkpoint — batch 16

W3 closed at 29a22a4 (CI 36584852239, five jobs green and dialog captures reviewed).
W4 adds a pinned WiX x64 MSI recipe, app-local OpenSSL/MSVC runtime deployment,
checksums and per-file inventory. A separate fresh Windows runner must accept
install, native Administrator setup, live connection, failed-upgrade rollback,
upgrade, downgrade rejection, ownership protection and uninstall preserving DSNs.
The rollback fixture is a separate test-only MSI, not the beta artifact.

Exact-revision six-job CI is required before W4 closes. The build recipe records
inputs but does not claim byte-identical MSIs across toolchains. No public license
is declared in the repository and no signing identity is configured: artifacts
are internal unsigned beta validation packages, not a public release. Resolve
licensing/signing under G11 before external distribution. G8 application acceptance
remains separate; once package acceptance is green, application-host readiness
becomes actionable. No Redshift-specific packaging changes are included.

W4 closed at c82efd2 with CI 36595134585 green across all six jobs. G8 remains
open. A first hosted SQL Server Developer-media prototype was cancelled before
its 45-minute boundary after roughly 40 minutes in the opaque media/setup step;
the cancellation does not establish either compatibility or incompatibility.
At the user's direction this application track is deferred while other work
continues. Its workflow is manual-only so normal pushes keep the fast Windows
and package gates. Power BI Desktop and Excel remain untested.

Redshift pilot preparation now separates evidence by build target. PostgreSQL
test discovery excludes `it_redshift_real`. A Redshift build requires an explicit
TLS-enabled connection string and fixture schema/table, verifies the server's
`version()` identifies Redshift, and exercises the bounded G1 query, prepared
NULL/scalar, metadata and recovery flow. This prevents false evidence but does
not close G1; execution still waits on a real user-supplied endpoint.
The non-live Redshift target build and unit suite are now a normal CI gate, along
with an expected-failure check for absent pilot configuration. This is compile
and contract coverage only, not a substitute for the M2 endpoint run.

## SDK/MySQL planning checkpoint — 2026-09-30

Live Redshift M2 is temporarily blocked because the user's AWS/Redshift account
access is being restored. The user approved MS1 as bounded engineering work
during that wait. G12 now requires a real MySQL 8 reference slice to prove the
shared SDK against an unrelated protocol; G13 retains public SDK stability and
distribution work. PostgreSQL and MySQL are sibling backends. Redshift remains
a PostgreSQL-family specialization and resumes at M2 when access returns.

ADBC is recorded as a future client adapter and an architectural constraint,
not an implementation claim. The MS1 result contract must permit a later
columnar path without importing ODBC types into backends, but Arrow, the ADBC
ABI and ADBC packaging remain outside scope. Exact work packages, 25–41 base
engineering-day estimate, 30% contingency, tests, non-goals and stop rules are
in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md). No release or conformance status
changed through this planning-only checkpoint.

## S1 architecture and security review — 2026-09-30

Four read-only audits examined dependency direction, MySQL/external-author
usability, pooling/cache lifecycles and security at baseline `089e085`. The
accepted contract is [SDK_ARCHITECTURE.md](SDK_ARCHITECTURE.md); the threat model
and required controls are [SECURITY_MODEL.md](SECURITY_MODEL.md).

The current `GenericDatabaseConnection`/`IProtocolParser` pair is PostgreSQL-
family machinery and will not be extended with MySQL branches. S2 introduces a
provider/live-session split, normalized result and structured error boundaries,
one composition registration point, internal target boundaries and explicit
reuse/cache contracts. The existing pool remains unadvertised prototype code
and its copyable/manual-release model will be replaced before any reuse claim.

The review also found supported-path security blockers. The first S2 batch
(`df8a67e`, `179f6a6`) resolved omitted-TLS/cleartext-password authentication,
custom CA application and working-directory configuration discovery. Aggregate
server results still lack a resource budget, and the prototype pool cannot
isolate borrowers. S2 continues through the remaining contracts in reviewable
batches; G12 remains open.

## Cryptography provider and linkage checkpoint — 2026-09-30

The product must support a build-time choice of cryptography implementation and
dependency linkage. OpenSSL remains the current implementation. AWS-LC is a
bounded G12 Linux qualification target because vendors may require it; it is not
yet a support claim. Runtime DSNs select security policy and trust, never a
library. Static versus system-shared versus bundled-shared crypto is independent
of the shared ODBC driver artifact and must be proven by binary inspection.

The accepted boundary, S2C package, compatibility risks, qualification matrix
and stop rules are in [CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md). S2C
adds 5–9 base engineering days and revises MS1 to 25–41 base, 33–54 buffered.
FIPS and untested platform/provider combinations remain separate claims.

## S2 provider/composition batch — 2026-09-30

The compiled driver now has one C++ product-registration point that supplies an
immutable backend provider. The provider owns product identity, driver lookup
name, connection defaults, static capabilities, transaction profile, baseline
type catalog, option resolution and live-session construction. Shared ODBC no
longer selects a backend through build macros or constructs a disconnected
PostgreSQL session merely to answer pre-connect metadata calls.

PostgreSQL and Redshift are separate product profiles over the PostgreSQL-family
session. The session owns its copied display identity, so it cannot retain a
dangling provider string. Backend selection definitions are private build
details, and architecture tests reject selection macros or concrete backend
dependencies in shared ODBC, selection outside product composition, backend to
ODBC includes, sibling-backend includes and exported selection definitions.
Unimplemented MySQL and SQL Server product values now fail configuration instead
of producing placeholder artifacts.

This closes the narrow provider/composition batch, not the full S2 architecture.
The build remains monolithic, installed headers still need the S4 allowlist, and
the live-session/facet, normalized-result, structured-error/resource-budget and
safe-reuse batches remain open.
Legacy direct session/factory paths can still bypass provider default resolution;
their migration belongs to the live-session batch and they remain compatibility
paths rather than the extension contract.

## First S2C crypto-profile batch — 2026-09-30

Cryptography selection now uses strict build-time `ODBCPP_CRYPTO_PROVIDER`,
`ODBCPP_CRYPTO_LINKAGE` and `ODBCPP_CRYPTO_ROOT` profiles. OpenSSL system-shared,
bundled-shared and bundled-static discovery is explicit; bundled discovery must
remain inside its canonical controlled prefix. Unknown values, stale profile
changes, the removed public `OPENSSL_USE_STATIC_LIBS` knob and premature AWS-LC
selection fail configuration.

Every configured build emits a versioned crypto manifest containing the
requested profile and resolved configure-time inputs. It explicitly records
that artifact linkage remains unverified and makes no FIPS claim. Windows beta
packaging accepts only its OpenSSL bundled-shared profile, installs and hashes
the manifest, and rejects mismatched profile evidence before staging.

This batch establishes configuration and evidence plumbing only. It does not
qualify any matrix row, introduce AWS-LC, isolate OpenSSL source APIs, prove
runtime linkage, or make a FIPS claim. Those S2C gates remain open.

## S2C private cryptography boundary — 2026-09-30

PostgreSQL SCRAM and legacy MD5 authentication now use a private,
provider-neutral primitive adapter for digests, HMAC, PBKDF2, randomness,
constant-time comparison and cleansing. Strict portable Base64 decoding rejects
malformed and non-canonical authentication inputs. SCRAM secret intermediates
are cleansed on both success and exception paths.

The installed synchronous TLS header no longer exposes OpenSSL handles.
Provider-specific TLS helpers and the primitive adapter are not installed, and
architecture checks reject direct provider includes in backend code and
provider types in installed core headers. This closes the authentication
primitive and installed-header portions of S2C only. The common private TLS
adapter, artifact linkage inspection, runtime diagnostics, OpenSSL qualification
and bounded AWS-LC Linux proof remain open.

## S2C runtime identity and artifact-form evidence — 2026-09-30

The private crypto adapter reports compile-time and loaded runtime provider
versions and active FIPS state. A platform-aware test inspects the completed
driver with `readelf`, `otool` or `dumpbin`, verifies shared versus embedded
crypto dependency form, and writes evidence tied to the artifact SHA-256.
Negative fixtures cover missing shared dependencies and unexpected dynamic
dependencies in a static profile.

This evidence does not yet prove dependency origin, packaged loader behavior,
symbol isolation or coexistence and explicitly makes no qualification claim.
Those checks, OpenSSL matrix qualification and the bounded AWS-LC Linux proof
remain open. The following checkpoint closes the common TLS adapter.

## S2C common private TLS adapter — 2026-09-30

One private client contract now serves synchronous socket TLS and asynchronous
memory-BIO TLS. It owns provider context/session lifetimes, trust and peer-name
verification, SNI, provider error capture, clean versus truncated EOF handling,
and ciphertext ingress/egress. Transport workflows retain their deadlines,
cancellation, socket readiness and queue ownership without provider APIs.

Provider-specific transport helpers were removed and architecture validation
now rejects provider includes, types or calls anywhere in the transport layer.
Focused adapter lifecycle tests and the existing synchronous/asynchronous TLS
suites cover happy paths and misuse, SNI, trust, timeout and EOF edges.

This is a source-boundary milestone only. It does not qualify AWS-LC, OpenSSL or
any linkage profile. Dependency origin, packaged loader behavior, symbol
visibility/coexistence, the qualified OpenSSL matrix and the bounded AWS-LC
Linux proof remain required for G12.

## S2C dependency-origin evidence — 2026-09-30

The artifact inspector now resolves shared OpenSSL dependencies on Linux and
macOS to canonical files and records their hashes with the driver hash. For a
bundled-static profile it verifies both configured archives remain under the
controlled dependency root and records their hashes, while keeping embedded
code origin unverified. Evidence schema 2 keeps dependency form, resolved
origin, configured inputs and qualification claims independent; qualification
remains false.

The next package-evidence step must bind Windows PE import names to byte-identical
controlled-prefix DLLs and fresh-runner loaded paths. Preloaded-module
coexistence, symbol visibility, completed OpenSSL qualification and AWS-LC are
still open, so this checkpoint adds no support claim.

## S2C Windows packaged-loader evidence — 2026-09-30

The Windows `BUNDLED_SHARED` package binds exact PE import names to the driver
hash, stages only matching controlled-root DLLs, and records fresh-runner loaded
paths, hashes and provider version under hostile PATH conditions. A missing
app-local runtime must fail instead of using a PATH decoy.

This does not close qualification. Preloaded same-basename coexistence and
symbol-isolation evidence remain required, and AWS-LC/FIPS claims remain absent.

## S2C driver export isolation — 2026-09-30

The shared driver now exposes exactly 76 canonical ODBC entry points across PE,
Mach-O and ELF builds. A single allowlist generates platform linker controls;
artifact checks compare the resulting export table exactly and negative fixtures
cover missing, unexpected and forwarded symbols. OpenSSL is a private link
dependency of the shared driver, and the static core's test-facing surface is
unchanged.

This is driver-binary symbol evidence, not provider-profile qualification.
Bundled shared-library and preloaded-provider coexistence checks, AWS-LC and
FIPS evidence remain open.

## S2C Linux OpenSSL static artifact gate — 2026-09-30

The release workflow now builds the Linux OpenSSL `BUNDLED_STATIC` candidate
from a controlled ephemeral prefix, runs the shared TLS/authentication and
architecture tests, verifies the final ELF has no dynamic OpenSSL dependency,
and retains the source package version and archive hashes alongside hashes bound
to the driver artifact. Copied headers are dereferenced and checked to remain
inside the prefix.

This is a candidate artifact gate. Static embedded-byte provenance, live
profile qualification, provider coexistence, AWS-LC and FIPS remain open.

## S2C Linux OpenSSL static live gate — 2026-09-30

The Linux `BUNDLED_STATIC` candidate now completes a required query through
unixODBC, PostgreSQL verified TLS and SCRAM. Separate mandatory cases reject an
untrusted CA and hostname mismatch through unixODBC and confirm their precise
TLS diagnostics through the driver entry points. The same run preloads the system shared
OpenSSL, loads and uses the static driver, verifies that provider identity is
unchanged, and rejects unresolved provider symbols in the driver.

Evidence is retained against exact driver and shared-library hashes. This is
same-package duplicate-instance cohabitation evidence, not cross-version or
cross-implementation proof. Static provenance and the AWS-LC comparison remain
open, and the matrix row is not yet qualified.

## S2C Linux OpenSSL static link-trace gate — 2026-09-30

The CI static candidate now retains a hashed GNU ld linker map and requires both
controlled OpenSSL archives to be loaded and to contribute archive members.
Its `OUTPUT` record, map hash and exact archive hashes are bound to a retained
copy of the inspected driver; a stale or incomplete map fails the build. This is
link-input/member-extraction evidence, while embedded-byte provenance,
cross-implementation coexistence and the AWS-LC proof remain open. It does not
qualify the matrix row.

## S2C pinned AWS-LC adapter proof — 2026-09-30

An isolated CMake project under `tests/security/awslc` pins AWS-LC revision
`574fbd729ca31aeebe80a98d35742c0402435790` and verifies the downloaded archive
SHA-256. Linux CI builds its non-FIPS static and shared variants and runs the
existing cryptographic primitive, SCRAM vector/error and TLS adapter lifecycle
tests against the same private adapter sources used by the driver. Provider
identity and FIPS-state reporting are selected inside the private adapter.

This is an adapter compatibility experiment. `ODBCPP_CRYPTO_PROVIDER=AWS_LC`
remains rejected by the main build. The proof does not cover a live PostgreSQL
TLS handshake, certificate/hostname verification, driver artifact linkage or
cross-provider coexistence; all remain required before qualification. Local
macOS runs are development checks, not an additional supported profile.

Source reference: [AWS-LC incorporation guide at the pinned revision](https://github.com/aws/aws-lc/blob/574fbd729ca31aeebe80a98d35742c0402435790/INCORPORATING.md).

## S2C AWS-LC verified TLS adapter gate — 2026-09-30

Both isolated AWS-LC linkage variants now require TLS 1.2/1.3 interoperability
with an independent Python/OpenSSL server: trusted DNS/IP round trips, wrong
DNS/IP identity and unrelated-CA rejection, and cleared verification state
after reset. This is adapter evidence only; PostgreSQL live authentication,
driver artifact linkage and coexistence remain open before AWS-LC qualification.
See CRYPTO_PROVIDER_PLAN.md for fixture prerequisites and exact scope.

## S2C AWS-LC live PostgreSQL session gate — 2026-09-30

The isolated static/shared proof now exercises PostgreSQL 17 through existing
session/parser/transport sources: verified TLS, observed SCRAM authentication,
direct and prepared results, authentication/trust/hostname rejection and fresh
reconnection. Both CI variants require the live fixture and retain CTest evidence.
The OpenSSL static gate and AWS-LC jobs share disposable TLS fixture setup.
AWS-LC driver linkage, packaging and coexistence remain unqualified; the main
AWS-LC profile is still disabled. See CRYPTO_PROVIDER_PLAN.md for exact scope.

## S2C AWS-LC probe artifact gate — 2026-09-30

The Linux static/shared AWS-LC jobs now retain artifact-hash-bound linkage
evidence for their live PostgreSQL executable. Shared dependencies must resolve
within the controlled build root; static archives require complete GNU ld input
and member records. The inspector's fixtures reject foreign/missing dependencies
and incomplete or misleading maps. This qualifies neither the ODBC driver nor
packaging/coexistence; see CRYPTO_PROVIDER_PLAN.md for remaining boundaries.

## S2C AWS-LC async transport gate — 2026-09-30

Static/shared AWS-LC proof jobs now run verified PostgreSQL sessions through
sync, thread-pool and Linux epoll TLS paths, and reuse the existing async TLS
lifecycle/deadline/cancellation tests. The live negative cases still require
connection retirement and successful fresh authentication. This covers internal
transport behavior only; genuine asynchronous ODBC APIs, driver packaging and
provider coexistence are not claimed. See CRYPTO_PROVIDER_PLAN.md.

## S2C AWS-LC preloaded-provider gate — 2026-09-30

Both Linux AWS-LC proofs now require system-OpenSSL preload coexistence checks,
including distinct provider identities and repeated crypto/TLS/live PostgreSQL
behavior. Shared AWS-LC uses upstream suffixed names and ELF symbol versions;
static archive symbols stay private. Evidence remains limited to probe
executables and the selected system OpenSSL 3 build, with ODBC-driver loading,
packaging and broader coexistence still unqualified. See CRYPTO_PROVIDER_PLAN.md.

## S2C AWS-LC ODBC shared-driver gate — 2026-09-30

The isolated Linux proof now builds the real PostgreSQL ODBC driver from the
shared production source inventory. Both linkage variants require export and
linkage inspection plus common unixODBC live TLS acceptance under OpenSSL preload,
including certificate/hostname rejection. Main-build behavior remains protected;
the AWS-LC product option stays disabled pending package/loader and final
qualification evidence. See CRYPTO_PROVIDER_PLAN.md for scope.

## S2C AWS-LC relocated driver package gate — 2026-09-30

The isolated Linux proof now stages a runtime-only package with dependency
license texts, relocates it, verifies package-relative ELF search paths and
provider resolution, and reruns ODBC TLS acceptance. Shared packages must reject
fresh loads with either provider missing and no override search path. Archives
and byte inventories are retained. Hostile loader paths and final release
qualification remain explicitly open; see CRYPTO_PROVIDER_PLAN.md.

## S2C AWS-LC archive round-trip gate — 2026-09-30

The retained internal driver archive must now pass inventory verification and
live TLS acceptance after extraction, with the original staging tree removed.
Negative fixture tests cover changed/missing/extra files, duplicate members and
escaping archive paths/links. This strengthens package evidence without claiming
archive authenticity, hostile-loader protection or AWS-LC profile qualification.

## S2C AWS-LC protocol downgrade gate — 2026-10-01

Both isolated linkage proofs require TLS 1.1 rejection, preceded by a successful
independent TLS 1.1 control handshake and binary exchange. Existing TLS 1.2/1.3
acceptance and invalid-certificate/hostname checks remain mandatory. No production
TLS policy is weakened and AWS-LC profile qualification remains open.

## S2C packaged dependency inventory gate — 2026-10-01

The internal AWS-LC archive includes its provider/linkage manifest, declared
source recipes and override flags, bundled fmt identity, and required license
hashes. Extraction tests verify those license bytes and reject wrong profiles
or premature qualification claims. Existing dependency versions remain pinned;
complete SBOM/provenance and final provider qualification are still open.

## S2C AWS-LC loaded-origin gate — 2026-10-01

Fresh Linux hosts must map the shared proof's exact packaged AWS-LC libraries;
static drivers must map none separately. Shared-profile library-path and preload
canaries require detection of external byte-identical provider copies. Evidence
explicitly distinguishes origin detection from runtime prevention. Final loader
trust/deployment policy and provider qualification remain open.

## S2C AWS-LC bounded-proof closure — 2026-10-01

The remaining recorded-identity and deployment-responsibility items are now
explicit in CRYPTO_PROVIDER_PLAN.md. The final exact-revision CI run must pass
both Linux linkage proofs and all protected gates before this bounded proof is
accepted. G12, the OpenSSL matrix and main-build AWS-LC enablement remain open.
Further AWS-LC proof expansion requires a demonstrated failed gate or contract
change. Next, reconcile the finite OpenSSL qualification evidence and return to
S2 error/lifecycle migration after the S2C matrix review.

## S2C Linux OpenSSL TLS qualification batch — 2026-10-01

Both Linux OpenSSL linkage rows run the shared independent TLS acceptance and
controlled downgrade fixture. System-shared also gains mandatory live driver
TLS/SCRAM and wrong-CA/hostname rejection with no skipped cases. Retained results
and binaries support later matrix acceptance; OpenSSL qualification remains open.

## S2C macOS OpenSSL TLS batch — 2026-10-01

Both iODBC Unicode builds gain mandatory independent TLS and live driver
TLS/SCRAM acceptance/rejection gates. Their disposable PostgreSQL fixture now
uses SCRAM rather than trust authentication. Stopped-cluster and private-key
permission guards protect fixture setup. Retained artifacts support later row
acceptance; macOS qualification and Windows TLS evidence remain open.

## S2C Windows live TLS batch — 2026-10-01

The Windows x64 hosted gate now runs mandatory Driver Manager TLS/SCRAM,
wrong-CA and hostname-rejection cases with retained no-skip results. Its
PostgreSQL setup adds an opt-in TLS fixture before startup. The shared test gains
native module loading while preserving POSIX gates. Windows row qualification
remains open for independent downgrade and identity/coexistence evidence.

## S2C OpenSSL identity evidence batch — 2026-10-01

The OpenSSL rows gain mandatory portable production-adapter identity/default TLS
policy checks against their manifests, retained alongside driver/probe hashes.
Parser negatives and end-to-end stale-evidence/hash-binding tests protect the
qualification record. This is probe evidence, not loaded-driver identity
enforcement; exact-revision CI and remaining coexistence/package/downgrade
evidence are still required before matrix acceptance. S2 migration and bounded
MySQL sequencing are unchanged.

## S2C Windows independent TLS proof batch — 2026-10-01

Windows now opts into the common independent TLS qualification peer using a
Winsock test probe. The gate requires TLS 1.2/1.3 success, trust/identity rejection
and TLS 1.1 rejection after a successful independent legacy control. It preserves
production behavior and retains native proof inputs/logs. Exact-revision Windows
CI is required; coexistence/package/matrix acceptance remain the finite S2C
follow-up work.

## S2C unit-evidence retention and binding batch — 2026-10-01

Windows preserves its unit JUnit report before integration overwrites temporary
CTest logs. All bounded OpenSSL rows retain a common hash-bound unit evidence
summary that rejects missing/skipped/failed cases, incomplete peer/control output
and inconsistent identity/artifact inputs. This supports the existing matrix
review without qualifying a row or replacing live/package/coexistence acceptance.

## S2C Linux static runtime archive batch — 2026-10-01

The bundled-static OpenSSL row gains a driver-only relocated archive proof with
bound distro/logging dependency licenses, exact extraction integrity, a fresh
host load without shared crypto mappings and real extracted-driver TLS/SCRAM
success/rejection cases. Existing archive checks are reused without changing
their AWS-LC default root. Public installer/static SDK and source-attestation
scope remain deferred; matrix acceptance is still separate.

Unix shared OpenSSL acceptance now retains host lifecycle evidence in mandatory
unit reports: configured-provider identity, host SHA-256/TLS operations across
actual-driver load/release, ODBC handle allocation, and overlapping loader
references. Missing or incomplete evidence fails the summary gate. This is a
bounded configured-provider sharing check; Windows same-basename preload
coexistence and the final S2C matrix acceptance remain open.

Windows bundled-shared preload acceptance has an explicit limitation: search
flags do not establish app-local ownership when a host has already loaded a
same-basename provider from another directory. Package CI requires a positive
same-installed-file preload, a specific foreign bound-import collision canary,
and a missing-preload canary, retaining input/report hashes. These checks are
harness detection after load, not production prevention. Windows public provider
qualification remains open for a reviewed isolation strategy; final S2C review
must carry that blocker without extending SDK migration indefinitely.

## S2 PostgreSQL reset checkpoint — 2026-10-01

An optional `SameAuthenticatedServerSession` reset facet is explicitly enabled
only for the PostgreSQL product. Backend-private exact completion validation and
mandatory PostgreSQL live tests protect ROLLBACK plus DISCARD ALL cleanup. All
cleanup failures retire; reset never reconnects or replays.

This is partial lifecycle evidence for G12, not closure. RAII exclusive leases,
credential/cache isolation and reuse acceptance remain open. Redshift reset is
unavailable pending its own live profile; provider/linkage qualification and
real-application PG-BETA gates are unchanged.

## S2 exclusive ownership checkpoint — 2026-10-01

An internal, composition-owned move-only session lease now protects one physical
session from duplicate borrowers and handles moves, owner destruction and
exception unwinding. Every return is terminal retirement; reset success cannot
authorize requeue. Focused concurrency/TSan and mandatory PG live evidence support
this narrow lifecycle step. Credential/cache policy and reusable return remain
required for G12. S2/S2C, Driver Manager pooling, PG-BETA and Redshift live gates
remain open; no public SDK API is added.

## S2 credential authority checkpoint — 2026-10-01

Added private opaque generation/expiry authorities and optional exact-token
session admission. Rotation/revocation/expiry/orphaning prevent new admission;
foreign/wrong-generation tokens cannot retire valid owners. Allocation failure
revokes previous authority. Active borrowers finish and all returns retire.

This is a bounded primitive with unit, race/TSan and PG live evidence, not complete
credential-provider or reuse qualification. Cache tokens wait for observable
coordinator reset/invalidation. G12 still requires integrated credential/cache
isolation and safe reusable return; S2/S2C and real-application gates remain open.

## S2 coordinator cleanup checkpoint — 2026-10-01

The private lease now has an explicit bounded reset path: exact profile and
Idle/Reusable plus passive connected/Idle success preserve the active borrower;
missing profile, expiry, failures, exceptions, inconsistent state or late
completion retire. Owning failure details retain no retry grant. Return still
retires even after cleanup success. Unit fixtures cover reentrant admission,
owner destruction/revocation during reset and misleading backend outcomes; real
PostgreSQL checks success and expired-deadline physical closure.

S2/G12 remain open. Direct raw reset can bypass the coordinator; guard/route that
path before qualifying cache invalidation or reusable return. No ODBC behavior,
Redshift reset, authentication-provider or linkage claim changes.

## S2 closed borrower facade checkpoint — 2026-10-02

Private leases no longer expose raw session/facet pointers. Direct and typed
prepared execution preserve inputs/deadlines and owning backend outcomes;
Retire snapshots or exceptions retire before returning/rethrowing. Coordinated
reset is the only borrower reset path, closing the prior ordinary-call bypass.
Same-borrower Reusable/ResetRequired remain distinct from cross-borrower reuse.
Unit cases cover input identity, disposition/error/exception handling, owning
results after destruction and moved/retired operations; PG live execution uses
the facade, including prepared execution after coordinator cleanup.

S2/G12 remain open pending cache invalidation (including SQL-driven changes),
credential-provider integration and reusable return. New facets need narrow
policy-aware lease methods. No ODBC or backend/provider qualification changes.

## S2 conservative cache-scope checkpoint — 2026-10-02

Private weak scope tokens now bind credential-current Idle leases and exact origin.
All execution/reset attempts invalidate before callbacks, and closure, retirement
and credential staleness invalidate without keeping sessions alive. Unbound,
non-Idle, unknown, failed-passive or allocation-failed issuance grants no scope.
Unit/race/TSan and PG live query-error/reset/closure evidence accompany this step.
No cache storage or external-schema freshness claim is made.

S2/G12 remain open: bounded reusable return, cache payload limits/keys/secret and
external-change policies, and production credential-provider integration still
need evidence. No ODBC, Redshift reset, provider/linkage or FIPS claim changes.

## S2 explicit same-owner return checkpoint — 2026-10-02

Private reusable return is opt-in, credential-bound and reset-gated. It reissues
only the original authenticated physical session to its existing owner; failed or
implicit return retires. Credentials/deadline/admission are rechecked atomically,
old scopes invalidated and the old lease detached before checkout. PostgreSQL live
evidence must prove same PID and cleared temporary/session/transaction state.
S2/G12 remain open: production provider integration, bounded pool policy, real
cache policies and public SDK conformance are not supplied by this primitive.
G8 real applications and crypto-provider qualification are unchanged.

## S2 lifetime/idle policy checkpoint — 2026-10-02

Opt-in private owner policy prevents same-owner reuse after finite lifetime or
idle expiry. Equality, active borrow survival, return-time expiry, large duration,
non-extending denial and checkout races have focused evidence; PG live checks
retirement of an expired returned session. This is no ODBC/pool/health capability
claim. Active health, product defaults, capacity and production credential binding
remain open alongside real cache policy. S2/G12 and application gates stay open.

## S2 checked active-health checkpoint — 2026-10-02

Coordinated private checkout reserves an exclusive bound lease, probes once with
the original deadline, validates exact Idle/Reusable/passive state and rechecks
admission/credentials/lifetime before delivery. Every probe/eligibility failure
after reservation retires; admission denial is nondisruptive. PG live exercises
same-session reissue and rejection after server-side termination. No health claim
is implied by primitive no-I/O checkout. S2/G12 still require production credential
binding, bounded composition/product policy and actual cache policy; pool/ODBC
reuse, real application gates and crypto qualification remain independently open.

## S2 managed authentication coordinator checkpoint — 2026-10-02

Private managed connection binds an owned credential authority only after fresh
physical authentication succeeds with exact validated Idle/Reusable state. Owned
exact tokens permit fail-closed expiry/revocation without token export or rebind.
PG live evidence covers successful managed reuse, active revoke survival/retirement
and failed authentication. ODBC still uses its existing session owner/facets;
required lease facades, backend password scrubbing, product defaults/capacity and
cache policy remain open before S2/G12 closure. No pool/public SDK/provider crypto
qualification follows from this helper.

## S2 bounded operation lease facade checkpoint — 2026-10-02

Transaction, isolation and description operations now stay inside the exclusive
lease, with unchanged deadlines and owning outcomes. Cache scopes invalidate
before every attempt; exceptions/Retire outcomes retire, while recoverable errors
retain only the same borrower. Optional facet absence preserves passive state and
never grants return. No raw facet pointer is exposed.

Focused coverage includes forwarding, exclusive callback access, cache invalidation,
missing-facet passive states, exception kinds, ambiguous success and owned metadata
and native diagnostics after retirement. Live PostgreSQL covers Serializable,
begin/commit/rollback, failed-transaction recovery and description ownership.

Bounded authentication retention cleanup is complete, with parser-local intermediates
and allocator copies outside its claim. Next: owned catalog/capability/status
interfaces and ODBC lifecycle adoption. S2/G12, product pool/defaults and real cache
policy remain open; MySQL and Redshift live work are unchanged.

Validation: focused unit/live PostgreSQL tests and all 76 credential/ownership
ThreadSanitizer tests passed. Read-only review found no implementation blocker;
its passive-state and owning-description test suggestions are covered. Complete
local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and Redshift build/absent-endpoint
gates passed. Exact-head Windows/packaging and crypto CI evidence awaits the run.

## S2 owned passive observation and catalog construction — 2026-10-02

SessionLease::inspect returns owning connection/state/version information,
transaction capabilities and optional description/catalog presence. It performs
no network I/O or health probe. SessionLease::catalog_query borrows the request
only until return and returns owning SQL or diagnostics, without executing it.
Neither operation exposes the physical session or a facet pointer.

Successful passive reads and local catalog errors preserve cache scopes because
these facet contracts perform no session mutation. Contradictory connection/state
observations and exceptions retire; passive Disconnected outcomes retire. Idle
snapshots retain only the same borrower, while Transaction/FailedTransaction/Unknown
remain ResetRequired. No passive snapshot proves health or grants return/requeue.
All callbacks run outside ownership locks and no retries/replay are introduced.

Focused unit/live tests cover owned values after retirement, request forwarding,
exclusive callback access, absent facets, local diagnostic ownership, cache
preservation, passive-state matrices, inconsistent observations and exception kinds.
The live PostgreSQL fixture executes generated catalog SQL through ordinary query
execution and verifies advertised version and capability values.

ODBC lifecycle adoption is next; production pooling/defaults, cache payload policy
and S2/G12 remain open. MySQL scope, Redshift live access and crypto qualification
are unchanged. Read-only review found no blocker; facet flags explicitly denote
presence, not universal request support. All 81 credential/ownership ThreadSanitizer
tests passed. Complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and
Redshift build/absent-endpoint gates passed. Windows/packaging and crypto evidence
await exact-head CI.

## S2 ODBC operation containment before ownership adoption — 2026-10-02

ODBCConnection no longer exposes get_db_connection or a replacement physical/facet
pointer. Statement direct/prepared execution, description and catalog construction
use private typed connection methods returning owning values/errors. SQLGetFunctions
uses a boolean catalog-facet presence query. Missing description remains rejected
before parameter-vector construction; existing diagnostic and execution timing are
preserved. Live health/reset tests retain their original ODBC configuration/transport
fixtures through a test-only view that owns the handle and returns values/errors,
never a physical session/facet pointer. An exact-name compile
regression guard rejects restoration of get_db_connection; it is not a general
scan for every possible renamed pointer escape. The test view is move-only,
requires serialized use before handle unregistration and has friendship only
with ODBCConnection.

This checkpoint deliberately retains the existing private unique physical owner.
The next ownership swap will adopt once into an unbound SessionOwner and retain
one terminal lease for the ODBC connection lifetime, without cache scopes,
return/reissue, probes at connect, or invented lifetime/idle product defaults.
Exact credential binding is mandatory before future reuse/pooling. ODBC-facing
mock successes must first gain truthful explicit snapshots; default Unknown/Retire
must remain terminal at the lease boundary. Active-health test routing also needs
a bounded lease operation or independent backend fixtures, never a raw ODBC escape.

No pooling, SDK/public qualification, MySQL expansion or Redshift live change is
claimed. S2/G12 and lifecycle adoption remain open. Focused backend-contract,
live handle-lifecycle and reset checks passed; read-only review found no blocker
after preserving the original configured-transport fixtures. Complete local
PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and Redshift build/absent-endpoint
gates passed. Final focused PostgreSQL checks passed after narrowing test authority.
Windows/packaging and crypto evidence await exact-head CI.

## S2 ODBC exclusive ownership adoption — 2026-10-02

ODBC now adopts an authenticated physical session into a private, unbound
SessionOwner and retains one exclusive terminal lease for the connection lifetime.
Execution, descriptions, catalogs, transactions and isolation use bounded lease
operations. No return/reissue, pooling, automatic reconnect, replay, cache payloads
or lifetime defaults are introduced. Exact credential binding remains mandatory
before future reuse. Backend default Unknown/Retire snapshots remain terminal;
ODBC-facing mocks now report explicit truthful Idle/Reusable snapshots.

Negotiated server version and capability/facet observations are owned at adoption;
SQL_ATTR_CONNECTION_DEAD uses passive lease observation without network I/O.
Logical ODBC-open state remains separate from physical liveness, so terminal rows
and diagnostics remain readable and explicit disconnect/reconnect remains required.
Disconnect clears observations back to provider defaults. Authentication and passive admission require exact Idle/Reusable evidence;
dirty or terminal successful setup results are rejected. Negotiated feature answers
remain stable while logically open after retirement. Setup exceptions clean
up the attempt, allocation failures report HY001, and a retired setup lease cannot
be published as an open connection. Lease retirement destroys the physical session
once; freeing a logically open connection remains a sequence error.

The private test view routes active health through a bounded borrower operation:
it invalidates local cache authority, preserves transaction-state results and
retires on terminal results or exceptions. It does not grant checked admission or
reuse authority. Timed-out reset retires its facet along with the physical session.
ODBC's internal include manifest/probes now permit ownership contracts while
continuing to deny protocol and prototype-pool headers. Credential header visibility
is a transitive owner-header requirement, not qualification for ODBC credential
authority use; separating those header surfaces remains follow-up work.

Validation: focused backend/ownership unit and live lifecycle/reset tests pass,
and all 83 credential/ownership tests pass under ThreadSanitizer. Complete local
PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and Redshift build/absent-endpoint
gates passed. Final focused diagnostic assertions also passed. Windows/packaging
and crypto evidence await exact-head CI. S2/G12 remain open;
credential-bound product reuse, cache policy and SDK qualification are subsequent
work. MySQL, Redshift live access and crypto qualification are unchanged.

## S2 credential-authority header isolation — 2026-10-02

The follow-up from ODBC ownership adoption is closed: `session_owner.h` now
forward-declares credential types instead of importing authority definitions.
Only trusted composition/implementation and explicitly bound test fixtures
include `credential_context.h`; the ODBC staged include tree excludes it.
Positive C++20 probes require the ownership header to compile independently,
keep both credential types incomplete, and preserve move/unbound-construction
contracts. Negative probes reject direct ODBC/backend authority includes while
composition still compiles its authority header. Runtime ownership, authentication,
reuse and ODBC behavior are unchanged; this is private header isolation, not a
hostile-plugin security boundary or public SDK/ABI qualification.

Focused ownership/backend and architecture tests pass; read-only review found no
blocker. All 83 ownership/credential ThreadSanitizer tests and complete local
PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and Redshift build/absent-endpoint
gates passed. Windows/packaging and crypto evidence await exact-head hosted CI.
S2/G12, product reuse/cache policy, MySQL proof and crypto qualification remain open.

## S2 prepared metadata session affinity — 2026-10-02

The existing statement-local description cache now keys successful metadata to
an opaque weak logical-session identity, in addition to preparation and IPD
revision. Cache hits require the same identity and an active exclusive lease.
Fresh successful connection setup creates the identity; failed setup and close
clear it. No credential, principal, SQL text or numeric generation is added to
this identity. It grants no backend reuse or cross-session cache authority.

Tests cover same-session hits without extra descriptions, timeout-close retaining
statement handles, failed opens preserving output arguments, reconnect with changed
parameter metadata, and terminal retirement rejecting stale prepared metadata.
A live PostgreSQL test changes temporary-table shape across timeout/reconnect and
requires fresh column metadata on the retained statement. Already-executed owning
rows/metadata remain readable after retirement. Public SQLDisconnect continues to
unregister its child handles. Read-only review found no blocker.

This closes connection-lifecycle affinity only. External schema freshness,
same-session mutation/reset invalidation, cache observability and full G12 cache
qualification remain open; no new prepared-statement payload cache, pooling,
performance or public SDK claim is made. Focused backend-contract and live metadata
tests passed. Complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and
Redshift build/absent-endpoint gates passed. Windows/packaging and crypto evidence
await exact-head hosted CI.

## S2 conservative metadata epochs and control routing — 2026-10-02

Prepared description validity now uses a private connection-owned metadata epoch.
Direct/prepared execution, transaction/isolation, description, health and reset
attempts invalidate it before dispatch, including recoverable errors and absent
facets. Test-only health/reset views use private ODBC wrappers, closing an
invalidation bypass. Passive observation and local catalog construction preserve
it; executing catalog SQL still passes through execution invalidation.

Successful validated descriptions publish a fresh weak statement epoch only after
metadata application and only with an active lease. Terminal successful descriptions
return their owned metadata without publication; failed operations never restore
the previous epoch. Same-statement hits remain possible within an unchanged epoch.
Describing statement B deliberately invalidates statement A, so alternating
statements refetch; this conservative policy makes no performance claim. Already
executed results retain their owned metadata. The scope remains one logical
connection, one current epoch, existing statement payload/resource bounds and
serialized handle operations; no credentials or serialized identity are added.

Fixed Debug events expose hit, miss, stale rejection, invalidation and publication
without SQL, identifiers, principal, credential, pointer or epoch values. A SQL
canary regression verifies cache events do not disclose query contents. Unit tests
cover passive reads, control attempts, success/recoverable failure, alternating
statements and terminal descriptions. Live PostgreSQL tests prove same-session DDL
refresh, health/reset routing, DISCARD-related missing-table diagnostics and recovery.
Focused unit/live checks and complete local PostgreSQL, iODBC UTF-16/UCS-4,
ASan/UBSan and Redshift build/absent-endpoint gates passed. Final-source PostgreSQL
and UTF-16 rechecks passed after removing the unnecessary connect-time marker
allocation. Windows/packaging and crypto evidence await exact-head hosted CI. External
schema-change freshness remains unqualified; pooling, cross-session/prepared
payload caches, G10 tuning and public SDK claims are not enabled. S2/G12 remain open.

## S2 session baseline release evidence — 2026-10-02

An SDK-contract-only internal runner is shared by standalone synthetic and
mandatory PostgreSQL integration targets. It verifies normalized owning results,
prepared parameter behavior, recoverable-error state and recovery, passive state
agreement, disconnect and ownership after destruction. Fixed check IDs avoid
leaking SQL, settings or native diagnostics. Fault fixtures exercise rejection
and cleanup. Redshift builds exclude the PostgreSQL live adapter.

Validation: focused synthetic/live tests and complete local PostgreSQL, iODBC
UTF-16/UCS-4, ASan/UBSan, and Redshift build/absent-endpoint gates passed.
The final-source PostgreSQL recheck passed. Exact-head hosted Windows/packaging
and crypto profile gates await CI.
This does not close S2/G12, PG-BETA/G8, S4, provider/pool qualification, or any
Redshift live gate.
