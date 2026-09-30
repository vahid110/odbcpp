# Cryptography provider and linkage plan

Decision date: 2026-09-30.
Status: accepted build and SDK contract; implementation and qualification are
open in S2C.

## Decision

The driver product chooses one cryptography implementation and one dependency
linkage profile at build time. The ODBC connection string and DSN select TLS
policy and trust inputs, but cannot select a cryptography library. This keeps a
host process from loading a different implementation for each connection and
makes the shipped artifact, security updates and support claim auditable.

The planned CMake interface is:

| Setting | Initial values | Meaning |
|---|---|---|
| `ODBCPP_CRYPTO_PROVIDER` | `OPENSSL`, `AWS_LC` | Cryptography and TLS implementation used by the artifact |
| `ODBCPP_CRYPTO_LINKAGE` | `SYSTEM_SHARED`, `BUNDLED_SHARED`, `BUNDLED_STATIC` | Where the selected implementation comes from and how it is linked |
| `ODBCPP_CRYPTO_ROOT` | path, profile-dependent | Exact dependency prefix for a bundled profile |

These settings replace direct use of `OPENSSL_USE_STATIC_LIBS` as the product
contract. That CMake variable may remain an internal discovery input during
migration, but it is insufficient evidence of which library was actually
linked. The ODBC driver itself remains a shared library required by the Driver
Manager; this decision controls its cryptography dependency independently.

Strict provider/linkage profiles and the configure-time evidence manifest are
implemented. No matrix row is qualified yet. `AWS_LC` is planned and remains
unsupported until it passes the qualification matrix below.
Selecting an unknown provider, an impossible linkage mode, or headers and
libraries from different installations must fail configuration.

FIPS is a separate, explicit product profile. A provider name, static linkage,
or an AWS-LC build does not by itself establish a FIPS claim. Any future FIPS
profile must identify the exact validated module, build settings and operating
environment and must pass its own evidence gate.

## Architecture boundary

Provider APIs stay behind private transport/security adapters. Shared SDK,
backend and public extension headers expose provider-neutral concepts:

- TLS client configuration, trust source, peer name and verification result;
- secure random bytes;
- SHA-256, HMAC, PBKDF2, constant-time comparison and secure cleansing needed
  by authentication;
- provider/version/linkage identity for diagnostics and the artifact manifest.

Provider handles and constants such as `SSL_CTX`, `SSL`, `X509` and OpenSSL
error codes cannot cross the private boundary. Runtime workflows contain no
provider switch. PostgreSQL, MySQL and later Redshift receive the same security
contracts regardless of the selected implementation.

The authentication primitives and both synchronous and asynchronous TLS
workflows now use provider-neutral private adapters. PostgreSQL SCRAM/MD5 and
transport code no longer include provider headers or invoke provider APIs.
OpenSSL objects, certificate matching, SNI selection, error-queue handling and
memory-BIO operations are owned by one private security implementation. An
automated architecture check rejects provider includes, types and calls in the
transport layer. AWS-LC remains an implementation and qualification task, not a
relink-only claim.

The private primitive adapter reports compile-time and loaded runtime provider
versions plus active FIPS state. A cross-platform binary inspector verifies
whether the driver has shared crypto dependencies or embeds them. On Linux and
macOS it also resolves shared provider libraries to canonical paths and hashes
the loaded files. Bundled-static evidence separately hashes the configured
controlled-prefix link inputs without treating those inputs as proof of the
embedded bytes' origin. Windows packaged-loader origin, static provenance,
symbol visibility and coexistence evidence remain required before any matrix
row is qualified.

## Linkage profiles

- `SYSTEM_SHARED` resolves a supported installed provider and records the exact
  loaded dependency. It requires compatible system libraries at deployment and
  rejects `ODBCPP_CRYPTO_ROOT` to avoid disguising a private prefix as a system
  profile.
- `BUNDLED_SHARED` uses an exact controlled prefix prepared by a pinned project
  build recipe or supplied through `ODBCPP_CRYPTO_ROOT`, packages those libraries
  next to the driver, uses safe platform loader paths, and includes their
  licenses and update responsibility.
- `BUNDLED_STATIC` uses the same controlled-prefix rule and embeds the selected
  provider in the driver artifact. It
  requires symbol visibility and coexistence checks because an ODBC host may
  already contain another OpenSSL-compatible implementation.

Every claimed profile must be inspected rather than inferred from configuration:
ELF dependencies and symbol visibility on Linux, Mach-O load commands on macOS,
and PE imports plus DLL search behavior on Windows. Packaging must stop
hard-coding OpenSSL DLL names and consume the selected profile manifest. A
driver-only package must not require provider development files. A separately
installed static SDK target must encode and export its exact provider dependency;
driver runtime linkage and SDK consumer linkage are distinct package contracts.

## S2C work package and qualification

S2C is bounded at **5–9 base engineering days**, with **7–12 days including 30%
contingency**:

1. Add strict provider/linkage cache variables, compatibility checks and a
   generated build manifest.
2. Move TLS objects and cryptographic primitives behind private adapters and
   remove provider types from installed extension headers.
3. Preserve and qualify OpenSSL shared and static profiles.
4. Pin and qualify an AWS-LC non-FIPS Linux shared and static profile, including
   SCRAM and certificate/hostname verification.
5. Make packaging, dependency inventory, license/SBOM inputs and diagnostics
   consume the manifest.

The bounded G12 matrix is:

| Operating system | Provider | Linkage |
|---|---|---|
| Linux | OpenSSL | `SYSTEM_SHARED`, `BUNDLED_STATIC` |
| Linux | AWS-LC | `BUNDLED_SHARED`, `BUNDLED_STATIC` |
| macOS | OpenSSL | `SYSTEM_SHARED` |
| Windows | OpenSSL | `BUNDLED_SHARED` |

This matrix proves both provider choices and all three linkage models while
preserving today's primary packaging paths. Other combinations, including
AWS-LC on macOS/Windows and OpenSSL static on those platforms, remain unclaimed
until separately qualified. Packaging changes in S2C are limited to these rows.

Qualification requires the common TLS/authentication suites, invalid certificate
and hostname rejection, custom trust, downgrade rejection, SCRAM vectors and
secret-cleanup tests. Artifact inspection must prove the requested linkage.
A coexistence test loads the driver in a process that already has a different
OpenSSL-compatible library and checks for symbol or loader collisions. The
build manifest records provider, compile-time version, linkage, origin or source
hash, claimed FIPS module/profile identity and license inventory. Runtime
diagnostics record the loaded provider version, active FIPS state and effective
connection security policy without exposing secrets.

G12 requires the qualified OpenSSL profiles and the bounded AWS-LC Linux proof.
macOS, Windows, other providers and FIPS are claimed only after their individual
matrix entries pass. G13 decides the final public variant matrix, stable SDK
packaging and long-term update policy.

## Stop rules

- Do not weaken verified TLS, authentication or trust behavior to make a provider
  compile.
- Do not add provider conditionals to backend or ODBC workflows.
- Do not claim a linkage mode from a CMake cache value without inspecting the
  resulting artifact.
- If AWS-LC requires broader protocol or public-ABI changes, stop at the Linux
  proof, record the gap and re-estimate rather than expanding S2C silently.

AWS-LC documents OpenSSL-compatible headers and libraries, CMake integration,
shared/static builds, cohabitation packaging and collision constraints in its
official [incorporation](https://github.com/aws/aws-lc/blob/main/INCORPORATING.md)
and [build](https://github.com/aws/aws-lc/blob/main/BUILDING.md) guides.

## S2C Windows packaged-loader evidence — 2026-09-30

Windows packaging consumes exact OpenSSL 3 PE import basenames from
artifact-hash-bound evidence instead of assuming DLL names. It stages those
files only from the controlled dependency root, verifies source and staged
hashes, and records the relationship in the package inventory. Fresh-runner
acceptance verifies actual loaded paths and runtime version with hostile
same-name PATH decoys present, and proves that a missing app-local runtime is
not replaced by a PATH copy.

This closes controlled staging and fresh-process packaged-loader evidence for
the Windows OpenSSL `BUNDLED_SHARED` candidate. Preloaded same-basename module
coexistence, symbol isolation and the remaining matrix evidence are still open;
`qualificationClaimed` remains false. No AWS-LC or FIPS claim is made.

## S2C driver export isolation — 2026-09-30

The shared ODBC driver now exports exactly the 76 canonical ODBC entry points
on Linux, macOS and Windows. One allowlist generates the GNU linker script,
Mach-O exported-symbol list and PE definition file; artifact inspection compares
the finished binary against it exactly and rejects missing, unexpected, aliased
or forwarded exports. The static core remains unchanged, while the shared
driver's provider libraries are private link dependencies.

This closes the driver-binary export portion of symbol isolation, including the
requirement that statically incorporated provider symbols stay out of the
driver's dynamic export table. It does not prove bundled shared-library symbol
coexistence or a process that preloaded a same-basename provider. Qualification,
AWS-LC and FIPS claims remain open.

## S2C Linux OpenSSL static artifact gate — 2026-09-30

CI now creates an ephemeral controlled OpenSSL prefix containing the installed
headers and static archives, configures the Linux `BUNDLED_STATIC` candidate,
and runs the common unit, TLS, authentication, architecture and export-surface
suite. The gate dereferences and bounds copied headers, selects both archives
from the active multiarch `libssl-dev` package, and retains its package version,
source paths and hashes. Artifact evidence binds copied archive hashes to the
driver, verifies that the finished ELF artifact has no dynamic OpenSSL
dependency, and keeps embedded-byte origin and qualification false.

This establishes a repeatable artifact gate for the matrix candidate. It does
not complete provenance, live TLS/ODBC qualification, coexistence or the AWS-LC
proof, so the Linux OpenSSL static row remains unqualified.

## S2C Linux OpenSSL static live gate — 2026-09-30

The static candidate now runs mandatory unixODBC tests against PostgreSQL 17
using verified TLS, a private trust root and SCRAM authentication. A successful
query is required; an untrusted certificate and a hostname mismatch must both
fail through unixODBC, while direct driver calls retain the precise TLS failure
diagnostics as additional evidence. The test process preloads the system shared OpenSSL before the statically
linked driver is loaded and used, then checks that the preloaded provider
identity stays in place. The artifact gate also rejects unresolved provider
symbols, in addition to dynamic dependencies and non-ODBC exports.

Retained evidence binds these results to the driver and preloaded-library
hashes. Because the shared and static OpenSSL instances come from the same
package version, this proves duplicate-instance cohabitation only. It does not
prove cross-version or cross-implementation coexistence, embedded-byte
provenance, or AWS-LC compatibility, so `qualificationClaimed` remains false.

## S2C Linux OpenSSL static link-trace gate — 2026-09-30

The CI Linux static build now emits and retains a GNU ld linker map. Artifact
inspection requires both controlled, hashed archives to appear as loaded inputs
and as sources of extracted members, and binds the map hash plus archive hashes
to a retained copy of the exact hashed driver. The map's `OUTPUT` record must
resolve to that inspected driver; missing, stale or incomplete maps fail closed.

This is bounded link-input and member-extraction evidence. It does not prove
which extracted sections survived linker garbage collection or byte-level
ancestry of embedded code. Embedded-byte provenance, cross-implementation
coexistence and AWS-LC compatibility remain open, so the matrix row and
`qualificationClaimed` remain open.

## S2C provider header identity — 2026-09-30

OpenSSL discovery now compiles the selected headers to reject known compatible
providers, versions below OpenSSL 3.0, and version text that does not identify
OpenSSL. Both identity headers must exist within the selected include root.
The probe is repeated
on reconfiguration so a cached success cannot hide changed headers. Fixtures
cover OpenSSL, AWS-LC, BoringSSL, LibreSSL, old versions, incomplete headers,
unknown identity and recovery in the
same build directory. This verifies header identity only; it does not qualify
AWS-LC or prove the identity of a library selected by the runtime loader.
