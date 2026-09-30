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

## Future FIPS readiness — architecture now, implementation deferred

FIPS is low-priority deferred product scope, not an additional S2/S2C delivery
or G12 qualification requirement. Preserve the following design properties
during existing architecture reviews; do not introduce speculative interfaces
or change current authentication behavior solely for a future FIPS profile:

- Keep cryptographic operations behind the shared private adapters across all
  drivers. Provider selection and security policy are distinct: selecting
  AWS-LC or OpenSSL does not select an approved-operation policy.
- Keep policy enforcement centralized, with backend-owned authentication
  method mappings. Future restrictions must not require scattered provider or
  FIPS conditionals in ODBC workflows or backend code.
- Leave identity/evidence reporting extensible to exact module/version,
  operating mode and operation approval evidence. Today's `fips_enabled` state
  is informational; it proves neither approved services nor driver compliance.
- Keep security context initialization and lifetime explicit. A future strict
  profile must initialize its module before sessions and fail closed on required
  initialization or self-test failures. Per-connection changes must not mutate
  process-global policy for other drivers sharing the host.
- Preserve immutable session security policy and reuse isolation. A future
  stricter policy must not inherit a pooled session established under weaker
  policy. Artifact identity and linkage evidence must remain attributable to
  the actual packaged/loaded module.

Revisit implementation only for a concrete customer/deployment requirement or
explicit reprioritization. At that point, separately scope validated-module and
operating-environment selection, permitted TLS/authentication operations,
approved-service evidence, fail-closed tests, module integrity/self-tests,
packaging and update rules, and specialist compliance review. Qualify each
claimed driver/platform profile against the selected module's security policy.
AWS-LC and OpenSSL remain candidates, not commitments to a validated profile.
Do not make FIPS mandatory for ordinary users or claim the whole driver is a
validated cryptographic module merely because it uses one.

Current milestone order, estimates and non-FIPS qualification gates are
unchanged. FIPS implementation and qualification receive a separate estimate
when activated; they must not consume the existing S2C contingency.

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

## S2C pinned AWS-LC adapter proof — 2026-09-30

An isolated CMake project under `tests/security/awslc` pins AWS-LC revision
`574fbd729ca31aeebe80a98d35742c0402435790` and verifies the downloaded archive
SHA-256. Linux CI builds its non-FIPS static and shared variants and runs the
existing cryptographic primitive, SCRAM vector/error and TLS adapter lifecycle
tests against the same private adapter sources used by the driver. Provider
identity and FIPS-state reporting are selected inside the private adapter.

This is an adapter compatibility experiment. `ODBCPP_CRYPTO_PROVIDER=AWS_LC`
remains rejected by the main build. The proof does not cover a live PostgreSQL
TLS handshake, certificate/hostname verification, driver artifact linkage or
cross-provider coexistence; all remain required before qualification. Local
macOS runs are development checks, not an additional supported profile.

Source reference: [AWS-LC incorporation guide at the pinned revision](https://github.com/aws/aws-lc/blob/574fbd729ca31aeebe80a98d35742c0402435790/INCORPORATING.md).

## S2C AWS-LC verified TLS interoperability — 2026-09-30

The isolated static/shared adapter proof now requires complete TLS 1.2 and 1.3
handshakes against an independent Python/OpenSSL peer. Fresh temporary
certificates exercise trusted DNS/IP identities, encrypted binary round trips,
wrong DNS/IP identity rejection and unrelated trust-root rejection. Negative
cases require an unverified client state and no application bytes received by
the peer; reset must clear active/verified session state. Socket, subprocess
and CTest deadlines bound failures. Python TLS 1.3 support and the OpenSSL CLI
are required fixture dependencies; unsupported runtimes fail rather than skip.

This adds adapter-level handshake/verification evidence to both Linux CI
variants. It does not establish PostgreSQL startup/authentication over TLS,
ODBC driver linkage, provider coexistence or a qualified AWS-LC driver profile.
The main AWS-LC build selection remains disabled.

## S2C AWS-LC live PostgreSQL session proof — 2026-09-30

Both isolated Linux AWS-LC variants now register mandatory PostgreSQL 17 live
checks in CI. The probe compiles the existing PostgreSQL parser, generic session
and synchronous TLS transport sources against the private AWS-LC adapters; it
does not implement a second test-only PostgreSQL client. It observes actual SASL
and SASLFinal exchanges to reject a fixture that silently uses trust or MD5,
checks server-side TLS state, and verifies direct/prepared results including NULL.

Wrong password, unrelated CA and hostname mismatch must leave the session
unconnected; certificate failures must precede authentication. Each failed
connection is followed by a fresh successful SCRAM query on the same session
object. Missing fixture settings fail tests rather than skip. Local standalone
builds opt into the fixture using `ODBCPP_AWSLC_LIVE_TESTS`; both CI variants
require it. The retained manifest records that selection, while CTest results
supply execution evidence. The disposable PostgreSQL TLS setup is shared with
the OpenSSL static gate through `tools/ci/prepare-postgres-tls.sh`.

This closes the isolated synchronous PostgreSQL TLS/SCRAM proof gap. It does not
qualify an ODBC AWS-LC driver, asynchronous transport, artifact linkage,
packaging or provider coexistence. Main-build AWS-LC selection remains disabled.

## S2C AWS-LC probe linkage evidence — 2026-09-30

Linux CI now inspects the exact executable used by the isolated live PostgreSQL
proof. Shared builds require SSL and crypto dynamic dependencies resolved to
the exact selected targets inside the controlled AWS-LC build root; static builds
require no dynamic crypto reference and a GNU ld map binding both archives and extracted members to that
executable. The inspector accepts exact absolute or build-relative archive
records and rejects filename-prefix lookalikes. Fixtures cover missing libraries,
foreign shared origins, incomplete/wrong-output maps and unsupported system
linkage. AWS-LC inspection is limited to Linux bundled profiles with a manifest.

The retained executable, provider libraries/archives, manifest and static map
are accompanied by artifact/input/resolved-file hashes. These are linkage and
link-input evidence for the session probe, not byte-level static provenance,
actual-runtime symbol binding, ODBC export isolation, relocatable packaging or
cross-provider coexistence. Provider identity still requires the companion
runtime adapter tests. Qualification remains false and main-build AWS-LC
selection remains disabled.

## S2C AWS-LC asynchronous transport proof — 2026-09-30

The isolated PostgreSQL live suite now runs the same verified TLS/SCRAM,
direct/prepared query and rejection/reconnect cases through synchronous socket,
thread-pool TLS and Linux epoll TLS transports. This exercises the production
memory-BIO TLS path without changing provider-neutral session contracts.

Both AWS-LC variants also compile and run the existing asynchronous TLS suite,
covering handshake deadlines, cancellation, queued-request capacity release,
error-queue isolation and clean versus abrupt TLS closure. The isolated session
library shares the actual transport/parser sources between those tests and the
live probe. Local macOS checks cover synchronous/thread-pool paths; Linux CI
adds the native epoll path. This is internal transport evidence, not support for
asynchronous ODBC APIs or qualification of AWS-LC packaging/coexistence. Main
AWS-LC driver selection remains disabled.

## S2C AWS-LC system-OpenSSL preload proof — 2026-09-30

The isolated Linux shared build now uses the pinned upstream distribution mode:
suffixed AWS-LC library names and ELF symbol versions. Linux proof executables
hide archive exports. Mandatory coexistence checks preload system OpenSSL 3 SSL
and crypto libraries before the executable starts, verify the host crypto path,
identity, SHA-256 operation and TLS context/cipher setup before and after AWS-LC
operations, and require AWS-LC identity and operations to
remain distinct. A missing-preload canary must fail rather than silently test a
different load order.

Crypto primitives, async TLS lifecycle and all live PostgreSQL transport cases
also run under that preload in both static/shared jobs. The host paths and hashes,
requested upstream modes, test results and probe artifacts are retained. This
is bounded executable-level evidence with the runner's OpenSSL 3 provider. It
does not prove loading a finished ODBC shared driver, arbitrary provider versions,
reverse load order, or relocatable packaging. Those driver-level gates and
qualification remain open; the main AWS-LC profile is still disabled.

## S2C isolated AWS-LC ODBC driver proof — 2026-09-30

The Linux proof now builds a real PostgreSQL ODBC shared library from the same
production source inventory and pinned logging dependency setup as the main
build. The product option remains disabled: this target is an isolated
qualification artifact. Both provider linkage variants require the canonical
ODBC export surface and artifact-hash-bound linkage inspection of that DSO.

The existing common live crypto-profile test loads this driver through unixODBC
under system OpenSSL preload, verifies a TLS query, and rejects unrelated trust
and hostname mismatch through the Driver Manager and direct driver entry points.
Temporary registration selects only the built proof driver. Test diagnostics
accept the equivalent OpenSSL and AWS-LC certificate-verification spellings.
The DSO and linkage evidence are retained alongside the session proof artifacts.

This supplies driver-level loading/ODBC evidence for the selected Linux setup.
It is not a relocated package or public SDK artifact; loader-path hardening,
package dependency/license inventory and final profile qualification remain open.

## S2C internal AWS-LC relocated package proof — 2026-09-30

A dedicated install component stages only the PostgreSQL driver, required shared
AWS-LC libraries for the shared variant, and AWS-LC/spdlog/bundled-fmt license
texts. Static driver packages omit provider archives and headers. Installed ELF
runtime paths must be exactly `$ORIGIN`, with no build-directory search path.

The mandatory Linux fixture moves the installed tree to a different directory,
resolves shared providers inside that tree, and runs the common unixODBC TLS
query and rejection suite under system-OpenSSL preload. Both Driver Manager and
direct diagnostic calls use the relocated DSO. Removing each packaged provider
must prevent a fresh load when no library-path override is supplied. The package
archive and per-file/symlink hashes are retained with relocation test evidence.

This is an internal package experiment, not a public distribution or complete
SBOM/legal review. Hostile `LD_LIBRARY_PATH`/preload substitution, host ABI breadth,
update policy and final provider/profile qualification remain open. The evidence
explicitly leaves hostile-loader-path coverage and qualification false; the main
AWS-LC product option remains disabled.

## S2C extracted AWS-LC archive evidence — 2026-09-30

The internal package proof now creates the retained archive before testing,
removes the staging tree, and runs the existing live TLS and missing-provider
checks against a fresh extraction. An independent pre-archive inventory must
match extracted bytes, symlink targets and file permission modes. The fixture
rejects duplicate archive entries, traversal and external symlinks, and tests
changed bytes, missing and unexpected files. The isolated proof requires Python
3.12 or newer for explicit standard-library data-filtered extraction; this is
not a driver runtime dependency.

This closes the archive-versus-staging evidence gap only. Inventory comparison
is not package signing or an authenticity claim. Hostile loader substitution,
final profile qualification and the disabled main-build AWS-LC option are
unchanged.

## S2C AWS-LC downgrade rejection gate — 2026-10-01

The independent TLS peer fixture now requires a TLS 1.1 control connection to
negotiate that exact protocol and complete a binary round trip. The production
adapter must then reject the same peer with the protocol-version alert and
retain no verified peer identity. This prevents a server with disabled legacy
TLS from creating a false-positive client-policy test. TLS 1.2/1.3 acceptance
and certificate/hostname rejection still run afterward in both linkage jobs.

Only the independent control fixture permits legacy TLS and lowers its cipher
security level. Production configuration is unchanged. A peer runtime unable to
complete the control fails this mandatory proof rather than silently skipping.
This supplies the below-minimum-version rejection evidence; it does not close
loader-substitution or final profile qualification.

## S2C internal package dependency manifest — 2026-10-01

The AWS-LC proof package now installs a machine-readable dependency manifest
with the selected linkage, non-FIPS/unqualified status, declared AWS-LC and
spdlog archive recipes, bundled fmt version, and hashes of each required license
text. Source overrides are recorded explicitly; declared recipe identities are
not asserted to verify an overridden source tree. The existing dependency pins
remain unchanged and are shared with manifest generation.

The extracted-package gate requires the manifest's provider/linkage and scope
claims to match the proof, and verifies all four license texts against their
recorded hashes. Focused negatives reject altered/missing licenses, incomplete
inventories, wrong profiles and premature qualification/provenance claims.
The retained relocation evidence includes the packaged manifest and its file
hash. This is dependency/license input evidence, not a complete SBOM, a source
attestation or final profile qualification. Loader substitution and the final
qualification review remain open.

## S2C Linux loaded-provider origin and substitution evidence — 2026-10-01

The extracted driver proof now inspects `/proc/self/maps` after loading the DSO
in a fresh host process. Shared AWS-LC mappings must match the exact packaged
provider paths; the static proof must map no separate AWS-LC provider libraries.
This adds actual-host origin evidence to the earlier dependency resolver checks.

Shared-profile canaries copy the known provider bytes outside the package, then
exercise `LD_LIBRARY_PATH` and `LD_PRELOAD` separately. Each must demonstrably map
an external provider and fail the qualification fixture's origin comparison.
Identical bytes do not bypass this origin check. These are detection tests using
trusted copies, not execution of attacker code or production runtime enforcement.
Evidence distinguishes `loaderSubstitutionDetected` from
`loaderSubstitutionPrevented=false`; qualification remains false. Static builds
record these shared-provider substitution canaries as not applicable.

The final qualification decision must define trusted host/loader configuration,
package-directory ownership and dependency update responsibility. `$ORIGIN`
alone does not enforce that trust boundary. The main AWS-LC option stays disabled
until that decision and the remaining profile evidence are accepted.

## S2C bounded AWS-LC proof acceptance — 2026-10-01

Acceptance scope is the pinned non-FIPS Linux experiment, in both static and
shared linkage jobs, under the disposable PostgreSQL 17 and system-OpenSSL host
fixtures. The package manifest records the compile version. A retained adapter
probe record binds compile/runtime versions, inactive FIPS state and the TLS
probe's effective defaults (TLS 1.2 minimum, peer and hostname verification,
explicit CA in live cases) to probe and manifest hashes. Mandatory negative
checks reject missing fields, version/provider mismatches and weaker policy.
This record describes the adapter probe, not arbitrary end-user connections or
production-driver runtime identity enforcement.

Within this proof, the complete evidence set is now finite: private boundaries,
primitive/SCRAM vectors and cleansing, verified TLS 1.2/1.3 and controlled legacy
rejection, sync/thread-pool/epoll sessions and recovery, ODBC TLS queries and
rejections, export/linkage inspection, system-OpenSSL coexistence, extracted
packages with dependency/license inventories, and loaded-origin/substitution
checks. Acceptance requires both Linux linkage jobs and all protected platform
gates to pass for the commit containing this closure record. Earlier green
checkpoints are supporting evidence, not a substitute for that final run.

Deployment assumptions and responsibility:

- The embedding application and administrator control loader environment,
  native preload modules and driver registration. Ambient providers may coexist
  only within the tested symbol-isolation boundary. Arbitrary hostile native
  code in the host is outside this in-process proof's protection scope.
- The package directory and its parents are owned by the deployment administrator
  and are not writable by untrusted users. Shared provider origins must remain
  inside the controlled package. External overrides fail qualification origin
  checks; runtime prevention is not claimed.
- ODBCPP maintainers own the pinned provider/logging recipe updates and license
  inventory. A dependency or compiler/linkage recipe update reruns the complete
  proof and regenerates hashes. Consumers must replace/redeploy the matching
  package; replacing one bundled provider is not a qualified update.

After the final run is green, the bounded AWS-LC Linux proof is accepted for the
S2C/G12 architecture milestone. This does not qualify a public product matrix
row, enable `AWS_LC` in the main build or close all S2C/G12 work. Public variant
support, complete SBOM/attestation/signing and expanded platform/host coverage
remain G13 decisions; FIPS stays deferred. No further unplanned AWS-LC proof
expansion is required unless a gate fails or the accepted contract changes.

Remaining S2C closure work: reconcile each OpenSSL row against the same finite
requirements, fill only demonstrated missing evidence, then perform one matrix
acceptance review before returning to S2 error/lifecycle migration. The AWS-LC
source tree is still composed separately for this experiment; main-build provider
selection remains a separately reviewable integration step before product use.

## S2C Linux OpenSSL mandatory TLS evidence — 2026-10-01

The production OpenSSL adapter now reuses the same independent peer/probe as the
accepted AWS-LC proof. Linux system-shared and bundled-static CI explicitly enable
`ODBCPP_CRYPTO_TLS_PROOF`: TLS 1.2/1.3 acceptance, custom trust, DNS/IP mismatch,
unrelated CA rejection and controlled TLS 1.1 rejection are mandatory. Provider
error spelling differences are handled only in the shared test probe.

Linux system-shared additionally enables the existing actual-driver unixODBC
TLS/SCRAM query and wrong-CA/hostname rejection cases against a disposable TLS
PostgreSQL fixture. The runner verifies exactly those cases passed without skips.
Existing sync/epoll/auto non-TLS suites still run before this dedicated fixture.
JUnit results and probe/driver/manifest artifacts are retained for both Linux rows.
The probe switch is opt-in test infrastructure, not a runtime driver option.

This fills the demonstrated Linux TLS gate gap only. The OpenSSL rows remain
unqualified pending their finite coexistence/identity/package acceptance work;
macOS and Windows TLS fixtures are the next platform gaps. No additional AWS-LC
proof or public/FIPS scope is introduced.
