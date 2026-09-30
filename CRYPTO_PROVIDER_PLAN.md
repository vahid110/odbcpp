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
