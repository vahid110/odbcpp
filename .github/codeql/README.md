# CodeQL extraction and publication

The security workflow builds the real REDSHIFT product before trusting C++
results. The 2026-10-25 no-build audit indexed nearly every file while recording
parser errors in 210 of 245 inferred compilations. It also omitted
`tests/unit/test_backend_contract.cpp` from both extraction and its baseline
denominator. A successful scanner process or a zero finding count was insufficient.

## Analysis profiles

| Profile | Actual scope | Evidence and limits |
|---|---|---|
| `cpp-redshift` | Fresh Ubuntu 24.04/GCC Debug REDSHIFT, C++20, OpenSSL system shared, unixODBC/SQLWCHAR2; core/driver, existing ordinary test sources and generated SDK header-isolation units | Actual CMake compiler records and successful extraction must agree. No parser errors or incomplete compilations are accepted. `test_backend_contract.cpp` is required independently of CodeQL's baseline. |
| `cpp-source` | Supplemental no-build analysis of all tracked C++ sources and headers | Prevents dormant/platform/backend/optional code from disappearing when adding a real build. Reports extraction quality without presenting inferred code as actual-build proof. Only the two exact existing omissions in `coverage-exceptions.json` are allowed; new omissions fail. |
| `python` | All tracked `.py` files, including workflow controls | Successful-extraction diagnostics must cover the Git inventory. This is not execution, secret-history or cloud admission proof. |
| `actions` | All tracked workflow YAML | Successful-extraction diagnostics must cover the Git inventory. Standalone PowerShell, shell, CMake, C# and installer sources need their own review. |

All profiles run the default security queries plus `security-extended`. The audit's
`security-and-quality` experiment added thousands of maintainability results;
this recurring workflow keeps the security expansion without that unrelated
quality backlog. No source paths or security rules are filtered from SARIF.

The C++ build uses two workers and compiles, but never executes, tests. Two
existing REDSHIFT `EXCLUDE_FROM_ALL` test targets are built explicitly so the
actual compile-command inventory has no silent unbuilt targets. Their live-test
registration remains OFF. There are no services, PostgreSQL startup, DSNs, CTest,
native authentication, credentials, cloud calls, caches or restored binaries in
this workflow. It does not repeat the primary test matrix.

The manual profile does not qualify POSTGRESQL-only composition, Windows setup/
registry/IOCP, Apple trust APIs, alternate ODBC widths, installed packages, runtime
TLS, or the default-off native AWS SDK modules. Those native modules require a
separate reviewed Darwin arm64/static OpenSSL/explicit SDK profile and explicit
targets. The broad source profile retains visibility, not universal coverage.

## Independent extraction check

`coverage.py` reads raw SARIF, successful-extraction diagnostics, the tracked-file
inventory, actual CMake cache and compiler commands. For the manual profile it
requires real C++20 arguments, both REDSHIFT composition compile records, every
owned compiled translation unit, every generated SDK header unit and six named
critical sources. It also requires positive complete compilation metrics and
zero extractor compiler/syntax errors. It does not infer extraction from an
archive, baseline denominator, native compiler success or result count.
Its only CLI input is the allowlisted profile; repository and artifact paths are
fixed by the checked-in helper, rather than accepted from environment or caller.
Each profile requires exactly one raw SARIF file containing exactly one CodeQL
run with its expected language's extraction diagnostics. Additional files or
runs fail closed before aggregation, so another run cannot overwrite failed
extraction metrics. Raw input is preserved unchanged.

Publication follows that check. `analyze` writes raw SARIF with `upload: never`;
`upload-sarif` publishes only after coverage passes. Raw SARIF and the coverage
report are retained even when verification fails. This is not finding suppression:
the helper does not edit, delete or relabel any result. A rejected scan fails CI
and retains its evidence instead of publishing an apparent clean replacement.

The action code is pinned to reviewed official commits; `tools: linked` selects
the compatible CodeQL bundle shipped with that pinned action. Actual tool
versions and SARIF/input hashes are recorded. All jobs use standard Ubuntu
runners with a 30-minute limit, two query threads and a 4096 MB evaluator setting.
That setting is not an OS-enforced process-tree memory cap. No automatic
in-progress cancellation or privileged pull-request trigger is used.

## Transition from default setup

Default setup and this advanced workflow must not be run as competing publishers.
The coordinator must complete independent review of the exact candidate before
changing GitHub settings. This implementation does not change settings itself.

1. Push the reviewed candidate branch when authorized. Its branch push does not
   trigger this workflow's `main` push graph.
2. Once the advanced workflow is ready for its first PR execution, the coordinator
   disables default setup and opens/updates the reviewed PR. Do not infer that
   adding this file disables default setup or makes custom uploads acceptable.
3. Follow one exact-head hosted execution through all four profiles. Inspect
   actual compiler commands, zero manual extraction errors, file inventories,
   raw SARIF, successful coverage reports, upload processing and categories.
   Local offline tests do not establish hosted build/extraction acceptance.
4. After reviewed integration and a successful exact-main advanced scan, inspect
   historical default-setup categories for stale configurations. Any retirement
   is a separate coordinator decision; do not bulk-dismiss findings as migration.

The categories include both language and profile, so manual and supplemental C++
results cannot overwrite each other. This workflow does not claim an OpenSSF
Passing badge, cross-platform acceptance or a clean security assessment merely
because its checks are green.

## Ownership and focused qualification

| Component | Owner | Rationale / allowed dependencies | Proof |
|---|---|---|---|
| Scheduling/build/extraction/publication | `.github/workflows/codeql.yml` | Repository CI policy; pinned official actions, standard CMake, scanner helper | Actionlint and independent workflow review; actual hosted run pending |
| Security suite and exact inferred omissions | `.github/codeql/config.yml`, `coverage-exceptions.json` | Scanner policy, not SDK or product behavior | Pinned compatible suite resolution; explicit per-path boundary review |
| Evidence verifier | `.github/codeql/coverage.py` | CodeQL/CMake/Git evidence only, Python standard library; no product/SDK API | Offline rejection tests and raw audit-SARIF smoke |
| Control regression tests | `.github/codeql/test_coverage.py` | Synthetic scanner records, no feature assertions or live scheduling | `python3 -B -m unittest discover -s .github/codeql -p 'test_*.py' -v` |

These helpers belong beside the scanner configuration because they interpret CI
artifacts; they are not reusable provider-neutral runtime foundations. SDK,
backend, ODBC, tests/CMake and product contracts are unchanged.

Primary references: [compiled-language build modes](https://docs.github.com/en/code-security/reference/code-scanning/codeql/build-options-for-compiled-languages),
[query suites](https://docs.github.com/en/code-security/concepts/code-scanning/codeql/codeql-query-suites),
[pinned init inputs](https://github.com/github/codeql-action/blob/9b3254085494b55a0f7c4c2b3d2d52cc712ce370/init/action.yml),
[pinned analyze inputs](https://github.com/github/codeql-action/blob/9b3254085494b55a0f7c4c2b3d2d52cc712ce370/analyze/action.yml),
[multiple SARIF categories](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/integrate-with-existing-tools/upload-sarif-file),
[default-setup transition](https://docs.github.com/en/code-security/reference/code-scanning/troubleshoot-analysis-errors/results-different-than-expected).
