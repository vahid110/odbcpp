# spdlog 1.17.0 / bundled fmt 12.1.0 security backports

This derived dependency remains **spdlog 1.17.0 with fmt 12.1.0 plus four
upstream security fixes**. It is not fmt 12.2.0; FMT_VERSION remains 120100.
The official latest stable spdlog release was still v1.17.0 at the 2026-10-25
review. No ambient external fmt or unrelated dependency upgrade is selected.

The original [spdlog archive](https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip)
resolves to spdlog commit `79524ddd08a4ec981b7fea76afd08ee05f83755d`;
archive SHA256 is `b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2`.
Upstream fmt 12.1.0 is `407c905e45ad75fc29bf0f9bb7c5c2fd3475976f`;
12.2.0 is `1be298e1bd68957e4cd352e1f676f00e07dcfb57`.
This is a backport onto the actual bundled header bytes, not replacement with
all 12.2.0 headers (which contain unrelated API and performance changes).

## Exact upstream source changes

- [fc17e825d9b53cb7920d36abfa568d59cde2dd7b](https://github.com/fmtlib/fmt/commit/fc17e825d9b53cb7920d36abfa568d59cde2dd7b): remove the entire conditional shell-based `fmt::say` helper from os.h.
- [dc05bee30755bb993add7ed90845d87c2315f9c6](https://github.com/fmtlib/fmt/commit/dc05bee30755bb993add7ed90845d87c2315f9c6): return before dereferencing an empty parse_header range. Only the header hunk is backported.
- [4ccf1d4faf977a77f157fcad2b96b663109f59cb](https://github.com/fmtlib/fmt/commit/4ccf1d4faf977a77f157fcad2b96b663109f59cb), PR4742: reject a trailing percent before parsing its header.
- [128ba144ab85ddd0ad9ea39f2febdfb51802cb1b](https://github.com/fmtlib/fmt/commit/128ba144ab85ddd0ad9ea39f2febdfb51802cb1b), PR4800: unsigned bounds comparison in unpacked basic_format_args::get rejects negative indices. This does not add the unrelated newer positional-width feature to 12.1.0.

The JSON control retains exact before/after snippets and upstream commit IDs.
Its expected whole-file identities are:

| Bundled header | Original SHA256 | Patched SHA256 |
| --- | --- | --- |
| base.h | `892fcc48f31a80382f0e304e427d68d2fd62a2d78aef142020199120db5269fb` | `a64ff86b82a7e8d9e9df836b9f2fe7a17c7313dd2b52f9d58ce48730b6b8e9a6` |
| printf.h | `afddb422a86125e04a7260bb0f0ae3423ea8d591181ed5d4ef60b7a5d068993a` | `7088f9840ba6e1d5b5dc465830217cd2bd15231258c70cb7113e963ef03fb626` |
| os.h | `45d4e84be4e84a128442f04eda06e1305c8e02c61166854a3b8b348194d35da5` | `0c51b8fc61226be0713feb3a64316912701a979e2cf4cc1ec302e73ea4174f37` |

FindLogging validates every header on **every configure**. All original hashes
are accepted only for the first application; all patched hashes make repeated
configuration a no-op after verification. Mixed, partially patched, missing,
symlinked or unknown header inputs fail. All replacements and result hashes
are computed before the first write. Each final file is read back and checked.
All fixed staging paths are preflighted too: any existing file, directory or
symlink (including a dangling link) is rejected before any write. The source/build
tree must be trusted and not concurrently mutated by another same-user process;
this recipe does not create a hostile-filesystem transaction boundary.
Patched output is explicitly LF on every platform. CMake file(WRITE) uses native
text mode on Windows and changes those bytes to CRLF, so the recipe instead uses
file(CONFIGURE) with NEWLINE_STYLE UNIX and a single-pass content placeholder.
The exact original/result hashes remain unchanged; CRLF dependency inputs are
rejected rather than silently accepted or rehashed. LF/CRLF checkouts of the
recipe/control metadata produce the same pinned output, without configuring
tokens embedded in the upstream C++ text.
An interrupted write can leave a mixed state that must be repopulated from the
pinned archive; it is intentionally not silently repaired. The patch does not
create a dependency on Python, git or an external patch utility at configure time.

spdlog's LICENSE and all bundled header copyright/permission notices are retained.
The upstream fmt license is also retained in [fmt-LICENSE](fmt-LICENSE) for the
backported snippets. No third-party code is relicensed under the project license.

## Focused checks and remaining qualification

The offline control suite consumes an existing pinned archive, without downloads:

```sh
python3 -B tools/ci/test_spdlog_fmt_patch.py --archive /path/to/spdlog-v1.17.0.zip -v
```

For CTest integration the same suite can use `--spdlog-source ${spdlog_SOURCE_DIR}`;
it validates and reconstructs original fixtures from either accepted source state
and always operates on temporary copies. It tests exact output, repeated actual
project(NONE) configure, tampering, wrong hashes/hunks and partial states. It does
not execute fmt. The test C++ GoogleTests in tools/ci/test_bundled_fmt_security.cpp
must be registered against GTest::gtest_main and the bundled include path with
FMT_HEADER_ONLY=1, then compiled/run normally and under ASan. They preserve
negative non-NUL-terminated trailing-percent, empty range and unpacked negative
index reproducers plus valid-format/index controls. A frozen unpatched archive
is the baseline fixture; no original runtime failure is claimed until actually
compiled/executed. Do not use a scanner version label or offline recipe tests
as memory-safety runtime acceptance.

At review time no ODBCPP printf/say include or call was found; these repairs remove
known defective dependency code and a latent shell API. They do not establish an
exploitable ODBCPP runtime entrypoint. Production/platform/compiler/ASan acceptance
and ordinary logging regression remain required independently of these controls.
