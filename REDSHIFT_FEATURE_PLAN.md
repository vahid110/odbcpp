# Redshift feature and compatibility plan

Recorded 2026-10-01 from the user's eight requested areas. This is future
Redshift planning only; active S2/S2C and the bounded SDK/MySQL proof keep their
current sequence. No feature below is implemented or qualified by this document.

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
