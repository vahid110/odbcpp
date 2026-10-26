# Versions and beta release candidates

`version.json` is the sole product version authority. `tools/release/version.py`
derives CMake's numeric version, `SQL_DRIVER_VER`, Windows driver/setup DLL
resources, package versions and a generated build identity. Do not independently
edit these values in C++, PowerShell or CI. Both PostgreSQL and Redshift products
use the same source version but retain separate driver registrations and MSI
UpgradeCodes. Git revision and tracked-source dirty status are separate from
SemVer ordering. Source archives and vendored exports also build without Git:
their provenance explicitly says `export`, revision/cleanliness are unknown, and
the display suffix is `+export.unknown`. An export nested in another repository
never borrows that repository's HEAD. Missing Git cannot block ordinary version
derivation. Release packaging/sealing requires exact, clean Git provenance and
refuses exported/unknown source identities.

Package from a fresh, clean, exact-commit configuration; the
packager rejects a stale configure-time identity. Reconfigure after a commit.

The proposed first tagged release is **1.1.0-beta.1**, not a declaration that a
stable 1.0 release ever occurred. Existing test installers used 1.0.0 (including
the Windows qualification installation), 1.0.1 and 1.0.2. No tag or release is
created by committing this policy. The beta designation means no stable SDK/ABI
compatibility promise has been made.

## Numeric representations and upgrades

| Consumer | Proposed beta identity | Meaning |
|---|---|---|
| SemVer / release name | `1.1.0-beta.1` | Authoritative product version |
| CMake | `1.1.0` | Numeric portion only |
| ODBC `SQL_DRIVER_VER` | `01.01.0000` | Required numeric identity; consult installed JSON for prerelease and revision |
| MSI `ProductVersion` | `1.1.1` | Ordered installer representation |
| Windows DLL `FileVersion` | `1.1.1.0` | Same ordered representation, fourth field zero |
| DLL `ProductVersion` / JSON display | `1.1.0-beta.1+g<revision>` | Human-readable source provenance; dirty development builds carry `.dirty` |

MSI uses only three numeric fields. Its limits are 255, 255 and 65535.
Our major/minor limit is stricter (99) to preserve ODBC's two-digit fields.
Each SemVer patch reserves 1000 installer values: `patch * 1000 + rank`, with
beta ranks 1–499, RC ranks 500–998 (`499 + sequence`), and stable rank 999.
Patch is limited to 64 and beta/RC sequence to 499; exhausting a range requires
a deliberate minor/major increment, never wraparound. Thus beta.499 < rc.1 <
rc.499 < stable < next-patch beta.1. A minor/major increment also increases MSI
ordering. `1.1.1` upgrades all historical `1.0.x` test versions, while stable
1.1.0 will be `1.1.999`. Persistent product UpgradeCodes are unchanged.
See [Microsoft ProductVersion](https://learn.microsoft.com/en-us/windows/win32/msi/productversion).

Every distributed rebuild must increment the semantic prerelease sequence (or
subsequent version). Git metadata is traceability, not an installer update rank.
Never replace an existing tag, release asset or installer under the same version.
Developer/rollback tests may pass `-LifecycleFixture -Version 1.0.x`; those files
are explicitly marked non-release and the release sealer rejects them. The
historical lifecycle scripts deliberately retain these fixture versions.

CMake SDK consumption uses exact numeric-version matching rather than promising
compatibility with arbitrary future major versions. A prerelease additionally
requires `set(ODBCPP_ACCEPT_PRERELEASE "1.1.0-beta.1")` before
`find_package(odbcpp 1.1.0 EXACT CONFIG REQUIRED)`. Other prereleases are refused.
This acknowledgement does not promise ABI compatibility between builds. The
manifest's `abi` controls the shared-library SOVERSION; changing public ABI needs
an explicit version/ABI review. End-user packages do not include SDK archives,
headers or examples. SDK distributions are a separate future artifact.

## Repeatable candidate pipeline

Run **Beta release candidate (draft only)** manually on `main`, after its required
platform CI and review are green. Dispatch selects that immutable Git SHA. The
workflow does not accept an arbitrary candidate branch or run for PR events.

1. Build each Windows x64 product once and run its unit suite from that build.
2. Stage the same DLLs and their actual OpenSSL/MSVC runtimes. Include the project
   MIT LICENSE, third-party notices, DLL versions and dependency hashes.
3. Load the staged driver/setup/runtime files using the native package loader;
   inspect both actual MSI databases, identities and payload inventories.
4. Seal the unchanged MSIs, unit XML, version/license/dependency evidence with
   SHA-256 inventories and machine-readable provenance. Upload immutable workflow
   artifacts. Preserve failures separately; no implicit retry or cancellation.
5. Optionally download those exact artifacts in the draft job, verify their hashes
   and source, then create a **draft prerelease**, with a transport archive per
   product containing the original MSI/evidence bytes. Publishing never rebuilds
   binaries. Existing tags/releases, including unpublished drafts found through the paginated
   authenticated release listing, are refused; no overwrite/clobber path exists.

Default token permission is read-only; only the optional main-only draft job has
contents-write permission. Checkout credentials are not persisted. There is no
automatic public publication. Artifacts are unsigned and provenance/checksums
are not code signing, an SBOM, security certification, or full release acceptance.

Before manually publishing a draft, independently review the exact source and
artifacts, complete actual macOS/Linux/Windows build gates and the complete
applicable live checkpoint, and test these **same MSI bytes** for install,
upgrade from the retained installed/test baseline, rollback, downgrade rejection,
product coexistence and uninstall on clean Windows. The current release workflow
runs unit, payload load and read-only MSI checks; it explicitly records installer
lifecycle and live acceptance as pending. Historical 1.0.x lifecycle fixtures do
not qualify new 1.1.x customer artifacts. Keep raw results and hashes with the
release. If a fix changes source or payload, increment version and qualify new
artifacts. Do not promote a stale successful run.

Release notes must state supported products, OS/architecture/driver-manager and
SQLWCHAR profiles, supported runtime/provider versions, API/configuration changes,
known limitations and upgrade instructions. Publish fixes for known vulnerabilities
with their identifiers when available. Maintain the unreleased changelog until an
actual release. Standard SBOM, artifact attestations, Windows signing and macOS
signing/notarization remain explicit follow-ups; a developer certificate is not
provided by this workflow. Qualify a supported OpenSSL provider before releasing;
the existing OpenSSL 3.6 line's support expiry is already tracked separately.

## Minimum support contract

See [proposed minimum support and binary checks](MINIMUM_SUPPORT.md) and
[the machine-readable profiles](../packaging/release/support-profiles.json).
Build-tool versions are separate from runtime requirements. Ubuntu 22.04,
RHEL 9 and Amazon Linux 2023 are proposed independent baselines; Windows/macOS
minimum OS versions remain unresolved. None is newly declared qualified here.

## Distribution sequence

| Audience | Package choice | Qualification boundary |
|---|---|---|
| Windows 11 x64 first | Separate PostgreSQL/Redshift `.msi` | Native Windows DM, SQLWCHAR 2; installer lifecycle and customer-app evidence required |
| Ubuntu LTS amd64 next | `.deb`, plus diagnostic `.tar.gz` | Name actual Ubuntu baseline, glibc/libstdc++/OpenSSL minimums and unixODBC ABI; WSL alone is not distro installer acceptance |
| macOS next | Native `.pkg`; optional `.dmg` outer container | Separately qualified x86_64/arm64 and iODBC/unixODBC profiles; no universal-binary or interchangeable SQLWCHAR 2/4 claim |
| RHEL 9 and Amazon Linux 2023 later | Separate `.rpm` targets | Independently build/test each actual distribution/runtime baseline; do not infer common compatibility from glibc 2.34 or the RPM format |
| Advanced users / CI | Explicitly labelled `.tar.gz` | Staged runtime only, pinned OS/CPU/DM/Unicode/provider profile and dependency instructions; not a universal Linux binary |

Do not implement every installer at once. This batch ships Windows candidate
plumbing and a reusable Unix staging/archive helper, **not** DEB/RPM/PKG installers
or qualified Unix customer packages. Stage the driver runtime with
`cmake --install <qualified-build> --component ODBCPPDriverRuntime --prefix <stage>`;
then `python3 tools/release/artifacts.py archive --stage <stage> --output <archive.tar.gz>
--profile <explicit-os-cpu-dm-ucs-provider-profile>`. That helper reads existing
files only; it never builds or installs on the host. It preserves contained
symlink target bytes, rejects symlink escapes, and writes a checksum. Linux ELF
artifacts also require the dependency scan described above; unresolved C++ runtime
ceilings currently prevent Linux archive qualification. Shared
runtime dependencies and licensing must be resolved for each profile before
customer distribution. Full SDK installation is deliberately a separate component.

## Filesystem and XML trust boundaries

These are local operator tools, not a service accepting remote filesystem paths.
The caller intentionally selects source, build, package, intake and output roots;
out-of-tree builds and exported sources outside the checkout remain supported.
Use directories owned against concurrent untrusted writers. The tools run with
the caller's ordinary permissions and do not promise a hostile shared-directory
sandbox or protection against another process racing filesystem mutations.

Package/checksum metadata cannot select another root: members reject absolute,
drive/stream, traversal and parent-symlink escapes. Archive staging validates
contained portable members before reading manifests. Generated version files use
atomic replacement instead of writing through existing symlinks/hardlinks.
Archives, dependency sidecars and checksums are created exclusively: an occupied
destination is refused before output, including when qualification would fail.
A failed/pending dependency report remains evidence; use a fresh destination for
a later attempt rather than overwriting it.

Unit XML uses the project's bounded evidence parser, rejecting DTD/entity
declarations (including UTF-16), external entities and excessive input/depth/node
resources. Raw input remains untouched. These controls are distinct from the
reviewed qualification assertions and do not turn a synthetic report into actual
execution evidence.
