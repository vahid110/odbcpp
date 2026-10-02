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
