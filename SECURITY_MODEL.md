# SDK and driver security model

Decision date: 2026-09-30.
Audit baseline: `089e085`.
Status: S1 security architecture accepted. The first S2 security batch resolves
SEC-1 and SEC-2; SEC-3, SEC-4 and the remaining G12 evidence stay open.

This model applies to PostgreSQL, the MySQL SDK proof and later Redshift work.
It records concrete current risks, mandatory contracts and release evidence. It
is not a claim that the current driver has completed a security review.

## Trust boundaries

1. Application to ODBC: handles, lengths, connection strings, SQL, parameters
   and output buffers are untrusted.
2. Configuration to driver: DSNs, environment, INI files, registry values,
   driver defaults and log destinations have explicit provenance and precedence.
3. Credentials to authentication: secrets, identity, expiry and refresh results
   cross a narrow lifecycle boundary.
4. Network to backend: DNS, TLS, authentication, frames, errors, metadata and
   results are controlled by a remote peer.
5. Logical borrower to physical session: transaction/session/prepared state,
   diagnostics, credentials and caches must not cross borrowers.
6. Driver to logs/diagnostics: server text, SQL, values, identifiers and paths
   may contain sensitive or attacker-controlled data.
7. Build/release to consumer: dependencies, CI actions, toolchains, artifacts,
   installers, hashes, signatures and provenance are supply-chain inputs.
8. Backend extension to host process: native extensions are trusted in-process
   code with the application's privileges; SDK validation is not a sandbox.
9. Cryptography dependency to host process: provider selection, linkage, loader
   paths, symbol visibility, version/update ownership and coexistence with crypto
   already loaded by an ODBC host are supply-chain and runtime boundaries.

## Current blockers

### SEC-1: password authentication without verified TLS

Implementation status: resolved by `df8a67e`. Omitted `SSL` now selects verified
TLS, plaintext requires an explicit false value, and PostgreSQL cleartext-password
authentication requires transport evidence that both the certificate chain and
host identity were verified. Custom CA file/directory settings are applied to
sync and async TLS transports or rejected before connection.

Current ODBC behavior treats omitted `SSL` as disabled, while the PostgreSQL
parser accepts cleartext-password authentication and can serialize the password.
This creates a concrete credential-disclosure path to a configured, redirected
or impersonated plaintext server.

Before general S2 refactoring:

- credential authentication defaults to `RequireVerified` TLS;
- plaintext is an explicit opt-out, never an omission default;
- cleartext-password authentication is rejected unless the authentication layer
  receives immutable proof of a verified encrypted channel;
- every authentication method declares its channel requirement;
- downgrade or TLS negotiation failure never falls back silently;
- existing explicit non-TLS test profiles use only permitted methods and do not
  weaken production defaults.

### SEC-2: implicit working-directory configuration

Implementation status: resolved by `179f6a6`. Production Unix/macOS discovery no
longer searches current or parent directory files. Explicit environment,
system, Homebrew and user DSN locations remain covered by focused tests.

Production Unix/macOS resolution currently searches current and parent directory
INI/DSN files. An unexpected working directory can redirect a missing DSN or
driver default. Production builds must use explicit Driver Manager/configuration
locations and trusted precedence. Relative test fixtures require a test-only
hook. Malformed or inaccessible authoritative sources fail according to the
documented precedence rather than silently falling through.

### SEC-3: unsafe internal pool ownership

The quarantined prototype pool exposes copyable physical-session pointers and
manual release, with no reset. Its wrapper serializes protocol execution, but it
cannot be exposed or used as pooling evidence. The replacement contract requires one move-only lease per
physical session, serialized operations, reset/retirement under deadline and
credential/cache isolation.

### SEC-4: unbounded aggregate server results

The PostgreSQL-family session now enforces SDK startup/query wire-byte and
message budgets, query row/cell/result and metadata-entry/name budgets, and
per-error/notice payload limits before diagnostic decoding. Metadata entries
count every row/parameter description across an exchange; name bytes exclude
terminators and count every row description, including replacements. Defaults
are 65536 entries, 1024 bytes per column name, 1 MiB aggregate names and 16 KiB
per diagnostic payload. Zero permits only empty names/metadata/diagnostics.
Outgoing SQL and parameter count/value/aggregate bytes are now bounded before
request encoding; connection fields are bounded before startup encoding or
transport setup. Defaults are 1 MiB SQL, 65535 parameters, 16 MiB per value,
64 MiB total values and 64 KiB per connection field. Null values contribute zero
bytes but consume a parameter entry. Binary input is counted before wire-format
expansion. Configured ceilings cannot exceed 64 MiB SQL, 65535 parameters,
64 MiB per value, 256 MiB total values or 1 MiB per connection field. Local
preflight failures perform no I/O and preserve the current owner's session and
transaction state; response limit failures still retire. These are input/count
bounds, not exact encoded-byte or heap quotas. Earlier ODBC conversion, marker
count/translation helpers, allocation boundaries and product-profile
configuration remain open. Each backend
must enforce configurable hard ceilings and return a resource-limit error with
mandatory retirement when protocol synchronization or memory safety is uncertain.

## Mandatory security contracts

### Channel and authentication

Use an explicit TLS policy rather than a Boolean. It records peer verification,
trust source, minimum protocol and downgrade behavior. The selected MySQL proof
is MySQL **8.4.11**, `caching_sha2_password`, verified TLS, and no RSA/plaintext
fallback. The image digest is pinned when S3 CI is implemented. MySQL documents
`caching_sha2_password` as the 8.4 default and requires secure transport or RSA
for full authentication: [MySQL 8.4 authentication](https://dev.mysql.com/doc/refman/8.4/en/caching-sha2-pluggable-authentication.html),
[8.4.11 release](https://dev.mysql.com/doc/relnotes/mysql/8.4/en/news-8-4-11.html).

PostgreSQL authentication policy is method-specific. CA file/directory inputs
must be applied or rejected. Hostname/IP verification and TLS 1.2 minimum remain
mandatory when verified TLS is selected.

Cryptography provider and dependency linkage are immutable build profiles, not
connection properties. Provider-neutral security contracts preserve the same
verification and authentication behavior across implementations. Each claimed
profile requires artifact inspection, dependency identity, coexistence evidence
and the qualification matrix in
[CRYPTO_PROVIDER_PLAN.md](CRYPTO_PROVIDER_PLAN.md). FIPS is an independent exact
module/profile claim and is never inferred from provider selection.

Reported FIPS mode is informational, not evidence that every operation is
approved. Future FIPS work must follow the selected module's security policy
and distinguish module validation from driver integration claims. Preserve
explicit initialization, host-process policy isolation and session reuse
boundaries now; implementation and qualification remain deferred under the
future-readiness section of the crypto plan. This adds no current FIPS claim
or change to supported authentication methods.

### Credentials

Credentials use a move-oriented owning type/provider with redacted description,
non-secret identity/generation and optional expiry. Minimize copies, cleanse
retained buffers where practical, and clear them at disconnect/retirement. Do
not place secrets in cache keys, logs, capability data or completed connection
strings. ODBC completed-string behavior must be specification-reviewed and
covered by explicit redaction tests before changing current behavior.

### Inputs and resource budgets

Every product profile defines hard bounds for configuration fields, frame size,
messages per operation, metadata name/count, rows, cells, cumulative result
bytes, pending results, diagnostics and log fields. Deadlines bound time, not
memory. Limit failures are deterministic, observable without user data and
retire sessions when synchronization is uncertain.

### Session isolation, reset and cancellation

One physical session has one logical borrower. Reset covers transaction state,
session settings, prepared/session resources, diagnostics, credential generation
and cache epochs within the advertised profile. Reset failure or timeout retires.
Cancellation returns a typed post-operation disposition; ambiguous partial I/O
retires by default.

### Caches

Cache keys include the required physical session, server identity/version,
security identity and credential generation. Each cache has a hard size bound,
synchronization, invalidation events, sensitivity classification and safe
observability. No cached secret or cross-user value may leak through metrics or
diagnostics.

### Logging and diagnostics

Structured fields are classified `Public`, `Sensitive` or `Secret`. Secret fields
are dropped. Sensitive fields are redacted by default. Attacker-controlled values
are escaped and length-bounded. Raw backend messages may be needed for ODBC
diagnostics, but production logs use a safe summary unless an explicit sensitive
debug mode is enabled. Query logging remains opt-in with a data-exposure warning.

Log destinations are trusted administrator/application configuration. Service
hosts must not accept untrusted per-connection paths silently.

### Extension trust

G12 extensions are statically linked and fully trusted in-process. Capability
declarations are explicit, and shared orchestration still validates normalized
results and errors. Runtime-loaded or untrusted plugins require a later signing,
discovery, compatibility and isolation design.

## Existing controls to preserve

- TLS 1.2 minimum, certificate-chain and hostname/IP verification with no
  requested-TLS fallback.
- SCRAM nonce/signature/iteration checks, strong randomness and private password
  cleansing.
- Embedded-NUL rejection and detailed protocol length/sequence/no-progress/EOF
  checks.
- Absolute I/O deadlines and retirement on malformed protocol state.
- C-entry exception containment and centralized handle locking.
- Logging off by default, query logging opt-in, escaping, rotation and password
  regression tests.
- Registry type/size validation, password-free DSN persistence, masked Windows
  setup UI and password-buffer clearing.
- Warnings-as-errors, ASan/UBSan, cross-platform CI, package lifecycle tests,
  checksums, payload inventory and dependency-license capture.

## G12 security evidence

G12 cannot close without:

1. coverage-guided fuzz targets for PostgreSQL/MySQL frame and authentication
   state machines, configuration parsing, SQL dialect parsing, result normalization
   and server-error decoding, run under sanitizers with time/memory limits;
2. hostile-transport tests for fragmentation/coalescing, invalid and huge lengths,
   endless nonterminal messages, no-progress I/O, sequence errors, partial writes,
   mid-frame EOF and authentication changes;
3. downgrade tests for secure defaults, cleartext rejection, certificate/hostname
   failures, custom CA enforcement and absence of plaintext fallback;
4. live PostgreSQL and MySQL TLS tests using a private test CA and verified host;
5. pool/session isolation tests across users, transactions, session variables,
   prepared state, timeouts, disconnects, credential expiry and cache epochs;
6. secret canaries injected through credentials, SQL, parameters, server errors,
   DSNs, paths and diagnostics, followed by scans of logs, completed strings,
   caches and artifacts;
7. bounded resource-limit tests with deterministic retirement;
8. curated static analysis, CodeQL, warnings-as-errors and sanitizer gates;
9. forbidden-dependency/security architecture checks;
10. dependency review/OSV scanning, hashes for fetched archives, pinned CI action
    revisions and an internal dependency/SBOM inventory;
11. release hardening flags plus artifact inspection on Linux, macOS and Windows.
12. provider/linkage matrix evidence for each claimed artifact, including common
    TLS/auth tests, binary dependency and symbol inspection, manifest accuracy,
    provider coexistence and dependency-license/SBOM inputs.

These gates are introduced incrementally in S2/S3. A missing tool may be recorded
while developing, but it cannot be reported as passing evidence.

## G13 public-distribution controls

- Independent security architecture/code review and remediation record.
- Continuous fuzzing with retained regression corpus.
- SPDX or CycloneDX SBOM for every artifact.
- Windows signing, macOS signing/notarization as applicable, signed checksums and
  CI provenance/attestations.
- Documented reproducible-build target or exact build-environment inventory.
- `SECURITY.md`, private reporting, supported-version policy, CVE response and
  dependency-update service levels.
- Release-key custody/rotation and installer privilege, ACL, DLL-search, rollback,
  upgrade and tamper tests.
- Stable security/authentication configuration documentation.

## Explicit non-claims

The S1 review is not penetration testing, a vulnerability certification, a
public support promise or evidence that current binaries meet the controls above.
Runtime plugins are not sandboxed. Redshift security behavior remains unverified
until real endpoint evidence exists.

## S2C driver export isolation checkpoint — 2026-09-30

The finished shared ODBC driver is now constrained and verified to the canonical
76-entry ODBC C ABI on Linux, macOS and Windows. This prevents private C++, SDK
and statically incorporated provider symbols from becoming part of the driver
dynamic export surface. It does not establish bundled shared-provider or
preloaded-module coexistence, so provider/linkage qualification remains open.

## S2C Linux static live-security checkpoint — 2026-09-30

The Linux OpenSSL static candidate now requires a successful verified-TLS and
SCRAM query through unixODBC, and requires rejection of both an untrusted CA and
a hostname mismatch through unixODBC, with direct driver calls preserving exact
TLS failure diagnostics. Its test host preloads the shared provider before loading
and using the static driver; the shared provider identity must remain stable,
and the driver must contain no unresolved provider symbols.

The preloaded and embedded instances originate from one package version. This
is duplicate-instance cohabitation evidence, while cross-version and
cross-implementation coexistence and embedded-byte provenance remain open.

## S2C static dependency link-trace checkpoint — 2026-09-30

CI Linux bundled-static evidence now includes a GNU ld map that must name the
inspected output and both controlled archives as loaded inputs and member
sources. The archives, map and exact driver hashes are retained together, and
stale or partial maps fail closed. This is bounded link-input evidence; it does
not establish embedded-byte ancestry, cross-implementation coexistence or
AWS-LC qualification.

## S2C Linux loader configuration boundary — 2026-10-01

The AWS-LC qualification fixture observes provider mappings in fresh hosts and
rejects origins outside the extracted package. Explicit library-path and preload
canaries prove the detector notices external copies even with identical bytes.
This is qualification-time detection, not a driver runtime security control.
An in-process driver cannot sandbox a host that intentionally preloads native
code. Deployment qualification must therefore state who controls the host's
loader environment and writable package directories, alongside update ownership.
The evidence makes no protection claim against arbitrary hostile native code.

Windows bundled-shared provider origin must be checked against bound driver
imports, rather than a basename-only module lookup. The package preload canary
now distinguishes identical installed files from byte-identical foreign copies,
with an explicit missing-input failure. A detected foreign binding is a known
qualification blocker: the test harness detects it after loading, and the driver
has no production rejection mechanism for that case yet. DLL search flags and
matching version/hash alone do not establish isolation from preloaded modules.
A future prevention strategy needs separate implementation and live/package
qualification; no loader-lock mitigation or safe-coexistence claim is inferred
from the canary.

### Allocation failure containment (S2 implementation evidence)

PostgreSQL-family request/startup encoders distinguish bad_alloc from malformed
input. Failures caught before request I/O preserve the current owner's session;
response I/O, parsing and extraction failures retire. Startup authentication
failures retain their phase and run connection cleanup. Typed allocation errors
carry empty detailed text and a fixed public summary; request diagnostics use
HY001. The exported C API guard also distinguishes bad_alloc and attempts an
HY001 diagnostic without allowing diagnostic allocation failures to escape.

Phase-specific injected failures prove these boundaries, not full recovery under
sustained memory exhaustion. Earlier ODBC conversion/local exception handlers,
standalone SQL helpers, encoded-buffer ceilings and product-profile limits are
still open. No exact heap quota or general pooling readiness is claimed.

### Encoded query exchange ceilings (S2 implementation evidence)

The SDK input profile also supplies a complete query-request wire ceiling:
128 MiB by default, configurable up to 1 GiB. PostgreSQL direct, prepared and
statement-description encoders include rewritten SQL, all frame/type/length
fields and binary expansion before allocating wire/value buffers. Over-limit
requests return a local ResourceLimit error without request I/O or retirement.
Temporary SQL rewriting is still controlled by the input ceiling; vector/string
capacity, earlier ODBC conversions, startup/auth buffers and exact heap quotas
remain separate unfinished boundaries. This does not close SEC-4 or G12.

### Startup/authentication wire ceilings (S2 implementation evidence)

SDK startup and per-authentication-response wire ceilings are independently
configurable (1 MiB defaults, at most 1 GiB each). Startup counts length/protocol,
keys, values and terminators before field copies and wire allocation. Startup
limit rejection preserves an existing owner. Cleartext/MD5 and SCRAM initial
and continuation responses count complete password/SASL framing before outgoing
wire allocation; authentication limit failure sends no oversized response and
retires with phase-preserving failed-connect cleanup. This does not relax any
channel/authentication policy. SCRAM intermediate string/crypto allocations,
provider handshake buffers and complete heap accounting remain separate;
product-profile configuration and earlier ODBC conversion/helper limits remain
unfinished. SEC-4 and G12 stay open.

### Resource profile configuration (S2 implementation evidence)

The implemented SDK ceilings are available through documented Max* driver,
DSN and connection-string options, with the resolver's existing precedence.
Unsigned decimal parsing rejects malformed/overflowing values without echoing
raw input. Shared profile validation runs during PostgreSQL-provider resolution
before session creation and again for direct SDK connection callers. Omitted
options preserve SDK defaults; explicitly larger supported ceilings permit
larger resource use. This closes configuration exposure for these specific
limits, not SEC-4: earlier ODBC conversions/helpers, intermediate crypto/provider
allocations and complete heap accounting remain unfinished. Windows GUI controls
for advanced resource options are deferred.

### ODBC SQL capture budgets (S2 implementation evidence)

ANSI/wide SQLExecDirect and SQLPrepare use the accepted connection profile's
MaxSqlBytes before copying input. Explicit lengths and NTS scans are bounded;
wide conversion checks UTF-8 expansion before appending code points. Overflow
returns HY000 with no partial SQL, backend work or prepared-state mutation.
Within-budget malformed wide input remains 22018. UTF-16/UCS-4 gates cover the
same conversion contract. Parameter conversion, connection-string capture,
standalone SQL helpers and complete heap accounting remain separate unfinished
boundaries. This does not close SEC-4, S2 or G12.

### ODBC bound-parameter budgets (S2 implementation evidence)

Preparation checks MaxParameters before replacing prepared state; execution
checks it before parameter-vector reservation. ANSI/binary explicit lengths and
ANSI NTS scans are bounded before copies. Wide input uses a bounded UTF-8
conversion, including expansion, and returns no partial value. Each value is
also checked after normalization, then counted against MaxParameterTotalBytes.
NULL consumes an entry and no bytes; explicit empty values remain distinct.
Limit failure reports HY000 and completes the supported parameter-set status
as error before transaction start or backend execution, preserving the session.
Malformed wide values within budget retain 22018. Fixed-size scalar formatting,
connection-string capture, standalone helpers and exact heap accounting remain
outside this evidence; SEC-4, S2 and G12 remain open.

### ODBC connection capture budgets (S2 implementation evidence)

ANSI/wide SQLDriverConnect captures at most 1 MiB of UTF-8 input before parsing;
SQLConnect applies the same fixed bootstrap ceiling independently to DSN, user
and password. NTS scans and explicit lengths are bounded before copies; wide
conversion checks actual UTF-8 expansion before append. This limit cannot be
raised by options in the input being captured and is separate from the later
resolved MaxConnectionFieldBytes limit. Limit rejection returns HY000 before
configuration lookup, provider/session creation or output writes. Existing
connected sessions survive rejected captures; malformed wide input within
budget retains 22018 and invalid lengths retain HY090. Contract tests cover
exact/overflow capture, expansion, all three fields, explicit slices/empty,
output preservation and recovery. Configuration-file ingestion, standalone
helpers and exact heap accounting remain outside this evidence; SEC-4 stays open.

### ODBC SQLNativeSql capture budgets (S2 implementation evidence)

ANSI/wide SQLNativeSql uses the accepted connection MaxSqlBytes before input
copy/conversion and dialect translation. NTS/explicit capture counts UTF-8 bytes,
including wide expansion. Overflow reports HY000 without translation calls,
backend execution, disconnect, output writes or length writes. Existing null,
length and not-connected validation order, embedded-NUL syntax errors and
within-budget malformed-wide 22018 remain intact. Tests cover exact/overflow,
expansion, empty/zero budgets, validation precedence and recovery. This bounds
raw input only; translated output and translator-internal allocation budgets,
configuration-file ingestion and complete heap accounting remain open.

### SEC-3 quarantine checkpoint — 2026-10-01

The legacy pool is now test/example-only: production source/object partitions,
shipped core/driver libraries and installed header/example surfaces exclude it.
Negative build/install guards and artifact inspection protect the quarantine;
existing fixtures remain enabled. This supersedes its former production exposure,
not the unsafe copyable/manual-release ownership model. Exclusive protocol locks
remove concurrent exchange overlap in its wrapper. Replacement move-only leases,
reset/retirement, credential generations and cache isolation remain required
before any SDK/Driver Manager pooling claim.

## S2 PostgreSQL cleanup evidence — 2026-10-01

The explicit same-authenticated-session reset profile uses ROLLBACK where needed
and DISCARD ALL under one absolute deadline. Native completion validation occurs
before SDK normalization; every cleanup failure retires the connection and
removes replay-safety hints. Wrong or ambiguous completions cannot authorize
reuse. Redshift and unconfigured providers expose no reset facet.

This closes only the server-cleanup primitive portion of SEC-3. Cross-borrower
authorization, credential freshness/generations, shared cache invalidation and
exclusive leases remain required. The prototype pool stays quarantined. The live
PostgreSQL reset suite is mandatory when selected and cannot skip connection
failures; no pooling or Driver Manager reuse qualification follows from it.

## S2 fail-closed exclusive ownership — 2026-10-01

The internal composition-owned `SessionOwner`/`SessionLease` primitive uniquely
adopts one session and admits at most one move-only borrower. An active borrower
survives owner destruction; all returns/abandonment retire and destroy the physical
session. Backend disconnect/destruction occurs outside the ownership mutex and
noexcept teardown contains disconnect failures. No reset/probe/reconnect or replay
is hidden in teardown. Borrowed pointers and operations require caller serialization.

Concurrent checkout and owner-destruction/retirement races have focused TSan
evidence; mandatory PostgreSQL live tests verify physical-session retirement.
This closes the initial ownership primitive portion of SEC-3, not reuse safety.
Credential expiry/generations, cache isolation/invalidation and reusable return
policy remain open. The API is neither installed nor part of SDK contracts, and
ODBC/the prototype pool do not consume it.

## S2 credential-generation admission primitive — 2026-10-01

Private, composition-owned authorities start revoked, publish opaque generations
after trusted authentication validation, and invalidate prior tokens on rotate,
revoke, expiry, destruction or allocation failure. Tokens carry no secret-bearing
strings or serialized IDs. Expiry uses a strict monotonic boundary sampled under
the authority lock. Authorities represent complete immutable security contexts
chosen by trusted composition; tokens do not verify that semantic binding or
isolate hostile in-process plugins.

A bound SessionOwner checks exact authority/generation before currentness. Missing,
foreign and wrong-generation inputs cannot disconnect another valid owner. Exact
stale binding detection closes admission and retires idle sessions outside locks.
Active leases are not interrupted; every return still retires. This is admission
policy evidence, not production credential-provider integration or pooled reuse.

Cache-generation claims remain open because direct backend resets are not yet
observable by the owner. A coordinator-owned reset/invalidation boundary is
required before cache tokens or reusable return are accepted. Existing ODBC
workflows and prototype pooling remain unchanged.

## S2 coordinator cleanup evidence — 2026-10-01

Explicit private lease cleanup retires on unsupported/failed/ambiguous/throwing
or late reset outcomes. It cannot turn a backend retry hint or a protocol-ready
snapshot into new-borrower authorization. Same-borrower success requires exact
Idle/Reusable plus connected/Idle passive state within the original deadline.
No backend calls run under ownership or credential mutexes. Exception diagnostics
use fixed text; backend errors stay owning trusted-consumer diagnostics.

Every lease return remains terminal. This is not cache isolation or pool reuse
qualification: direct raw-facet reset remains possible and must be guarded/routed
before issuing cache scope tokens or enabling any reusable-return path.

## S2 borrower mutation boundary — 2026-10-02

The private lease exposes no raw physical session/facet. Borrower execution goes
through typed lease methods and reset through the coordinator. Retire outcomes
or thrown execution exceptions destroy the physical session before the outcome
returns/rethrows; returned owned diagnostics retain their original operation-time
meaning. There is no automatic retry/replay. Same-borrower outcomes do not permit
sharing/requeue. Trusted composition must not retain pre-adoption pointers;
this encapsulation is not an in-process plugin sandbox.

Raw reset access is closed for ordinary callers. Cache isolation/reusable return
remain unqualified: SQL-driven session/schema changes, resets, retirement and
credential rotation require conservative cache scope/invalidation evidence.

## S2 local cache validity evidence — 2026-10-02

Weak opaque tokens bind one authenticated Idle lease and generation; requesting
leases must validate origin affinity as well as current scope. They retain no
physical session, identity object or secret payload. Execution/reset attempts,
closure, retirement and credential staleness invalidate conservatively. Passive
eligibility checks run unlocked and are revalidated under the fixed owner-to-
authority order; unknown/non-Idle/throwing checks deny scope without interrupting
an active borrower. Factory failure leaves no new capability.

Validation is point-in-time, not a reservation or a proof of external catalog
freshness. Real caches still require bounded payloads, secret exclusion, keys,
external-change revalidation and observability. Reusable return and production
credential-provider integration remain unqualified.
