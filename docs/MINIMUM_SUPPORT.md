# Proposed package baselines and qualification

The machine-readable contract is [support-profiles.json](../packaging/release/support-profiles.json).
Every profile is currently **proposed and unqualified**. A selected target is not
an established support promise. `null` means unresolved evidence, not “any version.”
The Windows candidate workflow may continue while Linux/macOS baselines are being
qualified; it records its own unresolved runtime policy in artifact provenance.

## Build tools are not customer dependencies

The source currently requires CMake 3.20, Python 3.9 and C++20. Windows packaging
uses WiX 6.0.2 and .NET tooling. These tools are not prerequisites for installing a
finished driver. A compiler minimum has not been qualified. OpenSSL 3 headers are
a build requirement; **3.0.0 is not a safe minimum patch-level promise**. Each
release must select a maintained provider and record exact resolved or bundled
OpenSSL versions, linkage, hashes, licenses and security support status.

## Proposed runtime matrix

| Profile | Proposed oldest target | Still required before claiming support |
|---|---|---|
| Ubuntu amd64 / unixODBC / SQLWCHAR 2 | Ubuntu 22.04 LTS; glibc 2.35 | Build complete product on this baseline; measure its actual libstdc++ GLIBCXX/CXXABI exports, DM and OpenSSL versions; test exact package on 22.04 and a named newer Ubuntu |
| Ubuntu amd64 / iODBC / SQLWCHAR 4 | Ubuntu 22.04 LTS; glibc 2.35 | Independent iODBC version/ABI and full runtime qualification; unixODBC evidence does not qualify this artifact |
| RHEL 9 x86_64 / unixODBC / SQLWCHAR 2 | RHEL 9; glibc 2.34 | Independent RPM build/install/runtime and C++/DM/provider baseline measurements |
| Amazon Linux 2023 x86_64 / unixODBC / SQLWCHAR 2 | AL2023; glibc 2.34 | Its own RPM build/install/runtime and C++/DM/provider measurements; RHEL evidence is not transferable |
| Windows x64 / Windows DM / SQLWCHAR 2 | Windows 11 evaluation target; minimum edition/build undecided | Declare and test minimum OS/build, required MSVC runtime, exact OpenSSL payload and actual customer MSI lifecycle |
| macOS x86_64 or arm64 / iODBC SQLWCHAR 4 or unixODBC SQLWCHAR 2 | Minimum OS and deployment target undecided for each separate profile | Pin deployment target, inspect Mach-O load commands/dependencies, qualify exact architecture/DM/provider and PKG on oldest plus named newer OS |

Ubuntu 22.04 is proposed because its toolchain includes GCC 11.2, glibc 2.35 and
OpenSSL 3; this does not establish that the entire project already builds with
that compiler. Standard maintenance ends in April 2027, so reassess the target
and security-maintenance arrangement before a customer release.
[Ubuntu release notes](https://discourse.ubuntu.com/t/jammy-jellyfish-release-notes/24668).
RHEL 9 and AL2023 each document glibc 2.34, but they are separate distributions
with different package/runtime baselines.
[RHEL 9 notes](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/9/html/9.0_release_notes/overview),
[AL2023 toolchain](https://docs.aws.amazon.com/linux/al2023/ug/glibc-gcc-and-binutils.html).

A glibc floor says nothing by itself about required `GLIBCXX` or `CXXABI` versions.
C++ runtime ceilings must be measured from the actual baseline's libstdc++ and
stored with that baseline's package/version/hash receipt. Do not copy a ceiling
from another distribution or infer it from a GCC major version.
[GCC libstdc++ ABI documentation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html).

Current CI runner names are build environments, not OS support floors. The
Windows installer currently guards x64 (`VersionNT64 AND NOT Arm64`), not a
minimum Windows release. macOS currently has no pinned deployment target; a
successful `macos-latest` build cannot establish compatibility with older macOS.
The four macOS architecture/manager profiles remain separate. DM minimum versions
are pending independently for Windows DM, unixODBC and iODBC. Record the actual
manager identity, headers, runtime and both driver/app `SQLWCHAR` widths; 2-byte
and 4-byte builds are not interchangeable installations.

## Inspect every shipped Linux ELF

Build on the oldest intended baseline, stage the final payload, then inspect it
without loading its code:

```sh
python3 tools/release/elf_dependencies.py \
  --stage /path/to/stage \
  --profile linux-ubuntu22.04-x86-64-unixodbc-ucs2-openssl-system-shared \
  --output /path/to/dependencies.json
```

The helper uses GNU `readelf --version-info --wide` and `--dynamic --wide`.
It hashes and scans **all ELF files in the staged tree**, including bundled
libraries, helper binaries and contained file aliases, checks ELF class/machine,
and records `DT_NEEDED`, raw tool output and required version maxima. It reads
version **needs**, not symbols defined/exported by the file. Numeric comparison
handles `2.10 > 2.9`; unknown/private version tags in the three checked families
fail explicitly instead of silently passing.
[GNU readelf documentation](https://sourceware.org/binutils/docs/binutils/readelf.html).

Exit 0 means the three numeric symbol-family checks passed. Exit 1 means a
requirement exceeds the profile ceiling, an artifact/tool failed inspection, or
no ELF was found. Exit 2 means baseline ceilings remain unknown. The current
profiles deliberately leave `GLIBCXX` and `CXXABI` ceilings pending, so they cannot
produce a passing Linux archive yet. After baseline measurement and review, fill
those fields with the evidence location; never weaken them to accommodate a
binary accidentally built on a newer host.

`artifacts.py archive` applies the same check for Linux profiles or any staged
ELF, rejects a mismatched crypto linkage, and writes a `.dependencies.json`
sidecar. It refuses both failed and pending checks before producing the archive.
A different profile label cannot bypass ELF inspection. The sidecar binds each
inspected ELF by hash; keep it with archive checksum and package qualification
receipts. Non-ELF Windows sealing and macOS staging do not use Linux ceilings.

A numeric pass is **not** release acceptance. Resolve the full system dependency
closure, non-GLIBC/C++ symbol families (retained as unmodeled requirements), CPU
instruction-set requirements, OpenSSL plugins, DM ABI, package manager metadata,
licensing and installation. Run offline suites and applicable live/application
checks using the **same package bytes** on the oldest baseline and a named newer
environment. WSL runtime tests help, but do not substitute for DEB/RPM installation
and removal on the declared distribution. Native distribution packages and their
metadata remain follow-ups: Ubuntu DEB first, separate RHEL/AL2023 RPMs later;
macOS PKG with optional DMG wrapper only after native qualification.
