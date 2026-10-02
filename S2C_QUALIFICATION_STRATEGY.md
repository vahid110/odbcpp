# S2C qualification strategy review

Review date: 2026-10-02. Reviewed baseline: `77b0a44`, branch
`codex/s2c-qualification-review`. This is a finite source review and implementation
recommendation, not qualification evidence. No build, CI, runtime implementation,
commit or push was performed. No applicable AGENTS.md was found in the checkout
or its ancestor directories. This report is the only authorized file change.

## Remaining obligations

**T13 is production Windows isolation, not another successful collision detector.**
RELEASE_PLAN.md:311 requires one separately selected and estimated prevention
strategy, followed by live/package acceptance before qualifying or releasing the
Windows crypto profile or closing a gate requiring it. S2C_MATRIX_REVIEW.md:32,
44–47 and CRYPTO_PROVIDER_PLAN.md:736–761 preserve that blocker.

The source makes the distinction concrete: tests/windows/it_package_load.cpp:94
loads the driver before resolving bound imports at lines 100–109. The foreign
case returns 3 only after discovering that the driver uses foreign modules.
tools/ci/windows-package.ps1:118–120 requires that collision; lines 167 and 175–176
retain prevention, general coexistence and qualification as false. More hashes
or version checks cannot fix this: the canary intentionally uses identical bytes.

**T14 is explicit per-row acceptance against the existing finite requirements.**
RELEASE_PLAN.md:312 and S2C_MATRIX_REVIEW.md:48–51 require the integration owner to
decide the supported-host compatibility model and provenance scope; they do not
waive missing mandatory evidence. Remaining demonstrated limits are:

| OpenSSL row | Evidence still requiring resolution/signoff |
|---|---|
| Linux SYSTEM_SHARED | Configured-provider host digest/TLS-context lifecycle is established; live queries inside that same cohabitation host and different/incompatible-provider coexistence are not. Define the supported compatibility contract, fill required missing evidence, and sign off. |
| Linux BUNDLED_STATIC | Existing preload/live/archive evidence covers one OpenSSL package. Different-implementation coexistence and embedded-byte ancestry are not established. Decide the permitted provenance claim against the actual gate, supply any required missing proof, and sign off. |
| macOS SYSTEM_SHARED | Both Unicode widths have bounded TLS/live/identity evidence and configured-provider host lifecycle. Same-host live/coexistence and broader incompatible-host support remain unresolved. Sign off each relevant width with its exact retained artifacts. |
| Windows BUNDLED_SHARED | Resolve T13, then establish combined installed-package TLS/authentication, coexistence and lifetime evidence; sign off with exact PE imports, origins and inventories. |

These limits are explicit at S2C_MATRIX_REVIEW.md:29–39. The Linux live-summary
addition at lines 82–94 binds the three successful live cases to input hashes;
it explicitly does not evaluate package/cohabitation acceptance or close T13/T14.
The independent TLS/downgrade gap is already described as filled by the retained
Windows gate; do not resurrect it as missing work without a failed gate.

The plan's common acceptance requirements remain TLS 1.2/1.3, custom trust,
certificate/hostname and downgrade rejection, SCRAM/primitives and cleansing,
artifact/linkage/export inspection, a host with a different OpenSSL-compatible
library, provider identity, origin/provenance and dependency/license evidence
(CRYPTO_PROVIDER_PLAN.md:139–185). A host-assumption decision cannot silently
replace the different-provider coexistence requirement with the current
configured-provider fixture. Any genuine gate narrowing needs an explicit
integration decision; this report proposes none.

## Recommended bounded Windows strategy

Select **a pinned private OpenSSL DLL namespace with centralized controlled
resolution before first provider use**, preserving OPENSSL/BUNDLED_SHARED.
Private names alone resolve the ordinary ambient OpenSSL name collision, but
cannot reject a foreign copy bearing the new private basename. The resolution
component is therefore part of this one strategy, not a later optional gate.

1. Build x64 non-FIPS OpenSSL from a pinned archive/hash into a controlled prefix.
   Use a project-specific build variant including recipe/version identity; build
   matching import libraries and both DLLs together. Do not rename files after
   linking: the driver's imports and SSL's transitive crypto import must contain
   the private names. Record recipe, license and DLL/input hashes.
2. Make driver references to those two libraries delay imports, with one private
   Windows resolver/notification hook owned by the security layer. Before any
   crypto or TLS operation, initialize once outside DllMain: derive the containing
   driver directory, reject foreign modules with the private basenames, require
   the expected app-local files, load crypto then SSL by absolute path with
   restricted dependency search, verify file identity/origin and SSL's bound
   crypto import, and permit only the verified module handles. A hook must never
   fall back to the default unrestricted loader when resolution fails.
3. Fail initialization through the existing private security/error boundary;
   prevent digest, random, SCRAM, identity and TLS calls from bypassing it. No
   loader calls from DllMain, static initializers, ODBC entry-point scatter or
   process-global SetDllDirectory/PATH changes. Keep verified provider references
   alive across sessions and overlapping driver references; define shutdown
   behavior before adding any unload mechanism.

Microsoft documents that dependency searches use module names even when the
top-level DLL was loaded by absolute path, and that the loaded-module list
precedes directory searching. This explains why existing search flags do not
solve the preload gate. See [DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order).
MSVC supports delay-import hooks and prohibits calling delay-loaded functions
from DllMain; a hook can supply the library handle. See [delay-loaded DLL support](https://learn.microsoft.com/en-us/cpp/build/reference/linker-support-for-delay-loaded-dlls?view=msvc-170)
and [notification hooks](https://learn.microsoft.com/en-us/cpp/build/reference/error-handling-and-notification?view=msvc-170).
OpenSSL 3.6.4's [Windows naming implementation](https://github.com/openssl/openssl/blob/openssl-3.6.4/Configurations/platform/Windows.pm)
uses shlib_variant in the shared DLL name. These are feasibility inputs, not proof
that the proposed recipe and every adapter reference work with delay loading.

This contract prevents ordinary ambient-provider collision and rejects deliberate
foreign private-name preload before driver provider operations. It does not undo
execution of native code the host already loaded, protect against arbitrary
in-process patching, or prove that a malicious concurrently loading host cannot
race native loading. State the supported cooperative host/loader contract in the
owner's T14 decision; do not label post-binding detection as prevention. If the
actual T13 acceptance requires isolation even under concurrent hostile native
loader manipulation, stop and re-estimate rather than claiming this design meets
it. Existing deployment assumptions appear at CRYPTO_PROVIDER_PLAN.md:524–537.

**Estimate:** 6–10 engineering days, plus 2–3 days contingency/native CI repair,
separately funded from the historical S2C estimate: 1–2 recipe/discovery days,
2–3 resolver/error/lifetime days, 2–3 live/package/coexistence days, 1–2 evidence
and review days. Stop after this Windows x64 row; no AWS-LC Windows, public SDK,
FIPS, protocol redesign or new provider abstraction in this batch.

Alternatives: private naming without controlled resolution is smaller (roughly
3–5 days) but leaves private-basename substitution and cannot alone close the
existing prevention obligation. Windows BUNDLED_STATIC removes shared-provider
imports, but substitutes a different matrix row, requires archive/provenance,
exports and package changes, and cannot silently qualify the required
BUNDLED_SHARED row. An out-of-process security helper changes transport and
lifecycle architecture substantially. Keeping only the probe detector changes
no production behavior and does not satisfy T13.

## Files and acceptance tests

Windows-local implementation can own a new pinned recipe and naming config under
packaging/windows/, packaging/windows/build.ps1 and test-inputs.ps1,
tests/windows/it_package_load.cpp, a new Windows host/coexistence test, and
tools/ci/windows-package.ps1 plus windows-crypto-profile-live.ps1.

Coordinate edits to CMakeLists.txt, cmake/DriverSources.cmake,
cmake/Tests.cmake, cmake/InspectCryptoArtifact.cmake and its adversarial tests,
cmake/GenerateCryptoManifest.cmake, core/security/openssl_crypto.cpp,
core/security/openssl_tls_client.cpp, .github/workflows/ci.yml, common evidence
recorders, and plan/review documents. The inspector currently admits only the
canonical OpenSSL 3 DLL names (InspectCryptoArtifact.cmake:60–66); extend it to
the exact selected private recipe, not a wildcard that accepts arbitrary SSL
DLLs. Packaging already stages manifest-described import names from the
controlled prefix (packaging/windows/build.ps1:90–106). Preserve those checks.

Meaningful acceptance must run in fresh Windows processes and retain driver,
recipe, provider, manifest, installed-inventory and report hashes:

- Positive: exact installed private DLL preload; ordinary same-name ambient
  OpenSSL from another controlled prefix/version preloaded first; reverse order;
  repeated overlapping driver references and connections. Verify driver-bound
  and SSL-bound imports serve package-private modules. Execute host digest and
  TLS-context operations before/during/after driver lifetimes and an actual
  installed-driver TLS/SCRAM query in the same host, through Driver Manager and
  direct entry points. Verify the host's provider addresses/origins stay intact.
- Negative: byte-identical foreign private-basename copies; both same version
  and compatible different version fixtures; missing/invalid preload inputs;
  missing each installed private DLL with valid PATH/executable-directory copies;
  missing export, mismatched architecture/recipe and tampered package inputs.
  Require production initialization/connection rejection before crypto/query
  execution, safe ODBC diagnostics, no fallback, no crash and continued host
  operations. Do not accept the probe's current exit 3 as production prevention.
- Security regressions: installed-package wrong CA and hostname cases reject;
  independent peer TLS 1.2/1.3 and controlled TLS 1.1 rejection remain mandatory;
  SCRAM/primitive/cleanup and exact 76 driver exports remain unchanged.
- Packaging: repeat MSI install, upgrade, intentional rollback, downgrade and
  foreign-registration/uninstall ownership checks. Run installed-package TLS
  tests after install and upgrade, plus rollback identity/live checks. Today's
  fresh-runner package starts PostgreSQL without EnableTls
  (windows-package.ps1:178); the TLS script hardcodes a build-tree executable
  (windows-crypto-profile-live.ps1:12). Transfer and parameterize the real live
  test for installed-package execution; it is not already combined evidence.
- Evidence failure tests: reject missing/skipped/duplicated cases, wrong driver
  or provider hashes, absent private resolver markers and stale success output.
  Inspect normal and delay import tables and transitive SSL imports explicitly.

## Independence and AWS-LC handoff

Everything above can proceed independently of MySQL: it uses PostgreSQL fixtures,
the existing private security boundary and Windows packaging. Windows-local
recipe/tests can be developed independently once the strategy is selected;
common adapter/CMake/CI and acceptance-document edits need integration ownership
and rebasing, particularly while S2/MySQL changes share source inventories.

**Main-build AWS-LC should wait for this batch and explicit owner scheduling.**
Its accepted Linux experiment must not be reopened indefinitely, but removing
CryptoProfile.cmake:34–36 is insufficient. Main discovery is still FindOpenSSL
(FindDependencies.cmake:17), header identity rejects OPENSSL_IS_AWSLC
(VerifyCryptoIdentity.cmake:25), both main targets link OpenSSL targets
(CMakeLists.txt:192–196,214–218), manifest generation uses OpenSSL discovery
(GenerateCryptoManifest.cmake:8–14), and negative profile tests require AWS-LC
rejection (TestCryptoProfileValidation.cmake:25–27).

The separate integration batch must provide provider-specific controlled-prefix
discovery/identity and neutral imported target wiring; carry the accepted pinned
non-FIPS Linux shared/static recipe, suffixed SONAME/symbol-isolation and static
link visibility into the production target; generate truthful provider/version,
origin and license evidence; install/relocate the main runtime package; and run
the same TLS/auth/session/ODBC/coexistence/export/origin negative gates against
the main artifact. Preserve strict unsupported-combination and stale-profile
failures. Public static SDK dependency/export contracts remain a separate G13
decision. References: tests/security/awslc/CMakeLists.txt:9–30 and
DriverProof.cmake:1–12,30–69; S2C_MATRIX_REVIEW.md:52–55;
CRYPTO_PROVIDER_PLAN.md:539–551. An accepted experiment is not main-build or
public product qualification.

The next batch is finite: select this Windows strategy, implement its one x64
profile with the stated tests, submit exact-revision evidence to the integration
owner, and leave T13/T14 and all qualification flags open until that acceptance
decision. Broader OpenSSL reconciliation and main AWS-LC integration get their
own bounded batches; S2/MySQL work need not wait for this strategy review.
