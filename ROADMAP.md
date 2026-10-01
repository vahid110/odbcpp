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

### S2 decoded result count budgets — 2026-10-01

SDK row/cell, per-description column/parameter and result-completion ceilings now
apply before decoded extraction in the PostgreSQL-family session. Counts span
ordered results; overflows retire with ResourceLimit/HY000 and no partial result.
Exact/overflow, two-result aggregation, parameter metadata, null/empty cells,
zero-data and invalid-reconnect tests cover the scope.

Continue SEC-4 with aggregate metadata/name and diagnostic limits, outgoing input,
product configuration and allocation boundaries. Wire/count budgets are not exact
heap quotas; native metadata and session/reuse migration remain S2 work. A5/S2,
G12 and crypto qualification follow-ups stay open.

### S2 metadata/name and diagnostic budgets — 2026-10-01

Row/parameter metadata entries and column-name bytes now accumulate across one
query exchange before decoded extraction. Per-name and aggregate limits reject
oversize metadata; malformed names/fixed fields remain protocol errors. Error
and Notice payload ceilings apply during authentication, startup and queries,
rejecting after the header before reading or allocating the body. All overflows
retire without partial results or server text in the resource diagnostic.

Exact/overflow, multi-result aggregation, parameter entries, empty names,
malformed metadata and startup/query diagnostic tests cover the scope. SEC-4
still needs outgoing input, allocation and product configuration boundaries;
S2 native metadata/session facets and crypto qualification remain open.

### S2 outgoing request/startup input budgets — 2026-10-01

SQL and prepared parameter count/per-value/aggregate input limits now apply
before request encoding; descriptions share the SQL/count checks. Connection
host, user, password, database and CA paths are bounded before startup encoding,
settings replacement or transport setup. Configured ceilings are themselves
bounded for the PostgreSQL-family encoder. Resource-limit diagnostics contain
no input values; preflight failures preserve the current owner's session and
transaction state because no I/O occurred. Response overflows still retire.

Tests cover exact/overflow SQL for all three operations, parameter counts,
null/empty/binary values, aggregate bytes, recovery after rejection, transaction
state, startup fields, invalid reconnect ceilings and zero budgets. SEC-4 remains
open for earlier ODBC conversion, standalone SQL helpers, allocation/encoded
buffer limits and product configuration. S2 native metadata/session facets and
crypto qualification remain open.

### S2 allocation failure boundaries — 2026-10-01

Allocation failures now have a distinct owning backend classification and map
to HY001 in request diagnostics. Request/startup encoder allocation failures
before I/O preserve the current owner's session; response I/O, parsing and
result extraction failures retire without partial results. Startup failures
retain authentication phase and cleanup behavior. Backend allocation errors
use empty detailed text plus a fixed public summary to avoid constructing a
long error string during memory pressure. The C API guard catches bad_alloc
separately and records HY001 when diagnostic storage remains available.

Injected failures cover encoding/recovery, reconnect preservation, startup,
response I/O/parse/extraction, no further sends after retirement, ODBC SQLSTATE
and C API lock recovery. This is exception classification and containment,
not a complete heap quota or proof of recovery under sustained exhaustion.
Earlier ODBC conversions/local catches, encoded buffer budgets and product
configuration remain SEC-4 work; S2 and crypto qualification remain open.

### S2 encoded query exchange ceilings — 2026-10-01

PostgreSQL-family query encoders receive the SDK request-wire ceiling (128 MiB
default, configurable up to 1 GiB). Direct, prepared and description requests
count complete framing, rewritten SQL, type/length fields and binary hex
expansion before allocating outgoing wire/value buffers. Encoder-limit failures
return local ResourceLimit/HY000 without I/O or session retirement. The internal
parser signature changed; PostgreSQL, placeholder MySQL and test adapters compile
against the same contract, with no new MySQL functionality or public SDK claim.

Exact/one-byte-over, NULL/empty/binary values, binary input hints, multi-digit
markers and invalid reconnect ceilings cover the contract. Temporary SQL rewrite
storage remains bounded by input limits, not by the wire ceiling. Startup/auth
encoded limits, earlier ODBC conversion/helpers, exact heap accounting and product
configuration remain SEC-4 work. S2/native metadata/session migration remains open.

### S2 startup/authentication wire ceilings — 2026-10-01

SDK startup and per-authentication-response ceilings default to 1 MiB each and
accept configured values up to 1 GiB. Startup fields and framing are counted
before field copies/wire allocation; an overflow performs no transport I/O or
session mutation. Cleartext/MD5 and SCRAM initial/continuation packets are bounded
before outgoing wire allocation. Authentication overflow retains its login phase,
sends no oversized packet and retires through failed-connect cleanup. Existing
verified-channel policy is unchanged.

Exact/overflow tests cover startup/options, password packet framing, escaped
SCRAM usernames and continuation proofs, zero/no-response authentication,
invalid ceilings and reconnect preservation. SCRAM intermediate crypto/string
allocations and provider TLS handshake buffers remain separate from these caps.
Earlier ODBC conversions/helpers and product configuration remain SEC-4 work;
S2/native metadata/session migration and crypto qualification remain open.

### S2 resource profile configuration — 2026-10-01

Completed SDK limits are exposed as documented Max* driver/DSN/connection-string
options. Parsing rejects signs, suffixes, overflow and invalid numeric bounds
before backend session creation. Provider resolution preserves defaults and
passes the profile to the session; shared validation prevents product/SDK drift.
Existing driver < DSN < connection-string precedence applies. No shared ODBC
backend branching or additional protocol feature is introduced.

Tests cover option propagation, malformed/unsafe values, recovery, defaults,
zero-data profiles and PostgreSQL-provider validation. Advanced Windows GUI
fields are deferred. SEC-4 remains open for earlier ODBC conversion/SQL-helper
allocations, SCRAM intermediates/provider buffers and complete heap accounting;
S2 native metadata/session migration and crypto qualification remain open.

### S2 ODBC SQL capture budgets — 2026-10-01

SQLExecDirect/SQLPrepare ANSI and wide APIs now apply the accepted MaxSqlBytes
before input copies. SQL_NTS scans stop at the configured bound; explicit lengths
are checked before allocation. Wide capture counts actual UTF-8 expansion during
conversion and returns no partial string. Limit failure reports HY000 without
backend work, disconnect or prepared-state mutation; malformed wide input retains
22018 when it fits the bound. The SDK still validates translated native SQL.

Tests cover all four APIs with NTS/explicit lengths, exact/overflow inputs,
multibyte expansion, invalid Unicode, recovery and existing prepared statements,
plus zero/empty conversion budgets. Parameter conversion, connection-string
capture and standalone SQL helpers remain separate SEC-4 boundaries. Exact heap
quotas and S2 native metadata/session migration remain open.

### S2 ODBC bound-parameter budgets — 2026-10-01

ODBC preparation now enforces MaxParameters before prepared-state replacement.
Execution applies per-value and aggregate limits before ANSI/binary copies and
during wide UTF-8 conversion, and rechecks normalized values before backend or
transaction work. NULL/empty distinctions and supported parameter-set error
reporting remain intact. Contract tests cover exact/overflow, NTS/explicit,
Unicode expansion/malformed input, aggregate text/scalars, zero budgets and
recovery. Connection-string capture, standalone helpers, exact heap accounting,
native metadata/session migration and crypto qualification remain open.

### S2 ODBC connection capture budgets — 2026-10-01

Connection API input is bounded before conversion/parsing using a fixed 1 MiB
UTF-8 bootstrap ceiling for SQLDriverConnect input and each SQLConnect field.
ANSI/wide capture cannot raise its own ceiling via configuration; resolved SDK
field limits still apply later. Tests cover exact/overflow, expansion, malformed
Unicode, explicit slices/empty values, untouched output and session recovery.
Configuration-file ingestion, standalone helpers, complete heap accounting,
native metadata/session migration and crypto qualification remain open.

### S2 ODBC SQLNativeSql capture budgets — 2026-10-01

ANSI/wide SQLNativeSql now shares the accepted MaxSqlBytes raw-input ceiling
with execution/preparation capture. Exact/overflow NTS and explicit inputs,
UTF-8 expansion, malformed Unicode, output preservation, recovery and zero-budget
validation precedence are covered by contract tests. No translation or I/O
occurs on capture rejection. Translated-output/internal helper allocations and
configuration-file ingestion remain separate security boundaries; normalized
results/session migration and crypto qualification remain S2 work.

### S2 normalized column metadata — 2026-10-01

PostgreSQL-family results now carry owning normalized column types across primary,
additional and description schemas. ODBC maps these exclusively; native column
ID interpretation no longer occurs there. Missing normalized metadata fails the
contract before result metadata is applied. Tests cover owning snapshots after
disconnect, multi-result/description schemas, independent-backend types and
missing-metadata recovery. Native field removal, parameter description migration,
canonical cells, ordered execution and session facets remain S2 work.

### S2 normalized parameter descriptions — 2026-10-01

Prepared/description results carry owning normalized parameter types, resolved by
the backend using the original deadline. ODBC no longer reads parameter native
IDs, invokes type resolution or stores a native-ID map. Description counts,
application bindings and metadata-error diagnostic behavior are preserved.
Tests cover independent normalized types, owned snapshots, original deadlines,
incomplete/failed resolution, output preservation and same-session recovery.
Native caches are deferred pending explicit epochs/invalidation; fresh domain
resolution may add catalog I/O. Canonical cells, native migration-field removal,
ordered execution and session facets remain S2 work.

### S2 canonical binary/Boolean cells — 2026-10-01

Known binary/Boolean result values are normalized in the backend once; shared
ODBC decoders are removed. Owning cell-error coordinates preserve fetch/GetData
22018 timing, row status and affected outputs without retaining malformed native
bytes. Tests cover wire encodings, NULL/empty, multiple results, immutable snapshots,
malformed coordinate rejection and recovery. Text UTF-8/richer scalar forms,
native migration-field removal and ordered execution/session facets remain open.

The private PostgreSQL codec is shared by the PostgreSQL session and the
lower-level session/parser composition used by provider proofs. Both paths
have valid/malformed wire snapshot tests; the AWS-LC live proof checks canonical
Boolean output and an empty cell-error ledger.

## S2 canonical UTF-8 text cells — 2026-10-01

The PostgreSQL-family session validates Char, VarChar and LongVarChar cells
before returning primary/additional results. Valid UTF-8 bytes are preserved
in place, including embedded NULs; empty and NULL remain distinct. Malformed
text is discarded and represented by the same owning deferred cell-error
coordinates used for binary/Boolean values. Fetch/GetData reports 22018 for
requested cells, preserving affected buffers and indicators for ANSI, wide and
binary targets; unrequested malformed cells do not fail execution or fetch.
Tests cover both session compositions, all three text families, Unicode limits,
overlong/surrogate/out-of-range/truncated encodings, immutable snapshots and
protocol/next-row recovery. Binary bytes bypass UTF-8 validation.

This is text-cell validation, not transcoding or metadata-name normalization.
Numeric/temporal canonical forms, native migration-field removal, ordered
execution and session/reuse facets remain S2 work. Existing wire/count ceilings
apply; no exact heap quota or additional provider qualification is claimed.

## S2 native metadata service boundary — 2026-10-01

Native type interpretation and domain resolution are removed from the required
IDatabaseConnection API. PostgreSQL retains its private parser/resolver hooks
and original deadline/error rules; the ID-keyed resolver map is moved out of
the normalized scalar header. Independent backend contract tests now implement
only normalized metadata services, with compile-time checks proving the SDK
has no native-ID lookup methods. PostgreSQL mapping, domain happy/error cases,
metadata output preservation and recovery remain regression gates.

QueryResult parser migration fields, ordered execution, richer numeric/temporal
canonical forms, metadata-name validation and session/reuse facets remain open.
This is an internal C++ contract change; it changes no external ODBC entry point
or supported PostgreSQL behavior and makes no crypto qualification claim.

## S2 result structure and column-name validation — 2026-10-01

Column names must be valid UTF-8 without embedded NULs; empty names are valid.
Every row must have exactly one cell per schema column. A shared validation
helper checks primary/additional results before publication at the PostgreSQL
backend and before ODBC applies synthetic-backend results. Fully drained invalid
metadata returns an owning InvalidMetadata error and preserves same-owner
protocol reuse. ODBC reports HY000 for structural/name contract failures and
owning metadata errors, including SQLMoreResults, without allowing native
SQLSTATE to override that classification. No partial result schema is exposed.

Tests cover Unicode/empty names through ANSI/wide metadata, malformed UTF-8/NUL
names, short/long/zero-schema rows, invalid additional results, untouched metadata
outputs and direct/prepared/deferred recovery. Native parser migration fields,
ordered execution, richer scalar forms and session/reuse facets remain open;
this batch adds no provider qualification or pooling claim.

## S2 typed server-version service — 2026-10-01

IDatabaseConnection exposes an owning, no-I/O server_version() value instead of
a named native server-parameter map. Empty means unknown/unavailable. PostgreSQL
ParameterStatus keys remain private implementation details, including its
version-dependent type catalog. ODBC and the existing internal synchronized
wrapper consume only the typed version service. Compile-time contract coverage
prevents reintroducing get_parameter on the SDK session interface.

Tests preserve ODBC ANSI/wide SQL_DBMS_VER formatting for normal, vendor-suffixed,
empty and malformed advertised versions without metadata I/O. Factory/session
tests cover pre-open emptiness, negotiated values, unrelated parameter updates,
disconnect clearing and owning snapshots; existing type-catalog and wrapper-read
tests remain green. Factory-based examples use the typed accessor, and the
main CI build now compiles examples to protect their SDK call sites. SDK
examples check owning backend results explicitly; PostgreSQL handshake/query/
prepared/error smoke tests and failed-connect exit behavior pass. This is
not a complete server-identity/cache-epoch facet,
and does not repair or qualify the prototype pool. Session/facet splitting,
ordered execution and native result migration fields remain S2 work.

## S2 native completion/provenance fields removed — 2026-10-01

QueryResult no longer exposes command_tag, and column metadata no longer carries
PostgreSQL table OIDs/attribute numbers. The private parser validates and consumes
those wire fields, classifies an ephemeral completion tag, and returns only
StatementKind and affected-row counts. No ODBC provenance feature used these
fields, so supported ODBC behavior is unchanged; future provenance must use
normalized semantics instead of native identifiers. Compile-time contract checks
prevent these fields from returning to the SDK result structures.

Parser tests retain type-field offset checks with nonzero provenance, completion
kind/count/order checks and owning snapshots; edge cases cover truncated
provenance and missing/trailing completion terminators followed by parser recovery.
Type IDs, widths/modifiers, format codes and parameter IDs still require a private
parser-result split. Ordered execution items, richer scalar forms and session/
reuse facets remain S2 work; no pooling/provider qualification is added.

## S2 private parser-result split — 2026-10-01

SDK `QueryResult` now exposes only normalized column and ordered parameter
metadata. Native type IDs, sizes/modifiers and wire format codes reside in private
`ParsedQueryResult`; PostgreSQL-family parsing and decoding stay below the session
boundary. Each drained response is validated and converted to an owning SDK
snapshot before publication. Additional parser results must be flat and ordered;
nested private results fail atomically as invalid metadata.

Prepared/description parameter resolution uses the original absolute deadline.
Resolver errors preserve their owning session/disposition snapshot, including local
input-limit rejection without I/O; incomplete resolution returns no partial result
and records the drained session state. Direct execution never triggers parameter
resolution, so catalog lookups cannot recurse through this conversion.

Compile-time SDK tests forbid native metadata fields. Parser tests retain native
wire-offset/format coverage; session/ODBC tests cover normalized ownership, primary
and additional results, malformed metadata/cells, binary-format rejection, lookup
failures and reusable-session recovery. Ordered execution items, richer scalar
forms, session/reuse facets and provider qualification remain open S2 work.

Validation for this split: focused parser/backend/session/native-type tests;
complete PostgreSQL, iODBC UTF-16 and UCS-4, ASan/UBSan, Redshift unit/build-contract
and missing-endpoint checks; all examples compile. Focused review verified
resolver-error disposition preservation and allocation cleanup. Windows/live and
packaging validation are required on the pushed commit in CI.

## S2 execution-sequence validation — 2026-10-01

The materialized SDK execution contract now explicitly requires a flat ordered
sequence. A primary operation failure uses BackendResult's error alternative;
additional deferred errors are error-only items and cannot also carry schema,
rows, cell errors, parameter descriptions or completion metadata. This prevents
silent item loss or publication of contradictory success/error results.

PostgreSQL validates the normalized sequence after response drain and before
return. Shared ODBC validates the entire sequence before queuing additional items
or publishing result state. Contract violations return fixed metadata diagnostics;
a protocol-ready session stays available to its current owner. This does not add
replay or pooling guarantees.

Tests cover direct and prepared rejection/recovery for nested sequences, root
errors and every contradictory deferred-item payload, plus native-parser
contradictions with post-drain recovery. Happy-path ordering distinguishes a
nonzero update count, an empty rowset with schema, a zero update count and a
deferred server error. Existing native multi-result tests protect PostgreSQL
ordering. A dedicated ExecutionItem/ExecutionResult representation, warning items,
final execution disposition and separate parameter-description delivery remain
open; this checkpoint enforces the current materialized contract rather than
closing the whole ordered-execution milestone.

Statement description has a separate metadata-only gate: primary errors, additional
items, rows and cell-error ledgers are rejected before parameter/column metadata
is published or cached. SQLDescribeParam/SQLNumResultCols rejection leaves caller
outputs unchanged and a later valid description can retry on the same session.

Validation: focused parser/session/backend tests and complete final-revision
PostgreSQL, iODBC UTF-16/UCS-4, sanitizer, Redshift unit/build-contract and
missing-endpoint gates pass; all examples compile. Focused review covered
pre-publication ordering and description retry/caching. Windows live/packaging
checks run on the pushed batch in CI.

## S2 owning operation session snapshots — 2026-10-01

BackendResult now provides a passive, owning SessionSnapshot for both success and
failure. Successful PostgreSQL query, prepared execution and statement description
outcomes record their final state/disposition after response drain and parameter
resolution. The snapshot belongs to the operation envelope, not individual
rowsets; callers retaining only QueryResult deliberately retain data rather than
the operation status. Deferred errors retain the same final session context.

Failures derive the snapshot from the existing owning BackendError, so later
internal error annotation has one source of truth. Successes that do not report a
snapshot default to Unknown/Retire. Void results support explicit snapshots, but
connect/transaction/health/reset success annotation remains later session-facet
work; no blanket success-status or production pooling claim is made.

Reusable means protocol-ready for the current owner at operation completion. It
is not a health probe, reset completion, permission to pool or a promise that a
later call cannot fail. Transaction/failed-transaction outcomes require reset.
Copy/move, later ambiguous retirement, disconnect and backend destruction cannot
rewrite retained success snapshots. Tests cover all three PostgreSQL states,
prepared/description completion, deferred-error agreement, conservative defaults
and the error record's single source of truth. Dedicated ordered ExecutionItems,
warning delivery and separate parameter descriptions remain open S2 work.

Validation: focused backend/session/parser tests; complete PostgreSQL, iODBC
UTF-16/UCS-4, ASan/UBSan, Redshift unit/build-contract and absent-endpoint gates;
all examples compile. Focused review confirmed lifetime, final-resolution timing
and conservative unstamped void results. Windows live/packaging and provider
proof checks run on the pushed batch in CI.

## S2 connection and transaction success snapshots — 2026-10-01

Successful PostgreSQL connection outcomes now own the final passive startup
state/disposition. Successful transaction and isolation adapters preserve the
underlying command outcome's SessionSnapshot rather than discarding it or
recomputing it from a later mutable session reading. Existing failure records,
operation context and absolute deadlines remain unchanged.

Tests cover BEGIN/COMMIT/ROLLBACK/isolation wire completions, all adapter actions
and isolation levels, conservative Unknown/Retire propagation and reported states
differing from the probe's current state. Connection tests retain snapshots across
copy/move, invalid reconnect rejected before I/O, later ambiguous loss, disconnect
and backend destruction. Invalid reconnect preserves the current owner's state;
it neither changes the earlier connection outcome nor grants a pooling lease.

This supersedes the earlier deferral of connection/transaction success annotation.
Health/reset/lease facets, optional service separation, ordered execution items
and warning delivery remain open S2 work. A completed BEGIN requires reset even
though it succeeded; no automatic replay, health check or production pooling
support is added.

Validation: focused backend/session/parser tests; complete PostgreSQL, iODBC
UTF-16/UCS-4, ASan/UBSan, Redshift unit/build-contract and absent-endpoint gates;
all examples compile. Focused review confirmed startup timing, adapter snapshot
propagation and unchanged failures. Windows live/packaging checks run on the
pushed batch in CI.

## S2 provider-owned SQL dialect service — 2026-10-01

ISqlDialect is a pure provider-owned service for parameter-marker counting and
SQL escape translation. IDatabaseConnection no longer requires either method.
PostgreSQL-family providers expose an immutable service using the existing lexer
and translator; native parser composition helpers remain private implementation
code. SQL dialect rules do not require credentials, a transport or a live session.

ODBC direct/prepared execution and SQLNativeSql A/W now use the selected provider
service. Existing connection, buffer/Unicode, SQL byte-budget, marker-budget,
NoScan and diagnostic guards retain their order and behavior. Translation strings
are owning snapshots; service references last for the provider lifetime.

The independent synthetic backend session compiles without dialect methods; its
provider owns the dialect. Tests cover use before session creation, borrowed
input overwritten after translation, retained results after provider destruction,
error/recovery, PostgreSQL quotes/comments/dollar quotes and stable service
identity across session creation/destruction. Compile-time checks prevent SQL
services returning to the SDK session interface.

This closes the pure SQL-dialect extraction within S2 session/service separation.
Type/catalog and optional transaction/description/reuse facets, internal target
separation, ordered execution representation and warning delivery remain open.
No MySQL protocol or speculative Redshift specialization is introduced.

Validation: focused backend/provider/session/parser tests; complete PostgreSQL,
iODBC UTF-16/UCS-4, ASan/UBSan, Redshift unit/build-contract and absent-endpoint
gates; all examples compile. Focused review checked provider lifetime, owning
translations, unchanged ODBC guards and all callers. Windows live/packaging and
provider-proof validation run on the pushed batch in CI.

## S2 optional transaction-session facet — 2026-10-01

Transaction control and isolation now live on the optional ITransactionSession
facet. IDatabaseConnection exposes stable borrowed facet discovery, defaulting to
absence, and no longer requires transaction/capability/isolation methods.
PostgreSQL implements the facet; the generic PostgreSQL-family machinery and
independent synthetic backend no longer need unsupported transaction stubs.

Shared ODBC obtains transaction capabilities from a live facet when connected.
Before connection, or after an unsuccessful open has disconnected its session,
the immutable provider declaration supports attribute planning. A provider
advertisement cannot grant missing live behavior: guarded adapter dispatch
returns an owning unsupported error without I/O. A failed deferred-isolation
open can correct its settings and retry. Existing PostgreSQL command/deadline,
error annotation and success snapshots are preserved.

Tests cover absent facet discovery, stable PostgreSQL facet lifetime across
open/disconnect, closed-session failures, live capability suppression despite
provider overadvertisement, no-I/O autocommit/isolation rejection, safe command
recovery and failed-open setting correction/retry. Compile-time checks keep
transaction operations off the required SDK session interface. Existing native
transaction and ODBC suites protect happy-path and failure behavior.

This extracts transaction behavior only. Statement-description/catalog/reuse
facets, static type services, internal build targets and the ordered execution
representation remain S2 work. No reset/health/pooling guarantee is added.

Validation: focused provider/backend/session/parser tests; complete PostgreSQL,
iODBC UTF-16/UCS-4, ASan/UBSan, Redshift unit/build-contract and absent-endpoint
gates; all examples compile. Focused review verified borrowed facet lifetime,
null dispatch, capability selection and failed-open retry. Windows live/packaging
and provider-proof validation run on the pushed batch in CI.

## S2 optional statement-description facet — 2026-10-01

Pre-execution statement description now lives on IStatementDescription, a
session-owned optional facet discovered through IDatabaseConnection. The required
session interface no longer needs a description stub. The borrowed facet remains
stable through open/disconnect for its session lifetime; calls retain the original
absolute deadline, borrow inputs only until return and return owning normalized
metadata and passive session snapshots. Existing metadata-only shape validation
remains mandatory.

The PostgreSQL-family session implements the facet using its existing description
exchange and type resolution. Shared ODBC checks facet presence before preparing
metadata inputs or dispatching I/O. Absence returns HYC00 without changing outputs,
marking metadata cached or disconnecting. Live SQL_DESCRIBE_PARAMETER cannot
advertise support without the facet; execution metadata remains available after
normal execution even when pre-execution description is absent.

Tests cover repeated absent-facet rejection with unchanged parameter/column
outputs and no I/O, capability suppression, prepared/direct execution recovery,
and stable facet lifetime before open and after disconnect. Successful description
retains the original deadline and an owning metadata/session snapshot that survives
session destruction; closed-session failures retain operation and retirement state.
Existing malformed metadata, allocation/input limits, native description and ODBC
suites continue to protect PostgreSQL behavior.

This closes optional statement-description extraction only. Catalog/reuse facets,
static type services, internal build targets, dedicated ordered execution items and
warning delivery remain open S2 work. No pooling, MySQL or provider qualification
claim is added.

Validation: full local PostgreSQL, iODBC UTF-16 and UCS-4, sanitizer and Redshift
build/absent-endpoint gates pass; examples compile. Focused read-only review found
no blockers in facet ownership, adapter dispatch, capability suppression or
metadata-cache publication. Windows/live/package evidence is supplied by the
exact-head GitHub Actions run after this batch push.

## S2 optional catalog-query facet — 2026-10-01

Catalog SQL construction now lives on the optional ICatalogQueries facet.
IDatabaseConnection supplies passive const discovery defaulting to absence;
GenericDatabaseConnection and the independent synthetic backend no longer require
unsupported catalog stubs. PostgreSQL supplies a stable session-owned facet using
its existing query builders. Construction performs no I/O or mutation, borrows
requests only until return and returns owning SQL. The facet remains valid through
open/disconnect for its session lifetime; presence does not promise every request
is supported. Execution still uses the normal deadline, diagnostics and normalized
result-validation path.

Shared ODBC rejects missing catalog discovery with HYC00 before modifying the
statement cursor or dispatching execution. SQLGetFunctions suppresses all eight
catalog functions when the live backend lacks the facet, consistently in scalar,
ODBC 2 array and ODBC 3 bitmap formats. General namespace and SQL-language capability
flags retain their meanings; they do not independently grant catalog discovery.
SQLGetTypeInfo still uses the separate advertised type catalog.

Tests cover all eight ANSI and wide catalog entry points with absent support,
no extra I/O or disconnect, preservation of the existing cursor and subsequent
prepared execution. They check all function-support formats and continued direct
execution support. PostgreSQL facet tests verify stable discovery before open,
while connected and after disconnect; owned SQL survives filter mutation and
session destruction. Existing catalog-pattern/quoting and live catalog suites
protect the supported PostgreSQL behavior.

This closes optional catalog-query extraction only. Static type services,
health/reset/lease facets, internal build targets, dedicated ordered execution
items and warning delivery remain open S2 work. No pooling or provider qualification
claim is added.

Validation: focused backend/catalog/capability tests and full local PostgreSQL,
iODBC UTF-16/UCS-4, sanitizer and Redshift build/absent-endpoint gates pass;
examples compile. Focused read-only review found no blockers in pure construction,
facet lifetime, cursor preservation or all three support-reporting formats.
Windows/live/package validation follows the exact-head GitHub Actions batch run.

## S2 provider-owned advertised type policy — 2026-10-01

Advertised normalized type definitions now belong exclusively to the immutable
provider. IDatabaseConnection no longer requires type_catalog; the PostgreSQL-family
session and independent synthetic backend no longer duplicate that service.
IBackendProvider::type_catalog accepts an explicit advertised server-version view,
retained only until return, and selects immutable definitions whose spans and strings
remain valid for the provider lifetime across later profile selections. An empty
version selects the conservative profile. Native live type resolution remains
private backend behavior and is not moved onto the provider.

Shared ODBC selects types through its provider using the session's owning
server_version snapshot when available. PostgreSQL's existing version policy is
unchanged: numeric/decimal negative scales are advertised from major version 15,
while missing, unparseable or overflowing versions select the conservative range.
Normal PostgreSQL disconnect clears its advertisement; ambiguous retirement can
retain the last known version. Policy selection follows the advertised value, never
infers session reuse or substitutes a health probe.

Tests verify default/modern/legacy/overflow profiles, immutable prior views across
version selections and overwritten borrowed version inputs. The synthetic provider
owns its type definitions without a session method; ODBC SQLGetTypeInfo uses its
version-selected column size without catalog/query I/O, preserves earlier views,
and falls back when the advertised version becomes empty. Existing PostgreSQL
metadata and descriptor/type-info suites protect public behavior.

This closes advertised-type service extraction. Static capability/error policy,
health/reset/lease facets, internal build targets, dedicated ordered execution
items and warning delivery remain open S2 work. This is an advertised-type policy,
not a new canonical scalar/Arrow representation or an ADBC readiness claim.

Validation: focused backend/provider/type/metadata/capability tests and full local
PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and Redshift build/absent-endpoint gates
pass; examples compile. Focused read-only review found no blockers in immutable
view lifetime, borrowed inputs, advertised-version lifecycle or adapter selection.
Windows/live/package evidence follows the exact-head GitHub Actions batch run.

## S2 provider-owned capability and diagnostic policy — 2026-10-01

Static capability declarations and native-state diagnostic mapping now belong to
IBackendProvider. IDatabaseConnection no longer requires either policy method;
GenericDatabaseConnection and independent synthetic sessions need no unsupported
policy stubs. PostgreSQL's session no longer accepts or stores a product display
name. The provider owns product identity and the capability strings remain valid
for its lifetime. Sessions can outlive their creating provider without borrowing
its static policy. ODBC retains its provider and derives live capability reporting
by masking a copy for absent facets, never mutating the declaration.

Pure native-state mapping receives borrowed native text and semantic ErrorContext,
returns owning normalized SQLSTATE text or no mapping for the caller's fallback,
and performs no I/O. The default provider policy supplies no mapping. PostgreSQL's
existing context-sensitive duplicate object/index and conversion/integrity mappings
are unchanged. ODBC validates mapped states and preserves class-first invalid
metadata, ResolveTypes and operation-specific fallback precedence. Diagnostic policy
never overrides the owning operation's session state, retirement or retry rules.

Tests cover provider identity ownership despite caller input mutation, independent
session use after provider destruction, stable static capability snapshots across
open/disconnect and absent-facet masking, and provider error policy before session
creation with owning results after provider destruction. Existing contextual native
state mappings, malformed/unknown state fallback, direct/prepared/deferred errors,
metadata failures and passive session-snapshot tests protect both happy-path and
failure behavior. Compile-time checks keep both policy methods off the required
session interface.

This closes the remaining static capability/error service extraction in S2.
Health/reset/lease facets, internal build targets, dedicated ordered execution
items, warning delivery and richer normalized values remain open. No pooling,
public SDK qualification or ADBC readiness claim is added.

Validation: focused provider/backend/capability/native/diagnostic/liveness checks
and full local PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and Redshift
build/absent-endpoint gates pass; examples compile. Focused read-only review found
no blockers in provider ownership, live masking, mapped-state validation or
fallback precedence. Windows/live/package validation follows the exact-head
GitHub Actions batch run.

## S2 isolated internal SDK contract target — 2026-10-01

The internal `odbcpp::sdk_contracts` C++20 interface target exposes an explicit
20-header manifest through a freshly generated allowlisted include tree. It carries
no ODBC, PostgreSQL, crypto-provider or other external include/link dependencies.
`cmake -S sdk -B <build>` configures this surface independently of driver dependency
discovery. Every header compiles as the first and only include in a separate unit;
a minimal independent session consumer verifies closed-session failure, successful
execution and owning results/snapshots after input mutation and disconnect.

Configuration and unit gates enforce the manifest's transitive closure, reject
private/unlisted includes, external headers, unverifiable macro includes, path
escapes, duplicate/missing entries and known backend-selection/ODBC/crypto types.
Negative fixtures check actionable rejection diagnostics, while ordinary TLS/SSL
text in diagnostics remains permitted. These lexical guards complement compilation
and review; they are not a complete semantic security or dependency audit.

The root build includes the same checks on its supported platforms. This is an
internal build-only contract boundary: no installation/export, stable public ABI,
complete runtime target split or S4 clean-room author qualification is claimed.
Runtime implementation isolation, health/reset/exclusive leases, dedicated ordered
execution items, warning delivery and richer normalized values remain open S2 work.


Validation: standalone individual-header compilation and all three contract tests,
focused root provider/backend/architecture tests, full PostgreSQL, iODBC UTF-16
and UCS-4, sanitizer and Redshift build/absent-endpoint gates pass; examples compile.
Focused read-only review found no blockers. Windows/live/package and cross-platform
validation follows the exact-head GitHub Actions batch run.

## S2 internal runtime target ownership — 2026-10-01

Production compilation now uses separate internal OBJECT targets for runtime
primitives, backend implementation, product/legacy composition and the ODBC adapter.
GenericDatabaseConnection stays in the PostgreSQL-family backend partition; the
prototype pool stays with composition because it calls DatabaseFactory. No pool
qualification or backend-independent session claim follows from target names.

Core/test and production-driver objects compile separately. Test hooks are enabled
only on the core ODBC objects; backend-selection macros belong only to composition.
C++20, compiler warnings, PIC, SQLWCHAR ABI checks and private dependency includes
are applied where compilation actually occurs. Objects are flattened into the same
static archive and shared driver, preserving final exports, OpenSSL linkage,
Windows system libraries, static-crypto link maps and installed-target interfaces.
The isolated AWS-LC proof retains its existing source collector and build profile.

Source-partition fixtures verify PostgreSQL-family, prototype pool and Windows
ownership, normalize repeated inventory entries, and reject unknown sources,
resource entries, path escapes and empty partitions. Fixtures run under a build
path containing spaces. Existing behavior, test-hook, export, package and crypto
gates protect the resulting artifacts.

This establishes internal build ownership, not complete compile-time dependency
isolation: private include restrictions, installed protocol-header exposure and
legacy public wrapper includes remain step 7 work. Health/reset/exclusive leases,
ordered execution items, warning delivery and public SDK qualification remain open.


Validation: focused partition/SDK/provider/backend tests, full local PostgreSQL,
iODBC UTF-16/UCS-4, sanitizer and Redshift build/absent-endpoint gates pass;
examples compile. Read-only review found a test-fixture quoting issue, repaired
and verified with spaces in the fixture path; no production blockers were found.
Windows/live/package and cross-platform validation remains the exact-head CI gate.

## S2 per-target private include closure — 2026-10-01

Each root-build runtime, backend, composition and ODBC object target now compiles
against its own explicit staged header manifest, without the full repository root
on its include path. Runtime cannot include database sessions; backend cannot
include ODBC; composition sees provider/factory/pool integration but not parser
machinery; ODBC sees contracts and its needed runtime helpers but no concrete
provider, protocol parser or prototype pool. An unused pool include was removed
from the ODBC handle header.

Configure-time checks enforce transitive quoted-include closure for every staged
header and component source. They reject unlisted headers, escapes, macro includes,
project angle-include bypasses including case/backslash/dot variants, duplicates,
missing entries and source-root escapes through header symlinks. Fresh generated
trees remove stale entries. Source edits trigger automatic reconfiguration, with
an incremental-build regression fixture covering source-local include bypasses. Compiler probes run inside the parent configuration,
using its generator/toolchain/platform, and verify allowed headers compile while
forbidden cross-component headers do not. Negative fixtures also run under paths
containing spaces; the symlink fixture runs where link creation is supported.

These are source dependency gates, not a compiler/filesystem security sandbox.
Legacy core/test include interfaces and installed header layout remain unchanged;
installed protocol-header exposure and public SDK packaging are still open work.
The separate AWS-LC qualification build retains its existing bounded profile.
Health/reset/exclusive leases, ordered execution items and warning delivery remain
open S2 contracts.

Validation: full local PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and Redshift
build/absent-endpoint gates pass; examples compile. The final incremental closure
fixture passes, including repeated symlink checks. Fifteen crypto evidence tests
pass. Independent read-only reviews found no remaining material blockers after
closing the incremental-source bypass. Windows/live/package and cross-platform
results remain subject to the exact-head CI gate.

## S2 active session health checkpoint — 2026-10-01

Added an optional, session-owned, deadline-bound active health facet, with one
PostgreSQL probe and owning outcomes. A backend without the facet remains valid.
Probes preserve transaction state and failure details, never reset/reconnect or
replay, and retire malformed response/ambiguous transport outcomes. Health success
is distinct from reset and pool eligibility. ODBC passive liveness is unchanged.

Focused contract tests cover deadline forwarding, owning snapshots, malformed
success retirement, error context/details and absent facets. Real PostgreSQL
checks cover idle/open/failed transactions, rollback recovery, deadline retirement
and subsequent disconnected probing. Native injected timeout, partial write and
malformed protocol paths retain retirement. Full regression and hosted gates are
required before accepting this batch. No MySQL/Redshift live qualification or
pool support is claimed; reset/exclusive leases and remaining S2 contracts stay open.

Validation: focused contract/liveness/SDK tests and real PostgreSQL health probes
pass. Complete local PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and Redshift
build/absent-endpoint gates pass; examples compile. Read-only review confirmed
outcome behavior; caller serialization and recovery/deadline assertions were
clarified. Windows live/package and the cross-platform crypto matrix remain
subject to the exact-head hosted gate.

## S2 prototype quarantine and serialized exchanges — 2026-10-01

Quarantined the legacy pool into a private test/example-only target. Production
core/driver sources, object partitions, composition headers and installed headers/
examples exclude it. Existing pool fixtures still run. Inventory/partition/target
negative cases, actual private-prefix installation and prototype-controlled
artifact inspection guard the boundary. The legacy wrapper serializes direct and
prepared protocol exchanges; all four pairings have a coordinated overlap test.

This removes a production exposure, not the need for the new reuse design.
Move-only leases, reset, credential/cache isolation and pooling acceptance remain
open. PostgreSQL ODBC behavior and provider/linkage claims are unchanged.

Validation: focused serialization, partition, installation and positive-control
artifact checks pass. Full local PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and
Redshift build/absent-endpoint gates pass; examples compile. The failed initial
quarantine fixture used CMake's reserved `clean` name; repaired before the final
gates. Read-only review closed partition and driver-only object injection bypasses;
no remaining blocker/high finding. Windows packaging and cross-platform crypto
profiles remain subject to the exact-head CI gate.

## S2 explicit PostgreSQL reset profile — 2026-10-01

Added optional reset contracts and explicit PostgreSQL product opt-in. The backend
rolls back active/failed transactions and discards server session resources under
the original deadline; exact native completion tags remain private. Failure
retires without reconnect or replay. Redshift remains opted out.

Focused fixtures cover cleanup ordering/deadlines, malformed responses, transport
and server failures, allocation failures and stable owning outcomes. Mandatory
PostgreSQL live tests cover baseline/resource cleanup, failed-transaction recovery
and timeout retirement. Full local and exact-head CI evidence is recorded after
the batch gates. Exclusive leases and credential/cache isolation remain next S2
work; S2, S2C, G12 and pooling qualification stay open.

Validation: focused reset/provider/wire and SDK contract checks pass. Full local
PostgreSQL, iODBC UTF-16/UCS-4, sanitizer and Redshift build/absent-endpoint
gates pass. PostgreSQL and UTF-16 were repeated after the final malformed-empty
completion fix. Read-only review found no remaining blocker/high issue. Windows,
packaging and cross-platform crypto checks remain subject to exact-head CI.
