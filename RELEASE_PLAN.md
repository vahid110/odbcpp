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
| T6 | Additional IAM/SSO providers, browser auth, automatic credential refresh beyond selected method | User-selected enterprise authentication or credential lifecycle requires it; chosen auth never deferred |
| T7 | Native handling for Redshift SUPER/spatial/sketch types and other extensions | Selected workload uses them. Only offer a text fallback if its metadata and representation are verified on real Redshift |
| T8 | Binary wire-format acceleration, new prepared-cache behavior, public/advanced pool policy, sizing/eviction tuning and transport micro-optimizations | G10 measurements identify a bottleneck; S1/G12 lifecycle correctness is mandatory, but no unmeasured speedup target is promoted |
| T9 | Public SDK stability/versioning promises, full MySQL beta breadth and TDS | Bounded G12 proof passes and public SDK/MySQL product scope is selected with funded requirements |
| T10 | Broad file splitting, universal plugin registry, broad static-analysis cleanup | A release defect/change needs broader cleanup; focused A1–A4/G9a extraction is mandatory in PostgreSQL M1, with G9b refinement in Redshift M3 |
| T11 | Extra PostgreSQL versions/features beyond frozen validation baseline | Customer deployment needs them or a shared/Redshift defect reproduces there |

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
