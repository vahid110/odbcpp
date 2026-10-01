# ODBCPP roadmap: shared foundation → connectivity SDK proof → Redshift

Replanned 2026-09-30 after the PostgreSQL Windows delivery and Redshift pilot
preparation. This replaces the old milestone dates,
completion percentages, and open-ended conversion checklist. Historical plans
remain in Git history. The detailed [conformance audit](ODBC_CONFORMANCE_AUDIT.md)
remains evidence, not the release stopping rule.

## Product goals

1. A dependable PostgreSQL implementation to validate shared ODBC and transport behavior.
2. A usable Redshift driver, adapted after the PostgreSQL beta checkpoint.
3. A reusable C++20 connectivity SDK, with database-specific behavior outside
   client API adapters and a path to both ODBC and ADBC.
4. A bounded MySQL 8 reference slice proving that the SDK supports an unrelated
   wire protocol before public SDK stability is promised.

Active implementation scope is PostgreSQL, Redshift and the bounded MySQL SDK
proof in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md). RDS, Aurora, Athena, MariaDB
and SQL Server/TDS are not added. SDK architecture began in PostgreSQL
consolidation (G9a), is tested across unrelated protocols by MySQL (G12), and is
refined through Redshift specialization (G9b). ADBC shapes the internal boundary
but its API, Arrow integration and packaging are deferred. Public SDK stability
and distribution remain G13. See the
[backend boundary and acceptance criteria](BACKEND_BOUNDARY.md).

The PostgreSQL technical and Windows delivery baseline may support SDK work while
host-dependent G8 application acceptance is deferred by explicit product
decision. This does not close PG-BETA or waive its evidence. Work must
close a release gate, fix a material defect, or establish a reusable boundary
needed by these products. Extra test permutations alone do not justify a batch.

## Current position

The PostgreSQL foundation works and has substantial hardening evidence. At the
baseline, 76 exported symbols cover 49 operations: 11 are fully verified under
the audit's strict rules and 38 remain partial. These are not effort percentages.
All five latest CI jobs passed; the local suite has 34 executables (24 unit,
10 integration). This does not establish application or Redshift compatibility.

Redshift is a build target using the PostgreSQL parser, but current CI uses
PostgreSQL 17. Windows CI runs unit tests, not live database integration.
The SDK has useful interfaces and install exports, but ODBC code still directly
constructs PostgreSQL components and contains PostgreSQL catalog/type logic.

See [the assessment and release gates](RELEASE_PLAN.md) for evidence, scope,
acceptance criteria, investigation budgets, and deferred work.

## Milestones and working estimates

Estimates are engineering working days for one focused implementation stream,
not scheduler wakeups or promised calendar dates. They exclude waiting for
credentials, infrastructure, product decisions, and external application access.
Ranges are provisional, with low confidence until M1 completes. A 30% contingency
is included in the cumulative ranges, rounded upward. It funds discoveries,
not additional features. Re-estimate after the real Redshift pilot.

| Order | Milestone | Base effort | Buffered cumulative target | Exit |
|---|---|---:|---:|---|
| M0 | Freeze the supported release profile and evidence inventory | 2–3 days | 3–4 days | G0 closed; target application/auth/platforms named; A1–A4 inventoried and estimated |
| M1 | PostgreSQL consolidation and usable beta checkpoint | 14–23 days | 21–34 days | PG-BETA below closed; G9a architecture, coherent behavior, application demonstration and installable artifact |
| MS1 | SDK foundation and bounded MySQL proof | 25–41 days | 33–54 days for MS1 | G12 closed; two real sibling backends use the shared contract; qualified crypto build profiles; no MySQL beta or public ABI claim |
| M2 | Real Redshift pilot and compatibility assessment | 3–5 days | Prior cumulative target resumes when access exists | G1 closed on a real Redshift endpoint |
| M3 | Usable Redshift beta | 8–12 days | Re-estimate after M2 | G6–G8 and G9b closed; PostgreSQL regression gates remain green |
| M4 | Scoped production release candidate | 8–12 days | Re-estimate after M2 | G10–G11 closed; both scoped beta baselines remain green |

The [M0 working inventory](PG_BETA_CHECKLIST.md) sizes architecture at 9–15
days within M1, revising M1 to 14–23 days. These estimates remain provisional,
not a measured forecast.
M0 must enumerate and estimate the finite PostgreSQL consolidation checklist
before committing to a date. M1 includes application and basic packaging work
that the earlier foundation-only estimate omitted. M0 must explicitly estimate
A1–A4/G9a and revise the M1 range if needed; architecture effort must not be
treated as free work or hidden in contingency. The sequence is sequential;
external waiting is excluded. Re-estimate at PG-BETA and after the Redshift pilot.
The live Redshift pilot is currently waiting on restored user account access.
MS1 is approved bounded work during that external wait. It does not replace M2
or authorize a full MySQL driver. Its package estimates, gates and stop rules are
in [SDK_PRODUCT_PLAN.md](SDK_PRODUCT_PLAN.md).

MS1 includes the bounded S2C cryptography-provider package. Provider and
dependency linkage are explicit build profiles; OpenSSL is preserved and AWS-LC
receives a non-FIPS Linux proof before any support claim. See
[CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md).

### PG-BETA: mandatory handoff checkpoint

- G0 frozen for PostgreSQL; G1 evidence integrity and G2–G5 pass for PostgreSQL.
- G9a passes: backend creation, dialect, native types/catalogs and capabilities
  are behind an exercised boundary; ownership, errors and deadlines are defined.
- Each promised query, parameter, result, metadata, transaction and diagnostic
  workflow is consistent; unfinished optional behavior is explicitly unsupported.
- G8 application workflow passes on PostgreSQL, including required metadata.
- Basic G11 delivery subset passes: install/configure/uninstall, runnable examples,
  TLS/auth instructions, supported features and limitations, and a versioned beta
  artifact. Full production delivery and soak gates remain M4 work.
- No known serious supported-path defects. Record the demonstration, evidence,
  accepted limitations and beta baseline before starting Redshift implementation.

Application scope update (2026-09-26): SQL Server linked server/OPENQUERY,
Power BI Desktop on Windows, and Excel on macOS are requested acceptance targets.
See the [application matrix](RELEASE_PLAN.md#application-acceptance-targets--added-2026-09-26).
The effort table is the prior one-application baseline; M1 and cumulative dates
require re-estimation for these three tracks. Windows live validation is now
required. No extra application scope is charged to contingency.

## Next implementation sequence

1. **S1 contract freeze — complete:** four read-only audits produced
   `SDK_ARCHITECTURE.md` and `SECURITY_MODEL.md`; no behavior changed.
2. **S2 contract implementation and shared proof harness — in progress:** the
   verified authentication/CA and trusted configuration prerequisites are
   complete; proceed with only the accepted extractions;
   keep PostgreSQL, iODBC, Windows and sanitizer gates green.
3. **S3 MySQL vertical slice:** implement and live-test the bounded MySQL 8
   workflow through shared ODBC orchestration.
4. **S4 SDK usability proof:** publish the backend test kit, extension guide and
   minimal out-of-tree sample; stop and review G12.
5. **M2 Redshift pilot:** run as soon as valid endpoint access is available.
   PostgreSQL G8 application evidence remains open and recorded meanwhile.

## Execution and stop rules

Every batch names a gate ID and expected acceptance evidence. Retain focused
checks during development and the full relevant gates before an implementation
push. Do not rerun database gates for prose-only changes. Record new discoveries
in the [decision/backlog table](RELEASE_PLAN.md#deferred-work-and-revisit-triggers).

PostgreSQL consolidation ends at PG-BETA, including G9a. The explicit 2026-09-30
reprioritization permits MS1 before host-dependent G8 evidence is available;
PG-BETA remains open. Redshift beta ends when
G0–G8 plus G9a/G9b pass for its frozen profile and no known blocker remains. Release-candidate work
ends when G10–G11 also pass. G12 is the bounded internal SDK/MySQL proof; G13 is
the deferred public SDK preview.
A remaining `Partial` audit row is acceptable only if its residual behavior is
explicitly outside the release profile and safely handled. No serious defect
in a supported path may be deferred to meet a date.

Execution follows the user's current resume and implementation-heartbeat
instructions, selecting bounded work by these gates. This plan does not authorize
cloud resource creation or resume separate paused application reminders.

### Windows delivery scope clarification — 2026-09-29

PG-BETA now explicitly requires W1–W4 from RELEASE_PLAN.md: native Windows DSNs,
connection-string interoperability, a minimal ODBC Administrator setup GUI and
x64 installer/uninstaller. Estimate 4.5–8 base engineering days plus separate 30%
contingency, replacing overlapping Windows delivery allowance. The earlier M1
range remains historical, not a revised completion forecast. Complete A4/G9a
first; then W1/W2 before Windows applications, and W3/W4 before packaged G8
acceptance. Redshift remains after PG-BETA. Broader installer polish and
interactive connection prompts remain conditional, not part of these packages.


### Architecture checkpoint — batch 12

A1–A4 implementation and fake-backend acceptance are complete; the dependency
review is recorded in BACKEND_BOUNDARY.md. Close G9a upon this batch's green full
local gates and exact-revision CI. Next is Windows W1–W4, followed by G8 real
applications. The application-host reminder is not due while Windows delivery
work is still next. That work is now complete; G8 remains open and host-dependent.
The 2026-09-30 reprioritization permits bounded MS1 work during the Redshift
access wait. Public SDK packaging remains G13.

### S1 architecture/security checkpoint — 2026-09-30

S1 accepts the provider/session/facet architecture, independent PostgreSQL and
MySQL session engines, normalized result/error boundary, product-composition
registration point, replacement pool lease model, cache lifecycle and future
ADBC batch seam. `GenericDatabaseConnection` and `IProtocolParser` are recognized
as PostgreSQL-family internals rather than a universal parser architecture.

The security model makes verified transport the credential-authentication
default, rejects cleartext-password authentication without verified encryption,
removes implicit production working-directory configuration, bounds hostile
server input and requires secret-safe diagnostics, session isolation, fuzzing,
supply-chain checks and release hardening. S2 implementation is next. S1 closes
planning only; it does not close G12 or change a current support claim.

### First S2 security batch — 2026-09-30

Commits `df8a67e` and `179f6a6` make verified TLS the omitted-SSL default,
require verified peer identity before PostgreSQL cleartext-password
authentication, apply or reject one custom CA trust source, and remove implicit
current/parent-directory configuration discovery. Explicit plaintext remains a
documented opt-out for intentional local profiles. PostgreSQL, both iODBC width
matrices and the sanitizer matrix each passed all 37 local tests.

### S2 provider/composition batch — 2026-09-30

One immutable provider now owns compiled product identity, defaults, static
metadata, option resolution and session creation. Shared ODBC has no backend
selection macros or concrete PostgreSQL dependency, pre-connect metadata creates
no dummy live session, and PostgreSQL versus Redshift identity is supplied by
product composition. Automated architecture checks protect these boundaries and
unimplemented backend selections fail during configuration. Later S2 batches
still split internal targets and session facets and establish the final S4
extension-header allowlist.

### First S2C crypto-profile batch — 2026-09-30

Strict provider/linkage/root build profiles replace the public legacy OpenSSL
hint. Configure-time validation rejects unsupported or ambiguous profiles and
prevents bundled discovery outside its controlled prefix. A generated manifest
records requested and resolved build evidence without claiming verified artifact
linkage or FIPS. Windows packaging validates, installs and inventories that
manifest. Provider-neutral crypto adapters, artifact inspection and the AWS-LC
Linux proof remain the next S2C work.

### S2C private cryptography boundary — 2026-09-30

PostgreSQL authentication now obtains MD5, SHA-256, HMAC, PBKDF2, random bytes,
constant-time comparison and secure cleansing through a private
provider-neutral adapter. Portable strict Base64 handling removes the remaining
OpenSSL dependency from SCRAM. Secret intermediate values are cleansed during
exception unwinding as well as successful exchanges.

The synchronous TLS public header now owns its provider state through an
incomplete private type. Provider-specific TLS helper headers and the crypto
adapter are excluded from installation, and architecture checks reject direct
provider includes in backends or provider types in installed core headers.
The common private TLS adapter is the next source-boundary step; artifact
inspection and every qualification row remain open at this checkpoint.

### S2C runtime identity and artifact-form evidence — 2026-09-30

The private crypto adapter now reports its compile-time and loaded runtime
provider versions and active FIPS state. Linux, macOS and Windows test builds
inspect the finished driver with the platform binary tool, verify shared versus
embedded crypto dependency form, and write evidence bound to the artifact hash.
Negative fixtures reject missing shared dependencies and unexpected dynamic
dependencies in a static profile.

The evidence deliberately records that dependency origin is unverified and
makes no qualification claim. Controlled-prefix resolution, packaged loader
behavior, symbol visibility and coexistence are still required before any
profile is qualified. The following checkpoint closes the common TLS adapter.

### S2C common private TLS adapter — 2026-09-30

Synchronous socket TLS and asynchronous memory-BIO TLS now use one private,
provider-neutral client contract. Provider objects, certificate/hostname
verification, SNI selection, error queues and ciphertext BIO operations are
confined to the security implementation. Architecture checks prevent transport
code from bypassing that boundary, and the obsolete provider-specific transport
helpers have been removed.

Existing deadline, cancellation, custom trust, hostname, SNI, clean shutdown,
truncated-stream and PostgreSQL behavior remain protected by the transport and
integration suites. This closes the common-adapter source boundary only.
OpenSSL profile qualification, dependency origin, loader/coexistence evidence
and the bounded AWS-LC Linux proof remain open; no new provider or linkage
profile is claimed.

### S2C dependency-origin evidence — 2026-09-30

Artifact evidence now resolves OpenSSL shared-library paths on Linux and macOS,
canonicalizes them and records hashes bound to the inspected driver. The static
profile separately verifies and hashes both configured link inputs beneath its
controlled dependency root, without claiming that this proves embedded-byte
origin. Negative fixtures reject unresolved shared libraries, profile
mismatches and static inputs outside the controlled root.

This closes dependency-origin evidence for the exercised Unix profiles only.
It does not qualify a matrix row. Windows must still bind exact PE imports to
the controlled-prefix DLLs and fresh-runner loaded paths; static provenance,
symbol visibility, preloaded-provider coexistence, OpenSSL matrix qualification
and the bounded AWS-LC Linux proof remain open.

### S2C Windows packaged-loader evidence — 2026-09-30

Windows packaging now derives exact OpenSSL runtime names from PE artifact
evidence, stages them from the controlled root, and verifies their actual loaded
paths and version on a fresh runner under hostile PATH conditions. A missing
app-local runtime cannot fall back to a same-name PATH decoy.

This is packaged-loader evidence only. Preloaded-module coexistence, symbol
isolation and provider/linkage qualification remain open, and no AWS-LC or FIPS
claim is made.

### S2C driver export isolation — 2026-09-30

Linux, macOS and Windows shared-driver builds now expose exactly the canonical
76-entry ODBC C surface. Platform linker controls come from one allowlist, and
artifact tests reject missing or private exports while keeping the static core
available to internal tests.

This closes driver-binary export isolation only. Bundled shared-provider symbol
behavior, preloaded-provider coexistence, completed profile qualification and
the bounded AWS-LC Linux proof remain open.

### S2C Linux OpenSSL static artifact gate — 2026-09-30

A dedicated Linux CI build now exercises the OpenSSL `BUNDLED_STATIC` candidate
from an ephemeral controlled prefix. It runs the common TLS/authentication and
architecture suite, verifies the final ELF dependency and export surfaces, and
retains the source package identity and archive hashes alongside driver-bound
archive hashes. The prefix rejects residual header symlinks.

The row remains unqualified until provenance, live qualification and coexistence
evidence are complete. AWS-LC and FIPS remain unclaimed.

### S2C Linux OpenSSL static live gate — 2026-09-30

The static candidate now has mandatory unixODBC evidence for a verified-TLS,
SCRAM-authenticated PostgreSQL query plus rejection of an untrusted CA and a
hostname mismatch through unixODBC; direct driver calls also retain the exact
TLS failure diagnostics. A shared OpenSSL instance is preloaded while the static
driver is loaded and used; its identity remains stable, and unresolved provider
symbols are rejected alongside the existing dependency and export checks.

This proves same-package duplicate-instance cohabitation. Cross-version and
cross-implementation coexistence, embedded static provenance and AWS-LC remain
open, so no qualification claim is made.

### S2C Linux OpenSSL static link-trace gate — 2026-09-30

The CI static candidate now retains its exact hashed driver with a GNU ld map
whose `OUTPUT` record names that artifact and whose trace shows both controlled,
hashed archives contributed members. The gate rejects stale or incomplete maps.
This does not prove embedded-byte ancestry; that work, cross-implementation
coexistence and AWS-LC remain open, so the row is still unqualified.

### Current S2C closure sequence — 2026-10-01

The bounded non-FIPS AWS-LC Linux proof now includes functional, identity,
artifact, coexistence, extracted-package and loader-origin evidence, with
explicit trusted-loader, directory-ownership and dependency-update assumptions.
Acceptance is conditional on the final closure commit's complete green CI run;
see CRYPTO_PROVIDER_PLAN.md. This closes the bounded AWS-LC proof only.

The finite evidence review is now recorded in
[S2C_MATRIX_REVIEW.md](S2C_MATRIX_REVIEW.md). Bounded internal architecture evidence
is accepted, but the matrix is not fully qualified: Windows same-basename preload
isolation is a known blocker, and explicit OpenSSL row signoff remains open.
Main-build AWS-LC integration is still separate and disabled. Resume accepted S2
work with structured owning errors and session disposition, followed by results
and session/facet migration before S3 MySQL. Keep crypto regression gates and
the named qualification follow-ups; no G12 criterion is waived. G12 and
PostgreSQL G8 remain open; public SDK/FIPS scope stays deferred.

### S2 owning query errors and passive disposition — 2026-10-01

Direct, prepared and description operations now return owning BackendError
snapshots, including optional native detail, operation and protocol-derived
session disposition. ODBC query diagnostics consume these snapshots and no
longer read mutable SQLSTATE. Focused tests cover copy/move and ownership across
later operations and backend destruction, idle/transaction/aborted-state errors,
read timeouts, malformed responses and partial writes with retirement.

Continue A5 with the remaining type-resolution error migration and the
safe-message/logging boundary; immediate query errors do not complete A5 or S2.
Ordered result normalization, resource budgets and safe reuse facets remain
separate batches. Existing crypto qualification blockers and all protected
PostgreSQL/platform gates remain unchanged.

### S2 owning transaction/isolation errors — 2026-10-01

Transaction control and isolation changes preserve owning native error details
and protocol disposition, with explicit begin/commit/rollback/isolation context.
The ODBC begin-transaction helper preserves that snapshot while retaining
existing diagnostic mapping and caller deadlines. Invalid local selections are
classified without I/O; server-state and timeout/partial-write retirement are
covered through both adapters, alongside existing successful command tests.

Setup/authentication migration is recorded below. Next is type-resolution and
safe-message/logging work. A5 and S2 remain open; pool/reset facets, aggregate
budgets, normalized results and the MySQL proof remain separate steps.

### S2 owning setup/authentication errors — 2026-10-01

Connection setup preserves native server state in owning errors, labels connect,
authentication and startup phases, and snapshots disposition after failed-attempt
cleanup. Local validation keeps an existing valid session intact without I/O.
The final mutable connection error-message getter/storage is removed. Focused
tests cover ownership, phase classification, rejection/timeout/malformed startup,
cleanup and invalid reconnect alongside existing successful TLS/auth paths.

Continue A5 with type resolution and safe-message/logging. This batch preserves
existing ODBC diagnostics and TLS policy; it does not qualify pooling, close S2
or change crypto qualification blockers. Normalized results and resource budgets
remain planned work before the bounded MySQL proof.

### S2 owning type-resolution errors — 2026-10-01

Type discovery now returns owning map/error results and preserves native query
failure details with ResolveTypes context. Malformed fully drained metadata
reports passive state without inferring transport ambiguity; partial maps never
reach the statement cache. Known/empty types, domain resolution, caller deadlines
and ODBC diagnostics are unchanged. Focused tests cover normal resolution,
invalid rows, all passive states, ownership, server errors and retirement.

Next is the safe-message/logging boundary and normalized deferred-result errors.
A5/S2 remain open; aggregate budgets, safe reuse and MySQL proof remain separate
planned steps, with existing crypto qualification blockers unchanged.

### S2 failure-log summaries and component bounds — 2026-10-01

Connection and direct/prepared failure log messages now use fixed summaries while
ODBC diagnostics retain their original detail. Text/JSON components have input
byte bounds; text control bytes and keys/events are escaped. Tests cover diagnostic
preservation, secret-bearing missing DSN text, hostile controls and long tails.

Continue with typed sensitivity/redaction, safe BackendError summaries and record
budgets before claiming the full logging boundary; deferred-result normalization,
aggregate result budgets and safe reuse remain separate S2 batches. A5 stays open.

### S2 typed log fields and record payload budgets — 2026-10-01

Structured fields now default to sensitive redaction; secrets (including keys)
are dropped, public telemetry is explicit and QueryText keeps its separate opt-in.
Encoded payload/field-count budgets bound escaped output and preserve closing
syntax. Tests cover both formats, Trace/query isolation, excess fields/escaped
expansion and subsequent records. ODBC diagnostics remain protected.

Next: safe BackendError summaries and normalized deferred-result errors. Invalid
source encoding, aggregate result budgets and safe reuse remain explicit work;
this batch does not close A5/S2 or any crypto qualification blocker.

### S2 public backend-error summaries — 2026-10-01

BackendError provides fixed class-based public summaries; typed ODBC failure logs
consume them while diagnostics preserve existing detailed messages/native mapping.
Direct/prepared fake-backend tests prove secret-bearing diagnostics stay out of
Trace logs; all error classes and unknown fallback ignore sensitive fields.

Next is normalized deferred-result errors, then remaining result/resource budgets
and reuse contracts. Invalid-source encoding remains a separate logging item.
A5/S2 and existing provider qualification blockers remain open.

### S2 owning deferred errors — 2026-10-01

Ordered results now carry optional owning BackendError, with PostgreSQL diagnostic
prefix/native state and exchange-end operation/disposition snapshots. MoreResults
preserves diagnostics and pending-result cleanup. Tests cover first/deferred errors,
ODBC text/state, three session states and ownership across later retirement and
backend destruction. Parallel raw result error strings are removed.

Next: remaining result/resource budgets and session/reuse contracts. Native result
metadata normalization and invalid-source encoding remain explicit work. This
batch does not close A5/S2, G12 or crypto qualification follow-ups.

### S2 buffered response wire budgets — 2026-10-01

Query/description exchanges now enforce SDK-configurable cumulative wire-byte and
message ceilings (defaults 64 MiB/100000). Declared oversize payloads are rejected
before body allocation/read; limits return typed ResourceLimit/HY000, retire the
session and expose no partial results. Exact/overflow, all operation paths,
invalid-reconnect and ODBC dead-connection tests cover the new contract.

Continue SEC-4 with rows/cells/columns/results, metadata/diagnostic and startup
budgets plus product configuration and allocation boundaries. These are not exact
heap quotas; session/reset facets and native result normalization remain S2 work.
A5/S2, G12 and provider qualification follow-ups remain open.

### S2 authentication/startup response budgets — 2026-10-01

Authentication/startup responses have independent SDK wire-byte/message ceilings
(defaults 1 MiB/10000). Notices and control frames consume the budget; declared
oversize payloads fail after only the header. Typed failures retain login phase,
cleanup once and retire; invalid reconnect settings preserve the existing session.
Exact/overflow, notice, huge-frame and incremental-read tests cover the scope.

Continue SEC-4 with rows/cells/columns/results and metadata/diagnostic limits,
product configuration and allocation boundaries. TLS-provider handshake and
outgoing-packet budgets are separate; native metadata and reuse facets remain S2
work. No security, SDK or crypto qualification gate is closed by this batch.
