# Connectivity SDK product plan

Decision date: 2026-09-30.
Status: S1 accepted; bounded S2 noncrypto migration implementation-complete,
with acceptance conditional on the protected gates for the
[S2 stopping review](S2_MIGRATION_REVIEW.md). S2C/G12 and S4 remain open; S3 has begun with private connection-phase codecs.

Future Redshift parity requirements are tracked in
[REDSHIFT_FEATURE_PLAN.md](REDSHIFT_FEATURE_PLAN.md). They preserve independent
backend/auth/catalog extensions and shared ODBC contracts, without adding
Redshift feature work to S2/S2C or the bounded MySQL proof. RP1/G14 scope and
advanced S3 transfer are estimated separately at their Redshift checkpoints.

## Product intent

ODBCPP will be developed as a database-connectivity SDK, with ODBC as its first
mature client API. PostgreSQL is the first production reference backend. A
bounded MySQL 8 implementation will prove that the SDK boundary works for an
unrelated wire protocol. Redshift remains the next product backend and will
prove specialization within the PostgreSQL protocol family when endpoint access
is restored.

ADBC is a planned second client API. This milestone must avoid architecture
that prevents Arrow columnar batches, but it does not add an Arrow dependency,
implement the ADBC ABI, or claim ADBC compatibility.

The product layers are:

1. **Client API adapters:** ODBC now; ADBC later.
2. **Shared connectivity SDK:** session and statement orchestration, ownership,
   deadlines, cancellation contract, normalized metadata and values,
   diagnostics, capabilities, and reusable contract tests.
3. **Backend implementations:** PostgreSQL and MySQL as sibling reference
   backends. Redshift specializes demonstrated PostgreSQL-family components
   without placing Redshift branches in shared client-API code.
4. **Transport and security primitives:** sockets, TLS, timeouts and logging,
   reused only where their contracts agree.

The shared layer is not an ODBC wrapper around a PostgreSQL implementation.
ODBC handles and constants stay in the ODBC adapter; native packet formats,
authentication exchanges, type identifiers and catalog SQL stay in backends.

## MS1: SDK foundation and MySQL proof

This is a bounded architecture-validation milestone, not a MySQL beta. It may
proceed while the Redshift pilot is blocked on user account access. PostgreSQL
regression gates remain mandatory. Redshift resumes at M2 as soon as usable
access returns; MS1 does not silently replace or waive Redshift acceptance.

| Package | Scope | Base estimate | Exit evidence |
|---|---|---:|---|
| S1 | Freeze SDK boundary, dependency direction, ownership/error/deadline rules, pooling/reuse, cache and security lifecycle contracts; inventory current PostgreSQL dependencies; pin the MySQL 8 test version and initial `caching_sha2_password`-over-TLS profile | 2–4 days | [Architecture](SDK_ARCHITECTURE.md) and [security](SECURITY_MODEL.md) baselines accepted after four read-only audits; no runtime behavior change |
| S2 | Evolve the smallest necessary non-cryptography interfaces and reusable conformance harness; keep PostgreSQL behavior and ABI-facing ODBC paths green | 4–6 days | PostgreSQL plus synthetic-backend contract tests pass; no PostgreSQL/native ODBC concepts cross the documented boundary; crypto-provider isolation is estimated only in S2C |
| S2C | Make the cryptography implementation and shared/static dependency linkage explicit build profiles; isolate provider APIs; preserve OpenSSL and qualify a bounded AWS-LC Linux proof | 5–9 days | Requested provider/linkage is verified from artifacts; common TLS/auth tests pass; no provider types leak through SDK contracts; manifest and packaging match the binary |
| S3 | MySQL 8 protocol vertical slice: verified TLS, one password method appropriate to the test server, connect/disconnect, direct and prepared execution, scalar/NULL fetch, parameters, transactions, essential table/column metadata, server errors and recovery | 10–16 days | Unit edge cases and live container tests pass through the same shared ODBC orchestration used by PostgreSQL |
| S4 | Backend author test kit, MySQL/PostgreSQL comparison review, extension guide and one minimal out-of-tree sample backend | 4–6 days | The clean-room backend-author exercise below passes; limitations and unstable interfaces are explicit |

Total: **25–41 base engineering days**, plus **8–13 days of 30% contingency**,
for a buffered working range of **33–54 engineering days**. These are focused
engineering days, not elapsed calendar promises. Review the estimate after the
first S2C OpenSSL artifact proof and after the first live MySQL handshake; do
not consume contingency on added features.

### Required proof

- PostgreSQL and MySQL are selected through the same backend contract and use
  the same ODBC handle, descriptor, diagnostic and conversion orchestration.
- A live MySQL 8 container gate covers successful TLS connection, direct query,
  prepared parameters, NULL and representative scalar results, commit/rollback,
  essential metadata, invalid credentials, invalid SQL, truncation/range
  behavior, recovery and clean disconnect.
- Parser tests cover fragmented/coalesced packets, malformed lengths, sequence
  errors, unknown capability/type values and bounded failure without crashes or
  hangs.
- PostgreSQL, iODBC UTF-16/UCS-4, Windows and sanitizer gates remain green for
  every implementation batch. MySQL gains focused unit and live Linux gates
  before its proof is accepted.
- Backend capabilities are explicit. Unsupported behavior fails predictably and
  is not advertised.
- The result contract remains representation-neutral: the current row path is
  supported, and a future columnar-batch path can be added without exposing
  ODBC types to backend implementations.
- Shared contract tests prove that a session can be reused only after required
  transaction/session cleanup and a successful backend health/reset decision;
  dead, timed-out or credential-expired sessions are retired.
- Existing metadata caches preserve correctness across reconnect, reprepare and
  server-identity changes. Caching must never change diagnostics, transaction
  visibility or advertised capabilities.
- The security invariants and G12 evidence in
  [SECURITY_MODEL.md](SECURITY_MODEL.md) pass. In particular, credential
  authentication cannot disclose a password over an unverified channel, server
  input is resource-bounded and logical borrowers are isolated.
- The cryptography provider/linkage profiles and qualification evidence in
  [CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md) pass. Provider and linkage
  are build choices, while DSNs remain provider-neutral runtime policy.

FIPS readiness is a lightweight architecture-review criterion within S2/S2C,
as defined in [CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md). FIPS
implementation and profile qualification are deferred until concrete demand or
explicit reprioritization; they add no work package, delivery promise or budget
to this milestone. The existing PostgreSQL, SDK/MySQL and Redshift sequence
remains unchanged.

## Pooling and caching boundary

Connection reuse is a core correctness concern even when the ODBC Driver Manager
owns the pool. S1 must document the backend hooks and shared decisions for health
checking, reset, reuse, retirement and credential expiry. G12 must exercise safe
reuse and rejection/retirement paths for PostgreSQL and MySQL, including open or
failed transactions, server-side session state, disconnects, timeouts and invalid
or expired credentials. The exact Driver Manager attributes and claimed pooling
modes are frozen from specification and platform evidence during S1; unverified
modes remain unadvertised.

An SDK-managed pool is optional and initially internal. Its policy belongs in a
separate shared component; backends report health/reset outcomes and must not
embed independent pool implementations. Public pool APIs, sizing defaults and
advanced policies require later measured product scope.

Every cache must declare:

- owner and scope: process, pool, physical connection, logical connection or
  statement;
- key inputs, size bound and concurrency rules;
- invalidation on reconnect, server identity/version change, transaction or
  session reset, schema-affecting events and credential expiry where applicable;
- whether entries contain credentials or other sensitive values, with no
  plaintext-secret persistence and no secret-bearing logs;
- observable hit, miss, eviction, invalidation and stale-entry rejection events
  without exposing user data.

Metadata/type and prepared-statement caches are permitted only behind these
contracts. Existing caches receive correctness tests in G12. New prepared-cache
behavior, eviction tuning, pool sizing and transport optimizations require G10
measurements before enablement or support claims. General query-result caching is
outside the SDK: its consistency and invalidation policy belong to applications
or a separately scoped data service.

## Engineering quality bar

G12 is intended to establish an SDK that an independent C++ engineer can
understand, extend and diagnose without learning PostgreSQL internals. Passing
functional driver tests is necessary but insufficient. The following qualities
are release conditions:

- The architecture has a concise dependency model with one-way flow from client
  adapters to SDK contracts to backend implementations and transport/security
  primitives. Dependency cycles and backend-to-ODBC dependencies are rejected.
- Each public-to-backend contract has one coherent responsibility. Large
  interfaces must be split by demonstrated capability or justified in the S1
  review; optional behavior uses explicit capabilities rather than empty stubs,
  downcasts or backend switches.
- Ownership, borrowing, lifetime, thread-safety, cancellation, deadlines, error
  translation, connection retirement and recovery are documented for every
  extension-facing operation.
- Pooling and cache contracts make physical versus logical connection lifetime,
  reset responsibility, cache scope and invalidation explicit. Hidden global
  caches and backend-owned pool policies are rejected.
- Adding a backend does not require editing shared ODBC workflows. Backend
  selection and build registration may use a documented product-registration
  point; protocol, type, authentication and catalog behavior must not require
  scattered core changes.
- Backend contract tests report the violated operation and expected lifecycle or
  capability rule. A generic crash, timeout or opaque assertion is not adequate
  author feedback.
- The examples use the same supported interfaces as real backends and expose
  essential complexity. They must not depend on test-only friends, private
  headers, repository-relative source paths or undocumented initialization.
- Duplication between PostgreSQL and MySQL is reviewed explicitly. Shared code
  is extracted only when semantics agree; coincidentally similar protocol logic
  remains backend-owned.
- Architectural claims are backed by automated forbidden-dependency checks,
  common contract tests and the clean-room exercise. Narrative documentation
  alone cannot close the gate.

### S1 architecture-quality review

Before S2 implementation, review the proposed dependency diagram and each
extension-facing interface from the perspective of an SDK consumer. The review
must record:

1. responsibility and dependency direction;
2. ownership, concurrency, deadline and error semantics;
3. physical/logical connection reuse, reset, retirement and credential-expiry
   behavior plus every cache's scope and invalidation contract;
4. required versus optional capabilities;
5. evidence from PostgreSQL and the anticipated MySQL use;
6. how row results work now and where a future columnar result path attaches;
7. rejected alternatives and remaining unstable decisions.

Reject an interface that merely renames PostgreSQL concepts, combines unrelated
responsibilities, requires backend-specific branching in shared code, or exists
only for hypothetical future use. Record the accepted design in a concise SDK
architecture document and architecture decision records where a tradeoff is not
obvious.

The 2026-09-30 review is recorded in
[SDK_ARCHITECTURE.md](SDK_ARCHITECTURE.md). Independent read-only audits covered
dependency direction, MySQL/external-author usability, pooling/cache lifecycle
and security. The accepted design reclassifies the current generic session as
PostgreSQL-family internals, defines an independent MySQL session, replaces the
prototype pool contract and makes security/resource limits part of the SDK.

### S4 clean-room backend-author exercise

Use a fresh external build directory and only the installed or staged SDK
headers, libraries, extension guide, sample and backend test kit. The exercise
must not rely on reading PostgreSQL/MySQL implementation sources or receiving
undocumented instructions from their authors. Prefer a reviewer who did not
author the relevant interface; otherwise run the exercise in a separate clean
checkout and record that limitation.

The participant must be able to register a minimal synthetic backend, connect,
execute, return metadata plus NULL/scalar rows, report a server error, declare an
unsupported capability and cleanly disconnect. It must build and pass contract
tests without editing shared ODBC wrappers or private SDK files. Record every
documentation gap, surprising dependency, required workaround and test message.
Unresolved reliance on internal knowledge blocks G12.

The final G12 evidence includes the dependency diagram, accepted interface
inventory, forbidden-dependency results, contract-test report, clean-room log,
core files changed by the sample, and a short friction review. The expected
number of shared ODBC workflow files changed by the sample is **zero**.

### Explicit non-goals

- A production or generally available MySQL driver.
- MariaDB compatibility claims; add a separate compatibility matrix later.
- Every MySQL authentication plugin, compression mode, replication command,
  administrative API, native type or server version.
- Runtime-loadable plugins, a frozen C++ ABI, semantic-version compatibility
  guarantees, or broad source compatibility for third parties.
- Arrow integration, an ADBC driver, Flight SQL, or ADBC packaging.
- General query-result caching, a public pooling API, or unmeasured cache/pool
  performance tuning.
- SQL Server/TDS, Athena, Aurora or another backend.
- Deferring a serious PostgreSQL regression or a supported-path safety defect
  in order to complete the proof.

## Gates and stop rules

G12 closes when S1–S4, the required proof and the engineering quality bar above
pass. It establishes a credible internal SDK and a real unrelated-protocol
reference backend. Functional success cannot waive a failed architecture review
or clean-room exercise. G12 does not establish a public stable SDK or MySQL beta.

G13 is the later public SDK preview: versioned installable SDK artifacts, API
stability policy, compatibility testing, complete reference documentation,
licensing, support lifecycle and an externally consumable sample. Commercial
availability and stable ABI promises require a separate product decision.

Each implementation batch must close one named S1–S4 contract gap and include
happy-path and relevant failure evidence. Initial investigation is limited by
the budgets in `RELEASE_PLAN.md`. Optional protocol breadth goes to a TODO with
a trigger. At S3 completion, stop MySQL feature work and perform the SDK boundary
review before considering any MySQL beta plan.

## Sequencing with existing releases

- PostgreSQL W1–W4 and G9a are complete. PG-BETA remains formally open because
  G8 real-application acceptance and final release evidence are outstanding.
- SQL Server/OPENQUERY, Power BI Desktop and Excel acceptance remain recorded;
  the user has deferred those host-dependent runs while other work proceeds.
- The Redshift pilot implementation and non-live CI gate are ready. Live M2 is
  waiting on restored AWS/Redshift access and cannot be simulated by PostgreSQL.
- MS1 remains active. S1 is complete. The bounded noncrypto S2 migration is
  implementation-complete, conditionally accepted only when the protected gates
  for [its stopping review](S2_MIGRATION_REVIEW.md) pass. Next is S3 preparation
  and the smallest MySQL TLS/auth handshake batch. Cache/reuse comparison and
  security evidence remain in G12; S4 author usability and S2C provider
  qualification remain separate and are not waived.
- When Redshift access returns, finish the current coherent MS1 batch, preserve
  its evidence, and run M2 before expanding MySQL scope. Re-estimate whether to
  finish the remaining MS1 packages or proceed directly to Redshift M3 based on
  the observed SDK boundary and product priority.

## Product validation after the technical proof

After G12, validate demand before funding a broad SDK surface. Seek design
partners among database and analytics vendors and test the value of commercial
SDK licensing, OEM distribution, paid driver development, compatibility
certification and enterprise support. Product discovery does not change the
technical release claims or authorize public distribution.

## S2 reset progress — 2026-10-01

The backend server-cleanup primitive now has an optional explicit profile and
PostgreSQL evidence. `SameAuthenticatedServerSession` makes no credential
freshness or new-borrower authorization promise. Shared policy must still provide
exclusive leases, credential generations/expiry and cache invalidation before
reuse acceptance. Redshift is opted out. This advances S2 lifecycle contracts
without closing S2/G12 or starting MySQL implementation.

## S2 exclusive ownership progress — 2026-10-01

The first internal RAII lease primitive now proves exclusive borrowing and safe
owner/lease lifetime. All returns retire, including after successful backend
reset. Public SDK exposure and reusable return remain deferred until shared
credential generation/expiry and cache epoch/invalidation policy have evidence.
No pool sizing, waiting, throughput claim or Driver Manager behavior is added.

## S2 credential admission progress — 2026-10-01

Opaque private credential authorities and optional exact-generation admission
now cover rotation, revocation, monotonic expiry and authority lifetime. Missing
or mismatched tokens cannot disrupt another valid owner; every return still
retires. Trusted composition must mint and bind authority only after validated
authentication for one complete immutable security context. Real credential
provider integration remains open.

Next is coordinator-observable reset/cache invalidation, before introducing cache
tokens and reusable return. A direct backend reset cannot currently notify the
owner, so asserting automatic cache invalidation now would overstate evidence.
No new cache, pool tuning, public API or Driver Manager behavior is introduced.

## S2 coordinator cleanup progress — 2026-10-01

Private lease cleanup now validates reset profile, original deadline, exact
success snapshot and passive state, and retires on all failures/exceptions or
late completion. Success preserves only the current exclusive borrower; all
returns still retire. Credential revocation/owner destruction during cleanup do
not interrupt that borrower. PostgreSQL live evidence includes successful cleanup
and physical retirement after an expired cleanup deadline.

Next: guard/route raw borrower reset before cache scope/invalidation and reusable
return. No cache, pool, credential refresh or Driver Manager qualification is
added, and S2/G12 remain open.

## S2 borrower facade progress — 2026-10-02

Removed the raw lease session accessor. Direct/prepared execution now uses narrow
lease methods; coordinated reset is the only exposed reset path. Inputs/deadlines
are forwarded unchanged, owning outcomes survive physical retirement, and
Retire dispositions or execution exceptions close the borrow terminally.
Existing PostgreSQL/ODBC code does not consume this internal ownership primitive.

Next: conservative cache scopes/invalidation across execution, reset, retirement
and credential rotation, then bounded reusable return. Future facets require
policy-aware lease entry points. No cache, pool, public SDK or Driver Manager
qualification is added, and S2/G12 remain open.

## S2 cache scope progress — 2026-10-02

Private credential-bound Idle leases now issue weak opaque local cache scopes.
Origin/generation checks and conservative invalidation cover all execution/reset
attempts, owner close, retirement and credential lifecycle; tokens do not retain
sessions. Unit/race/TSan and real PostgreSQL cases include recoverable query errors,
reset, weak lifetime and terminal physical closure. No cache storage is added.

Next: bounded reusable return combining reset, credential freshness and invalidated
scope, while actual cache payload policies/external-change revalidation and real
credential-provider integration remain open. S2/G12 and Driver Manager pooling
qualification remain open; cache performance still requires G10 measurement.

## S2 same-owner return evidence — 2026-10-02

The private ownership primitive now supports explicit credential-bound reusable
return after mandatory reset and atomic final admission checks. Unbound, abandoned
and failed-return leases remain terminal. Unit/race and PostgreSQL live evidence
cover old-borrower detachment, cache isolation, same backend PID, transaction and
session-state cleanup, and credential retirement. This does not close S2/G12 or
qualify pooling/ODBC reuse. Next: bounded shared composition policy for reuse
eligibility/health and lifetime, then production provider integration and cache
payload policy; crypto qualification remains independently gated.

## S2 opt-in bounded lifetime/idle policy — 2026-10-02

Private same-owner reuse now has optional finite lifetime and positive idle limits,
with deterministic boundary/race tests and PG live returned-idle retirement.
Active borrowers are not forcibly interrupted; expiry invalidates scope/admission
and prevents further reuse. Legacy/no-policy constructors preserve prior behavior.
Next: coordinated active-health eligibility, then production credential-provider
binding and actual cache payload/refresh policies. Pool capacity/defaults and
public SDK/ODBC reuse qualification remain open; S2/G12 are not closed.

## S2 checked health admission — 2026-10-02

Private ownership now has coordinated active-health checkout with an owning
move-only result and terminal failure semantics after reservation. Tests cover
exclusive reservations, denial nondisruption, probe failures, malformed snapshots,
credential/deadline/policy changes and server-terminated PostgreSQL sessions.
No-I/O checkout remains an explicit internal primitive, not health qualification.
Next: production credential-provider binding and bounded composition/defaults,
then actual cache payload/refresh policy. Pool capacity, ODBC reuse and public SDK
qualification remain open; S2/G12 are not closed.

## S2 managed physical authentication binding — 2026-10-02

Added an internal connection coordinator owning its credential authority and exact
bound token. It authenticates one fresh provider-created session, validates outcome
and bounds, then publishes. Managed checked checkout needs no external token;
revocation retires idle state and prevents reuse while active borrowers survive.
Unit/fault and PG live evidence cover connection failure, same-PID reuse and revoke.
This is not ODBC adoption or a public SDK feature. Next: backend post-authentication
secret scrubbing and required bounded lease facets before ODBC ownership migration.
Product defaults/capacity and actual cache payload/refresh policy remain open; S2/G12
are not closed.

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

## S2 reusable baseline progress — 2026-10-02

The first reusable **internal session baseline** runs against an independent
synthetic implementation and mandatory live PostgreSQL through caller-supplied
SQL. Isolated compilation proves the runner consumes only SDK contracts.
Synthetic negative modes reject incorrect snapshots, missing/fallback/wrong
types, malformed rows, contradictory root errors, broken prepared results, false
reuse after server errors, failed recovery and disconnect, and invalid deadlines.
Cleanup on early rejection is checked. This is bounded S2 harness evidence, not
S4 kit delivery or a stable public SDK claim; MySQL will supply its own fixtures.
S2/G12, production pooling, full extension qualification and S3 remain open.

Validation: focused synthetic/live tests and complete local PostgreSQL, iODBC
UTF-16/UCS-4, ASan/UBSan, and Redshift build/absent-endpoint gates passed.
The final-source PostgreSQL recheck passed. Exact-head hosted Windows/packaging
and crypto profile gates await CI.

## S3 connection-phase codec foundation — 2026-10-02

Private MySQL code now decodes one bounded connection-phase packet and the
protocol-v10 greeting shape for caching_sha2_password and SSL capability.
Version eligibility and verified TLS remain later session-policy decisions before
credential responses; parsing alone never admits a server.
The decoder stops at coalesced packet boundaries, checks the caller's expected
sequence before payload allocation, and rejects over-limit/continuation packets.
Completed/rejected decoders cannot resume. Greeting parsing requires protocol-41,
SSL, secure-connection and plugin-auth capabilities, the 20-byte challenge and
exact supported plugin; malformed fields, reserved bytes and trailing data fail
with fixed messages. Unknown capability bits are retained, not negotiated.

Tests cover every packet split and greeting truncation, byte fragments, coalesced
packets, explicit sequence wrap, zero/exact/overflow budgets, terminal failures,
unsupported capability/plugin and owned greeting data. Focused tests passed;
complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and Redshift
build/absent-endpoint gates passed. Exact-head hosted Windows/package and crypto
regression gates await CI. Production PostgreSQL/Redshift behavior
and exports are unchanged. No MySQL session/provider/product build, TLS upgrade,
password exchange, live handshake or MySQL compatibility is claimed. The old
MySQLProtocolParser placeholder is not the new backend's extension point.

Next: verified TLS request/upgrade and bounded authentication exchanges, then
pinned MySQL 8.4.11 live CI before S3 handshake acceptance. Continuation framing,
EOF/timeout handling, execution and additional MySQL features remain unqualified.

## S3 SSLRequest and derived credential material — 2026-10-02

Private MySQL helpers construct the fixed protocol-41 SSLRequest packet with
sequence 1, the four required supported capabilities, explicit bounded packet
budget and UTF8MB4 collation 45. Unknown server flags are not advertised and
reserved bytes remain zero. No username/password/database appears in this request.
The request does not perform or establish verified TLS.

The caching_sha2_password helper uses the existing private SHA-256 adapter and
requires the caller's verified-peer observation before hashing, including for
empty passwords. Password input is borrowed, bounded and NUL-checked; the
challenge must have 20 bytes. Empty passwords yield empty material. Returned
derived material is move-only; move/clear/destruction cleanse its retained
32-byte buffer. Named digest/combine work buffers have exception-safe cleanup.
This is not a universal compiler-temporary, allocator or provider-buffer scrub
claim. No RSA/plaintext fallback or provider-specific header is added.

Focused tests compare exact wire bytes and independent Python hashlib vectors,
including UTF-8 password and binary challenge; cover capability/budget failures,
TLS rejection, empty/exact/over-limit/NUL/invalid-challenge inputs, unchanged
borrowed data and observable source cleansing on move/clear. Focused checks
passed; complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan and
Redshift build/absent-endpoint gates passed. Exact-head Windows/package and
existing hosted crypto regression gates await CI.

No MySQL session, TLS upgrade, authentication exchange, final OK admission or
live connection is claimed. The future session must check supported server
version policy, upgrade under the original deadline, obtain verification from
the actual transport, retain identical negotiated flags in the credential packet,
and validate the full authentication state machine before publishing a session.
Pinned MySQL 8.4.11 live integration remains required before S3 handshake acceptance.
S2C/G12 qualification, PostgreSQL/Redshift behavior and public scope are unchanged.

## Parallel preparation and MySQL TLS composition — 2026-10-02

The integration owner keeps S3 MySQL implementation on
`codex/transport-foundation`. Redshift source inventory and S2C qualification
strategy review run in isolated worktrees; neither workstream may edit shared
runtime code, launch redundant CI, or claim live acceptance. Results are reviewed
and integrated centrally. Redshift runtime development still requires its named
live gates; account access remains unavailable.

[S2C_QUALIFICATION_STRATEGY.md](S2C_QUALIFICATION_STRATEGY.md) records the finite
Windows isolation proposal, alternatives, estimate and installed-package test
obligations. It is a recommendation pending integration-owner design selection,
not accepted implementation evidence. T13/T14 remain open; main-build AWS-LC
stays disabled until its separately scheduled integration is qualified.

The private MySQL TLS composition now connects in plaintext, reads exactly one
bounded sequence-zero greeting, admits only the pinned 8.4.11 proof profile,
sends the credential-free SSLRequest with partial-write handling and upgrades
the same transport under the original deadline. Success requires actual peer
identity verification and a post-upgrade deadline check. Failure and exception
unwinding retire the socket; returned diagnostics do not expose native text.

Focused tests cover fragmented input/output, unread bytes at the TLS boundary,
all greeting truncations, malformed length/sequence/capabilities/version, invalid
progress on receive and after partial send, transport errors, unverified TLS,
exception cleanup, expired deadline, invalid host/port and missing TLS extension.
Independent review found no source blocker; its three requested failure tests
were added. Complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan,
Redshift unit/build and absent-endpoint contract gates passed. Exact-head hosted
CI remains required for this batch. No credentials are sent and no authenticated MySQL session, live
MySQL integration, provider registration or S3 handshake acceptance is claimed.
Next: authentication exchange and pinned live fixture, not further speculative
connection codec expansion.

The initial hosted run rejected two unbraced conditionals around GoogleTest
assertions under GCC's dangling-else warning-as-error policy. Explicit braces
repair the test source without weakening warnings or altering driver behavior.
Focused tests and the complete local gate set passed again on the repaired
source; replacement exact-head hosted CI remains required.

## S3 bounded authentication exchange and live fixture — 2026-10-02

The private MySQL connection proof now sends protocol-41 HandshakeResponse
under the previously verified TLS transport with the identical negotiated
capabilities, supports cached and full `caching_sha2_password` authentication,
and admits success only after the expected final OK. Full authentication sends
the NUL-terminated password only inside the verified TLS channel; it is not
an RSA or unencrypted fallback. AuthSwitch and unsupported auth-more responses
fail closed. Credentials and server packets are bounded; username/password NUL
inputs are rejected. The original deadline remains unchanged throughout.

Every partial credential write rechecks peer verification. Outgoing derived-
credential and full-password packet storage is scoped and cleansed on return or
exception; borrowed caller credentials remain caller-owned. Cleanup ownership
passes from TLS negotiation to authentication exactly once. Errors return fixed
messages; server/native error text is not logged or echoed. This does not claim
universal cleansing of compiler/provider temporaries or caller-owned strings.

Unit tests cover immediate/cached/full success, final-OK and exact-sequence
admission, empty and over-limit/NUL credentials, all truncated final packets,
unsupported/repeated exchanges, malformed errors and OK, peer-verification loss
before and during both credential writes, zero/EOF/oversize progress, preserved
transport error codes and exception cleanup. Independent review identified a
nested-cleanup issue; its fix and requested failure tests passed re-review.

The Linux build-and-test job invokes a new private core probe against official
MySQL 8.4.11 image digest
`sha256:6ea90827b1100f8f2ae306a539f86d2c264a26ed435a2a9f75551dd5c3aeb242`.
The disposable private-CA fixture requires TLS, verifies the actual version and
authentication plugin, waits for the final TCP-enabled server, clears the auth
cache and proves cold full then warm cached login, wrong password, wrong CA and
wrong hostname. It publishes success evidence only after all cases pass; stale
evidence is removed first. Live acceptance is pending exact-head hosted CI.

This is a backend-private connection-phase proof, not an SDK session or MySQL
ODBC driver. Database selection, command/prepared execution, normalized results,
transactions, metadata, provider registration and shared ODBC orchestration
remain the subsequent bounded S3 work. S2C/T13/T14, G12 and live Redshift claims
remain open; no crypto profile or release gate is narrowed.

Complete local PostgreSQL, iODBC UTF-16/UCS-4, ASan/UBSan, Redshift unit/build
and missing-endpoint contract gates passed for this authentication batch.
The pinned live-container cases and Windows/package gates still require the
exact-head hosted run before acceptance.

The initial hosted Linux run retained five passing MySQL 8.4.11 cases: cold full
authentication, cached authentication, wrong password, wrong CA and wrong host.
Windows rejected `getenv` in the private probe under the existing MSVC warning
policy. The probe now uses `_dupenv_s` on Windows with scoped cleansing/freeing
of its owned environment buffers; other platforms keep borrowed environment
values. Compiler policy and driver behavior are unchanged. Focused tests and
all local protected gates passed again; the repaired exact-head hosted run is
still required for batch acceptance.

## Redshift account-preparation handoff — 2026-10-02

The user reports account access restored; no Redshift infrastructure exists yet.
[REDSHIFT_LIVE_TEST_PLAN.md](REDSHIFT_LIVE_TEST_PLAN.md) now governs the one
USD 15 initial testing allowance shared across all chats, protected configuration,
provisioning/driver ownership, canonical local ledger, required shared runner
and teardown. Verified infrastructure/cost controls and runner readiness replace
account access as the M2 activation dependency. No paid test execution or
provisioning occurred here; the runner is not implemented/qualified. The bounded
MySQL SDK proof continues until that activation gate is ready.

## S3 bounded SDK session and direct-query results — 2026-10-02

The preceding authentication batch passed exact-head CI `36978825121` at
`46f607a`. The next bounded slice adds a backend-private `MySqlSession`
implementing the shared `IDatabaseConnection` contract and owned transport
lifecycle. Clean authentication publishes Idle/Reusable for the same owner;
`SessionOwner::connect_authenticated` adoption is tested. This is not reset,
health, pooling or ODBC driver qualification. Credentials/settings are borrowed
for connect; only the server version and non-secret numeric limits are retained.
CA paths and all connection fields are NUL/size-checked before configuring TLS.
A conservative fixed authentication ceiling rejects tighter startup budgets
until the helper accepts per-exchange budgets. Database selection remains an
explicit unsupported operation in this slice.

Direct COM_QUERY uses the original deadline, verified peer, exact sequence
numbers (including rollover), bounded header/body reads and complete legacy
protocol-41 EOF termination. Request, response bytes/messages, columns, metadata,
rows, cells and diagnostics are bounded. Local input failures preserve the owner;
wire/protocol failures, unsupported result flows and server query errors retire
conservatively without retry or replay. Explicit status flags establish the
passive Idle/Transaction state; transaction/reset/health facets remain absent.

The intentionally narrow type profile includes integer families, unsigned BIGINT
as Numeric, supported UTF-8 char/varchar, binary bytes and NULL expressions.
Rows distinguish NULL, empty text, embedded NUL and binary bytes. Metadata and
results own their storage; invalid text/integer cells become ordered deferred
cell errors. Unsupported types/collations fail closed rather than being guessed.
Warnings, multiple results, local-infile transfers, cursor/out-parameter/session-
tracking flows and prepared execution are not implemented. Active unsupported
completion flags and warnings cannot publish a reusable session.

Unit tests cover metadata/row/completion truncation, length-encoded values,
integer range and UTF-8 failures, unsupported types/collations, sequence rollover,
trust-path validation, resource limits before I/O/allocation/admission, local
recovery, SDK-owner adoption, preserved deadlines and retirement after bad
progress, transport exceptions, peer loss or interrupted responses.
The pinned Linux MySQL 8.4.11 fixture adds direct SELECT, typed/null/empty/binary
results, temporary-table DDL/DML and affected counts, empty results, server-error
retirement, reconnect and result ownership after disconnect. Acceptance requires
focused tests, the complete local protected gates and exact-head hosted CI.
MySQL provider registration, ODBC integration, prepared execution and the wider
S3/S4 contract kit remain subsequent work; S2C/T13/T14 and G12 stay open.

Protocol references (design evidence, not live acceptance):
[COM_QUERY](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_query.html),
[text resultsets](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_com_query_response_text_resultset.html),
[column definitions](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_com_query_response_text_resultset_column_definition.html),
[OK](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_basic_ok_packet.html)
and [EOF](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_basic_eof_packet.html).

The first hosted session fixture failed. Its SELECT used unquoted `empty`, a
reserved MySQL 8.4 keyword; the fixture now uses `empty_value`, preserving all
NULL/empty/binary assertions. Fixed probe check/query IDs and numeric error codes
are allowlisted by the Python harness so another failure is actionable without
echoing SQL, credentials, values or native diagnostics. The repaired hosted live
run remains required before this session slice is accepted.

### S3 native database admission — 2026-10-02

The private MySQL SDK session now selects a nonempty requested database using
[COM_INIT_DB](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_init_db.html)
after verified authentication and before publishing connection success. Schema
names are native UTF-8 protocol fields, never SQL interpolation. Authentication
and selection share the original absolute deadline. Conservative authentication
reservations leave selection a bounded startup request/response allowance;
requests exceeding the packet or configured limits fail before credentials are
sent. Selection requires a zero-counter, warning-free, idle OK response.

Rejected schemas, malformed/truncated replies, sequence errors, unsupported
status and resource exhaustion retire the unpublished transport. Tests cover
literal punctuation/UTF-8 names, every truncated reply prefix, post-authentication
exceptions, exact cleanup, budget boundaries and successful reconnect. The
pinned hosted fixture now selects `odbcpp`, uses unqualified temporary-table
queries and rejects a missing database before reconnecting. Complete local gates
and exact-head hosted acceptance are required for this code batch. This closes
database selection only; prepared execution, transactions/metadata facets,
provider/ODBC registration and full S3/S4 acceptance remain open.

### S3 bounded native prepared execution — 2026-10-02

Native database admission passed exact-head CI `36988920504` at `54ed895`.
The next private SDK slice uses COM_STMT_PREPARE, COM_STMT_EXECUTE and
COM_STMT_CLOSE, with no SQL substitution and no server-statement cache. Supported
parameter hints are signed Int16/Int32/Int64, Boolean (`0`/`1`), UTF-8 Text and
Unspecified, Binary and typed NULL. Embedded NULs remain data; binary input is
accepted only for Binary. Floating, decimal and temporal hints remain explicitly
unsupported in this slice. Parameter count, per-value/total bytes and full
prepare+execute+close request bytes are checked before protocol mutation.

Preparation drains bounded parameter/result metadata; execution consumes fresh
metadata and owning binary rows. Native type identifiers remain backend-private.
NULL bitmaps, signed/unsigned integers, length-encoded text/binary and normalized
cell errors are covered. Prepare and execute share one absolute deadline and
cumulative response byte/message and metadata entry/name budgets. Successful
execution closes the statement without expecting a close acknowledgement.
Parameter-count mismatch closes it and preserves the same owner; malformed
responses, server errors, unsupported flows and cleanup failure retire the
transport without replay. Owning outcomes are constructed before cleanup guards
are disarmed, so allocation exceptions cannot publish contradictory state.

Focused tests cover codec truncation, bitmap boundaries, numeric range, invalid
UTF-8, unsupported hints, exact cleanup, aggregate limits, mismatch recovery,
no-parameter DML, deadline expiry and peer loss. The pinned Linux fixture adds
typed prepared SELECT/INSERT, NULL/empty/binary/literal text, no-parameter SELECT
and mismatch recovery. Complete protected local gates and exact-head hosted
live/platform acceptance remain mandatory for this batch. No MySQL ODBC product,
public prepared cache, complete scalar support or S3 completion is claimed;
transaction/catalog facets and shared ODBC registration remain next work.

Protocol references:
[prepare](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_com_stmt_prepare.html),
[execute](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_stmt_execute.html),
[binary resultsets](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_binary_resultset.html),
[close](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/page_protocol_com_stmt_close.html).

The initial hosted prepared batch caught two test-fixture portability warnings:
GCC requires braces around a conditional Google Test assertion, and MSVC requires
the packet sequence fixture counter to use its native uint8_t type. Both are
corrected without changing compiler policy or runtime behavior. Repaired full
gates and hosted live acceptance are required before closing this slice.

### S3 MySQL transaction and isolation facet — 2026-10-02

Bounded native prepared execution passed exact-head CI `36993383466` at
`4edcc37`. The next private session slice implements the shared session-owned
ITransactionSession facet for the pinned InnoDB profile. It declares the nominal
RepeatableRead default and all four isolation levels, with transactional DDL
false. This is a profile capability, not discovery of arbitrary server defaults
or a guarantee for nontransactional storage engines.

Begin uses START TRANSACTION only while idle: a nested begin is rejected locally
because MySQL would implicitly commit the active transaction. Commit/rollback
explicitly use AND NO CHAIN NO RELEASE so session completion_type settings cannot
chain a new transaction or disconnect the client. Idle-only isolation changes
use fixed SET SESSION TRANSACTION ISOLATION LEVEL commands. Invalid enum/state
requests cause no I/O and preserve the owning session; transport/server failures
retain precise transaction operation tags and the existing retirement policy.
Control completions must be warning-free, have zero affected/insert counters,
contain no resultset and report the expected IN_TRANS/Idle state.

Unit tests cover stable facet borrowing after disconnect, declared capabilities,
all isolation commands, nested begin prevention, idle-only isolation, exact
completion SQL, malformed counters/resultsets/status, server errors and operation
snapshots. Hosted live acceptance requires InnoDB commit persistence, rollback
removal after a rejected nested begin, completion_type CHAIN/RELEASE overrides,
all four observed session isolation values and restoration of fixture settings.
Complete protected local and exact-head hosted gates remain required. Essential
catalog metadata, wider scalar types, health/reset/recovery policy and shared
ODBC registration remain subsequent S3 work; S3/G12 is not closed.

Semantic references:
[MySQL transaction completion](https://dev.mysql.com/doc/refman/8.4/en/commit.html),
[session isolation](https://dev.mysql.com/doc/refman/8.4/en/set-transaction.html).


## Parallel MySQL protocol-41 rejection validation — 2026-10-03

The isolated MySQL package adds one private ERR header validator shared by authentication and session rejection handling. Protocol-41 errors require a complete header,0xff marker,# SQLSTATE marker and five uppercase ASCII alphanumeric SQLSTATE bytes. Malformed headers produce ProtocolError; valid native rejections preserve existing authentication/query classifications. Native message bytes remain opaque and public diagnostics stay fixed. Regression tests cover every SQLSTATE position, truncated headers, marker corruption, opaque message bytes and transport retirement. Root focused authentication/session/query-wire CMake tests passed; independent review found no functional blocker. Full code-batch and hosted pinned MySQL/Windows gates remain required before acceptance; no local live MySQL result is claimed because the Docker engine is unavailable. This does not close S3 or register a MySQL ODBC product.

Combined candidate validation passed: PostgreSQL67/67unit plus full integration/verifiedTLS; UTF16/UCS4 unit/integration/verifiedTLS; sanitizer unit; Redshift67/67unit/build;241offline runner safety tests; exact absent-endpoint rejection. Hosted Windows/package and pinned live MySQL remain CI gates, not local passes. No additional paid SQL in this code batch.


## S3 result-only NEWDECIMAL candidate — 2026-10-03

Independently reviewed private codecs now preserve exact Decimal(p,s) strings from direct and prepared result rows for the bounded p=1..65, s=0..30, s<=p profile. Sign/decimal-point display-width arithmetic is checked; malformed metadata/framing retires while complete malformed cells use ordered deferred errors. Parameter hints and native NEWDECIMAL parameter descriptors remain unsupported, as do DATE/FLOAT/legacy DECIMAL. Unsigned BIGINT Numeric(20,0) is unchanged. Codec/session tests cover 65-digit and p=s boundaries, unsigned signs, NULL versus empty, commands, original deadlines, draining/recovery and ownership. Native fixed-scale/p=s/unsigned behavior needs pinned live GoogleTests before qualification. SDK exact strings do not extend SQL_C_NUMERIC beyond its existing 38-digit conversion limit; MySQL ODBC registration and full S3 remain open.

### Dedicated native decimal qualification candidate (2026-10-03)

The independently reviewed GoogleTest checks signed DECIMAL(5,2), (5,5), (65,0) and (65,30) exact bounds, fixed scale, NULL, direct/prepared metadata agreement, recovery and owning results after disconnect. It is isolated from default CTest and receives explicit admission only from the existing pinned MySQL 8.4.11 verified-TLS fixture runner. The runner requires an exact one-case successful XML inventory and a 60-second child bound; uncertain container creation triggers exact-name cleanup, and success evidence requires independently confirmed container absence. Three offline admission/cleanup tests and the native executable build/unadmitted rejection passed. No native run has occurred: the local Docker daemon is unavailable, so hosted pinned CI must supply that evidence. No decimal parameter or MySQL ODBC claim follows.

Candidate platform-preservation checks also passed: full local PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer, PostgreSQL/Redshift 69 unit targets, 262 existing offline orchestration cases, and Redshift absent-endpoint rejection. Windows repair CI37153221833/676fe5d is green across required gates. Hosted pinned MySQL evidence for this new candidate remains pending.

Native pinned MySQL 8.4.11 evidence from CI37154210057/9fdf67d now passed the exact one decimal GoogleTest plus six preserved authentication/direct-session probes. Root independently validated the downloaded exact successful XML inventory and cleanup-confirmed JSON. This establishes the declared signed direct/parameter-free prepared decimal result scope, including fixed scale, p=s, 65-digit bounds, NULL and recovery/ownership. Overall platform CI remains separate until all jobs finish; unsigned BIGINT, decimal parameters and MySQL ODBC are not qualified by this case.


Result-only MySQL DATE candidate (2026-10-03): independently reviewed private type10 normalization to known Date10/0 and strict Gregorian canonical strings. Direct and binary metadata affinity is checked before NULL. Complete zero/invalid calendar cells remain non-NULL owning values with deferred encoding errors; malformed/truncated framing retires the session without partial results. Temporal parameter hints/native DATE parameter descriptions remain refused, and other temporal types remain unsupported. Root five focused targets, PostgreSQL/Redshift 70 unit targets, PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer and 264 offline checks pass. Full gates found an obsolete type10 refusal in the decimal test; independently reviewed repair retains other unsupported families and the dedicated DATE coverage. The local pinned MySQL launcher failed because Docker is unavailable; hosted pinned CI remains required. Original failure logs are retained. Native DATE interoperability is not yet qualified; a separate pinned GoogleTest scope will cover valid bounds/leaps/NULL before any explicitly mode-admitted invalid-date scope.


The reviewed private MySQL TIME duration codec candidate validates exact declared precision and owning text/binary 0/8/12 frames, signed bounds, fractions, zero/NULL, and malformed versus complete-invalid cells. Negative zero is rejected by its documented canonical profile. It is not wired to session execution or ScalarType::Time: native durations exceed the current ODBC time-of-day contract, which remains a separate design decision. Native TIME, temporal parameters and full duration conversions remain unqualified.


Result-only MySQL DATETIME candidate (2026-10-04): independently reviewed native12 physical metadata maps to the existing owning Timestamp calendar family with declared precision0..6 and checked width19 plus fractional digits. Direct and binary readers enforce metadata affinity before NULL; raw0/4/7/11 and exact precision text preserve complete invalid cells as deferred errors, while malformed metadata/framing retires. Shared preparation metadata explicitly refuses temporal parameter descriptors; all temporal hints including NULL remain unsupported. Native TIMESTAMP7 and TIME11 remain refused: session-zone identity and duration representation require separate policy. Seven pure codec and meaningful packet/session tests do not qualify native interoperability or UTC/storage endpoints. Root integration gates and future separately admitted pinned GoogleTest evidence remain required.

Candidate validation: seven focused MySQL targets, full PostgreSQL/Redshift73 unit targets, PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer,272 offline safety tests and native four-case absent-endpoint rejection passed. Local Docker remains unavailable; hosted pinned MySQL and Windows/package gates remain mandatory. The canonical schema-privilege correction passed31 lifecycle cases and preserves the original consumed failed scope. No further live SQL or DATETIME native qualification accompanies this code batch.


Dedicated DATETIME native qualification candidate: independently reviewed physical DATETIME(0/3/6) calendar/interior-fraction/NULL test is isolated from default CTest and Windows integration inventory with a default-OFF explicit target. The existing pinned8.4.11 fixture runner admits only its exact one-case marker/filter/repeat/XML under a60-second child bound, strips inherited test and fixture-admission controls, and preserves six probes plus decimal proof. Resolved/same-file output collisions are rejected before unlinking artifacts. Strict required XML inventory and independent container absence are prerequisites for success JSON; no extra server, SQL-mode or timezone change is added. Eleven offline admission tests and ON build/unadmitted rejection/OFF inventory passed; full local PostgreSQL/Redshift73 unit targets, PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer and272 offline safety checks passed; hosted native execution remains pending. Native DATETIME, temporal parameters, TIMESTAMP/TIME and MySQL ODBC are not qualified by source integration.

Native DATETIME checkpoint (2026-10-04): exact f31ca7c CI37167474368 passed all required gates. The pinned MySQL8.4.11 physical DATETIME(0/3/6) GoogleTest passed its exact one-case XML in0.05s; independent review accepted metadata, calendar bounds/leaps/interior fractions, NULL neighbor, direct and parameter-free prepared results, immediate zero warnings, recovery and owning results. The same runner preserved six ordered probes and signed decimal proof and emitted success JSON only after verified owned-container absence. This does not qualify temporal parameters, TIME, native TIMESTAMP7, UTC/storage endpoints or MySQL ODBC.

Future LEGACY missing-table scope: exact m2_pk_absent_20261004_c01 is never created, granted or dropped. A distinct two-case inventory binds ConnectionTest and OdbcPrimaryKeyMissingLegacyContract. The helper checks authenticated identity, canonical schema USAGE and owned activity under the original execution deadline, then exact object absence before tests; an independent absence check and activity cleanup use one frozen cleanup cutoff after tests, including failures. Post-absence failure invalidates qualification without suppressing activity cleanup. Before/after checks cannot rule out transient appearance between them. This is offline lifecycle/source coverage only; native execution requires a fresh separately reviewed finite admission after coherent head CI and artifact/control/accounting checks. SHOW missing and denied outcomes remain unknown separate scopes.

Missing-table/adapter batch validation:92 adapter cases, PostgreSQL/Redshift73 unit targets, PostgreSQL/UTF16/UCS4 integration with verified TLS, sanitizer,277 offline safety cases (including36 lifecycle cases), native build/registration and exact one-case absent-endpoint rejection passed. The initial absent-endpoint check used the not-yet-rebuilt binary and correctly refused its zero-case inventory; that original log remains separate from the rebuilt one-case rejection. Local pinned MySQL remains explicitly deferred because Docker is unavailable; hosted gates remain required. No missing-table live request or SQL was admitted.

Private DATE parameter packet candidate (2026-10-04): independently reviewed owning strict Gregorian encoder and explicit private DateCandidate writer profile are integrated for pure tests only. ExistingOnly remains the default, and no session call opts in. Independent whole-frame tests cover valid/typed NULL native10, bitmap boundaries across nine mixed parameters, raw and wire budgets, ownership and preserved default/other-family refusals. Runtime DATE parameters remain unsupported; actual native10/width10/decimals0 descriptor receipts, strict raw-shape policy and separately reviewed session admission/native qualification are still required.

LEGACY missing-table checkpoint:22b3638 CI37168983153 passed all required gates. A separately owner-reviewed finite proposal stopped at the fresh current-IP/existing exact IPv4 /32 control before request creation, durable consumption, events, SQL or GTests. Independent review accepted this as a preadmission network-control block. Network and original accounting remain unchanged; no native missing-table proof, automatic retry or new financial coverage is implied. The protected owner review and failed-control evidence remain separate.

Private DATE packet batch validation: five focused MySQL targets, full PostgreSQL/Redshift75 unit targets, PostgreSQL/UTF16/UCS4 integration and verified TLS, sanitizer and277 offline safety checks passed. The original focused launcher failure named a nonexistent target; the corrected target inventory passed without changing assertions. Hosted pinned MySQL and Windows/package gates remain required because local Docker is unavailable. Raw metadata observation and runtime descriptor admission are separate future packages; no session selects DateCandidate.

Private DATE receipt candidate (2026-10-04): raw native type/width/decimals observation publishes only after complete metadata validation and owning result construction; failures preserve the caller's observation, and DATE result normalization remains unchanged. A separate pure helper requires an explicit Date hint paired with the same validated receipt: native10/width10/decimals0 plus known normalized Date10/0. Contradictory native/normalized DATE shape returns ProtocolError; a validated supported policy mismatch returns UnsupportedFeature. Owning receipt, truncation, malformed metadata, budget and shape tests do not enable a session callsite, infer bare-placeholder DATE types, perform statement cleanup or qualify native parameters. Future session integration must preserve the original deadline, drain and counts before reusable mismatch close, and retire unread/malformed/unsupported metadata.

Redshift manual PrimaryKeys remains staged unsupported. The independently reviewed design requires backend-owned single-use session-generation receipts, serialization across admission/BEGIN/consume, preserved bindings and deadline, and transaction tracking reconciled with backend snapshots after failures. Native SHOW inside an explicit transaction remains a separate proof prerequisite. The current network-control block prevents live missing-table admission; no network change, SQL retry or allowance extension accompanies this batch.

Private DATE receipt batch validation: seven focused targets, full PostgreSQL/Redshift77 unit targets, PostgreSQL/UTF16/UCS4 integration with verified TLS, sanitizer and277 offline checks passed. Internal include ownership first rejected the unlisted query-wire dependency; the reviewed backend-only manifest repair passed without weakening the checker. Both the original configure refusal and corrected result are retained. The previous3339411 packet batch passed all required hosted CI37171293029 gates. Session runtime admission remains a separate reviewed candidate; no native DATE parameter claim or paid SQL accompanies this receipt batch.
