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

The current pool exposes copyable physical-session pointers, manual release,
shared-lock protocol execution and no reset. It cannot be exposed or used as
pooling evidence. The replacement contract requires one move-only lease per
physical session, serialized operations, reset/retirement under deadline and
credential/cache isolation.

### SEC-4: unbounded aggregate server results

Per-frame checks exist, but accepted frames can be very large and complete
operations have no cumulative byte/message/row/column/result limit. Each backend
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
