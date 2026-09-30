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

The scheduler remains paused. This plan does not resume it or authorize cloud
resource creation. On an explicit resume, its saved instructions should be
updated to select work by these gates and stop at the agreed milestone.

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

Next, audit the OpenSSL rows against the existing finite qualification contract,
implement demonstrated gaps, and record matrix acceptance. Main-build AWS-LC
integration is still separate and disabled. Then continue the accepted S2
error/lifecycle, results and session/facet migration before S3 MySQL. G12 and
PostgreSQL G8 remain open; public SDK/FIPS scope stays deferred.
