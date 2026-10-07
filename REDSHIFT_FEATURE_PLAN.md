# Redshift feature and compatibility plan

Initial inventory recorded 2026-10-01. The human-approved delivery order below,
updated 2026-10-06, governs subsequent Redshift work. Earlier dated sections retain
historical evidence; they do not override this order or establish qualification.
The human-approved lane transition on 2026-10-06 finishes AUTH's current
checkpoint: two sequential IAM acquisitions and verified-TLS Redshift logins
under one SDK lifetime. After live qualification, independent review and cleanup,
commit/push and green exact-head CI, AUTH is parked until the user resumes it.
Implementation then focuses entirely on Redshift through prepared-decimal live
qualification and the first packaged Excel acceptance on this Mac. Independent
review, security and regression/CI/cleanup gates remain active. MySQL and unrelated
SDK feature work remain parked by the user's request.

## Human-approved delivery order — 2026-10-06

Keep the existing backend/ODBC boundaries and reuse completed implementation and
tests. Improve delivery by qualifying usable application workflows on real
Redshift rather than extending narrow type refinements or orchestration chains.
Finish the already committed prepared hex-text-to-VARBYTE checkpoint first:
follow deed8b7 CI to completion, bind the reviewed two-case runner to the actual
binary and run its finite native test with existing protected setup/cleanup.
Record success or the concrete native failure; repair that failure proportionally.
Do not make raw binary binding, maximum storage or another auth method prerequisites
for finishing this checkpoint. Then use the following order.

| Order | Bounded checkpoint | Observable acceptance |
|---|---|---|
| 1 | Metadata-to-query workflow | Ordinary-user schema/table discovery, SQLColumns, prepare, parameter binding and typed fetching on the same representative fixture. Exercise SHOW and LEGACY where applicable; identify and repair a missing mode/API rather than inheriting PostgreSQL success or falling back after an error. Cover exact identifiers, quoted/Unicode names, patterns/no matches, ordinals, type dimensions, NULL, diagnostics and usable recovery. Preserve existing PK evidence. |
| 2 | Common types across their actual paths | Integers, exact decimals, Unicode/text, temporal and binary: separately qualify metadata, direct/prepared results, bound fetch/SQLGetData, input parameters, NULL, truncation/overflow/precision boundaries and recovery. Link existing live evidence before adding tests; prioritize defects that prevent checkpoint1 or real application use. Result support never proves parameter support. |
| 3 | Early packaged-driver application acceptance | Select one available representative Power BI or Excel workflow: connect, browse metadata, import/refresh a typed result and use application-generated queries. Start the smallest application proof after checkpoint1 can support it, overlapping type completion rather than waiting for broad parity. Compare the same fixtures with a pinned official AWS driver and equivalent TLS/configuration; record binary/platform correspondence and meaningful differences. An unavailable application/reference host is an explicit dependency, not a reason to stop ready native work. |
| 4 | Operational correctness and analytical scale | Transactions/autocommit, cancellation, timeouts, failure/reconnect, bounded large-result memory, chunked fetching and fetch-mode lifecycle. Retain existing tests, but require real Redshift evidence for backend-specific behavior and bounded resource observations before production-readiness claims. |
| 5 | Broader parity | Shared/external catalogs, less common types, remaining metadata APIs, settings and advanced fetch behavior follow the usable baseline in finite source-backed batches. All required roadmap items remain scheduled; advanced S3 transfer retains its separate design/scope decision. |

Cross-platform packaging/regression remains a gate throughout. Move the selected
live workflow to one CI platform early, then expand supported OS/Driver Manager/
Unicode/crypto coverage. Local native evidence does not certify other platforms.
Use checked-in GoogleTests for feature assertions, with Python only for bounded
setup, selection, credentials, accounting and cleanup. Run native checkpoints
promptly when their actual source/binary/control prerequisites are ready; do not
introduce generic proof frameworks or rerun unchanged matrices between them.
Independent review remains focused on consequential changes and final integration.

The next selected implementation package after the VARBYTE checkpoint is an
end-to-end discovery/SQLColumns/prepared-query fixture. First reuse and assess
current SHOW/LEGACY routing and checked-in metadata/type tests; name the smallest
actual blocker and its happy/edge/recovery tests. Keep acceptance per API/mode/
platform explicit. This order changes priorities, not spending, IAM/network scope,
resource authority, existing protected admissions or the eventual parity goal.

## Trust configuration in packaged acceptance — 2026-10-07

Human-approved priority addition: include trust-source usability in the current
first packaged Excel-on-this-Mac checkpoint. Reuse verified TLS and the existing
explicit CA bundle path; record the installed driver's actual crypto provider,
default CA locations or chosen bundle, chain/hostname verification, custom-CA setup
and certificate update ownership. Compare with the pinned official driver using
equivalent verified TLS, while documenting any differing trust-source defaults.
An explicit-CA pass does not establish macOS Keychain or Windows Certificate Store
support. Keep the pending Excel Driver Manager/administrator prerequisite concrete.

The shared SDK owns the planned
[TLS trust-source extension](SDK_PRODUCT_PLAN.md#shared-tls-trust-source-extension--2026-10-07).
Document and qualify the chosen packaged configuration at P0 for this application
checkpoint; broader platform/default-store isolation, rotation and advertised
native OS-store support are P1 before corresponding production packaging claims.
Missing/invalid/untrusted CA, expired peer and wrong-host failures must remain safe,
clear and recoverable through a newly valid configuration, without insecure fallback.
Do not make native OS-store integration a prerequisite for ready finite native
Redshift work that already uses a qualified explicit CA bundle. AUTH and MySQL
remain parked; no new authentication method, cloud authority or budget/horizon
extension follows from this planning addition.

## Scope, priorities and checkpoints

The product goal is coverage of the official AWS ODBC driver's implemented
features, attributes, explicit settings and observable behavior, with every
exception brought to the user for discussion and approval. A scoped M3 beta is
an intermediate delivery, not completion of that parity goal. Existing safe
unsupported behavior does not count as feature parity.

- **P0 / M2:** inventory and assess before freezing the Redshift beta scope.
- **P1 / M3:** supported-path correctness, required application workflows and
  the selected authentication/metadata profile; blockers cannot be deferred.
- **P2 / RP1:** planned Redshift parity expansion beyond the frozen beta,
  delivered in separately estimated, finite batches after M2 assessment.
- **P3 / advanced-feature review:** discuss and approve a design and estimate
  before implementation. This is not an automatic implementation commitment.

The integration owner maintains the inventory and evidence; the user approves
exceptions, advanced-feature scope and changes to the frozen delivery profile.
M2 must estimate RP1 after the inventory, separate base effort from contingency,
and split it into bounded checkpoints. Existing M2/M3/M4 estimates do not include
comprehensive parity or transparent S3 transfer. New upstream releases enter a
recorded delta assessment rather than expanding an active batch automatically.

## Data type reference review

The user's 2026-10-04 clarification applies to every data type. Before changing
type behavior, inspect the pinned official driver's actual conversion paths,
relevant historical fixes and tests. Record metadata, result decoding, bound
fetch/SQLGetData, prepared parameters, NULL, limits, precision loss, diagnostics
and recovery separately; a helper's behavior does not establish every path.

The reference is evidence rather than an automatic correctness authority.
Compare it with the applicable specification, server behavior and our checked-in
tests. Preserve useful fixes and document deliberate differences with their
reason and tests; do not reproduce unsafe parsing, silent loss or accidental
mutation merely to match source. Distinguish source observations from qualified
release-binary behavior and keep shared PostgreSQL regression in scope.

Apply this review to boolean, integer, numeric/decimal, floating point, text and
Unicode, binary/VARBYTE, temporal, interval, UUID and structured/other advertised
types. Unsupported families need explicit inventory entries. New discoveries
enter finite reviewed batches, with live comparison separately admitted.

## Requested work

| ID / user item | Priority and timing | Planned result and acceptance |
|---|---|---|
| RS1 / 1: data sharing | P0 assessment; P1 if selected BI/schema-discovery workflow needs it, otherwise P2 RP1 | Discover and query shared/cross-database objects with correct catalog/schema/table identity and privileges. Cover producer/consumer visibility, ordinary-user and denied access, name collisions, Unicode/pattern filtering and unavailable objects on real Redshift. Do not expose objects the caller cannot see. Map all affected catalog APIs, not only SQLTables/SQLColumns. Provisioned/Serverless and any external catalogs need explicit applicability evidence. |
| RS2 / 2: authentication methods | P0 inventory; P1 selected usable method; P2 remaining official methods in RP1 | Inventory database credentials, AWS credential-chain/profile/role/temporary credentials, provisioned/Serverless IAM, federation/browser SSO, IAM Identity Center and token-based methods against the pinned source. Confirm the exact supported providers/options instead of assuming every family is present. Deliver independent backend auth providers through shared secure contracts. Test success, invalid/expired credentials, refresh/reconnect, cancellation/deadlines, proxy/endpoint policy, principal isolation and secret redaction; include GUI/DSN and connection-string behavior where applicable. Existing D3/G7 and T6 cover only selected-method acceptance and deferred breadth. |
| RS3 / 3: declare/fetch mode | P0 comparison/design; P2 RP1, promoted to P1 if a required workload needs bounded fetching | Assess declare/fetch alongside the AWS streaming mode and Simba's documented/observable behavior. Freeze mode selection, defaults, batch sizing and precedence before implementation. Verify forward-only direct/prepared execution, empty/large/NULL/chunked data, transaction/autocommit interaction, multiple results, cancellation/timeouts, early close/disconnect and mid-batch failure. Measure peak memory and cleanup on real Redshift. Differences or omission of StreamingCursorRows versus declare/fetch require user approval. |
| RS4 / 4: behavior parity | P0 source analysis and matrix; P1 supported beta cases; P2 exhaustive planned RP1 coverage | Trace explicit application-visible code paths in the official source to our contracts and differential tests. Compare return codes, SQLSTATE/diagnostic chains, lengths/output preservation, conversions, state transitions, transactions, metadata, configuration precedence, auth lifecycle and error recovery. Record internal mechanisms only when they affect observable behavior; implementation structure need not be copied. Every identified behavior receives a disposition and evidence, with no unexplained differences at parity closure. |
| RS5 / 5: feature/attribute/configuration parity | P0 full inventory; P1 selected beta surface; P2 RP1 completion | Inventory exports (including A/W and legacy variants), attributes, descriptors, SQLGetInfo/SQLGetFunctions claims, DSN/GUI and connection-string keys/aliases, defaults, ranges, precedence and platform/build applicability. Every implemented official feature/setting must become available here or receive a user-approved exception. Parsing a key without providing its effect is not parity. Keep capability claims consistent with actual behavior. Fetch/streaming alternatives are decision items, not preapproved omissions. |
| RS6 / 6: SHOW metadata alternatives | P0 capability/permission assessment; P1 selected metadata APIs; P2 broader RP1 coverage | Design SHOW-based discovery alongside applicable SVV/catalog queries. Select by demonstrated server capabilities and permissions with explicit fallback rules. Normalize outputs into the same ODBC metadata contract. Verify current/all-database scope, shared objects, identifiers/escaping, patterns, ordering, metadata-ID behavior, privileges and ordinary-user access across both paths. Do not turn permission, transport or malformed-result failures into silent catalog fallback or fabricated empty success. |
| RS7 / 7: backward-compatible APIs | P0 mapping audit; P1 required legacy callers; P2 remaining RP1 coverage | Map replaced ODBC 2.x calls/options to ODBC 3.x/3.8 behavior and determine which mappings are supplied by each Driver Manager versus require driver exports. Cover handle allocation/free, transaction calls, option/attribute mappings, binding, extended fetch, column attributes and diagnostic APIs as applicable. Exercise negotiated ODBC versions and ANSI/wide/32-/64-bit rules through Windows DM, unixODBC and both iODBC widths. Preserve version-specific semantics without duplicating shared workflows. This is not a blanket full-ODBC-3.8 claim. |
| RS8 / 8: transparent S3 bulk transfer | P3 discussion after M2/RP1 baseline, or earlier only on explicit reprioritization | Explore COPY for S3-to-Redshift bulk loading and UNLOAD for Redshift-to-S3 export, with optional staged result delivery through ordinary ODBC fetch APIs. Distinguish database import/export from querying S3-backed external data. Discuss explicit modes and any automatic selection before coding; no invisible query/write rerouting is approved. Define eligibility, transaction/atomicity limits, row counts/diagnostics, format/type fidelity, IAM/KMS and bucket policy, staging cleanup, cancellation, retry/idempotency and cost boundaries. If ordinary API semantics cannot be preserved, expose a documented explicit opt-in or reject that path. |
| RS9 / live CI and platform coverage | P0 coverage design; P2 medium-term delivery, with mandatory CI gates before claiming the corresponding platform/profile | Migrate Redshift live GoogleTest integration to CI across supported Linux, Windows and macOS targets, Driver Managers, Unicode widths, process architectures and qualified crypto/linkage profiles. Include the full supported Redshift feature/behavior and negative/recovery matrix, with dedicated fixtures for authentication, sharing, external data and S3 where applicable. Track provisioned/Serverless applicability, reference-driver differential tests and packaging/application acceptance separately. Local paid testing is a temporary qualification stage, not the final CI architecture. See the coverage and migration contract below. |

## Medium-term Redshift CI coverage commitment

User confirmed that the current local live runner is acceptable temporarily and
requested full Redshift CI coverage, including multiple operating systems, in
the medium term. This is an explicit planned deliverable, not a promise that the
current small fixture or a build-only green run provides that coverage.

- Run live GoogleTest integration on Linux/unixODBC, Windows Driver Manager,
  and macOS/iODBC with the supported UTF-16/UCS-4 configurations. Pin OS,
  architecture, Driver Manager, driver/server versions and connection profile;
  qualify supported x64/ARM64 targets and crypto/linkage combinations according
  to their declared support matrix. A passing PostgreSQL run cannot qualify
  the corresponding Redshift combination.
- Maintain a feature-to-test matrix for RS1–RS8 and the shared ODBC contract,
  covering successful, boundary, denied-access, malformed/error and interruption
  paths. Add isolated fixtures for data sharing, supported authentication methods,
  external catalogs and approved S3 transfer designs as those features enter
  scope. Exercise applicable provisioned and Serverless configurations; record
  inapplicability explicitly instead of silently skipping a required feature.
- Preserve the single shared spending authority, cumulative accounting, exclusive
  execution, protected scoped AWS credentials, restricted networking, bounded
  client/server execution and independently verified cleanup across local and
  CI callers. Select hosted versus dedicated runners during implementation;
  runner-local locks alone cannot coordinate multiple CI machines. CI migration
  does not authorize new warehouses, spending or IAM expansion now.
- Publish sanitized GoogleTest results tied to the exact source/artifact/profile
  and fixture versions. Required live jobs must fail or report a blocked
  qualification when admission, environment or cleanup fails; skipped/absent
  live evidence must not produce a Redshift-qualified green result. Keep fast
  offline checks separate from paid live gates, choosing PR, scheduled and
  release frequency deliberately after cost and reliability measurements.
- Add the official-driver differential suites and the planned real-application
  acceptance tracks to the release evidence. Headless integration alone does
  not substitute for Windows Power BI/linked-server or macOS Excel workflows;
  dedicated application automation or recorded acceptance remains necessary
  until each workflow is reliably automated.

Deliver in bounded stages: qualify shared remote admission/cleanup first,
move the current baseline to one CI platform, expand the supported OS/profile
matrix, then add the broader feature fixtures and application gates. Estimate
and prioritize each stage with RP1 after the current bounded IAM/catalog work.
Close RS9 only when every supported platform/feature row has passing evidence
or an explicitly documented, user-approved scope exception. Advanced RS8 scope
still needs its design discussion; this coverage commitment does not approve
transparent S3 behavior before that decision.

## Reference baseline and known discrepancy

Initial reference: AWS open-source ODBC **v2.2.4**, main commit
`56d35297f9bee0cc31c0148581c87ca455639a39` (2026-09-28), inspected for planning
on 2026-10-01. Pin both source and installed binary/platform versions when the
actual comparison starts; confirm source/binary correspondence and retain the
server version, Driver Manager and configuration for each test.

- [Pinned AWS source](https://github.com/aws/amazon-redshift-odbc-driver/tree/56d35297f9bee0cc31c0148581c87ca455639a39)
- [Pinned release notes](https://github.com/aws/amazon-redshift-odbc-driver/blob/56d35297f9bee0cc31c0148581c87ca455639a39/CHANGELOG.md)
- [AWS option reference](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-configuration-options.html)
- [AWS authentication reference](https://docs.aws.amazon.com/redshift/latest/mgmt/odbc20-authentication-ssl.html)
- [AWS metadata guidance](https://docs.aws.amazon.com/redshift/latest/mgmt/discovering-metadata-driver-api.html)

The pinned changelog adds UseDeclareFetch in v2.2.3. AWS's option-page streaming
section still describes StreamingCursorRows as replacing the older declare/fetch
options. Resolve this source/documentation discrepancy in RS3; do not assume
declare/fetch remains proprietary-only or conflate the two fetch mechanisms.
Simba comparison requires a separately pinned available binary and documentation;
proprietary source is not a prerequisite for observable differential tests.
No detailed source audit, behavior parity or Simba qualification is claimed yet.

## Inventory and differential-test contract

Create two linked matrices during M2: **features/options** (RS5) and **observable
behaviors** (RS4). Use a stable ID per item, with:

1. Source permalink/line or documented/binary-only reference, reference version,
   applicability and parent API/option; record source/documentation conflicts.
2. Inputs, defaults/aliases/precedence, preconditions and relevant version/mode;
   expected return values, outputs, diagnostics and side effects.
3. Our current status: unaudited, present/unverified, verified match, gap,
   scheduled, or user-approved exception. Link existing tests before adding work.
4. Priority, milestone, owner, estimate and residual risk. An out-of-beta item
   stays scheduled for RP1; it is not silently treated as an accepted exception.
5. Happy-path, boundary/negative and recovery test IDs; exact local/CI/live/app
   run evidence and explanation of permitted nondeterministic differences.
6. For exceptions, alternatives, rationale, compatibility/security impact and
   the user's dated decision. Pending decisions block the affected parity claim.

Inventory all relevant source modules and platform/version branches in finite
passes; document reviewed files, unreviewed areas and exclusions. Include release
fixes and upstream tests as leads, not as proof that every branch was reviewed.
Compare both drivers against the same fixtures and equivalent security settings
on real Redshift. Driver-specific native messages may differ; diagnostics must
be analyzed, not normalized away indiscriminately. Run common contract and
PostgreSQL regression gates for any shared-layer change. Test promised application
compatibility through the packaged drivers at G8.

ODBC requirements and our security model remain constraints. Suspected upstream
bugs, unsafe defaults or conflicting version semantics become explicit decision
records with spec/security evidence; do not reproduce vulnerabilities or weaken
verified transport merely to get a matching result. Bring compatibility
alternatives to the user before accepting an exception.

## Release integration and stop rules

- **M2 / G1:** establish real endpoint evidence, build both inventories and
  decide/estimate the beta profile and RP1 batches. AWS access remains a live
  evidence dependency, not a reason to claim PostgreSQL simulations as parity.
- **M3 / G6, G7, G8, G9b:** implement and test the frozen profile's prioritized
  sharing/metadata/auth/fetch/legacy behavior. Publish outstanding RS1–RS7 rows
  alongside the scoped beta limitations; no full-parity announcement.
- **RP1 / G14:** close every RS1–RS7 inventory item by verified implementation or
  explicit user-approved exception, with traceable source coverage and test
  evidence. Missing rows, unresolved divergences or unapproved exclusions block
  the parity claim. G14 does not block an explicitly scoped M3 beta; estimate
  whether RP1 belongs before a broader production release at M2 scope review.
- **RS8:** a separate design review and explicit user approval precede any
  advanced S3 implementation or public seamless-transfer claim.

Do not start RS implementation from this planning update, extend MS1/MySQL scope,
or put Redshift branches in shared ODBC orchestration. Keep SDK extension and
security boundaries intact. Review discoveries at batch boundaries and replan
rather than absorbing unlimited parity work into the existing buffer.

## First parallel inventory — 2026-10-02

[REDSHIFT_PARITY_INVENTORY.md](REDSHIFT_PARITY_INVENTORY.md) records the first
bounded official-source pass across RS1–RS8, separates observations from proposed
tests, and retains an explicit unreviewed-source ledger. It is preparation, not
behavioral acceptance or an exhaustive parity claim. The next independent
research package is RP-INV-02 fetch-mode decisions; runtime and live estimates
remain separate. All existing scope approvals, security constraints and live
gates remain intact.

## Fetch-mode research checkpoint — 2026-10-02

[REDSHIFT_FETCH_MODE_REVIEW.md](REDSHIFT_FETCH_MODE_REVIEW.md) records RP-INV-02:
34 atomic source-review rows and finite differential fixtures for settings,
eligibility, batching, transaction ownership, cleanup and cancellation. Source
default conflicts and close/refill/cancel semantics are explicit live-dependent
decision items. No runtime change, feature exception, fetch profile or parity
acceptance is approved. The research package stops here until the named M2
profile/test decision or restored access; active MySQL scope is unchanged.

## Redshift account-preparation handoff — 2026-10-02

The user reports account access restored; no Redshift infrastructure exists yet.
[REDSHIFT_LIVE_TEST_PLAN.md](REDSHIFT_LIVE_TEST_PLAN.md) now governs the one
USD 15 initial testing allowance shared across all chats, protected configuration,
provisioning/driver ownership, canonical local ledger, required shared runner
and teardown. Verified infrastructure/cost controls and runner readiness replace
account access as the M2 activation dependency. No paid test execution or
provisioning occurred here; the runner is not implemented/qualified. The bounded
MySQL SDK proof continues until that activation gate is ready.

## Batch size and diagnostic execution — human agreed 2026-10-07

Default to coherent workflow batches, normally three to five related scenarios
when ready and compatible, rather than a separate integration cycle per case.
Run focused checks while developing, then the required shared regression graph,
independent integration review, push and exact-head CI once per coherent batch.
Preserve each scenario's assertions and result; never add unrelated work merely
to fill a batch or replay already qualified milestones.

CI and real Redshift tests provide distinct evidence. A bounded diagnostic run
may precede final CI when its exact candidate source, actual binary, relevant
local checks and finite cloud controls have received independent review. Record
it as diagnostic evidence; final acceptance still requires all applicable local
gates, independent review, exact-head green CI and actual live proof. This does
not bypass an existing launcher that requires CI: any diagnostic-specific control
change must be independently reviewed before use. Finish the current Unicode
alias batch under its already selected controls.

Group ready, reviewed live scenarios into one finite cluster session with explicit
per-case and aggregate statement/time bounds, durable one-use admission and
verified exact-principal cleanup and pause. Keep the cluster paused during coding,
builds, CI and longer gaps; do not keep it running to await speculative readiness.
Spending, resource, principal, network and authorization horizons are unchanged.
